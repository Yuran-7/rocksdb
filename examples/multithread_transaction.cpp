#include "rocksdb/db.h"
#include "rocksdb/utilities/transaction_db.h"
#include <thread>
#include <iostream>

using namespace rocksdb;

// 工作线程函数：模拟事务操作
void transfer_task(TransactionDB* txn_db, int from_id, int to_id, int amount, int thread_id) {
  Status s;
  for (int i = 0; i < 5; ++i) { // 尝试多次
    // 每个线程创建自己的事务
    Transaction* txn = txn_db->BeginTransaction(WriteOptions());
    assert(txn);

    std::string from_key = "account_" + std::to_string(from_id);
    std::string to_key = "account_" + std::to_string(to_id);
    std::string from_value, to_value;
    int from_balance, to_balance;

    // 1. 读取from的余额 (带锁)
    s = txn->GetForUpdate(ReadOptions(), from_key, &from_value);
    if (s.ok()) from_balance = std::stoi(from_value);
    else break;

    // 2. 读取to的余额 (带锁)
    s = txn->GetForUpdate(ReadOptions(), to_key, &to_value);
    if (s.ok()) to_balance = std::stoi(to_value);
    else break;

    // 3. 检查并更新余额
    if (from_balance >= amount) {
      from_balance -= amount;
      to_balance += amount;

      s = txn->Put(from_key, std::to_string(from_balance));
      if (!s.ok()) break;
      s = txn->Put(to_key, std::to_string(to_balance));
      if (!s.ok()) break;

      // 4. 提交事务！
      s = txn->Commit();
      if (s.ok()) {
        std::cout << "Thread " << thread_id << ": Transfer successful!" << std::endl;
        delete txn;
        return; // 成功则退出
      } else {
        std::cout << "Thread " << thread_id << ": Commit failed. Retrying... " << s.ToString() << std::endl;
      }
    } else {
      std::cout << "Thread " << thread_id << ": Insufficient balance." << std::endl;
      txn->Rollback();
      delete txn;
      return;
    }

    // 5. 如果提交失败，回滚并重试
    delete txn;
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 避免活锁
  }
  std::cout << "Thread " << thread_id << ": Failed after retries." << std::endl;
}

int main() {
  Options options;
  TransactionDBOptions txn_db_options;
  options.create_if_missing = true;
  TransactionDB* txn_db;

  // 1. 主线程打开数据库
  Status s = TransactionDB::Open(options, txn_db_options, "/tmp/test_db", &txn_db);
  assert(s.ok());

  // 2. 初始化一些测试数据
  WriteOptions wopts;
  txn_db->Put(wopts, "account_1", "1000");
  txn_db->Put(wopts, "account_2", "500");

  // 3. 创建多个线程模拟并发事务
  std::thread t1(transfer_task, txn_db, 1, 2, 200, 1); // 线程1：从1转200到2
  std::thread t2(transfer_task, txn_db, 1, 2, 300, 2); // 线程2：从1转300到2

  // 4. 等待所有线程结束
  t1.join();
  t2.join();

  // 5. 检查最终结果
  ReadOptions ropts;
  std::string final_value;
  txn_db->Get(ropts, "account_1", &final_value);
  std::cout << "Final balance of account_1: " << final_value << std::endl;
  txn_db->Get(ropts, "account_2", &final_value);
  std::cout << "Final balance of account_2: " << final_value << std::endl;

  delete txn_db;
  return 0;
}

/*
g++ -g3 -O0 -I./include examples/multithread_transaction.cpp librocksdb_debug.a -lz -lbz2 -lsnappy -llz4 -lzstd -lpthread -ldl -o examples/multithread_transaction
*/