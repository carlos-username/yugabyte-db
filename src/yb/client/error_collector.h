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
#ifndef YB_CLIENT_ERROR_COLLECTOR_H
#define YB_CLIENT_ERROR_COLLECTOR_H

#include <vector>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/util/locks.h"
#include "yb/util/status.h"

namespace yb {
namespace client {

class YBError;
class YBInsert;

namespace internal {

class ErrorCollector : public RefCountedThreadSafe<ErrorCollector> {
 public:
  ErrorCollector();

  void AddError(gscoped_ptr<YBError> error);

  // See YBSession for details.
  int CountErrors() const;

  // See YBSession for details.
  void GetErrors(std::vector<YBError*>* errors, bool* overflowed);

 private:
  friend class RefCountedThreadSafe<ErrorCollector>;
  virtual ~ErrorCollector();

  mutable simple_spinlock lock_;
  std::vector<YBError*> errors_;

  DISALLOW_COPY_AND_ASSIGN(ErrorCollector);
};

} // namespace internal
} // namespace client
} // namespace yb
#endif /* YB_CLIENT_ERROR_COLLECTOR_H */
