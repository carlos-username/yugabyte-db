// Copyright (c) 2015, Red Hat, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
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

#ifndef ROCKSDB_LITE

#include "yb/rocksdb/utilities/env_mirror.h"

namespace rocksdb {

// An implementaiton of Env that mirrors all work over two backend
// Env's.  This is useful for debugging purposes.
class SequentialFileMirror : public SequentialFile {
 public:
  unique_ptr<SequentialFile> a_, b_;
  std::string fname;
  explicit SequentialFileMirror(std::string f) : fname(std::move(f)) {}

  Status Read(size_t n, Slice* result, uint8_t* scratch) override {
    Slice aslice;
    Status as = a_->Read(n, &aslice, scratch);
    if (as.ok()) {
      std::unique_ptr<uint8_t[]> bscratch(new uint8_t[n]);
      Slice bslice;
      size_t off = 0;
      size_t left = aslice.size();
      while (left) {
        Status bs = b_->Read(left, &bslice, bscratch.get());
        assert(as.code() == bs.code());
        assert(memcmp(bscratch.get(), scratch + off, bslice.size()) == 0);
        off += bslice.size();
        left -= bslice.size();
      }
      *result = aslice;
    } else {
      Status bs = b_->Read(n, result, scratch);
      assert(as.code() == bs.code());
    }
    return as;
  }

  Status Skip(uint64_t n) override {
    Status as = a_->Skip(n);
    Status bs = b_->Skip(n);
    assert(as.code() == bs.code());
    return as;
  }

  Status InvalidateCache(size_t offset, size_t length) override {
    Status as = a_->InvalidateCache(offset, length);
    Status bs = b_->InvalidateCache(offset, length);
    assert(as.code() == bs.code());
    return as;
  }

  const std::string& filename() const override { return fname; }
};

class RandomAccessFileMirror : public RandomAccessFile {
 public:
  unique_ptr<RandomAccessFile> a_, b_;
  std::string fname;
  explicit RandomAccessFileMirror(std::string f) : fname(std::move(f)) {}

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const {
    Status as = a_->Read(offset, n, result, scratch);
    if (as.ok()) {
      char* bscratch = new char[n];
      Slice bslice;
      size_t off = 0;
      size_t left = result->size();
      while (left) {
        Status bs = b_->Read(offset + off, left, &bslice, bscratch);
        assert(as.code() == bs.code());
        assert(memcmp(bscratch, scratch + off, bslice.size()) == 0);
        off += bslice.size();
        left -= bslice.size();
      }
      delete[] bscratch;
    } else {
      Status bs = b_->Read(offset, n, result, scratch);
      assert(as.code() == bs.code());
    }
    return as;
  }

  bool ShouldForwardRawRequest() const {
    // NOTE: not verified
    return a_->ShouldForwardRawRequest();
  }

  size_t GetUniqueId(char* id, size_t max_size) const {
    // NOTE: not verified
    return a_->GetUniqueId(id, max_size);
  }
};

class WritableFileMirror : public WritableFile {
 public:
  unique_ptr<WritableFile> a_, b_;
  std::string fname;
  explicit WritableFileMirror(std::string f) : fname(std::move(f)) {}

  Status Append(const Slice& data) override {
    Status as = a_->Append(data);
    Status bs = b_->Append(data);
    assert(as.code() == bs.code());
    return as;
  }
  Status PositionedAppend(const Slice& data, uint64_t offset) override {
    Status as = a_->PositionedAppend(data, offset);
    Status bs = b_->PositionedAppend(data, offset);
    assert(as.code() == bs.code());
    return as;
  }
  Status Truncate(uint64_t size) override {
    Status as = a_->Truncate(size);
    Status bs = b_->Truncate(size);
    assert(as.code() == bs.code());
    return as;
  }
  Status Close() override {
    Status as = a_->Close();
    Status bs = b_->Close();
    assert(as.code() == bs.code());
    return as;
  }
  Status Flush() override {
    Status as = a_->Flush();
    Status bs = b_->Flush();
    assert(as.code() == bs.code());
    return as;
  }
  Status Sync() override {
    Status as = a_->Sync();
    Status bs = b_->Sync();
    assert(as.code() == bs.code());
    return as;
  }
  Status Fsync() override {
    Status as = a_->Fsync();
    Status bs = b_->Fsync();
    assert(as.code() == bs.code());
    return as;
  }
  bool IsSyncThreadSafe() const override {
    bool as = a_->IsSyncThreadSafe();
    assert(as == b_->IsSyncThreadSafe());
    return as;
  }
  void SetIOPriority(Env::IOPriority pri) override {
    a_->SetIOPriority(pri);
    b_->SetIOPriority(pri);
  }
  Env::IOPriority GetIOPriority() override {
    // NOTE: we don't verify this one
    return a_->GetIOPriority();
  }
  uint64_t GetFileSize() override {
    uint64_t as = a_->GetFileSize();
    assert(as == b_->GetFileSize());
    return as;
  }
  void GetPreallocationStatus(size_t* block_size,
                              size_t* last_allocated_block) override {
    // NOTE: we don't verify this one
    return a_->GetPreallocationStatus(block_size, last_allocated_block);
  }
  size_t GetUniqueId(char* id, size_t max_size) const override {
    // NOTE: we don't verify this one
    return a_->GetUniqueId(id, max_size);
  }
  Status InvalidateCache(size_t offset, size_t length) override {
    Status as = a_->InvalidateCache(offset, length);
    Status bs = b_->InvalidateCache(offset, length);
    assert(as.code() == bs.code());
    return as;
  }

 protected:
  Status Allocate(uint64_t offset, uint64_t length) override {
    Status as = a_->Allocate(offset, length);
    Status bs = b_->Allocate(offset, length);
    assert(as.code() == bs.code());
    return as;
  }
  Status RangeSync(uint64_t offset, uint64_t nbytes) override {
    Status as = a_->RangeSync(offset, nbytes);
    Status bs = b_->RangeSync(offset, nbytes);
    assert(as.code() == bs.code());
    return as;
  }
};

Status EnvMirror::NewSequentialFile(const std::string& f,
                                    unique_ptr<SequentialFile>* r,
                                    const EnvOptions& options) {
  if (f.find("/proc/") == 0) {
    return a_->NewSequentialFile(f, r, options);
  }
  SequentialFileMirror* mf = new SequentialFileMirror(f);
  Status as = a_->NewSequentialFile(f, &mf->a_, options);
  Status bs = b_->NewSequentialFile(f, &mf->b_, options);
  assert(as.code() == bs.code());
  if (as.ok())
    r->reset(mf);
  else
    delete mf;
  return as;
}

Status EnvMirror::NewRandomAccessFile(const std::string& f,
                                      unique_ptr<RandomAccessFile>* r,
                                      const EnvOptions& options) {
  if (f.find("/proc/") == 0) {
    return a_->NewRandomAccessFile(f, r, options);
  }
  RandomAccessFileMirror* mf = new RandomAccessFileMirror(f);
  Status as = a_->NewRandomAccessFile(f, &mf->a_, options);
  Status bs = b_->NewRandomAccessFile(f, &mf->b_, options);
  assert(as.code() == bs.code());
  if (as.ok())
    r->reset(mf);
  else
    delete mf;
  return as;
}

Status EnvMirror::NewWritableFile(const std::string& f,
                                  unique_ptr<WritableFile>* r,
                                  const EnvOptions& options) {
  if (f.find("/proc/") == 0) return a_->NewWritableFile(f, r, options);
  WritableFileMirror* mf = new WritableFileMirror(f);
  Status as = a_->NewWritableFile(f, &mf->a_, options);
  Status bs = b_->NewWritableFile(f, &mf->b_, options);
  assert(as.code() == bs.code());
  if (as.ok())
    r->reset(mf);
  else
    delete mf;
  return as;
}

Status EnvMirror::ReuseWritableFile(const std::string& fname,
                                    const std::string& old_fname,
                                    unique_ptr<WritableFile>* r,
                                    const EnvOptions& options) {
  if (fname.find("/proc/") == 0)
    return a_->ReuseWritableFile(fname, old_fname, r, options);
  WritableFileMirror* mf = new WritableFileMirror(fname);
  Status as = a_->ReuseWritableFile(fname, old_fname, &mf->a_, options);
  Status bs = b_->ReuseWritableFile(fname, old_fname, &mf->b_, options);
  assert(as.code() == bs.code());
  if (as.ok())
    r->reset(mf);
  else
    delete mf;
  return as;
}

}  // namespace rocksdb
#endif
