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

#ifndef YB_MASTER_MASTER_DEFAULTS_H
#define YB_MASTER_MASTER_DEFAULTS_H

#include <set>
#include <string>
#include <utility>

namespace yb {
namespace master {

static const char* const kSystemNamespaceName = "system";
static const char* const kSystemNamespaceId = "00000000000000000000000000000001";

static const char* const kSystemSchemaNamespaceName = "system_schema";
static const char* const kSystemSchemaNamespaceId = "00000000000000000000000000000002";

static const char* const kSystemAuthNamespaceName = "system_auth";
static const char* const kSystemAuthNamespaceId = "00000000000000000000000000000003";

static const char* const kSystemDistributedNamespaceName = "system_distributed";
static const char* const kSystemTracesNamespaceName = "system_traces";

static const char* const kSystemPeersTableName = "peers";
static const char* const kSystemLocalTableName = "local";
static const char* const kSystemPartitionsTableName = "partitions";
static const char* const kSystemSizeEstimatesTableName = "size_estimates";

static const char* const kSystemSchemaAggregatesTableName = "aggregates";
static const char* const kSystemSchemaColumnsTableName = "columns";
static const char* const kSystemSchemaFunctionsTableName = "functions";
static const char* const kSystemSchemaIndexesTableName = "indexes";
static const char* const kSystemSchemaTriggersTableName = "triggers";
static const char* const kSystemSchemaTypesTableName = "types";
static const char* const kSystemSchemaViewsTableName = "views";
static const char* const kSystemSchemaPartitionsTableName = "partitions";
static const char* const kSystemSchemaKeyspacesTableName = "keyspaces";
static const char* const kSystemSchemaTablesTableName = "tables";

static const char* const kSystemAuthRolesTableName = "roles";
static const char* const kSystemAuthRolePermissionsTableName = "role_permissions";
static const char* const kSystemAuthResourceRolePermissionsIndexTableName =
                  "resource_role_permissions_index";

static const char* const kDefaultSchemaVersion = "00000000-0000-0000-0000-000000000000";

// Needs to be updated each time we add a new system namespace.
static constexpr int kNumSystemNamespaces = 3;

// Needs to be updated each time we add a new system table. Currently, this is only used for unit
// tests which don't have access to the master object (for ex: unit tests which use ExternalMaster).
static constexpr int kNumSystemTables = 16;

constexpr uint16_t kMasterDefaultPort = 7100;
constexpr uint16_t kMasterDefaultWebPort = 7000;

} // namespace master
} // namespace yb

#endif // YB_MASTER_MASTER_DEFAULTS_H
