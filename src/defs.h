/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>

// 此处重载了<<操作符，在ColMeta中进行了调用
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value, T>::type>
std::ostream &operator<<(std::ostream &os, const T &enum_val) {
    os << static_cast<int>(enum_val);
    return os;
}

template<typename T, typename = typename std::enable_if<std::is_enum<T>::value, T>::type>
std::istream &operator>>(std::istream &is, T &enum_val) {
    int int_val;
    is >> int_val;
    enum_val = static_cast<T>(int_val);
    return is;
}

struct Rid {
    int page_no;
    int slot_no;

    friend bool operator==(const Rid &x, const Rid &y) {
        return x.page_no == y.page_no && x.slot_no == y.slot_no;
    }

    friend bool operator!=(const Rid &x, const Rid &y) { return !(x == y); }
};

enum ColType {
    TYPE_INT, TYPE_BIGINT, TYPE_FLOAT, TYPE_STRING, TYPE_DATETIME
};

inline std::string coltype2str(ColType type) {
    std::map<ColType, std::string> m = {
            {TYPE_INT,    "INT"},
            {TYPE_BIGINT, "BIGINT"},
            {TYPE_FLOAT,  "FLOAT"},
            {TYPE_STRING, "STRING"},
            {TYPE_DATETIME, "DATETIME"}
    };
    return m.at(type);
}

inline bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline bool parse_datetime_value(const std::string &str, int64_t &value) {
    if (str.size() != 19 || str[4] != '-' || str[7] != '-' || str[10] != ' ' ||
        str[13] != ':' || str[16] != ':') {
        return false;
    }
    for (size_t i = 0; i < str.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(str[i]))) {
            return false;
        }
    }

    int year = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day = std::stoi(str.substr(8, 2));
    int hour = std::stoi(str.substr(11, 2));
    int minute = std::stoi(str.substr(14, 2));
    int second = std::stoi(str.substr(17, 2));

    if (year < 1000 || year > 9999 || month < 1 || month > 12 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    static const int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = days_in_month[month];
    if (month == 2 && is_leap_year(year)) {
        max_day = 29;
    }
    if (day < 1 || day > max_day) {
        return false;
    }

    value = static_cast<int64_t>(year) * 10000000000LL +
            static_cast<int64_t>(month) * 100000000LL +
            static_cast<int64_t>(day) * 1000000LL +
            static_cast<int64_t>(hour) * 10000LL +
            static_cast<int64_t>(minute) * 100LL +
            static_cast<int64_t>(second);
    return true;
}

inline std::string datetime_value_to_string(int64_t value) {
    int second = value % 100;
    value /= 100;
    int minute = value % 100;
    value /= 100;
    int hour = value % 100;
    value /= 100;
    int day = value % 100;
    value /= 100;
    int month = value % 100;
    value /= 100;
    int year = value;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return std::string(buf);
}

class RecScan {
public:
    virtual ~RecScan() = default;

    virtual void next() = 0;

    virtual bool is_end() const = 0;

    virtual Rid rid() const = 0;
};
