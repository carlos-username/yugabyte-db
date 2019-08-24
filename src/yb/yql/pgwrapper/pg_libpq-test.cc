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

#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper_test_base.h"

#include "yb/common/common.pb.h"

using namespace std::literals;

DECLARE_int64(external_mini_cluster_max_log_bytes);
DECLARE_int64(retryable_rpc_single_call_timeout_ms);

METRIC_DECLARE_entity(tablet);
METRIC_DECLARE_counter(transaction_not_found);

namespace yb {
namespace pgwrapper {

class PgLibPqTest : public PgWrapperTestBase {
 protected:
  void SetUp() override {
    // postgres has very verbose logging in case of conflicts
    FLAGS_external_mini_cluster_max_log_bytes = 512_MB;
    PgWrapperTestBase::SetUp();
  }

  Result<PGConnPtr> Connect() {
    auto deadline = CoarseMonoClock::now() + 60s;
    for (;;) {
      PGConnPtr result(PQconnectdb(Format(
          "host=$0 port=$1 user=postgres", pg_ts->bind_host(), pg_ts->pgsql_rpc_port()).c_str()));
      auto status = PQstatus(result.get());
      if (status == ConnStatusType::CONNECTION_OK) {
        return result;
      }
      if (CoarseMonoClock::now() >= deadline) {
        return STATUS_FORMAT(NetworkError, "Connect failed: $0", status);
      }
    }
  }

  void TestMultiBankAccount(const std::string& isolation_level);

  void DoIncrement(int key, int num_increments, IsolationLevel isolation);

  void TestParallelCounter(IsolationLevel isolation);

  void TestConcurrentCounter(IsolationLevel isolation);

  void TestOnConflict(bool kill_master, const MonoDelta& duration);
};

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(Simple)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT, value TEXT)"));
  ASSERT_OK(Execute(conn.get(), "INSERT INTO t (key, value) VALUES (1, 'hello')"));

  auto res = ASSERT_RESULT(Fetch(conn.get(), "SELECT * FROM t"));

  {
    auto lines = PQntuples(res.get());
    ASSERT_EQ(1, lines);

    auto columns = PQnfields(res.get());
    ASSERT_EQ(2, columns);

    auto key = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(key, 1);
    auto value = ASSERT_RESULT(GetString(res.get(), 0, 1));
    ASSERT_EQ(value, "hello");
  }
}

// Test that repeats example from this article:
// https://blogs.msdn.microsoft.com/craigfr/2007/05/16/serializable-vs-snapshot-isolation-level/
//
// Multiple rows with values 0 and 1 are stored in table.
// Two concurrent transaction fetches all rows from table and does the following.
// First transaction changes value of all rows with value 0 to 1.
// Second transaction changes value of all rows with value 1 to 0.
// As outcome we should have rows with the same value.
//
// The described prodecure is repeated multiple times to increase probability of catching bug,
// w/o running test multiple times.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SerializableColoring)) {
  static const std::string kTryAgain = "Try again.";
  constexpr auto kKeys = RegularBuildVsSanitizers(10, 20);
  constexpr auto kColors = 2;
  constexpr auto kIterations = 20;

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT PRIMARY KEY, color INT)"));

  auto iterations_left = kIterations;

  for (int iteration = 0; iterations_left > 0; ++iteration) {
    SCOPED_TRACE(Format("Iteration: $0", iteration));

    auto status = Execute(conn.get(), "DELETE FROM t");
    if (!status.ok()) {
      ASSERT_STR_CONTAINS(status.ToString(), kTryAgain);
      continue;
    }
    for (int k = 0; k != kKeys; ++k) {
      int32_t color = RandomUniformInt(0, kColors - 1);
      ASSERT_OK(Execute(conn.get(),
          Format("INSERT INTO t (key, color) VALUES ($0, $1)", k, color)));
    }

    std::atomic<int> complete{ 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i != kColors; ++i) {
      int32_t color = i;
      threads.emplace_back([this, color, kKeys, &complete] {
        auto conn = ASSERT_RESULT(Connect());

        ASSERT_OK(Execute(conn.get(), "BEGIN"));
        ASSERT_OK(Execute(conn.get(), "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"));

        auto res = Fetch(conn.get(), "SELECT * FROM t");
        if (!res.ok()) {
          auto msg = res.status().message().ToBuffer();
          ASSERT_STR_CONTAINS(res.status().ToString(), kTryAgain);
          return;
        }
        auto columns = PQnfields(res->get());
        ASSERT_EQ(2, columns);

        auto lines = PQntuples(res->get());
        ASSERT_EQ(kKeys, lines);
        for (int i = 0; i != lines; ++i) {
          if (ASSERT_RESULT(GetInt32(res->get(), i, 1)) == color) {
            continue;
          }

          auto key = ASSERT_RESULT(GetInt32(res->get(), i, 0));
          auto status = Execute(
              conn.get(), Format("UPDATE t SET color = $1 WHERE key = $0", key, color));
          if (!status.ok()) {
            auto msg = status.message().ToBuffer();
            // Missing metadata means that transaction was aborted and cleaned.
            ASSERT_TRUE(msg.find("Try again.") != std::string::npos ||
                        msg.find("Missing metadata") != std::string::npos) << status;
            break;
          }
        }

        auto status = Execute(conn.get(), "COMMIT");
        if (!status.ok()) {
          auto msg = status.message().ToBuffer();
          ASSERT_TRUE(msg.find("Operation expired") != std::string::npos) << status;
          return;
        }

        ++complete;
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    if (complete == 0) {
      continue;
    }

    auto res = ASSERT_RESULT(Fetch(conn.get(), "SELECT * FROM t"));
    auto columns = PQnfields(res.get());
    ASSERT_EQ(2, columns);

    auto lines = PQntuples(res.get());
    ASSERT_EQ(kKeys, lines);

    std::vector<int32_t> zeroes, ones;
    for (int i = 0; i != lines; ++i) {
      auto key = ASSERT_RESULT(GetInt32(res.get(), i, 0));
      auto current = ASSERT_RESULT(GetInt32(res.get(), i, 1));
      if (current == 0) {
        zeroes.push_back(key);
      } else {
        ones.push_back(key);
      }
    }

    std::sort(ones.begin(), ones.end());
    std::sort(zeroes.begin(), zeroes.end());

    LOG(INFO) << "Zeroes: " << yb::ToString(zeroes) << ", ones: " << yb::ToString(ones);
    ASSERT_TRUE(zeroes.empty() || ones.empty());

    --iterations_left;
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SerializableReadWriteConflict)) {
  const auto kKeys = RegularBuildVsSanitizers(20, 5);

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT PRIMARY KEY)"));

  size_t reads_won = 0, writes_won = 0;
  for (int i = 0; i != kKeys; ++i) {
    auto read_conn = ASSERT_RESULT(Connect());
    ASSERT_OK(Execute(read_conn.get(), "BEGIN ISOLATION LEVEL SERIALIZABLE"));
    auto res = Fetch(read_conn.get(), Format("SELECT * FROM t WHERE key = $0", i));
    auto read_status = ResultToStatus(res);

    auto write_conn = ASSERT_RESULT(Connect());
    ASSERT_OK(Execute(write_conn.get(), "BEGIN ISOLATION LEVEL SERIALIZABLE"));
    auto write_status = Execute(write_conn.get(), Format("INSERT INTO t (key) VALUES ($0)", i));

    std::thread read_commit_thread([&read_conn, &read_status] {
      if (read_status.ok()) {
        read_status = Execute(read_conn.get(), "COMMIT");
      }
    });

    std::thread write_commit_thread([&write_conn, &write_status] {
      if (write_status.ok()) {
        write_status = Execute(write_conn.get(), "COMMIT");
      }
    });

    read_commit_thread.join();
    write_commit_thread.join();

    LOG(INFO) << "Read: " << read_status << ", write: " << write_status;

    if (!read_status.ok()) {
      ASSERT_OK(write_status);
      ++writes_won;
    } else {
      ASSERT_NOK(write_status);
      ++reads_won;
    }
  }

  LOG(INFO) << "Reads won: " << reads_won << ", writes won: " << writes_won;
  if (RegularBuildVsSanitizers(true, false)) {
    ASSERT_GE(reads_won, kKeys / 4);
    ASSERT_GE(writes_won, kKeys / 4);
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(ReadRestart)) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT PRIMARY KEY)"));

  std::atomic<bool> stop(false);
  std::atomic<int> last_written(0);

  std::thread write_thread([this, &stop, &last_written] {
    auto write_conn = ASSERT_RESULT(Connect());
    int write_key = 1;
    while (!stop.load(std::memory_order_acquire)) {
      SCOPED_TRACE(Format("Writing: $0", write_key));

      ASSERT_OK(Execute(write_conn.get(), "BEGIN"));
      auto status = Execute(write_conn.get(), Format("INSERT INTO t (key) VALUES ($0)", write_key));
      if (status.ok()) {
        status = Execute(write_conn.get(), "COMMIT");
      }
      if (status.ok()) {
        last_written.store(write_key, std::memory_order_release);
        ++write_key;
      } else {
        LOG(INFO) << "Write " << write_key << " failed: " << status;
      }
    }
  });

  auto se = ScopeExit([&stop, &write_thread] {
    stop.store(true, std::memory_order_release);
    write_thread.join();
  });

  auto deadline = CoarseMonoClock::now() + 30s;

  while (CoarseMonoClock::now() < deadline) {
    int read_key = last_written.load(std::memory_order_acquire);
    if (read_key == 0) {
      std::this_thread::sleep_for(100ms);
      continue;
    }

    SCOPED_TRACE(Format("Reading: $0", read_key));

    ASSERT_OK(Execute(conn.get(), "BEGIN"));

    auto res = ASSERT_RESULT(Fetch(conn.get(), Format("SELECT * FROM t WHERE key = $0", read_key)));
    auto columns = PQnfields(res.get());
    ASSERT_EQ(1, columns);

    auto lines = PQntuples(res.get());
    ASSERT_EQ(1, lines);

    auto key = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(key, read_key);

    ASSERT_OK(Execute(conn.get(), "ROLLBACK"));
  }

  ASSERT_GE(last_written.load(std::memory_order_acquire), 100);
}

// Concurrently insert records to table with index.
TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(ConcurrentIndexInsert)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(
      conn.get(),
      "CREATE TABLE IF NOT EXISTS users(id text, ename text, age int, PRIMARY KEY(id))"));

  ASSERT_OK(Execute(
      conn.get(), "CREATE INDEX IF NOT EXISTS name_idx ON users(ename)"));

  constexpr auto kWriteThreads = 4;

  std::atomic<bool> stop(false);
  std::vector<std::thread> write_threads;

  while (write_threads.size() != kWriteThreads) {
    write_threads.emplace_back([this, &stop] {
      auto write_conn = ASSERT_RESULT(Connect());
      auto this_thread_id = std::this_thread::get_id();
      auto tid = std::hash<decltype(this_thread_id)>()(this_thread_id);
      int idx = 0;
      while (!stop.load(std::memory_order_acquire)) {
        ASSERT_OK(Execute(
            write_conn.get(),
            Format("INSERT INTO users (id, ename, age) VALUES ('user-$0-$1', 'name-$1', $2)",
                   tid, idx, 20 + (idx % 50))));
        ++idx;
      }
    });
  }

  auto se = ScopeExit([&stop, &write_threads] {
    stop.store(true, std::memory_order_release);
    for (auto& thread : write_threads) {
      thread.join();
    }
  });

  std::this_thread::sleep_for(30s);
}

bool TransactionalFailure(const Status& status) {
  auto message = status.ToString();
  return message.find("Restart read required at") != std::string::npos ||
         message.find("Transaction expired") != std::string::npos ||
         message.find("Transaction aborted") != std::string::npos ||
         message.find("Unknown transaction") != std::string::npos ||
         message.find("Transaction metadata missing") != std::string::npos ||
         message.find("Transaction was recently aborted") != std::string::npos ||
         message.find("Conflicts with committed transaction") != std::string::npos ||
         message.find("Value write after transaction start") != std::string::npos ||
         message.find("Conflicts with higher priority transaction") != std::string::npos;
}

Result<int64_t> ReadSumBalance(
    PGconn* conn, int accounts, const std::string& begin_transaction_statement,
    std::atomic<int>* counter) {
  RETURN_NOT_OK(Execute(conn, begin_transaction_statement));
  bool failed = true;
  auto se = ScopeExit([conn, &failed] {
    if (failed) {
      EXPECT_OK(Execute(conn, "ROLLBACK"));
    }
  });

  int64_t sum = 0;
  for (int i = 1; i <= accounts; ++i) {
    LOG(INFO) << "Reading: " << i;
    sum += VERIFY_RESULT(FetchValue<int64_t>(
        conn, Format("SELECT balance FROM account_$0 WHERE id = $0", i)));
  }

  failed = false;
  RETURN_NOT_OK(Execute(conn, "COMMIT"));
  return sum;
}

void PgLibPqTest::TestMultiBankAccount(const std::string& isolation_level) {
  constexpr int kAccounts = RegularBuildVsSanitizers(20, 10);
  constexpr int64_t kInitialBalance = 100;

#ifndef NDEBUG
  const auto kTimeout = 180s;
  constexpr int kThreads = RegularBuildVsSanitizers(12, 5);
#else
  const auto kTimeout = 60s;
  constexpr int kThreads = 5;
#endif

  PGConnPtr conn;
  ASSERT_OK(WaitFor([this, &conn] {
    auto res = Connect();
    if (!res.ok()) {
      return false;
    }
    conn = std::move(*res);
    return true;
  }, 5s, "Initial connect"));

  const std::string begin_transaction_statement =
      "START TRANSACTION ISOLATION LEVEL " + isolation_level;

  for (int i = 1; i <= kAccounts; ++i) {
    ASSERT_OK(Execute(
        conn.get(),
        Format("CREATE TABLE account_$0 (id int, balance bigint, PRIMARY KEY(id))", i)));
    ASSERT_OK(Execute(
        conn.get(),
        Format("INSERT INTO account_$0 (id, balance) VALUES ($0, $1)", i, kInitialBalance)));
  }

  std::atomic<int> writes(0);
  std::atomic<int> reads(0);

  std::atomic<int> counter(100000);
  TestThreadHolder thread_holder;
  for (int i = 1; i <= kThreads; ++i) {
    thread_holder.AddThreadFunctor(
        [this, &writes, &begin_transaction_statement,
         &stop_flag = thread_holder.stop_flag()]() {
      auto conn = ASSERT_RESULT(Connect());
      while (!stop_flag.load(std::memory_order_acquire)) {
        int from = RandomUniformInt(1, kAccounts);
        int to = RandomUniformInt(1, kAccounts - 1);
        if (to >= from) {
          ++to;
        }
        int64_t amount = RandomUniformInt(1, 10);
        ASSERT_OK(Execute(conn.get(), begin_transaction_statement));
        auto status = Execute(conn.get(), Format(
              "UPDATE account_$0 SET balance = balance - $1 WHERE id = $0", from, amount));
        if (status.ok()) {
          status = Execute(conn.get(), Format(
              "UPDATE account_$0 SET balance = balance + $1 WHERE id = $0", to, amount));
        }
        if (status.ok()) {
          status = Execute(conn.get(), "COMMIT;");
        } else {
          ASSERT_OK(Execute(conn.get(), "ROLLBACK;"));
        }
        if (!status.ok()) {
          ASSERT_TRUE(TransactionalFailure(status)) << status;
        } else {
          LOG(INFO) << "Updated: " << from << " => " << to << " by " << amount;
          ++writes;
        }
      }
    });
  }

  thread_holder.AddThreadFunctor(
      [this, &counter, &reads, &begin_transaction_statement,
       &stop_flag = thread_holder.stop_flag()]() {
    auto conn = ASSERT_RESULT(Connect());
    while (!stop_flag.load(std::memory_order_acquire)) {
      auto sum = ReadSumBalance(conn.get(), kAccounts, begin_transaction_statement, &counter);
      if (!sum.ok()) {
        ASSERT_TRUE(TransactionalFailure(sum.status())) << sum.status();
      } else {
        ASSERT_EQ(*sum, kAccounts * kInitialBalance);
        ++reads;
      }
    }
  });

  constexpr auto kRequiredReads = RegularBuildVsSanitizers(5, 2);
  constexpr auto kRequiredWrites = RegularBuildVsSanitizers(1000, 500);
  auto wait_status = WaitFor([&reads, &writes, &stop = thread_holder.stop_flag()] {
    return stop.load() || (writes.load() >= kRequiredWrites && reads.load() >= kRequiredReads);
  }, kTimeout, Format("At least $0 reads and $1 writes", kRequiredReads, kRequiredWrites));

  LOG(INFO) << "Writes: " << writes.load() << ", reads: " << reads.load();

  ASSERT_OK(wait_status);

  thread_holder.Stop();

  ASSERT_OK(WaitFor([&conn, &begin_transaction_statement, &counter]() -> Result<bool> {
    auto sum = ReadSumBalance(conn.get(), kAccounts, begin_transaction_statement, &counter);
    if (!sum.ok()) {
      if (!TransactionalFailure(sum.status())) {
        return sum.status();
      }
      return false;
    }
    EXPECT_EQ(*sum, kAccounts * kInitialBalance);
    return true;
  }, 10s, "Final read"));

  auto total_not_found = 0;
  for (auto* tserver : cluster_->tserver_daemons()) {
    auto tablets = ASSERT_RESULT(cluster_->GetTabletIds(tserver));
    for (const auto& tablet : tablets) {
      int64_t value;
      auto status = tserver->GetInt64Metric(
          &METRIC_ENTITY_tablet, tablet.c_str(), &METRIC_transaction_not_found, "value", &value);
      if (status.ok()) {
        total_not_found += value;
      } else {
        ASSERT_TRUE(status.IsNotFound()) << status;
      }
    }
  }

  LOG(INFO) << "Total not found: " << total_not_found;
  // Check that total not found is not too big.
  ASSERT_LE(total_not_found, 200);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(MultiBankAccountSnapshot)) {
  TestMultiBankAccount("REPEATABLE READ");
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(MultiBankAccountSerializable)) {
  TestMultiBankAccount("SERIALIZABLE");
}

void PgLibPqTest::DoIncrement(int key, int num_increments, IsolationLevel isolation) {
  auto conn = ASSERT_RESULT(Connect());

  // Perform increments
  int succeeded_incs = 0;
  while (succeeded_incs < num_increments) {
    ASSERT_OK(Execute(conn.get(), isolation == IsolationLevel::SERIALIZABLE_ISOLATION ?
        "START TRANSACTION ISOLATION LEVEL SERIALIZABLE" :
        "START TRANSACTION ISOLATION LEVEL REPEATABLE READ"));
    bool committed = false;
    auto exec_status = Execute(conn.get(),
                               Format("UPDATE t SET value = value + 1 WHERE key = $0", key));
    if (exec_status.ok()) {
      auto commit_status = Execute(conn.get(), "COMMIT");
      if (commit_status.ok()) {
        succeeded_incs++;
        committed = true;
      }
    }
    if (!committed) {
      ASSERT_OK(Execute(conn.get(), "ROLLBACK"));
    }
  }
}

void PgLibPqTest::TestParallelCounter(IsolationLevel isolation) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT, value INT)"));

  const auto kThreads = RegularBuildVsSanitizers(3, 2);
  const auto kIncrements = RegularBuildVsSanitizers(100, 20);

  // Make a counter for each thread and have each thread increment it
  std::vector<std::thread> threads;
  while (threads.size() != kThreads) {
    int key = threads.size();
    ASSERT_OK(Execute(conn.get(),
                      Format("INSERT INTO t (key, value) VALUES ($0, 0)", key)));

    threads.emplace_back([this, key, isolation] {
      DoIncrement(key, kIncrements, isolation);
    });
  }

  // Wait for completion
  for (auto& thread : threads) {
    thread.join();
  }

  // Check each counter
  for (int i = 0; i < kThreads; i++) {
    auto res = ASSERT_RESULT(Fetch(conn.get(),
                                   Format("SELECT value FROM t WHERE key = $0", i)));

    auto row_val = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
    ASSERT_EQ(row_val, kIncrements);
  }
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestParallelCounterSerializable)) {
  TestParallelCounter(IsolationLevel::SERIALIZABLE_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestParallelCounterRepeatableRead)) {
  TestParallelCounter(IsolationLevel::SNAPSHOT_ISOLATION);
}

void PgLibPqTest::TestConcurrentCounter(IsolationLevel isolation) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (key INT, value INT)"));

  ASSERT_OK(Execute(conn.get(),
                    "INSERT INTO t (key, value) VALUES (0, 0)"));

  const auto kThreads = RegularBuildVsSanitizers(3, 2);
  const auto kIncrements = RegularBuildVsSanitizers(100, 20);

  // Have each thread increment the same already-created counter
  std::vector<std::thread> threads;
  while (threads.size() != kThreads) {
    threads.emplace_back([this, isolation] {
      DoIncrement(0, kIncrements, isolation);
    });
  }

  // Wait for completion
  for (auto& thread : threads) {
    thread.join();
  }

  // Check that we incremented exactly the desired number of times
  auto res = ASSERT_RESULT(Fetch(conn.get(),
                                 "SELECT value FROM t WHERE key = 0"));

  auto row_val = ASSERT_RESULT(GetInt32(res.get(), 0, 0));
  ASSERT_EQ(row_val, kThreads * kIncrements);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestConcurrentCounterSerializable)) {
  TestConcurrentCounter(IsolationLevel::SERIALIZABLE_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(TestConcurrentCounterRepeatableRead)) {
  TestConcurrentCounter(IsolationLevel::SNAPSHOT_ISOLATION);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(SecondaryIndexInsertSelect)) {
  constexpr int kThreads = 4;

  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE t (a INT PRIMARY KEY, b INT)"));
  ASSERT_OK(Execute(conn.get(), "CREATE INDEX ON t (b, a)"));

  TestThreadHolder holder;
  std::array<std::atomic<int>, kThreads> written;
  for (auto& w : written) {
    w.store(0, std::memory_order_release);
  }

  for (int i = 0; i != kThreads; ++i) {
    holder.AddThread([this, i, &stop = holder.stop_flag(), &written] {
      auto conn = ASSERT_RESULT(Connect());
      SetFlagOnExit set_flag_on_exit(&stop);
      int key = 0;

      while (!stop.load(std::memory_order_acquire)) {
        if (RandomUniformBool()) {
          int a = i * 1000000 + key;
          int b = key;
          ASSERT_OK(Execute(conn.get(), Format("INSERT INTO t (a, b) VALUES ($0, $1)", a, b)));
          written[i].store(++key, std::memory_order_release);
        } else {
          int writer_index = RandomUniformInt(0, kThreads - 1);
          int num_written = written[writer_index].load(std::memory_order_acquire);
          if (num_written == 0) {
            continue;
          }
          int read_key = num_written - 1;
          int b = read_key;
          int read_a = ASSERT_RESULT(FetchValue<int32_t>(
              conn.get(), Format("SELECT a FROM t WHERE b = $0 LIMIT 1", b)));
          ASSERT_EQ(read_a % 1000000, read_key);
        }
      }
    });
  }

  holder.WaitAndStop(60s);
}

void AssertRows(PGconn *conn, int expected_num_rows) {
  auto res = ASSERT_RESULT(Fetch(conn, "SELECT * FROM test"));
  ASSERT_EQ(PQntuples(res.get()), expected_num_rows);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(InTxnDelete)) {
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE test (pk int PRIMARY KEY)"));
  ASSERT_OK(Execute(conn.get(), "BEGIN"));
  ASSERT_OK(Execute(conn.get(), "INSERT INTO test VALUES (1)"));
  ASSERT_NO_FATALS(AssertRows(conn.get(), 1));
  ASSERT_OK(Execute(conn.get(), "DELETE FROM test"));
  ASSERT_NO_FATALS(AssertRows(conn.get(), 0));
  ASSERT_OK(Execute(conn.get(), "INSERT INTO test VALUES (1)"));
  ASSERT_NO_FATALS(AssertRows(conn.get(), 1));
  ASSERT_OK(Execute(conn.get(), "COMMIT"));

  ASSERT_NO_FATALS(AssertRows(conn.get(), 1));
}

struct OnConflictKey {
  int key;
  int operation_index = 0;
};

constexpr int kMaxBatchSize = 5;

struct BatchInfo {
  int key;
  char append_char; // Zero means read request
  std::string read_value;

  std::string ToString() const {
    if (append_char) {
      char x[2] = {append_char, 0};
      return Format("[$0+$1]", key, x);
    } else {
      return Format("[$0 $1]", key, read_value);
    }
  }

  bool ComesBefore(const BatchInfo& rhs) const {
    if (key != rhs.key) {
      return false;
    }
    if (append_char) {
      if (rhs.append_char) {
        return false;
      }
      // rhs see our append
      return rhs.read_value.find(append_char) != std::string::npos;
    } else if (!rhs.append_char) {
      // rhs has larger list
      return read_value.length() < rhs.read_value.length();
    } else {
      // we don't see the result of rhs
      return read_value.find(rhs.append_char) == std::string::npos;
    }
  }
};

struct TransactionInfo {
  typedef std::array<BatchInfo, kMaxBatchSize> Batches;
  typedef Batches::const_iterator const_iterator;

  int batch_size = 0;
  Batches batches;
  int last_visit = 0; // Used to check whether this vertex was visited by current DFS run.

  const_iterator begin() const {
    return batches.begin();
  }

  const_iterator end() const {
    return batches.begin() + batch_size;
  }

  bool ComesBefore(const TransactionInfo& rhs) const {
    for (const auto& lbatch : *this) {
      for (const auto& rbatch : rhs) {
        if (lbatch.ComesBefore(rbatch)) {
          return true;
        }
      }
    }
    return false;
  }
};

class OnConflictHelper {
 public:
  explicit OnConflictHelper(size_t concurrent_keys)
      : concurrent_keys_(concurrent_keys), active_keys_(concurrent_keys) {
    for(size_t i = 0; i != concurrent_keys; ++i) {
      active_keys_[i].key = ++next_key_;
    }
    for (auto i = 'A'; i <= 'Z'; ++i) {
      chars_.push_back(i);
    }
  }

  std::pair<int, char> RandomPair() {
    size_t i = RandomUniformInt<size_t>(0, concurrent_keys_ - 1);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& key = active_keys_[i];
    char append_char;
    if (RandomUniformBool()) {
      append_char = 0; // Read key
    } else {
      append_char = chars_[key.operation_index];
      if (++key.operation_index == chars_.size()) {
        key.key = ++next_key_;
        key.operation_index = 0;
      }
    }
    return std::make_pair(key.key, append_char);
  }

  void Committed(TransactionInfo&& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    committed_.push_back(std::move(info));
  }

  void Report() {
    LOG(INFO) << "Committed transactions:";

    ordered_.reserve(committed_.size());
    // Iteration order does not matter here, so we iterate from end to have lower keys at the start
    // of the list.
    for (auto it = committed_.rbegin(); it != committed_.rend(); ++it) {
      if (it->last_visit == 0) {
        DepthFirstSearch(&*it, nullptr /* dest */);
      }
    }

    std::reverse(ordered_.begin(), ordered_.end());

    for (const auto* info : ordered_) {
      LOG(INFO) << "  " << yb::ToString(*info);
    }

    int inversions = 0;
    for (auto it = ordered_.begin(); it != ordered_.end(); ++it) {
      for (auto j = ordered_.begin(); j != it; ++j) {
        if ((**it).ComesBefore(**j)) {
          LOG(INFO) << "Order inversion: " << yb::ToString(**it) << " and " << yb::ToString(**j);
          ++inversions;
          ++query_;
          DepthFirstSearch(*j, *it);
        }
      }
    }

    ASSERT_EQ(inversions, 0);
  }

 private:
  // Returns true if dest was reached.
  bool DepthFirstSearch(TransactionInfo* v, TransactionInfo* dest) {
    v->last_visit = query_;
    if (v == dest) {
      LOG(INFO) << "  " << yb::ToString(*v);
      return true;
    }
    for (auto& target : committed_) {
      if (target.last_visit < query_ && v->ComesBefore(target)) {
        if (DepthFirstSearch(&target, dest)) {
          LOG(INFO) << "  " << yb::ToString(*v);
          return true;
        }
      }
    }
    if (!dest) {
      ordered_.push_back(v);
    }
    return false;
  }

  const size_t concurrent_keys_;
  std::string chars_;

  std::mutex mutex_;
  int next_key_ = 0;
  std::vector<OnConflictKey> active_keys_;
  std::vector<TransactionInfo> committed_;
  std::vector<TransactionInfo*> ordered_;
  // Number of depth-first search run, used to filter visited vertexes.
  int query_ = 1;
};

// Check that INSERT .. ON CONFLICT .. does not generate duplicate key errors.
void PgLibPqTest::TestOnConflict(bool kill_master, const MonoDelta& duration) {
#ifndef NDEBUG
  constexpr int kWriters = RegularBuildVsSanitizers(15, 5);
#else
  constexpr int kWriters = 25;
#endif
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE test (k int PRIMARY KEY, v TEXT)"));

  std::atomic<int> processed(0);
  TestThreadHolder thread_holder;
  OnConflictHelper helper(3);
  for (int i = 0; i != kWriters; ++i) {
    thread_holder.AddThreadFunctor(
        [this, &stop = thread_holder.stop_flag(), &processed, &helper] {
      SetFlagOnExit set_flag_on_exit(&stop);
      auto conn = ASSERT_RESULT(Connect());
      char value[2] = "0";
      while (!stop.load(std::memory_order_acquire)) {
        int batch_size = RandomUniformInt(2, kMaxBatchSize);
        TransactionInfo transaction_info;
        transaction_info.batch_size = batch_size;
        bool ok = false;
        if (batch_size != 1) {
          ASSERT_OK(Execute(conn.get(), "START TRANSACTION ISOLATION LEVEL SERIALIZABLE"));
        }
        auto se = ScopeExit([&conn, batch_size, &ok, &processed, &helper, &transaction_info] {
          if (batch_size != 1) {
            if (ok) {
              auto status = Execute(conn.get(), "COMMIT");
              if (status.ok()) {
                ++processed;
                helper.Committed(std::move(transaction_info));
                return;
              }
              auto msg = status.message().ToBuffer();
              if (msg.find("Transaction expired") == std::string::npos) {
                ASSERT_OK(status);
              }
            }
            ASSERT_OK(Execute(conn.get(), "ROLLBACK"));
          } else if (ok) {
            // To re-enable this we need to decrease the lower bound of batch_size to 1.
            ++processed;
          }
        });
        ok = true;
        for (int j = 0; j != batch_size; ++j) {
          auto key_and_appended_char = helper.RandomPair();
          Status status;
          auto& current_batch = transaction_info.batches[j];
          current_batch.key = key_and_appended_char.first;
          current_batch.append_char = key_and_appended_char.second;
          if (key_and_appended_char.second) {
            value[0] = key_and_appended_char.second;
            status = Execute(
                conn.get(),
                Format(
                    "INSERT INTO test (k, v) VALUES ($0, '$1') ON CONFLICT (K) DO "
                    "UPDATE SET v = CONCAT(test.v, '$1')",
                    key_and_appended_char.first,
                    value));
          } else {
            auto result = Fetch(
                conn.get(), Format("SELECT v FROM test WHERE k = $0", key_and_appended_char.first));
            if (!result.ok()) {
              status = result.status();
            } else {
              auto tuples = PQntuples(result->get());
              if (tuples == 1) {
                ASSERT_EQ(PQnfields(result->get()), 1);
                current_batch.read_value = ASSERT_RESULT(
                    GetString(result->get(), 0, 0));
              } else {
                ASSERT_EQ(tuples, 0);
              }
            }
          }
          if (status.ok()) {
            continue;
          }
          ok = false;
          if (TransactionalFailure(status)) {
            break;
          }
          auto msg = status.message().ToBuffer();
          if (msg.find("Snapshot too old: Snapshot too old.") != std::string::npos ||
              msg.find("Commit of expired transaction") != std::string::npos ||
              msg.find("Catalog Version Mismatch") != std::string::npos ||
              msg.find("Soft memory limit exceeded") != std::string::npos ||
              msg.find("Missing metadata for transaction") != std::string::npos) {
            break;
          }

          ASSERT_OK(status);
        }
      }
    });
  }

  if (!kill_master) {
    thread_holder.WaitAndStop(duration.ToSteadyDuration());
  } else {
    // Every 15 seconds, pick a random master, then kill it if it is running, otherwise resume it.
    auto deadline = CoarseMonoClock::now() + duration;
    int num_masters = cluster_->num_masters();
    while (!thread_holder.stop_flag().load(std::memory_order_acquire)) {
      MonoDelta left(deadline - CoarseMonoClock::now());
      if (left < MonoDelta::kZero) {
        break;
      }
      std::this_thread::sleep_for(std::min(left, MonoDelta(15s)).ToSteadyDuration());
      auto* master = cluster_->master(RandomUniformInt(0, num_masters - 1));
      if (master->IsProcessAlive()) {
        LOG(INFO) << "Killing: " << master->uuid();
        master->Shutdown();
      } else {
        LOG(INFO) << "Resuming: " << master->uuid();
        ASSERT_OK(master->Start());
      }
      int live_masters = 0;
      for (int i = 0; i != num_masters; ++i) {
        if (cluster_->master(i)->IsProcessAlive()) {
          ++live_masters;
        }
      }
      LOG(INFO) << "Live masters: " << live_masters;
    }

    for (int i = 0; i != num_masters; ++i) {
      if (!cluster_->master(i)->IsProcessAlive()) {
        ASSERT_OK(cluster_->master(i)->Start());
      }
    }

    thread_holder.Stop();
  }

  for (;;) {
    auto res = Fetch(conn.get(), "SELECT * FROM test ORDER BY k");
    if (!res.ok()) {
      ASSERT_TRUE(TransactionalFailure(res.status())) << res.status();
      continue;
    }
    int cols = PQnfields(res->get());
    ASSERT_EQ(cols, 2);
    int rows = PQntuples(res->get());
    for (int i = 0; i != rows; ++i) {
      auto key = GetInt32(res->get(), i, 0);
      auto value = GetString(res->get(), i, 1);
      LOG(INFO) << "  " << key << ": " << value;
    }
    LOG(INFO) << "Total processed: " << processed.load(std::memory_order_acquire);
    break;
  }

  helper.Report();
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(OnConflict)) {
  TestOnConflict(false /* kill_master */, 120s);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(OnConflictWithKillMaster)) {
  TestOnConflict(true /* kill_master */, 180s);
}

TEST_F(PgLibPqTest, YB_DISABLE_TEST_IN_TSAN(NoTxnOnConflict)) {
  constexpr int kWriters = 5;
  constexpr int kKeys = 20;
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(Execute(conn.get(), "CREATE TABLE test (k int PRIMARY KEY, v TEXT)"));

  TestThreadHolder thread_holder;
  for (int i = 0; i != kWriters; ++i) {
    thread_holder.AddThreadFunctor([this, &stop = thread_holder.stop_flag()] {
      SetFlagOnExit set_flag_on_exit(&stop);
      auto conn = ASSERT_RESULT(Connect());
      char value[2] = "0";
      while (!stop.load(std::memory_order_acquire)) {
        int key = RandomUniformInt(1, kKeys);
        value[0] = RandomUniformInt('A', 'Z');
        auto status = Execute(
            conn.get(),
            Format(
                "INSERT INTO test (k, v) VALUES ($0, '$1') ON CONFLICT (K) DO "
                "UPDATE SET v = CONCAT(test.v, '$1')",
                key,
                value));
        if (status.ok() || TransactionalFailure(status)) {
          continue;
        }
        ASSERT_OK(status);
      }
    });
  }

  thread_holder.WaitAndStop(30s);
  LogResult(ASSERT_RESULT(Fetch(conn.get(), "SELECT * FROM test ORDER BY k")).get());
}

} // namespace pgwrapper
} // namespace yb
