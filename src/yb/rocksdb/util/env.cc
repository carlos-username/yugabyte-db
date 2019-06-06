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
//
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/env.h"

#include <thread>
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/port/sys_time.h"

#include "yb/rocksdb/options.h"
#include "yb/rocksdb/util/arena.h"
#include "yb/rocksdb/util/autovector.h"

namespace rocksdb {

Env::~Env() {
}

uint64_t Env::GetThreadID() const {
  std::hash<std::thread::id> hasher;
  return hasher(std::this_thread::get_id());
}

Status Env::ReuseWritableFile(const std::string& fname,
                              const std::string& old_fname,
                              unique_ptr<WritableFile>* result,
                              const EnvOptions& options) {
  Status s = RenameFile(old_fname, fname);
  if (!s.ok()) {
    return s;
  }
  return NewWritableFile(fname, result, options);
}

Status Env::GetChildrenFileAttributes(const std::string& dir,
                                      std::vector<FileAttributes>* result) {
  assert(result != nullptr);
  std::vector<std::string> child_fnames;
  Status s = GetChildren(dir, &child_fnames);
  if (!s.ok()) {
    return s;
  }
  result->resize(child_fnames.size());
  size_t result_size = 0;
  for (size_t i = 0; i < child_fnames.size(); ++i) {
    const std::string path = dir + "/" + child_fnames[i];
    s = GetFileSize(path, &(*result)[result_size].size_bytes);
    if (!s.ok()) {
      if (FileExists(path).IsNotFound()) {
        // The file may have been deleted since we listed the directory
        continue;
      }
      return s;
    }
    (*result)[result_size].name = std::move(child_fnames[i]);
    result_size++;
  }
  result->resize(result_size);
  return Status::OK();
}

yb::Result<uint64_t> Env::GetFileSize(const std::string& fname) {
  uint64_t result;
  RETURN_NOT_OK(GetFileSize(fname, &result));
  return result;
}

SequentialFile::~SequentialFile() {
}

RandomAccessFile::~RandomAccessFile() {
}

WritableFile::~WritableFile() {
}

Logger::~Logger() {
}

FileLock::~FileLock() {
}

void LogFlush(Logger *info_log) {
  if (info_log) {
    info_log->Flush();
  }
}

void LogWithContext(const char* file,
                    const int line,
                    Logger* info_log,
                    const char* format,
                    ...) {
  if (info_log && info_log->GetInfoLogLevel() <= InfoLogLevel::INFO_LEVEL) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::INFO_LEVEL, format, ap);
    va_end(ap);
  }
}

void Logger::Logv(const InfoLogLevel log_level, const char* format, va_list ap) {
  LogvWithContext(__FILE__, __LINE__, log_level, format, ap);
}

void Logger::LogvWithContext(const char* file,
    const int line,
    const InfoLogLevel log_level,
    const char* format,
    va_list ap) {
  static const char* kInfoLogLevelNames[6] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL", "HEADER"};
  static_assert(
      sizeof(kInfoLogLevelNames) / sizeof(kInfoLogLevelNames[0]) == NUM_INFO_LOG_LEVELS,
      "kInfoLogLevelNames must have an element for each log level");
  if (log_level < log_level_) {
    return;
  }

  if (log_level == InfoLogLevel::INFO_LEVEL) {
    // Doesn't print log level if it is INFO level.
    // This is to avoid unexpected performance regression after we add
    // the feature of log level. All the logs before we add the feature
    // are INFO level. We don't want to add extra costs to those existing
    // logging.
    Logv(format, ap);
  } else {
    char new_format[500];
    snprintf(new_format, sizeof(new_format) - 1, "[%s] %s",
        kInfoLogLevelNames[log_level], format);
    Logv(new_format, ap);
  }
}


void LogWithContext(const char* file,
                    const int line,
                    const InfoLogLevel log_level,
                    Logger* info_log,
                    const char* format,
                    ...) {
  if (info_log && info_log->GetInfoLogLevel() <= log_level) {
    va_list ap;
    va_start(ap, format);

    if (log_level == InfoLogLevel::HEADER_LEVEL) {
      info_log->LogHeaderWithContext(file, line, format, ap);
    } else {
      info_log->LogvWithContext(file, line, log_level, format, ap);
    }

    va_end(ap);
  }
}

void HeaderWithContext(const char* file, const int line,
    Logger *info_log, const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogHeaderWithContext(file, line, format, ap);
    va_end(ap);
  }
}

void DebugWithContext(const char* file, const int line,
    Logger *info_log, const char *format, ...) {
// Log level should be higher than DEBUG, but including the ifndef for compiletime optimization.
#ifndef NDEBUG
  if (info_log && info_log->GetInfoLogLevel() <= InfoLogLevel::DEBUG_LEVEL) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::DEBUG_LEVEL, format, ap);
    va_end(ap);
  }
#endif
}

void InfoWithContext(const char* file, const int line,
    Logger *info_log, const char *format, ...) {
  if (info_log && info_log->GetInfoLogLevel() <= InfoLogLevel::INFO_LEVEL) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::INFO_LEVEL, format, ap);
    va_end(ap);
  }
}

void WarnWithContext(const char* file, const int line,
    Logger *info_log, const char *format, ...) {
  if (info_log && info_log->GetInfoLogLevel() <= InfoLogLevel::WARN_LEVEL) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::WARN_LEVEL, format, ap);
    va_end(ap);
  }
}
void ErrorWithContext(const char* file, const int line,
    Logger *info_log, const char *format, ...) {
  if (info_log && info_log->GetInfoLogLevel() <= InfoLogLevel::ERROR_LEVEL) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::ERROR_LEVEL, format, ap);
    va_end(ap);
  }
}
void FatalWithContext(const char* file, const int line,
    Logger *info_log, const char *format, ...) {
  if (info_log && info_log->GetInfoLogLevel() <= InfoLogLevel::FATAL_LEVEL) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::FATAL_LEVEL, format, ap);
    va_end(ap);
  }
}

void LogFlush(const shared_ptr<Logger>& info_log) {
  if (info_log) {
    info_log->Flush();
  }
}

void LogWithContext(const char* file,
                    const int line,
                    const InfoLogLevel log_level,
                    const shared_ptr<Logger>& info_log,
                    const char* format,
                    ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, log_level, format, ap);
    va_end(ap);
  }
}

void HeaderWithContext(
    const char* file,
    const int line,
    const shared_ptr<Logger> &info_log,
    const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogHeaderWithContext(file, line, format, ap);
    va_end(ap);
  }
}

void DebugWithContext(
    const char* file,
    const int line,
    const shared_ptr<Logger> &info_log,
    const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::DEBUG_LEVEL, format, ap);
    va_end(ap);
  }
}

void InfoWithContext(
    const char* file,
    const int line,
    const shared_ptr<Logger> &info_log,
    const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::INFO_LEVEL, format, ap);
    va_end(ap);
  }
}

void WarnWithContext(
    const char* file,
    const int line,
    const shared_ptr<Logger> &info_log,
    const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::WARN_LEVEL, format, ap);
    va_end(ap);
  }
}

void ErrorWithContext(
    const char* file,
    const int line,
    const shared_ptr<Logger> &info_log,
    const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::ERROR_LEVEL, format, ap);
    va_end(ap);
  }
}

void FatalWithContext(
    const char* file,
    const int line,
    const shared_ptr<Logger> &info_log,
    const char *format, ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::FATAL_LEVEL, format, ap);
    va_end(ap);
  }
}

void LogWithContext(const char* file,
                    const int line,
                    const shared_ptr<Logger>& info_log,
                    const char* format,
                    ...) {
  if (info_log) {
    va_list ap;
    va_start(ap, format);
    info_log->LogvWithContext(file, line, InfoLogLevel::INFO_LEVEL, format, ap);
    va_end(ap);
  }
}

Status WriteStringToFile(Env* env, const Slice& data, const std::string& fname,
                         bool should_sync) {
  unique_ptr<WritableFile> file;
  EnvOptions soptions;
  Status s = env->NewWritableFile(fname, &file, soptions);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok() && should_sync) {
    s = file->Sync();
  }
  if (!s.ok()) {
    env->DeleteFile(fname);
  }
  return s;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  EnvOptions soptions;
  data->clear();
  unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file, soptions);
  if (!s.ok()) {
    return s;
  }
  static const int kBufferSize = 8192;
  char* space = new char[kBufferSize];
  while (true) {
    Slice fragment;
    s = file->Read(kBufferSize, &fragment, space);
    if (!s.ok()) {
      break;
    }
    data->append(fragment.cdata(), fragment.size());
    if (fragment.empty()) {
      break;
    }
  }
  delete[] space;
  return s;
}

EnvWrapper::~EnvWrapper() {
}

namespace {  // anonymous namespace

void AssignEnvOptions(EnvOptions* env_options, const DBOptions& options) {
  env_options->use_os_buffer = options.allow_os_buffer;
  env_options->use_mmap_reads = options.allow_mmap_reads;
  env_options->use_mmap_writes = options.allow_mmap_writes;
  env_options->set_fd_cloexec = options.is_fd_close_on_exec;
  env_options->bytes_per_sync = options.bytes_per_sync;
  env_options->compaction_readahead_size = options.compaction_readahead_size;
  env_options->random_access_max_buffer_size =
      options.random_access_max_buffer_size;
  env_options->rate_limiter = options.rate_limiter.get();
  env_options->writable_file_max_buffer_size =
      options.writable_file_max_buffer_size;
  env_options->allow_fallocate = options.allow_fallocate;
}

}  // anonymous namespace

EnvOptions Env::OptimizeForLogWrite(const EnvOptions& env_options,
                                    const DBOptions& db_options) const {
  EnvOptions optimized_env_options(env_options);
  optimized_env_options.bytes_per_sync = db_options.wal_bytes_per_sync;
  return optimized_env_options;
}

EnvOptions Env::OptimizeForManifestWrite(const EnvOptions& env_options) const {
  return env_options;
}

EnvOptions::EnvOptions(const DBOptions& options) {
  AssignEnvOptions(this, options);
}

EnvOptions::EnvOptions() {
  DBOptions options;
  AssignEnvOptions(this, options);
}


}  // namespace rocksdb
