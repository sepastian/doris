// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
// This file is copied from
// https://github.com/ClickHouse/ClickHouse/blob/master/src/Interpreters/Cache/FileCacheFactory.h
// and modified by Doris

#pragma once

#include <vector>

#include "io/cache/block/block_file_cache.h"
#include "io/cache/block/block_file_cache_fwd.h"
#include "io/cache/block/block_file_cache_settings.h"
namespace doris {
namespace io {

enum FileCacheType {
    NORMAL,
    DISPOSABLE,
};
/**
 * Creates a FileCache object for cache_base_path.
 */
class FileCacheFactory {
public:
    static FileCacheFactory& instance();

    Status create_file_cache(const std::string& cache_base_path,
                             const FileCacheSettings& file_cache_settings, FileCacheType type);

    CloudFileCachePtr get_by_path(const IFileCache::Key& key);
    CloudFileCachePtr get_disposable_cache(const IFileCache::Key& key);
    std::vector<IFileCache::QueryFileCacheContextHolderPtr> get_query_context_holders(
            const TUniqueId& query_id);
    FileCacheFactory() = default;
    FileCacheFactory& operator=(const FileCacheFactory&) = delete;
    FileCacheFactory(const FileCacheFactory&) = delete;

private:
    std::vector<std::unique_ptr<IFileCache>> _caches;
    std::vector<std::unique_ptr<IFileCache>> _disposable_cache;
};

} // namespace io
} // namespace doris
