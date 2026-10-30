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

#include "paimon/common/utils/java_string_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

#include "fmt/format.h"
#include "paimon/status.h"

namespace paimon {

namespace {

constexpr int64_t kMillisPerSecond = 1000;
constexpr int64_t kSecondsPerDay = 86400;
constexpr int32_t kNanosPerMilli = 1000000;

int64_t FloorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

int64_t FloorMod(int64_t a, int64_t b) {
    return a - FloorDiv(a, b) * b;
}

struct CivilDate {
    int64_t year;
    int32_t month;
    int32_t day;
};

// Howard Hinnant's days_from_civil / civil_from_days, proleptic Gregorian like java.time.
CivilDate CivilFromDays(int64_t days) {
    days += 719468;
    const int64_t era = FloorDiv(days, 146097);
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    return {yoe + era * 400 + (m <= 2 ? 1 : 0), static_cast<int32_t>(m), static_cast<int32_t>(d)};
}

int64_t DaysFromCivil(int64_t year, int32_t month, int32_t day) {
    year -= month <= 2 ? 1 : 0;
    const int64_t era = FloorDiv(year, 400);
    const int64_t yoe = year - era * 400;
    const int64_t mp = month > 2 ? month - 3 : month + 9;
    const int64_t doy = (153 * mp + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool IsLeapYear(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int32_t DaysInMonth(int64_t year, int32_t month) {
    static const int32_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

struct LocalDateTime {
    CivilDate date;
    int32_t hour;
    int32_t minute;
    int32_t second;
    int32_t nano;
};

LocalDateTime ToLocalDateTime(const Timestamp& timestamp) {
    const int64_t millis = timestamp.GetMillisecond();
    const int64_t epoch_second = FloorDiv(millis, kMillisPerSecond);
    const int32_t nano = static_cast<int32_t>(FloorMod(millis, kMillisPerSecond)) * kNanosPerMilli +
                         timestamp.GetNanoOfMillisecond();
    const int64_t days = FloorDiv(epoch_second, kSecondsPerDay);
    const int64_t second_of_day = FloorMod(epoch_second, kSecondsPerDay);
    return {CivilFromDays(days), static_cast<int32_t>(second_of_day / 3600),
            static_cast<int32_t>(second_of_day / 60 % 60), static_cast<int32_t>(second_of_day % 60),
            nano};
}

// LocalDate.toString: four digits with a sign below year 0, a leading '+' above 9999.
std::string FormatYear(int64_t year) {
    if (year >= 0 && year <= 9999) {
        return fmt::format("{:04d}", year);
    }
    if (year < 0 && year > -10000) {
        return fmt::format("-{:04d}", -year);
    }
    return year > 9999 ? fmt::format("+{}", year) : fmt::format("{}", year);
}

std::string FormatFloating(bool negative, const std::string& digits, int32_t exponent) {
    // value = 0.d1d2...dn * 10^(exponent + 1) = d1.d2...dn * 10^exponent
    std::string result = negative ? "-" : "";
    const auto n = static_cast<int32_t>(digits.size());
    if (exponent >= -3 && exponent < 7) {
        if (exponent >= 0) {
            if (n <= exponent + 1) {
                result += digits + std::string(exponent + 1 - n, '0') + ".0";
            } else {
                result += digits.substr(0, exponent + 1) + "." + digits.substr(exponent + 1);
            }
        } else {
            result += "0." + std::string(-exponent - 1, '0') + digits;
        }
        return result;
    }
    result += digits.substr(0, 1) + "." + (n > 1 ? digits.substr(1) : "0") + "E" +
              std::to_string(exponent);
    return result;
}

template <typename T>
std::string JavaFloatingToString(T value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    if (value == 0) {
        return std::signbit(value) ? "-0.0" : "0.0";
    }
    // Shortest round-trip digits in scientific form, e.g. "-1.2345e-05". Java always renders at
    // least two significant digits and, among those, picks the one closest to the exact value,
    // so a single shortest digit is replaced by the correctly rounded two-digit form
    // (Float.MIN_VALUE is "1.4E-45", not "1.0E-45").
    char buffer[64];
    auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    std::string sci(buffer, result.ptr);
    if (sci.find('.') == std::string::npos) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                               std::chars_format::scientific, /*precision=*/1);
        sci.assign(buffer, result.ptr);
    }
    bool negative = sci[0] == '-';
    if (negative) {
        sci.erase(0, 1);
    }
    auto e_pos = sci.find('e');
    std::string mantissa = sci.substr(0, e_pos);
    int32_t exponent = std::stoi(sci.substr(e_pos + 1));
    mantissa.erase(std::remove(mantissa.begin(), mantissa.end(), '.'), mantissa.end());
    while (mantissa.size() > 1 && mantissa.back() == '0') {
        mantissa.pop_back();
    }
    return FormatFloating(negative, mantissa, exponent);
}

template <typename T>
Result<T> JavaParseFloating(const std::string& str) {
    std::string s = str;
    bool negative = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        negative = s[0] == '-';
        s.erase(0, 1);
    }
    if (s == "NaN") {
        return std::numeric_limits<T>::quiet_NaN();
    }
    if (s == "Infinity") {
        return negative ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
    }
    T value{};
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || result.ec != std::errc() || result.ptr != s.data() + s.size() ||
        std::isnan(value) || std::isinf(value)) {
        return Status::Invalid(fmt::format("cannot parse '{}' as a floating point value", str));
    }
    return negative ? -value : value;
}

std::string Int128ToString(Decimal::int128_t value) {
    if (value == 0) {
        return "0";
    }
    bool negative = value < 0;
    auto magnitude = negative ? -static_cast<Decimal::uint128_t>(value)
                              : static_cast<Decimal::uint128_t>(value);
    std::string digits;
    while (magnitude > 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
        magnitude /= 10;
    }
    if (negative) {
        digits.push_back('-');
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

bool ParseDigits(const std::string& str, size_t* pos, size_t min_len, size_t max_len,
                 int64_t* value) {
    size_t start = *pos;
    int64_t result = 0;
    while (*pos < str.size() && *pos - start < max_len &&
           std::isdigit(static_cast<unsigned char>(str[*pos]))) {
        result = result * 10 + (str[*pos] - '0');
        ++*pos;
    }
    if (*pos - start < min_len) {
        return false;
    }
    *value = result;
    return true;
}

bool Expect(const std::string& str, size_t* pos, char c) {
    if (*pos < str.size() && str[*pos] == c) {
        ++*pos;
        return true;
    }
    return false;
}

}  // namespace

std::string JavaStringUtils::FloatToString(float value) {
    return JavaFloatingToString(value);
}

std::string JavaStringUtils::DoubleToString(double value) {
    return JavaFloatingToString(value);
}

Result<float> JavaStringUtils::ParseFloat(const std::string& str) {
    return JavaParseFloating<float>(str);
}

Result<double> JavaStringUtils::ParseDouble(const std::string& str) {
    return JavaParseFloating<double>(str);
}

std::string JavaStringUtils::TimestampToLocalDateTimeString(const Timestamp& timestamp) {
    LocalDateTime ldt = ToLocalDateTime(timestamp);
    std::string result = fmt::format("{}-{:02d}-{:02d}T{:02d}:{:02d}", FormatYear(ldt.date.year),
                                     ldt.date.month, ldt.date.day, ldt.hour, ldt.minute);
    if (ldt.second > 0 || ldt.nano > 0) {
        result += fmt::format(":{:02d}", ldt.second);
        if (ldt.nano > 0) {
            if (ldt.nano % kNanosPerMilli == 0) {
                result += fmt::format(".{:03d}", ldt.nano / kNanosPerMilli);
            } else if (ldt.nano % 1000 == 0) {
                result += fmt::format(".{:06d}", ldt.nano / 1000);
            } else {
                result += fmt::format(".{:09d}", ldt.nano);
            }
        }
    }
    return result;
}

std::string JavaStringUtils::TimestampToString(const Timestamp& timestamp, int32_t precision) {
    LocalDateTime ldt = ToLocalDateTime(timestamp);
    std::string fraction = fmt::format("{:09d}", ldt.nano);
    while (static_cast<int32_t>(fraction.size()) > precision && fraction.back() == '0') {
        fraction.pop_back();
    }
    std::string result =
        fmt::format("{}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", FormatYear(ldt.date.year),
                    ldt.date.month, ldt.date.day, ldt.hour, ldt.minute, ldt.second);
    if (!fraction.empty()) {
        result += "." + fraction;
    }
    return result;
}

Result<Timestamp> JavaStringUtils::ParseTimestamp(const std::string& str) {
    auto invalid = [&]() {
        return Status::Invalid(fmt::format(
            "cannot parse '{}' as a timestamp, expected yyyy-MM-dd[( |T)HH:mm[:ss[.fffffffff]]]",
            str));
    };
    size_t pos = 0;
    bool negative_year = Expect(str, &pos, '-');
    if (!negative_year) {
        Expect(str, &pos, '+');
    }
    int64_t year = 0;
    int64_t month = 0;
    int64_t day = 0;
    if (!ParseDigits(str, &pos, 4, 9, &year) || !Expect(str, &pos, '-') ||
        !ParseDigits(str, &pos, 2, 2, &month) || !Expect(str, &pos, '-') ||
        !ParseDigits(str, &pos, 2, 2, &day)) {
        return invalid();
    }
    if (negative_year) {
        year = -year;
    }
    if (month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, static_cast<int32_t>(month))) {
        return invalid();
    }
    int64_t hour = 0;
    int64_t minute = 0;
    int64_t second = 0;
    int64_t nano = 0;
    if (pos < str.size()) {
        if (str[pos] != ' ' && str[pos] != 'T') {
            return invalid();
        }
        ++pos;
        if (!ParseDigits(str, &pos, 2, 2, &hour) || !Expect(str, &pos, ':') ||
            !ParseDigits(str, &pos, 2, 2, &minute)) {
            return invalid();
        }
        if (Expect(str, &pos, ':')) {
            if (!ParseDigits(str, &pos, 2, 2, &second)) {
                return invalid();
            }
            if (Expect(str, &pos, '.')) {
                size_t start = pos;
                if (!ParseDigits(str, &pos, 1, 9, &nano)) {
                    return invalid();
                }
                for (size_t i = pos - start; i < 9; ++i) {
                    nano *= 10;
                }
            }
        }
        if (pos != str.size() || hour > 23 || minute > 59 || second > 59) {
            return invalid();
        }
    }
    int64_t days = DaysFromCivil(year, static_cast<int32_t>(month), static_cast<int32_t>(day));
    int64_t epoch_second = days * kSecondsPerDay + hour * 3600 + minute * 60 + second;
    return Timestamp(epoch_second * kMillisPerSecond + nano / kNanosPerMilli,
                     static_cast<int32_t>(nano % kNanosPerMilli));
}

std::string JavaStringUtils::DecimalToPlainString(const Decimal& decimal) {
    std::string digits = Int128ToString(decimal.Value());
    int32_t scale = decimal.Scale();
    if (scale <= 0) {
        return scale == 0 ? digits : digits + std::string(-scale, '0');
    }
    bool negative = digits[0] == '-';
    if (negative) {
        digits.erase(0, 1);
    }
    if (static_cast<int32_t>(digits.size()) <= scale) {
        digits = std::string(scale + 1 - digits.size(), '0') + digits;
    }
    digits.insert(digits.size() - scale, ".");
    return negative ? "-" + digits : digits;
}

Result<Decimal> JavaStringUtils::ParseDecimal(const std::string& str, int32_t precision,
                                               int32_t scale) {
    auto invalid = [&](const std::string& reason) {
        return Status::Invalid(fmt::format("cannot parse '{}' as DECIMAL({}, {}): {}", str,
                                           precision, scale, reason));
    };
    size_t pos = 0;
    bool negative = false;
    if (pos < str.size() && (str[pos] == '+' || str[pos] == '-')) {
        negative = str[pos] == '-';
        ++pos;
    }
    std::string int_digits;
    std::string frac_digits;
    while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos]))) {
        int_digits.push_back(str[pos++]);
    }
    if (pos < str.size() && str[pos] == '.') {
        ++pos;
        while (pos < str.size() && std::isdigit(static_cast<unsigned char>(str[pos]))) {
            frac_digits.push_back(str[pos++]);
        }
    }
    if (pos != str.size() || (int_digits.empty() && frac_digits.empty())) {
        return invalid("not a plain decimal");
    }
    bool round_up = false;
    if (static_cast<int32_t>(frac_digits.size()) > scale) {
        round_up = frac_digits[scale] >= '5';
        frac_digits.resize(scale);
    } else {
        frac_digits.append(scale - frac_digits.size(), '0');
    }
    std::string unscaled = int_digits + frac_digits;
    unscaled.erase(0, std::min(unscaled.find_first_not_of('0'), unscaled.size()));
    if (static_cast<int32_t>(unscaled.size()) > precision) {
        return invalid("value does not fit the precision");
    }
    Decimal::int128_t value = 0;
    for (char c : unscaled) {
        value = value * 10 + (c - '0');
    }
    if (round_up) {
        value += 1;
        if (static_cast<int32_t>(Int128ToString(value).size()) > precision) {
            return invalid("value does not fit the precision");
        }
    }
    return Decimal(precision, scale, negative ? -value : value);
}

}  // namespace paimon
