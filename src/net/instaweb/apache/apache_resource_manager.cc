// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jmarantz@google.com (Joshua Marantz)

#include "net/instaweb/apache/apache_resource_manager.h"

#include "httpd.h"                  // NOLINT
#include "http_protocol.h"          // NOLINT
#include "net/instaweb/apache/apache_cache.h"
#include "net/instaweb/apache/apache_config.h"
#include "net/instaweb/apache/apache_rewrite_driver_factory.h"
#include "net/instaweb/apache/serf_url_async_fetcher.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/util/public/abstract_mutex.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/null_message_handler.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/thread_system.h"

namespace {

// TODO(jmarantz): add a configuration that allows turning off cache.flush
// checking or possibly customizes the filename.
const int64 kDefaultCacheFlushIntervalSec = 5;

const char kCacheFlushCount[] = "cache_flush_count";
const char kStatistics404Count[] = "statistics_404_count";

}  // namespace

namespace net_instaweb {

class Statistics;
class RewriteStats;
class HTTPCache;

ApacheResourceManager::ApacheResourceManager(
    ApacheRewriteDriverFactory* factory,
    server_rec* server,
    const StringPiece& version)
    : ResourceManager(factory),
      apache_factory_(factory),
      server_rec_(server),
      version_(version.data(), version.size()),
      hostname_identifier_(StrCat(server->server_hostname, ":",
                                  IntegerToString(server->port))),
      initialized_(false),
      subresource_fetcher_(NULL),
      cache_flush_mutex_(thread_system()->NewMutex()),
      last_cache_flush_check_sec_(0),
      cache_flush_poll_interval_sec_(kDefaultCacheFlushIntervalSec),
      cache_flush_count_(NULL) {  // Lazy-initialized under mutex.
  config()->set_description(hostname_identifier_);
  // We may need the message handler for error messages very early, before
  // we get to InitResourceManager in ChildInit().
  set_message_handler(apache_factory_->message_handler());

  // Currently, mod_pagespeed always runs upstream of mod_headers when used as
  // an origin server.  Note that in a proxy application, this might not be the
  // case.  I'm not sure how we can detect this on a per-request basis so
  // that might require a small refactor.
  //
  // TODO(jmarantz): We'd like to change this for various reasons but are unsure
  // of the impact.
  set_response_headers_finalized(false);
}

ApacheResourceManager::~ApacheResourceManager() {
}

void ApacheResourceManager::Initialize(Statistics* statistics) {
  statistics->AddVariable(kCacheFlushCount);
  statistics->AddVariable(kStatistics404Count);
}

Variable* ApacheResourceManager::statistics_404_count() {
  return statistics()->GetVariable(kStatistics404Count);
}

bool ApacheResourceManager::InitFileCachePath() {
  GoogleString file_cache_path = config()->file_cache_path();
  if (file_system()->IsDir(file_cache_path.c_str(),
                           message_handler()).is_true()) {
    return true;
  }
  bool ok = file_system()->RecursivelyMakeDir(file_cache_path,
                                              message_handler());
  if (ok) {
    apache_factory_->AddCreatedDirectory(file_cache_path);
  }
  return ok;
}

ApacheConfig* ApacheResourceManager::config() {
  return ApacheConfig::DynamicCast(global_options());
}

void ApacheResourceManager::ReportNotFoundHelper(StringPiece error_message,
                                                 request_rec* request,
                                                 Variable* error_count) {
  error_count->Add(1);
  request->status = HttpStatus::kNotFound;
  ap_send_error_response(request, 0);
  message_handler()->Message(kWarning, "%s: not found (404)",
                             (error_message.empty() ? "(null)" :
                              error_message.as_string().c_str()));
}

void ApacheResourceManager::ChildInit() {
  DCHECK(!initialized_);
  if (!initialized_) {
    initialized_ = true;
    ApacheCache* cache = apache_factory_->GetCache(config());
    set_http_cache(cache->http_cache());
    set_page_property_cache(cache->page_property_cache());
    set_client_property_cache(cache->client_property_cache());
    set_metadata_cache(cache->cache());
    set_lock_manager(cache->lock_manager());
    UrlPollableAsyncFetcher* fetcher = apache_factory_->GetFetcher(config());
    set_default_system_fetcher(fetcher);
    if (!config()->slurping_enabled_read_only()) {
      subresource_fetcher_ = fetcher;
    }

    // To allow Flush to come in while multiple threads might be
    // referencing the signature, we must be able to mutate the
    // timestamp and signature atomically.  RewriteOptions supports
    // an optional read/writer lock for this purpose.
    global_options()->set_cache_invalidation_timestamp_mutex(
        thread_system()->NewRWLock());
    apache_factory_->InitResourceManager(this);
  }
}

bool ApacheResourceManager::PoolDestroyed() {
  ShutDownDrivers();
  return apache_factory_->PoolDestroyed(this);
}

// TODO(jmarantz): implement an HTTP request in instaweb_handler.cc that
// writes the cache-flush file, so we can allow cache flush via:
// http://yourhost.com:port/flushcache.  We still have to write the file
// so that all child processes see the flush, and so the flush persists
// across server restart.
void ApacheResourceManager::PollFilesystemForCacheFlush() {
  if (cache_flush_poll_interval_sec_ > 0) {
    int64 now_sec = timer()->NowMs() / Timer::kSecondMs;
    bool check_cache_file = false;
    {
      ScopedMutex lock(cache_flush_mutex_.get());
      if (now_sec >= (last_cache_flush_check_sec_ +
                      cache_flush_poll_interval_sec_)) {
        last_cache_flush_check_sec_ = now_sec;
        check_cache_file = true;
      }
      if (cache_flush_count_ == NULL) {
        cache_flush_count_ = statistics()->AddVariable(kCacheFlushCount);
      }
    }

    if (check_cache_file) {
      if (cache_flush_filename_.empty()) {
        cache_flush_filename_ = "cache.flush";
      }
      if (cache_flush_filename_[0] != '/') {
        // Note that we catch this in mod_instaweb.cc in the parsing of
        // option kModPagespeedFileCachePath.
        DCHECK_EQ('/', config()->file_cache_path()[0]);
        cache_flush_filename_ = StrCat(config()->file_cache_path(), "/",
                                       cache_flush_filename_);
      }
      int64 cache_flush_timestamp_sec;
      NullMessageHandler null_handler;
      if (file_system()->Mtime(cache_flush_filename_,
                               &cache_flush_timestamp_sec,
                               &null_handler)) {
        int64 timestamp_ms = cache_flush_timestamp_sec * Timer::kSecondMs;
        if (global_options()->UpdateCacheInvalidationTimestampMs(
                timestamp_ms, lock_hasher())) {
          cache_flush_count_->Add(1);
        }
      }
    }
  }
}

}  // namespace net_instaweb