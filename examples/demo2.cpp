#include <iostream>
#include <string>

struct WriteOptions {};
struct WriteBatch {};
struct TransactionDBWriteOptimizations {};

class DB {
public:
    virtual int Write(const WriteOptions&, WriteBatch*) {
        std::cout << "DB::Write(opts, batch)" << std::endl;
        return 0;
    }
};

class StackableDB : public DB {
public:
    int Write(const WriteOptions& opts, WriteBatch* batch) override {
        std::cout << "StackableDB::Write" << std::endl;
        return DB::Write(opts, batch);
    }
};

// 注意：这里没有 using StackableDB::Write;
class TransactionDB : public StackableDB {
public:
    // using StackableDB::Write;
    int Write(const WriteOptions& opts,
              const TransactionDBWriteOptimizations&,
              WriteBatch* batch) {
        std::cout << "TransactionDB::Write with optimizations" << std::endl;
        Write(opts, batch);  // 调用 StackableDB::Write
        return 0;
    }

    int Write1(const WriteOptions& opts, WriteBatch* batch) {
        std::cout << "TransactionDB::Write(opts, batch)" << std::endl;
        return StackableDB::Write(opts, batch);  // 显式调用 StackableDB::Write
    }
};

int main() {
    TransactionDB* txn_db = new TransactionDB();

    WriteOptions opts;
    WriteBatch batch;
    TransactionDBWriteOptimizations optim;

    // ✅ 这个能编译
    txn_db->Write(opts, optim, &batch);

    // ❌ 这个会报错（甚至不用等编译，IDE就报错了），因为 TransactionDB 里定义了新的 Write，
    //    把 StackableDB::Write 隐藏掉了
    txn_db->Write(opts, &batch);

    delete txn_db;
    return 0;
}
