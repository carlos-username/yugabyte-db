// Copyright (c) YugaByte, Inc.
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

#ifndef ENT_SRC_YB_MASTER_CDC_RPC_TASKS_H
#define ENT_SRC_YB_MASTER_CDC_RPC_TASKS_H

#include <stdlib.h>

#include "yb/client/client.h"
#include "yb/client/table.h"
#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
#include "yb/master/master.proxy.h"
#include "yb/rpc/messenger.h"
#include "yb/util/status.h"
#include "yb/util/result.h"
#include "yb/util/net/net_util.h"

namespace yb {
namespace master {

class CDCRpcTasks {
 public:
  static Result<std::shared_ptr<CDCRpcTasks>> CreateWithMasterAddrs(
      const std::string& master_addrs);

  client::YBClient* client() const {
    return yb_client_.get();
  }

 private:
  std::string master_addrs_;
  std::unique_ptr<client::YBClient> yb_client_;
};

} // namespace master
} // namespace yb


#endif // ENT_SRC_YB_MASTER_CDC_RPC_TASKS_H
