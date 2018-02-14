//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_UTIL_STRING_UTIL_H
#define YB_UTIL_STRING_UTIL_H

#pragma once

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "yb/util/tostring.h"

namespace yb {

std::vector<std::string> StringSplit(const std::string& arg, char delim);

template <typename T>
inline std::string VectorToString(const std::vector<T>& vec) {
  std::stringstream os;
  os << "[";
  bool need_separator = false;
  for (auto item : vec) {
    if (need_separator) {
      os << ", ";
    }
    need_separator = true;
    os << item;
  }
  os << "]";
  return os.str();
}

std::string RightPadToWidth(const std::string& s, int w);

// Returns true if s ends with substring end, and s has at least one more character before
// end. If left is a valid string pointer, it will contain s minus the end substring.
// Example 1: s = "15ms", end = "ms", then this function will return true and set left to "15".
// Example 2: s = "ms", end = "ms", this function will return false.
bool StringEndsWith(
    const std::string& s, const char* end, size_t end_len, std::string* left = nullptr);

inline bool StringEndsWith(
    const std::string& s, const std::string end, std::string* left = nullptr) {
  return StringEndsWith(s, end.c_str(), end.length(), left);
}

}  // namespace yb

namespace rocksdb {
using yb::ToString;
using yb::StringSplit;
using yb::VectorToString;
}

#endif // YB_UTIL_STRING_UTIL_H
