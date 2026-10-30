/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "arrow/api.h"
#include "paimon/visibility.h"

namespace paimon {

/// One top-level schema change. Field ids already present on an added column are ignored
/// and assigned by the catalog when the change is committed.
class PAIMON_EXPORT SchemaChange {
 public:
    enum class Kind {
        kAddColumn,
        kDropColumn,
        kRenameColumn,
        kUpdateColumnType,
        kSetOption,
        kRemoveOption
    };

    static SchemaChange AddColumn(std::shared_ptr<arrow::Field> field) {
        SchemaChange change;
        change.kind_ = Kind::kAddColumn;
        change.field_ = std::move(field);
        return change;
    }

    static SchemaChange DropColumn(std::string name) {
        SchemaChange change;
        change.kind_ = Kind::kDropColumn;
        change.name_ = std::move(name);
        return change;
    }

    static SchemaChange RenameColumn(std::string old_name, std::string new_name) {
        SchemaChange change;
        change.kind_ = Kind::kRenameColumn;
        change.name_ = std::move(old_name);
        change.new_name_ = std::move(new_name);
        return change;
    }

    static SchemaChange UpdateColumnType(std::string name, std::shared_ptr<arrow::DataType> type) {
        SchemaChange change;
        change.kind_ = Kind::kUpdateColumnType;
        change.name_ = std::move(name);
        change.type_ = std::move(type);
        return change;
    }

    static SchemaChange SetOption(std::string key, std::string value) {
        SchemaChange change;
        change.kind_ = Kind::kSetOption;
        change.name_ = std::move(key);
        change.new_name_ = std::move(value);
        return change;
    }

    static SchemaChange RemoveOption(std::string key) {
        SchemaChange change;
        change.kind_ = Kind::kRemoveOption;
        change.name_ = std::move(key);
        return change;
    }

    Kind GetKind() const {
        return kind_;
    }
    const std::shared_ptr<arrow::Field>& Field() const {
        return field_;
    }
    const std::string& Name() const {
        return name_;
    }
    const std::string& NewName() const {
        return new_name_;
    }
    const std::shared_ptr<arrow::DataType>& NewType() const {
        return type_;
    }
    const std::string& OptionKey() const {
        return name_;
    }
    const std::string& OptionValue() const {
        return new_name_;
    }

 private:
    Kind kind_ = Kind::kAddColumn;
    std::shared_ptr<arrow::Field> field_;
    std::shared_ptr<arrow::DataType> type_;
    std::string name_;
    std::string new_name_;
};

}  // namespace paimon
