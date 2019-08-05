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

#ifndef YB_TABLET_OPERATIONS_CHANGE_METADATA_OPERATION_H
#define YB_TABLET_OPERATIONS_CHANGE_METADATA_OPERATION_H

#include <mutex>
#include <string>

#include "yb/common/index.h"
#include "yb/gutil/macros.h"
#include "yb/tablet/operations/operation.h"
#include "yb/util/locks.h"

namespace yb {

class Schema;

namespace log {
class Log;
}

namespace tablet {

class TabletPeer;

// Operation Context for the AlterSchema operation.
// Keeps track of the Operation states (request, result, ...)
class ChangeMetadataOperationState : public OperationState {
 public:
  ~ChangeMetadataOperationState() {
  }

  ChangeMetadataOperationState(Tablet* tablet, log::Log* log,
                            const tserver::ChangeMetadataRequestPB* request = nullptr)
      : OperationState(tablet), log_(log), request_(request) {
  }

  explicit ChangeMetadataOperationState(const tserver::ChangeMetadataRequestPB* request)
      : ChangeMetadataOperationState(nullptr, nullptr, request) {
  }

  const tserver::ChangeMetadataRequestPB* request() const override { return request_; }

  void UpdateRequestFromConsensusRound() override;

  void set_schema(const Schema* schema) { schema_ = schema; }
  const Schema* schema() const { return schema_; }

  void SetIndexes(const google::protobuf::RepeatedPtrField<IndexInfoPB>& indexes);

  IndexMap& index_map() {
    return index_map_;
  }

  std::string new_table_name() const {
    return request_->new_table_name();
  }

  bool has_new_table_name() const {
    return request_->has_new_table_name();
  }

  uint32_t schema_version() const {
    return request_->schema_version();
  }

  uint32_t wal_retention_secs() const {
    return request_->wal_retention_secs();
  }

  bool has_wal_retention_secs() const {
    return request_->has_wal_retention_secs();
  }

  void AcquireSchemaLock(rw_semaphore* l);

  // Release the acquired schema lock.
  // Crashes if the lock was not already acquired.
  void ReleaseSchemaLock();

  // Note: request_ is set to NULL after this method returns.
  void Finish() {
    // Make the request NULL since after this transaction commits
    // the request may be deleted at any moment.
    request_ = nullptr;
  }

  log::Log* log() const { return log_; }

  log::Log* mutable_log() { return log_; }

  virtual std::string ToString() const override;

 private:
  log::Log* const log_;

  // The new (target) Schema.
  const Schema* schema_ = nullptr;

  // Lookup map for the associated indexes.
  IndexMap index_map_;

  // The original RPC request and response.
  const tserver::ChangeMetadataRequestPB *request_;

  // The lock held on the tablet's schema_lock_.
  std::unique_lock<rw_semaphore> schema_lock_;

  DISALLOW_COPY_AND_ASSIGN(ChangeMetadataOperationState);
};

// Executes the metadata change operation.
class ChangeMetadataOperation : public Operation {
 public:
  explicit ChangeMetadataOperation(std::unique_ptr<ChangeMetadataOperationState> operation_state);

  ChangeMetadataOperationState* state() override {
    return down_cast<ChangeMetadataOperationState*>(Operation::state());
  }

  const ChangeMetadataOperationState* state() const override {
    return down_cast<const ChangeMetadataOperationState*>(Operation::state());
  }

  consensus::ReplicateMsgPtr NewReplicateMsg() override;

  // Executes a Prepare for the metadata change operation.
  //
  // TODO: need a schema lock?

  CHECKED_STATUS Prepare() override;

  // Executes an Apply for the metadata change operation.
  CHECKED_STATUS Apply(int64_t leader_term) override;

  // Actually commits the transaction.
  void Finish(OperationResult result) override;

  std::string ToString() const override;

 private:
  // Starts the ChangeMetadataOperation by assigning it a timestamp.
  void DoStart() override;

  DISALLOW_COPY_AND_ASSIGN(ChangeMetadataOperation);
};

CHECKED_STATUS SyncReplicateChangeMetadataOperation(
    const tserver::ChangeMetadataRequestPB* req,
    tablet::TabletPeer* tablet_peer,
    int64_t term);

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_OPERATIONS_CHANGE_METADATA_OPERATION_H
