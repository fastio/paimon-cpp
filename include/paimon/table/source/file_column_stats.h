/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "paimon/predicate/literal.h"
#include "paimon/result.h"
#include "paimon/visibility.h"

namespace paimon {

/// Min/max/null statistics for one column of one data file, after schema evolution.
/// Min and max are both set or both absent. They are absent when the file needs
/// schema or write-column mapping, except for a partition column whose value is
/// constant for the file.
struct PAIMON_EXPORT FileColumnBound {
    std::optional<int64_t> null_count;
    std::optional<Literal> min_value;
    std::optional<Literal> max_value;
};

/// Column statistics for one ADD file in a snapshot.
/// `columns` follows the read schema field order. `delete_row_count` is unset when
/// the file metadata does not record a deletion-vector cardinality.
struct PAIMON_EXPORT DataFileColumnStats {
    int64_t row_count = 0;
    std::optional<int64_t> delete_row_count;
    int64_t schema_id = 0;
    // True when schema evolution or write columns make non-partition min/max unusable.
    bool values_mapped = false;
    std::vector<FileColumnBound> columns;
};

/// Read ADD-file column statistics for an explicit snapshot.
/// A snapshot id that does not exist returns NotExist. An invalid option used to
/// open the table returns Invalid. The result lists every ADD file of that snapshot,
/// including files a predicate would prune.
PAIMON_EXPORT Result<std::vector<DataFileColumnStats>> ReadDataFileColumnStats(
    const std::string& table_path, const std::string& schema_json,
    const std::map<std::string, std::string>& options, int64_t snapshot_id);

}  // namespace paimon
