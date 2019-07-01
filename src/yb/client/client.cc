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

#include "yb/client/client.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <limits>

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/client/meta_cache.h"
#include "yb/client/session.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/tablet_server.h"

#include "yb/common/common.pb.h"
#include "yb/common/entity_ids.h"
#include "yb/common/common_flags.h"
#include "yb/common/partition.h"
#include "yb/common/roles_permissions.h"
#include "yb/common/wire_protocol.h"

#include "yb/master/master.proxy.h"
#include "yb/master/master_defaults.h"
#include "yb/master/master_util.h"
#include "yb/yql/redis/redisserver/redis_constants.h"
#include "yb/yql/redis/redisserver/redis_parser.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/yb_rpc.h"
#include "yb/util/flag_tags.h"
#include "yb/util/init.h"
#include "yb/util/logging.h"
#include "yb/util/net/dns_resolver.h"
#include "yb/util/oid_generator.h"
#include "yb/util/tsan_util.h"
#include "yb/util/crypt.h"

using yb::master::AlterTableRequestPB;
using yb::master::AlterTableRequestPB_Step;
using yb::master::AlterTableResponsePB;
using yb::master::CreateTableRequestPB;
using yb::master::CreateTableResponsePB;
using yb::master::DeleteTableRequestPB;
using yb::master::DeleteTableResponsePB;
using yb::master::GetTableSchemaRequestPB;
using yb::master::GetTableSchemaRequestPB;
using yb::master::GetTableSchemaResponsePB;
using yb::master::GetTableLocationsRequestPB;
using yb::master::GetTableLocationsResponsePB;
using yb::master::GetTabletLocationsRequestPB;
using yb::master::GetTabletLocationsResponsePB;
using yb::master::ListMastersRequestPB;
using yb::master::ListMastersResponsePB;
using yb::master::ListTablesRequestPB;
using yb::master::ListTablesResponsePB;
using yb::master::ListTablesResponsePB_TableInfo;
using yb::master::ListTabletServersRequestPB;
using yb::master::ListTabletServersResponsePB;
using yb::master::ListTabletServersResponsePB_Entry;
using yb::master::CreateNamespaceRequestPB;
using yb::master::CreateNamespaceResponsePB;
using yb::master::DeleteNamespaceRequestPB;
using yb::master::DeleteNamespaceResponsePB;
using yb::master::ListNamespacesRequestPB;
using yb::master::ListNamespacesResponsePB;
using yb::master::ReservePgsqlOidsRequestPB;
using yb::master::ReservePgsqlOidsResponsePB;
using yb::master::GetYsqlCatalogConfigRequestPB;
using yb::master::GetYsqlCatalogConfigResponsePB;
using yb::master::CreateUDTypeRequestPB;
using yb::master::CreateUDTypeResponsePB;
using yb::master::AlterRoleRequestPB;
using yb::master::AlterRoleResponsePB;
using yb::master::CreateRoleRequestPB;
using yb::master::CreateRoleResponsePB;
using yb::master::DeleteUDTypeRequestPB;
using yb::master::DeleteUDTypeResponsePB;
using yb::master::DeleteRoleRequestPB;
using yb::master::DeleteRoleResponsePB;
using yb::master::GetPermissionsRequestPB;
using yb::master::GetPermissionsResponsePB;
using yb::master::GrantRevokeRoleRequestPB;
using yb::master::GrantRevokeRoleResponsePB;
using yb::master::ListUDTypesRequestPB;
using yb::master::ListUDTypesResponsePB;
using yb::master::GetUDTypeInfoRequestPB;
using yb::master::GetUDTypeInfoResponsePB;
using yb::master::GrantRevokePermissionResponsePB;
using yb::master::GrantRevokePermissionRequestPB;
using yb::master::MasterServiceProxy;
using yb::master::ReplicationInfoPB;
using yb::master::TabletLocationsPB;
using yb::master::RedisConfigSetRequestPB;
using yb::master::RedisConfigSetResponsePB;
using yb::master::RedisConfigGetRequestPB;
using yb::master::RedisConfigGetResponsePB;
using yb::rpc::Messenger;
using yb::rpc::MessengerBuilder;
using yb::rpc::RpcController;
using yb::tserver::NoOpRequestPB;
using yb::tserver::NoOpResponsePB;
using yb::util::kBcryptHashSize;
using std::set;
using std::string;
using std::vector;
using google::protobuf::RepeatedPtrField;

using namespace yb::size_literals;  // NOLINT.

DEFINE_bool(client_suppress_created_logs, false,
            "Suppress 'Created table ...' messages");
TAG_FLAG(client_suppress_created_logs, advanced);
TAG_FLAG(client_suppress_created_logs, hidden);

DECLARE_bool(running_test);

namespace yb {
namespace client {

using internal::MetaCache;
using ql::ObjectType;
using std::shared_ptr;

#define CALL_SYNC_LEADER_MASTER_RPC(req, resp, method) \
  do { \
    auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout(); \
    CALL_SYNC_LEADER_MASTER_RPC_WITH_DEADLINE(req, resp, deadline, method); \
  } while(0);

#define CALL_SYNC_LEADER_MASTER_RPC_WITH_DEADLINE(req, resp, deadline, method) \
  do { \
    Status s = data_->SyncLeaderMasterRpc<BOOST_PP_CAT(method, RequestPB), \
                                          BOOST_PP_CAT(method, ResponsePB)>( \
        deadline, \
        this, \
        req, \
        &resp, \
        nullptr, \
        BOOST_PP_STRINGIZE(method), \
        &MasterServiceProxy::method); \
    RETURN_NOT_OK(s); \
    if (resp.has_error()) { \
      return StatusFromPB(resp.error().status()); \
    } \
  } while(0);

// Adapts between the internal LogSeverity and the client's YBLogSeverity.
static void LoggingAdapterCB(YBLoggingCallback* user_cb,
                             LogSeverity severity,
                             const char* filename,
                             int line_number,
                             const struct ::tm* time,
                             const char* message,
                             size_t message_len) {
  YBLogSeverity client_severity;
  switch (severity) {
    case yb::SEVERITY_INFO:
      client_severity = SEVERITY_INFO;
      break;
    case yb::SEVERITY_WARNING:
      client_severity = SEVERITY_WARNING;
      break;
    case yb::SEVERITY_ERROR:
      client_severity = SEVERITY_ERROR;
      break;
    case yb::SEVERITY_FATAL:
      client_severity = SEVERITY_FATAL;
      break;
    default:
      LOG(FATAL) << "Unknown YB log severity: " << severity;
  }
  user_cb->Run(client_severity, filename, line_number, time,
               message, message_len);
}

void InitLogging() {
  InitGoogleLoggingSafeBasic("yb_client");
}

void InstallLoggingCallback(YBLoggingCallback* cb) {
  RegisterLoggingCallback(Bind(&LoggingAdapterCB, Unretained(cb)));
}

void UninstallLoggingCallback() {
  UnregisterLoggingCallback();
}

void SetVerboseLogLevel(int level) {
  FLAGS_v = level;
}

Status SetInternalSignalNumber(int signum) {
  return SetStackTraceSignal(signum);
}

YBClientBuilder::YBClientBuilder()
  : data_(new YBClientBuilder::Data()) {
}

YBClientBuilder::~YBClientBuilder() {
}

YBClientBuilder& YBClientBuilder::clear_master_server_addrs() {
  data_->master_server_addrs_.clear();
  return *this;
}

YBClientBuilder& YBClientBuilder::master_server_addrs(const vector<string>& addrs) {
  for (const string& addr : addrs) {
    data_->master_server_addrs_.push_back(addr);
  }
  return *this;
}

YBClientBuilder& YBClientBuilder::add_master_server_addr(const string& addr) {
  data_->master_server_addrs_.push_back(addr);
  return *this;
}

YBClientBuilder& YBClientBuilder::add_master_server_endpoint(const string& endpoint) {
  data_->master_server_endpoint_ = endpoint;
  return *this;
}

YBClientBuilder& YBClientBuilder::default_admin_operation_timeout(const MonoDelta& timeout) {
  data_->default_admin_operation_timeout_ = timeout;
  return *this;
}

YBClientBuilder& YBClientBuilder::default_rpc_timeout(const MonoDelta& timeout) {
  data_->default_rpc_timeout_ = timeout;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_num_reactors(int32_t num_reactors) {
  CHECK_GT(num_reactors, 0);
  data_->num_reactors_ = num_reactors;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_cloud_info_pb(const CloudInfoPB& cloud_info_pb) {
  data_->cloud_info_pb_ = cloud_info_pb;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_metric_entity(
    const scoped_refptr<MetricEntity>& metric_entity) {
  data_->metric_entity_ = metric_entity;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_client_name(const std::string& name) {
  data_->client_name_ = name;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_callback_threadpool_size(size_t size) {
  data_->threadpool_size_ = size;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_tserver_uuid(const TabletServerId& uuid) {
  data_->uuid_ = uuid;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_parent_mem_tracker(const MemTrackerPtr& mem_tracker) {
  data_->parent_mem_tracker_ = mem_tracker;
  return *this;
}

YBClientBuilder& YBClientBuilder::set_skip_master_leader_resolution(bool value) {
  data_->skip_master_leader_resolution_ = value;
  return *this;
}

Status YBClientBuilder::DoBuild(rpc::Messenger* messenger, std::unique_ptr<YBClient>* client) {
  RETURN_NOT_OK(CheckCPUFlags());

  std::unique_ptr<YBClient> c(new YBClient());

  // Init messenger.
  if (messenger) {
    c->data_->messenger_holder_ = nullptr;
    c->data_->messenger_ = messenger;
  } else {
    MessengerBuilder builder(data_->client_name_);
    builder.set_num_reactors(data_->num_reactors_);
    builder.set_metric_entity(data_->metric_entity_);
    builder.UseDefaultConnectionContextFactory(data_->parent_mem_tracker_);
    c->data_->messenger_holder_ = VERIFY_RESULT(builder.Build());
    c->data_->messenger_ = c->data_->messenger_holder_.get();
    if (FLAGS_running_test) {
      c->data_->messenger_->TEST_SetOutboundIpBase(VERIFY_RESULT(HostToAddress("127.0.0.1")));
    }
  }
  c->data_->proxy_cache_ = std::make_unique<rpc::ProxyCache>(c->data_->messenger_);
  c->data_->metric_entity_ = data_->metric_entity_;

  c->data_->master_server_endpoint_ = data_->master_server_endpoint_;
  c->data_->master_server_addrs_ = data_->master_server_addrs_;
  c->data_->default_admin_operation_timeout_ = data_->default_admin_operation_timeout_;
  c->data_->default_rpc_timeout_ = data_->default_rpc_timeout_;

  // Let's allow for plenty of time for discovering the master the first
  // time around.
  auto deadline = CoarseMonoClock::Now() + c->default_admin_operation_timeout();
  RETURN_NOT_OK_PREPEND(
      c->data_->SetMasterServerProxy(c.get(), deadline, data_->skip_master_leader_resolution_),
      "Could not locate the leader master");

  c->data_->meta_cache_.reset(new MetaCache(c.get()));
  c->data_->dns_resolver_.reset(new DnsResolver());

  // Init local host names used for locality decisions.
  RETURN_NOT_OK_PREPEND(c->data_->InitLocalHostNames(),
                        "Could not determine local host names");
  c->data_->cloud_info_pb_ = data_->cloud_info_pb_;
  c->data_->uuid_ = data_->uuid_;
  if (data_->threadpool_size_ > 0) {
    ThreadPoolBuilder tpb(data_->client_name_ + "_cb");
    tpb.set_max_threads(data_->threadpool_size_);
    std::unique_ptr<ThreadPool> tp;
    RETURN_NOT_OK_PREPEND(tpb.Build(&tp), "Could not create callback threadpool");
    c->data_->cb_threadpool_ = std::move(tp);
  }

  client->swap(c);
  return Status::OK();
}

Result<std::unique_ptr<YBClient>> YBClientBuilder::Build(rpc::Messenger* messenger) {
  std::unique_ptr<YBClient> client;
  RETURN_NOT_OK(DoBuild(messenger, &client));
  return client;
}

Result<std::unique_ptr<YBClient>> YBClientBuilder::Build(
    std::unique_ptr<rpc::Messenger>&& messenger) {
  std::unique_ptr<YBClient> client;
  RETURN_NOT_OK(DoBuild(messenger.get(), &client));
  client->data_->messenger_holder_ = std::move(messenger);
  return client;
}

YBClient::YBClient() : data_(new YBClient::Data()) {
  yb::InitCommonFlags();
}

YBClient::~YBClient() {
  if (data_->messenger_holder_) {
    data_->messenger_holder_->Shutdown();
  }
  if (data_->meta_cache_) {
    data_->meta_cache_->Shutdown();
  }
  if (data_->cb_threadpool_) {
    data_->cb_threadpool_->Shutdown();
  }
}

YBTableCreator* YBClient::NewTableCreator() {
  return new YBTableCreator(this);
}

Status YBClient::IsCreateTableInProgress(const YBTableName& table_name,
                                         bool *create_in_progress) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->IsCreateTableInProgress(this, table_name, "" /* table_id */, deadline,
                                        create_in_progress);
}

Status YBClient::TruncateTable(const string& table_id, bool wait) {
  return TruncateTables({table_id}, wait);
}

Status YBClient::TruncateTables(const vector<string>& table_ids, bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->TruncateTables(this, table_ids, deadline, wait);
}

Status YBClient::DeleteTable(const YBTableName& table_name, bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            table_name,
                            "" /* table_id */,
                            false /* is_index_table */,
                            deadline,
                            nullptr /* indexed_table_name */,
                            wait);
}

Status YBClient::DeleteTable(const string& table_id, bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            YBTableName(),
                            table_id,
                            false /* is_index_table */,
                            deadline,
                            nullptr /* indexed_table_name */,
                            wait);
}

Status YBClient::DeleteIndexTable(const YBTableName& table_name,
                                  YBTableName* indexed_table_name,
                                  bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            table_name,
                            "" /* table_id */,
                            true /* is_index_table */,
                            deadline,
                            indexed_table_name,
                            wait);
}

Status YBClient::DeleteIndexTable(const string& table_id,
                                  YBTableName* indexed_table_name,
                                  bool wait) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->DeleteTable(this,
                            YBTableName(),
                            table_id,
                            true /* is_index_table */,
                            deadline,
                            indexed_table_name,
                            wait);
}

YBTableAlterer* YBClient::NewTableAlterer(const YBTableName& name) {
  return new YBTableAlterer(this, name);
}

YBTableAlterer* YBClient::NewTableAlterer(const string id) {
  return new YBTableAlterer(this, id);
}

Status YBClient::IsAlterTableInProgress(const YBTableName& table_name,
                                        const string& table_id,
                                        bool *alter_in_progress) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->IsAlterTableInProgress(this, table_name, table_id, deadline, alter_in_progress);
}

Status YBClient::GetTableSchema(const YBTableName& table_name,
                                YBSchema* schema,
                                PartitionSchema* partition_schema) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  YBTableInfo info;
  RETURN_NOT_OK(data_->GetTableSchema(this, table_name, deadline, &info));

  // Verify it is not an index table.
  if (info.index_info) {
    return STATUS(NotFound, "The table does not exist");
  }

  *schema = std::move(info.schema);
  *partition_schema = std::move(info.partition_schema);
  return Status::OK();
}

Status YBClient::CreateNamespace(const std::string& namespace_name,
                                 const boost::optional<YQLDatabase>& database_type,
                                 const std::string& creator_role_name,
                                 const std::string& namespace_id,
                                 const std::string& source_namespace_id,
                                 const boost::optional<uint32_t>& next_pg_oid) {
  CreateNamespaceRequestPB req;
  CreateNamespaceResponsePB resp;
  req.set_name(namespace_name);
  if (!creator_role_name.empty()) {
    req.set_creator_role_name(creator_role_name);
  }
  if (database_type) {
    req.set_database_type(*database_type);
  }
  if (!namespace_id.empty()) {
    req.set_namespace_id(namespace_id);
  }
  if (!source_namespace_id.empty()) {
    req.set_source_namespace_id(source_namespace_id);
  }
  if (next_pg_oid) {
    req.set_next_pg_oid(*next_pg_oid);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, CreateNamespace);
  return Status::OK();
}

Status YBClient::CreateNamespaceIfNotExists(const std::string& namespace_name,
                                            const boost::optional<YQLDatabase>& database_type,
                                            const std::string& creator_role_name,
                                            const std::string& namespace_id,
                                            const std::string& source_namespace_id,
                                            const boost::optional<uint32_t>& next_pg_oid) {
  Result<bool> namespace_exists = (!namespace_id.empty() ? NamespaceIdExists(namespace_id)
                                                         : NamespaceExists(namespace_name));
  if (VERIFY_RESULT(namespace_exists)) {
    return Status::OK();
  }

  return CreateNamespace(namespace_name, database_type, creator_role_name, namespace_id,
                         source_namespace_id, next_pg_oid);
}

Status YBClient::DeleteNamespace(const std::string& namespace_name,
                                 const boost::optional<YQLDatabase>& database_type,
                                 const std::string& namespace_id) {
  DeleteNamespaceRequestPB req;
  DeleteNamespaceResponsePB resp;
  req.mutable_namespace_()->set_name(namespace_name);
  if (!namespace_id.empty()) {
    req.mutable_namespace_()->set_id(namespace_id);
  }
  if (database_type) {
    req.set_database_type(*database_type);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteNamespace);
  return Status::OK();
}

Status YBClient::ListNamespaces(const boost::optional<YQLDatabase>& database_type,
                                std::vector<std::string>* namespace_names,
                                std::vector<std::string>* namespace_ids) {
  ListNamespacesRequestPB req;
  ListNamespacesResponsePB resp;
  if (database_type) {
    req.set_database_type(*database_type);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListNamespaces);

  for (auto ns : resp.namespaces()) {
    if (namespace_names != nullptr) {
      namespace_names->push_back(ns.name());
    }
    if (namespace_ids != nullptr) {
      namespace_ids->push_back(ns.id());
    }
  }
  return Status::OK();
}

Status YBClient::ReservePgsqlOids(const std::string& namespace_id,
                                  const uint32_t next_oid, const uint32_t count,
                                  uint32_t* begin_oid, uint32_t* end_oid) {
  ReservePgsqlOidsRequestPB req;
  ReservePgsqlOidsResponsePB resp;
  req.set_namespace_id(namespace_id);
  req.set_next_oid(next_oid);
  req.set_count(count);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ReservePgsqlOids);
  *begin_oid = resp.begin_oid();
  *end_oid = resp.end_oid();
  return Status::OK();
}

Status YBClient::GetYsqlCatalogMasterVersion(uint64_t *ysql_catalog_version) {
  GetYsqlCatalogConfigRequestPB req;
  GetYsqlCatalogConfigResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetYsqlCatalogConfig);
  *ysql_catalog_version = resp.version();
  return Status::OK();
}

Status YBClient::GrantRevokePermission(GrantRevokeStatementType statement_type,
                                       const PermissionType& permission,
                                       const ResourceType& resource_type,
                                       const std::string& canonical_resource,
                                       const char* resource_name,
                                       const char* namespace_name,
                                       const std::string& role_name) {
  // Setting up request.
  GrantRevokePermissionRequestPB req;
  req.set_role_name(role_name);
  req.set_canonical_resource(canonical_resource);
  if (resource_name != nullptr) {
    req.set_resource_name(resource_name);
  }
  if (namespace_name != nullptr) {
    req.mutable_namespace_()->set_name(namespace_name);
  }
  req.set_resource_type(resource_type);
  req.set_permission(permission);

  req.set_revoke(statement_type == GrantRevokeStatementType::REVOKE);

  GrantRevokePermissionResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GrantRevokePermission);
  return Status::OK();
}

Result<bool> YBClient::NamespaceExists(const std::string& namespace_name,
                                       const boost::optional<YQLDatabase>& database_type) {
  std::vector<std::string> namespace_names;
  RETURN_NOT_OK(ListNamespaces(database_type, &namespace_names));

  for (const string& name : namespace_names) {
    if (name == namespace_name) {
      return true;
    }
  }
  return false;
}

Result<bool> YBClient::NamespaceIdExists(const std::string& namespace_id,
                                         const boost::optional<YQLDatabase>& database_type) {
  std::vector<std::string> namespace_ids;
  RETURN_NOT_OK(ListNamespaces(database_type, nullptr /* namespace_names */, &namespace_ids));

  for (const string& id : namespace_ids) {
    if (namespace_id == id) {
      return true;
    }
  }
  return false;
}

CHECKED_STATUS YBClient::GetUDType(const std::string& namespace_name,
                                   const std::string& type_name,
                                   std::shared_ptr<QLType>* ql_type) {
  // Setting up request.
  GetUDTypeInfoRequestPB req;
  req.mutable_type()->mutable_namespace_()->set_name(namespace_name);
  req.mutable_type()->set_type_name(type_name);

  // Sending request.
  GetUDTypeInfoResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetUDTypeInfo);

  // Filling in return values.
  std::vector<string> field_names;
  for (const auto& field_name : resp.udtype().field_names()) {
    field_names.push_back(field_name);
  }

  std::vector<shared_ptr<QLType>> field_types;
  for (const auto& field_type : resp.udtype().field_types()) {
    field_types.push_back(QLType::FromQLTypePB(field_type));
  }

  (*ql_type)->SetUDTypeFields(resp.udtype().id(), field_names, field_types);

  return Status::OK();
}

CHECKED_STATUS YBClient::CreateRole(const RoleName& role_name,
                                    const std::string& salted_hash,
                                    const bool login, const bool superuser,
                                    const RoleName& creator_role_name) {

  // Setting up request.
  CreateRoleRequestPB req;
  req.set_salted_hash(salted_hash);
  req.set_name(role_name);
  req.set_login(login);
  req.set_superuser(superuser);

  if (!creator_role_name.empty()) {
    req.set_creator_role_name(creator_role_name);
  }

  CreateRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, CreateRole);
  return Status::OK();
}

CHECKED_STATUS YBClient::AlterRole(const RoleName& role_name,
                                   const boost::optional<std::string>& salted_hash,
                                   const boost::optional<bool> login,
                                   const boost::optional<bool> superuser,
                                   const RoleName& current_role_name) {
  // Setting up request.
  AlterRoleRequestPB req;
  req.set_name(role_name);
  if (salted_hash) {
    req.set_salted_hash(*salted_hash);
  }
  if (login) {
    req.set_login(*login);
  }
  if (superuser) {
    req.set_superuser(*superuser);
  }
  req.set_current_role(current_role_name);

  AlterRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, AlterRole);
  return Status::OK();
}

CHECKED_STATUS YBClient::DeleteRole(const std::string& role_name,
                                    const std::string& current_role_name) {
  // Setting up request.
  DeleteRoleRequestPB req;
  req.set_name(role_name);
  req.set_current_role(current_role_name);

  DeleteRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteRole);
  return Status::OK();
}

static const string kRequirePass = "requirepass";
CHECKED_STATUS YBClient::SetRedisPasswords(const std::vector<string>& passwords) {
  // TODO: Store hash instead of the password?
  return SetRedisConfig(kRequirePass, passwords);
}

CHECKED_STATUS YBClient::GetRedisPasswords(vector<string>* passwords) {
  Status s = GetRedisConfig(kRequirePass, passwords);
  if (s.IsNotFound()) {
    // If the redis config has no kRequirePass key.
    passwords->clear();
    s = Status::OK();
  }
  return s;
}

CHECKED_STATUS YBClient::SetRedisConfig(const string& key, const vector<string>& values) {
  // Setting up request.
  RedisConfigSetRequestPB req;
  req.set_keyword(key);
  for (const auto& value : values) {
    req.add_args(value);
  }
  RedisConfigSetResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, RedisConfigSet);
  return Status::OK();
}

CHECKED_STATUS YBClient::GetRedisConfig(const string& key, vector<string>* values) {
  // Setting up request.
  RedisConfigGetRequestPB req;
  RedisConfigGetResponsePB resp;
  req.set_keyword(key);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, RedisConfigGet);
  values->clear();
  for (const auto& arg : resp.args())
    values->push_back(arg);
  return Status::OK();
}

CHECKED_STATUS YBClient::GrantRevokeRole(GrantRevokeStatementType statement_type,
                                         const std::string& granted_role_name,
                                         const std::string& recipient_role_name) {
  // Setting up request.
  GrantRevokeRoleRequestPB req;
  req.set_revoke(statement_type == GrantRevokeStatementType::REVOKE);
  req.set_granted_role(granted_role_name);
  req.set_recipient_role(recipient_role_name);

  GrantRevokeRoleResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GrantRevokeRole);
  return Status::OK();
}

Status YBClient::GetPermissions(client::internal::PermissionsCache* permissions_cache) {
  if (!permissions_cache) {
    DFATAL_OR_RETURN_NOT_OK(STATUS(InvalidArgument, "Invalid null permissions_cache"));
  }

  boost::optional<uint64_t> version = permissions_cache->version();

  // Setting up request.
  GetPermissionsRequestPB req;
  if (version) {
    req.set_if_version_greater_than(*version);
  }

  GetPermissionsResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetPermissions);

  VLOG(1) << "Got permissions cache: " << resp.ShortDebugString();

  // The first request is a special case. We always replace the cache since we don't have anything.
  if (!version) {
    // We should at least receive cassandra's permissions.
    if (resp.role_permissions_size() == 0) {
      DFATAL_OR_RETURN_NOT_OK(
          STATUS(IllegalState, "Received invalid empty permissions cache from master"));

    }
  } else if (resp.version() == *version) {
      // No roles should have been received if both versions match.
      if (resp.role_permissions_size() != 0) {
        DFATAL_OR_RETURN_NOT_OK(STATUS(IllegalState,
            "Received permissions cache when none was expected because the master's "
            "permissions versions is equal to the client's version"));
      }
      // Nothing to update.
      return Status::OK();
  } else if (resp.version() < *version) {
    // If the versions don't match, then the master's version has to be greater than ours.
    DFATAL_OR_RETURN_NOT_OK(STATUS_SUBSTITUTE(IllegalState,
        "Client's permissions version $0 can't be greater than the master's permissions version $1",
        *version, resp.version()));
  }

  permissions_cache->UpdateRolesPermissions(resp);
  return Status::OK();
}

CHECKED_STATUS YBClient::CreateUDType(const std::string& namespace_name,
                                      const std::string& type_name,
                                      const std::vector<std::string>& field_names,
                                      const std::vector<std::shared_ptr<QLType>>& field_types) {
  // Setting up request.
  CreateUDTypeRequestPB req;
  req.mutable_namespace_()->set_name(namespace_name);
  req.set_name(type_name);
  for (const string& field_name : field_names) {
    req.add_field_names(field_name);
  }
  for (const std::shared_ptr<QLType> field_type : field_types) {
    field_type->ToQLTypePB(req.add_field_types());
  }

  CreateUDTypeResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, CreateUDType);
  return Status::OK();
}

CHECKED_STATUS YBClient::DeleteUDType(const std::string& namespace_name,
                                      const std::string& type_name) {
  // Setting up request.
  DeleteUDTypeRequestPB req;
  req.mutable_type()->mutable_namespace_()->set_name(namespace_name);
  req.mutable_type()->set_type_name(type_name);

  DeleteUDTypeResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, DeleteUDType);
  return Status::OK();
}

Status YBClient::TabletServerCount(int *tserver_count, bool primary_only) {
  ListTabletServersRequestPB req;
  ListTabletServersResponsePB resp;
  req.set_primary_only(primary_only);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTabletServers);
  *tserver_count = resp.servers_size();
  return Status::OK();
}

Status YBClient::ListTabletServers(vector<std::unique_ptr<YBTabletServer>>* tablet_servers) {
  ListTabletServersRequestPB req;
  ListTabletServersResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTabletServers);
  for (int i = 0; i < resp.servers_size(); i++) {
    const ListTabletServersResponsePB_Entry& e = resp.servers(i);
    auto ts = std::make_unique<YBTabletServer>(
        e.instance_id().permanent_uuid(),
        DesiredHostPort(e.registration().common(), data_->cloud_info_pb_).host());
    tablet_servers->push_back(std::move(ts));
  }
  return Status::OK();
}

void YBClient::SetLocalTabletServer(const string& ts_uuid,
                                    const shared_ptr<tserver::TabletServerServiceProxy>& proxy,
                                    const tserver::LocalTabletServer* local_tserver) {
  data_->meta_cache_->SetLocalTabletServer(ts_uuid, proxy, local_tserver);
}

Status YBClient::GetTablets(const YBTableName& table_name,
                            const int32_t max_tablets,
                            RepeatedPtrField<TabletLocationsPB>* tablets) {
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;
  table_name.SetIntoTableIdentifierPB(req.mutable_table());

  if (max_tablets == 0) {
    req.set_max_returned_locations(std::numeric_limits<int32_t>::max());
  } else if (max_tablets > 0) {
    req.set_max_returned_locations(max_tablets);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetTableLocations);
  *tablets = resp.tablet_locations();
  return Status::OK();
}

Status YBClient::GetTabletLocation(const TabletId& tablet_id,
                                   master::TabletLocationsPB* tablet_location) {
  GetTabletLocationsRequestPB req;
  GetTabletLocationsResponsePB resp;
  req.add_tablet_ids(tablet_id);
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, GetTabletLocations);

  if (resp.tablet_locations_size() != 1) {
    return STATUS_SUBSTITUTE(IllegalState, "Expected single tablet for $0, received $1",
                             tablet_id, resp.tablet_locations_size());
  }

  *tablet_location = resp.tablet_locations(0);
  return Status::OK();
}

Status YBClient::GetTablets(const YBTableName& table_name,
                            const int32_t max_tablets,
                            vector<TabletId>* tablet_uuids,
                            vector<string>* ranges,
                            std::vector<master::TabletLocationsPB>* locations,
                            bool update_tablets_cache) {
  RepeatedPtrField<TabletLocationsPB> tablets;
  RETURN_NOT_OK(GetTablets(table_name, max_tablets, &tablets));
  tablet_uuids->reserve(tablets.size());
  if (ranges != nullptr) {
    ranges->reserve(tablets.size());
  }
  for (const TabletLocationsPB& tablet : tablets) {
    if (locations) {
      locations->push_back(tablet);
    }
    tablet_uuids->push_back(tablet.tablet_id());
    if (ranges != nullptr) {
      const PartitionPB& partition = tablet.partition();
      ranges->push_back(partition.ShortDebugString());
    }
  }

  if (update_tablets_cache) {
    data_->meta_cache_->ProcessTabletLocations(tablets, nullptr /* partition_group_start */);
  }

  return Status::OK();
}

rpc::Messenger* YBClient::messenger() const {
  return data_->messenger_;
}

const scoped_refptr<MetricEntity>& YBClient::metric_entity() const {
  return data_->metric_entity_;
}

rpc::ProxyCache& YBClient::proxy_cache() const {
  return *data_->proxy_cache_;
}

ThreadPool *YBClient::callback_threadpool() {
  return data_->cb_threadpool_.get();
}

const std::string& YBClient::proxy_uuid() const {
  return data_->uuid_;
}

const ClientId& YBClient::id() const {
  return data_->id_;
}

std::pair<RetryableRequestId, RetryableRequestId> YBClient::NextRequestIdAndMinRunningRequestId(
    const TabletId& tablet_id) {
  std::lock_guard<simple_spinlock> lock(data_->tablet_requests_mutex_);
  auto& tablet = data_->tablet_requests_[tablet_id];
  auto id = tablet.request_id_seq++;
  tablet.running_requests.insert(id);
  return std::make_pair(id, *tablet.running_requests.begin());
}

void YBClient::RequestFinished(const TabletId& tablet_id, RetryableRequestId request_id) {
  std::lock_guard<simple_spinlock> lock(data_->tablet_requests_mutex_);
  auto& tablet = data_->tablet_requests_[tablet_id];
  auto it = tablet.running_requests.find(request_id);
  if (it != tablet.running_requests.end()) {
    tablet.running_requests.erase(it);
  } else {
    LOG(DFATAL) << "RequestFinished called for an unknown request: "
                << tablet_id << ", " << request_id;
  }
}

void YBClient::LookupTabletByKey(const YBTable* table,
                                 const std::string& partition_key,
                                 CoarseTimePoint deadline,
                                 LookupTabletCallback callback) {
  data_->meta_cache_->LookupTabletByKey(table, partition_key, deadline, std::move(callback));
}

void YBClient::LookupTabletById(const std::string& tablet_id,
                                CoarseTimePoint deadline,
                                LookupTabletCallback callback,
                                UseCache use_cache) {
  data_->meta_cache_->LookupTabletById(
      tablet_id, deadline, std::move(callback), use_cache);
}

HostPort YBClient::GetMasterLeaderAddress() {
  return data_->leader_master_hostport();
}

Status YBClient::ListMasters(CoarseTimePoint deadline, std::vector<std::string>* master_uuids) {
  ListMastersRequestPB req;
  ListMastersResponsePB resp;
  CALL_SYNC_LEADER_MASTER_RPC_WITH_DEADLINE(req, resp, deadline, ListMasters);

  master_uuids->clear();
  for (const ServerEntryPB& master : resp.masters()) {
    if (master.has_error()) {
      LOG(ERROR) << "Master " << master.ShortDebugString() << " hit error "
        << master.error().ShortDebugString();
      return StatusFromPB(master.error());
    }
    master_uuids->push_back(master.instance_id().permanent_uuid());
  }
  return Status::OK();
}

Result<HostPort> YBClient::RefreshMasterLeaderAddress() {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->SetMasterServerProxy(this, deadline));

  return GetMasterLeaderAddress();
}

Status YBClient::RemoveMasterFromClient(const HostPort& remove) {
  return data_->RemoveMasterAddress(remove);
}

Status YBClient::AddMasterToClient(const HostPort& add) {
  return data_->AddMasterAddress(add);
}

Status YBClient::GetMasterUUID(const string& host,
                               int16_t port,
                               string* uuid) {
  HostPort hp(host, port);
  ServerEntryPB server;
  RETURN_NOT_OK(master::GetMasterEntryForHosts(
      data_->proxy_cache_.get(), {hp}, default_rpc_timeout(), &server));

  if (server.has_error()) {
    return STATUS(RuntimeError,
        strings::Substitute("Error $0 while getting uuid of $1:$2.",
                            "", host, port));
  }

  *uuid = server.instance_id().permanent_uuid();

  return Status::OK();
}

Status YBClient::SetReplicationInfo(const ReplicationInfoPB& replication_info) {
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  return data_->SetReplicationInfo(this, replication_info, deadline);
}

Status YBClient::ListTables(
    vector<YBTableName>* tables,
    const string& filter,
    bool exclude_ysql) {
  std::vector<std::pair<std::string, YBTableName>> tables_with_ids;
  RETURN_NOT_OK(ListTablesWithIds(&tables_with_ids, filter, exclude_ysql));
  tables->clear();
  tables->reserve(tables_with_ids.size());
  for (const auto& table_with_id : tables_with_ids) {
    tables->emplace_back(table_with_id.second);
  }
  return Status::OK();
}

Status YBClient::ListTablesWithIds(
    std::vector<std::pair<std::string, YBTableName>>* tables_with_ids,
    const std::string& filter,
    bool exclude_ysql) {
  tables_with_ids->clear();
  ListTablesRequestPB req;
  ListTablesResponsePB resp;

  if (!filter.empty()) {
    req.set_name_filter(filter);
  }
  CALL_SYNC_LEADER_MASTER_RPC(req, resp, ListTables);
  for (int i = 0; i < resp.tables_size(); i++) {
    const ListTablesResponsePB_TableInfo& table_info = resp.tables(i);
    DCHECK(table_info.has_namespace_());
    DCHECK(table_info.namespace_().has_name());
    if (exclude_ysql && table_info.table_type() == TableType::PGSQL_TABLE_TYPE) {
      continue;
    }
    tables_with_ids->emplace_back(
        table_info.id(),
        YBTableName(table_info.namespace_().name(), table_info.name()));
  }
  return Status::OK();
}

Result<bool> YBClient::TableExists(const YBTableName& table_name) {
  vector<YBTableName> tables;
  RETURN_NOT_OK(ListTables(&tables, table_name.table_name()));
  for (const YBTableName& table : tables) {
    if (table == table_name) {
      return true;
    }
  }
  return false;
}

Status YBClient::OpenTable(const YBTableName& table_name, shared_ptr<YBTable>* table) {
  YBTableInfo info;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->GetTableSchema(this, table_name, deadline, &info));

  // In the future, probably will look up the table in some map to reuse YBTable
  // instances.
  std::shared_ptr<YBTable> ret(new YBTable(this, info));
  RETURN_NOT_OK(ret->Open());
  table->swap(ret);
  return Status::OK();
}

Status YBClient::OpenTable(const TableId& table_id, shared_ptr<YBTable>* table) {
  YBTableInfo info;
  auto deadline = CoarseMonoClock::Now() + default_admin_operation_timeout();
  RETURN_NOT_OK(data_->GetTableSchema(this, table_id, deadline, &info));

  // In the future, probably will look up the table in some map to reuse YBTable
  // instances.
  std::shared_ptr<YBTable> ret(new YBTable(this, info));
  RETURN_NOT_OK(ret->Open());
  table->swap(ret);
  return Status::OK();
}

shared_ptr<YBSession> YBClient::NewSession() {
  return std::make_shared<YBSession>(this);
}

bool YBClient::IsMultiMaster() const {
  std::lock_guard<simple_spinlock> l(data_->master_server_addrs_lock_);
  if (data_->master_server_addrs_.size() > 1) {
    return true;
  }
  // For single entry case, check if it is a list of host/ports.
  vector<Endpoint> addrs;
  const auto status = ParseAddressList(data_->master_server_addrs_[0],
                                       yb::master::kMasterDefaultPort,
                                       &addrs);
  if (!status.ok()) {
    return false;
  }
  return addrs.size() > 1;
}

const MonoDelta& YBClient::default_admin_operation_timeout() const {
  return data_->default_admin_operation_timeout_;
}

const MonoDelta& YBClient::default_rpc_timeout() const {
  return data_->default_rpc_timeout_;
}

const uint64_t YBClient::kNoHybridTime = 0;

uint64_t YBClient::GetLatestObservedHybridTime() const {
  return data_->GetLatestObservedHybridTime();
}

void YBClient::SetLatestObservedHybridTime(uint64_t ht_hybrid_time) {
  data_->UpdateLatestObservedHybridTime(ht_hybrid_time);
}

}  // namespace client
}  // namespace yb
