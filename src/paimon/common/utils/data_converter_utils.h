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
#include <charconv>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "arrow/type.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "paimon/common/data/binary_row.h"
#include "paimon/common/data/binary_row_writer.h"
#include "paimon/common/data/binary_string.h"
#include "paimon/common/utils/date_time_utils.h"
#include "paimon/common/utils/java_string_utils.h"
#include "paimon/common/utils/string_utils.h"
#include "paimon/core/casting/date_to_string_cast_executor.h"
#include "paimon/defs.h"
#include "paimon/predicate/literal.h"
#include "paimon/result.h"
#include "paimon/status.h"

namespace paimon {
class MemoryPool;

#define RETURN_INVALID_WITH_FIELD_INFO(value, field_idx, value_str, type)                    \
    if ((value) == std::nullopt) {                                                           \
        return Status::Invalid(                                                              \
            fmt::format("cannot convert field idx {}, field value {} to type {}", field_idx, \
                        value_str, type));                                                   \
    }

class DataConverterUtils {
 public:
    DataConverterUtils() = delete;
    ~DataConverterUtils() = delete;

    using StrToBinaryRowConverter =
        std::function<Status(const std::string&, int32_t, BinaryRowWriter*)>;
    using BinaryRowFieldToStrConverter =
        std::function<Result<std::string>(const BinaryRow&, int32_t)>;

    static Result<StrToBinaryRowConverter> CreateDataToBinaryRowConverter(arrow::Type::type type,
                                                                          MemoryPool* pool) {
        StrToBinaryRowConverter converter;
        switch (type) {
            case arrow::Type::BOOL:
                converter = [](const std::string& value_str, int32_t field_idx,
                               BinaryRowWriter* writer) {
                    auto value = StringUtils::StringToValue<bool>(value_str);
                    RETURN_INVALID_WITH_FIELD_INFO(value, field_idx, value_str,
                                                   arrow::internal::ToString(arrow::Type::BOOL));
                    writer->WriteBoolean(field_idx, value.value());
                    return Status::OK();
                };
                break;
            case arrow::Type::INT8:
                converter = [](const std::string& value_str, int32_t field_idx,
                               BinaryRowWriter* writer) {
                    auto value = StringUtils::StringToValue<int8_t>(value_str);
                    RETURN_INVALID_WITH_FIELD_INFO(value, field_idx, value_str,
                                                   arrow::internal::ToString(arrow::Type::INT8));
                    writer->WriteByte(field_idx, value.value());
                    return Status::OK();
                };
                break;
            case arrow::Type::INT16:
                converter = [](const std::string& value_str, int32_t field_idx,
                               BinaryRowWriter* writer) {
                    auto value = StringUtils::StringToValue<int16_t>(value_str);
                    RETURN_INVALID_WITH_FIELD_INFO(value, field_idx, value_str,
                                                   arrow::internal::ToString(arrow::Type::INT16));
                    writer->WriteShort(field_idx, value.value());
                    return Status::OK();
                };
                break;
            case arrow::Type::INT32:
                converter = [](const std::string& value_str, int32_t field_idx,
                               BinaryRowWriter* writer) {
                    auto value = StringUtils::StringToValue<int32_t>(value_str);
                    RETURN_INVALID_WITH_FIELD_INFO(value, field_idx, value_str,
                                                   arrow::internal::ToString(arrow::Type::INT32));
                    writer->WriteInt(field_idx, value.value());
                    return Status::OK();
                };
                break;
            case arrow::Type::INT64:
                converter = [](const std::string& value_str, int32_t field_idx,
                               BinaryRowWriter* writer) {
                    auto value = StringUtils::StringToValue<int64_t>(value_str);
                    RETURN_INVALID_WITH_FIELD_INFO(value, field_idx, value_str,
                                                   arrow::internal::ToString(arrow::Type::INT64));
                    writer->WriteLong(field_idx, value.value());
                    return Status::OK();
                };
                break;
            case arrow::Type::STRING:
                converter = [pool](const std::string& value_str, int32_t field_idx,
                                   BinaryRowWriter* writer) {
                    BinaryString value = BinaryString::FromString(value_str, pool);
                    writer->WriteString(field_idx, value);
                    return Status::OK();
                };
                break;
            case arrow::Type::DATE32:
                converter = [](const std::string& value_str, int32_t field_idx,
                               BinaryRowWriter* writer) {
                    PAIMON_ASSIGN_OR_RAISE(int32_t date_value,
                                           StringUtils::StringToDate(value_str));
                    writer->WriteInt(field_idx, date_value);
                    return Status::OK();
                };
                break;
            default:
                return Status::NotImplemented(
                    fmt::format("Do not support type {} in partition binary row",
                                arrow::internal::ToString(type)));
        }
        return converter;
    }

    static Result<BinaryRowFieldToStrConverter> CreateBinaryRowFieldToStringConverter(
        arrow::Type::type type, bool legacy_partition_name_enabled) {
        BinaryRowFieldToStrConverter converter;
        switch (type) {
            case arrow::Type::BOOL:
                converter = [](const BinaryRow& row, int32_t field_idx) {
                    bool data = row.GetBoolean(field_idx);
                    std::string result = data ? "true" : "false";
                    return result;
                };
                break;
            case arrow::Type::INT8:
                converter = [](const BinaryRow& row, int32_t field_idx) {
                    auto data = static_cast<int8_t>(row.GetByte(field_idx));
                    return std::to_string(data);
                };
                break;
            case arrow::Type::INT16:
                converter = [](const BinaryRow& row, int32_t field_idx) {
                    auto data = row.GetShort(field_idx);
                    return std::to_string(data);
                };
                break;
            case arrow::Type::INT32:
                converter = [](const BinaryRow& row, int32_t field_idx) {
                    auto data = row.GetInt(field_idx);
                    return std::to_string(data);
                };
                break;
            case arrow::Type::INT64:
                converter = [](const BinaryRow& row, int32_t field_idx) {
                    auto data = row.GetLong(field_idx);
                    return std::to_string(data);
                };
                break;
            case arrow::Type::STRING:
                converter = [](const BinaryRow& row, int32_t field_idx) {
                    BinaryString data = row.GetString(field_idx);
                    return data.ToString();
                };
                break;
            case arrow::Type::DATE32: {
                if (legacy_partition_name_enabled) {
                    converter = [](const BinaryRow& row, int32_t field_idx) -> Result<std::string> {
                        int32_t data = row.GetDate(field_idx);
                        return std::to_string(data);
                    };
                } else {
                    auto date_to_string_cast_executor =
                        std::make_shared<DateToStringCastExecutor>();
                    converter = [date_to_string_cast_executor](
                                    const BinaryRow& row,
                                    int32_t field_idx) -> Result<std::string> {
                        int32_t data = row.GetDate(field_idx);
                        PAIMON_ASSIGN_OR_RAISE(Literal literal,
                                               date_to_string_cast_executor->Cast(
                                                   Literal(FieldType::DATE, data), arrow::utf8()));
                        return literal.GetValue<std::string>();
                    };
                }
                break;
            }
            default:
                return Status::NotImplemented(
                    fmt::format("Do not support arrow {} in partition binary row",
                                arrow::internal::ToString(type)));
        }
        return converter;
    }

    using NullFieldWriter = std::function<void(int32_t, BinaryRowWriter*)>;

    /// Like the `arrow::Type::type` overload, and also supports the partition types whose
    /// `BinaryRow` layout depends on type parameters: TIMESTAMP, DECIMAL, FLOAT and DOUBLE.
    static Result<StrToBinaryRowConverter> CreateDataToBinaryRowConverter(
        const std::shared_ptr<arrow::DataType>& type, MemoryPool* pool) {
        switch (type->id()) {
            case arrow::Type::FLOAT:
                return StrToBinaryRowConverter(
                    [](const std::string& value_str, int32_t field_idx, BinaryRowWriter* writer) {
                        PAIMON_ASSIGN_OR_RAISE(float value, JavaStringUtils::ParseFloat(value_str));
                        writer->WriteFloat(field_idx, value);
                        return Status::OK();
                    });
            case arrow::Type::DOUBLE:
                return StrToBinaryRowConverter(
                    [](const std::string& value_str, int32_t field_idx, BinaryRowWriter* writer) {
                        PAIMON_ASSIGN_OR_RAISE(double value,
                                               JavaStringUtils::ParseDouble(value_str));
                        writer->WriteDouble(field_idx, value);
                        return Status::OK();
                    });
            case arrow::Type::TIMESTAMP: {
                int32_t precision = DateTimeUtils::GetPrecisionFromType(
                    std::static_pointer_cast<arrow::TimestampType>(type));
                return StrToBinaryRowConverter([precision](const std::string& value_str,
                                                           int32_t field_idx,
                                                           BinaryRowWriter* writer) {
                    PAIMON_ASSIGN_OR_RAISE(Timestamp value,
                                           JavaStringUtils::ParseTimestamp(value_str));
                    writer->WriteTimestamp(field_idx, value, precision);
                    return Status::OK();
                });
            }
            case arrow::Type::DECIMAL128: {
                auto decimal_type = std::static_pointer_cast<arrow::Decimal128Type>(type);
                int32_t precision = decimal_type->precision();
                int32_t scale = decimal_type->scale();
                return StrToBinaryRowConverter([precision, scale](const std::string& value_str,
                                                                  int32_t field_idx,
                                                                  BinaryRowWriter* writer) {
                    PAIMON_ASSIGN_OR_RAISE(Decimal value,
                                           JavaStringUtils::ParseDecimal(value_str, precision, scale));
                    writer->WriteDecimal(field_idx, value, precision);
                    return Status::OK();
                });
            }
            default:
                return CreateDataToBinaryRowConverter(type->id(), pool);
        }
    }

    /// Renders partition values like Java `InternalRowPartitionComputer`: with legacy partition
    /// names the internal value's `toString()`, otherwise the cast to STRING.
    static Result<BinaryRowFieldToStrConverter> CreateBinaryRowFieldToStringConverter(
        const std::shared_ptr<arrow::DataType>& type, bool legacy_partition_name_enabled) {
        switch (type->id()) {
            case arrow::Type::FLOAT:
                return BinaryRowFieldToStrConverter(
                    [](const BinaryRow& row, int32_t field_idx) -> Result<std::string> {
                        return JavaStringUtils::FloatToString(row.GetFloat(field_idx));
                    });
            case arrow::Type::DOUBLE:
                return BinaryRowFieldToStrConverter(
                    [](const BinaryRow& row, int32_t field_idx) -> Result<std::string> {
                        return JavaStringUtils::DoubleToString(row.GetDouble(field_idx));
                    });
            case arrow::Type::TIMESTAMP: {
                auto timestamp_type = std::static_pointer_cast<arrow::TimestampType>(type);
                int32_t precision = DateTimeUtils::GetPrecisionFromType(timestamp_type);
                if (legacy_partition_name_enabled) {
                    return BinaryRowFieldToStrConverter(
                        [precision](const BinaryRow& row, int32_t field_idx) -> Result<std::string> {
                            return JavaStringUtils::TimestampToLocalDateTimeString(
                                row.GetTimestamp(field_idx, precision));
                        });
                }
                if (!timestamp_type->timezone().empty()) {
                    // Java renders TIMESTAMP WITH LOCAL TIME ZONE in the JVM default time zone.
                    return Status::NotImplemented(
                        "Do not support TIMESTAMP WITH LOCAL TIME ZONE partition names without "
                        "partition.legacy-name");
                }
                return BinaryRowFieldToStrConverter(
                    [precision](const BinaryRow& row, int32_t field_idx) -> Result<std::string> {
                        return JavaStringUtils::TimestampToString(
                            row.GetTimestamp(field_idx, precision), precision);
                    });
            }
            case arrow::Type::DECIMAL128: {
                auto decimal_type = std::static_pointer_cast<arrow::Decimal128Type>(type);
                int32_t precision = decimal_type->precision();
                int32_t scale = decimal_type->scale();
                return BinaryRowFieldToStrConverter(
                    [precision, scale](const BinaryRow& row,
                                       int32_t field_idx) -> Result<std::string> {
                        return JavaStringUtils::DecimalToPlainString(
                            row.GetDecimal(field_idx, precision, scale));
                    });
            }
            default:
                return CreateBinaryRowFieldToStringConverter(type->id(),
                                                             legacy_partition_name_enabled);
        }
    }

    /// Non-compact TIMESTAMP and DECIMAL keep a variable-length slot, so a null must go through
    /// `WriteTimestamp` / `WriteDecimal` rather than `SetNullAt`.
    static NullFieldWriter CreateNullFieldWriter(const std::shared_ptr<arrow::DataType>& type) {
        if (type->id() == arrow::Type::TIMESTAMP) {
            int32_t precision = DateTimeUtils::GetPrecisionFromType(
                std::static_pointer_cast<arrow::TimestampType>(type));
            if (!Timestamp::IsCompact(precision)) {
                return [precision](int32_t field_idx, BinaryRowWriter* writer) {
                    writer->WriteTimestamp(field_idx, std::nullopt, precision);
                };
            }
        } else if (type->id() == arrow::Type::DECIMAL128) {
            int32_t precision = std::static_pointer_cast<arrow::Decimal128Type>(type)->precision();
            if (!Decimal::IsCompact(precision)) {
                return [precision](int32_t field_idx, BinaryRowWriter* writer) {
                    writer->WriteDecimal(field_idx, std::nullopt, precision);
                };
            }
        }
        return [](int32_t field_idx, BinaryRowWriter* writer) { writer->SetNullAt(field_idx); };
    }
};

}  // namespace paimon
