// demo3.cpp
// 模拟RocksDB中 StackableDB -> TransactionDB -> PessimisticTransactionDB 的继承关系
// 证明无法在派生类初始化列表中直接初始化基类成员

#include <iostream>
#include <string>
#include <map>

// 模拟基类 DB
class DB {
public:
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual std::string get(const std::string& key) = 0;
    virtual ~DB() = default;
};

// 模拟 StackableDB
class StackableDB : public DB {
public:
    // StackableDB 构造函数，负责初始化 db_
    StackableDB(DB* db) : db_(db) {
        std::cout << "StackableDB constructor called, db_ = " << db_ << std::endl;
    }

    // 实现DB接口 - 简单转发
    void put(const std::string& key, const std::string& value) override {
        if (db_) db_->put(key, value);
    }

    std::string get(const std::string& key) override {
        return db_ ? db_->get(key) : "No DB available";
    }

    ~StackableDB() override {
        std::cout << "StackableDB destructor" << std::endl;
    }

protected:
    DB* db_; // 被包装的DB对象
};

// 模拟 TransactionDB
class TransactionDB : public StackableDB {
public:
    // TransactionDB 构造函数，调用基类StackableDB的构造函数
    TransactionDB(DB* db) : StackableDB(db) {
        std::cout << "TransactionDB constructor called" << std::endl;
    }

    void beginTransaction() {
        std::cout << "Transaction begun" << std::endl;
    }

    ~TransactionDB() override {
        std::cout << "TransactionDB destructor" << std::endl;
    }
};

// 模拟具体的DB实现
class MockDB : public DB {
public:
    void put(const std::string& key, const std::string& value) override {
        std::cout << "MockDB: Putting " << key << " = " << value << std::endl;
        data[key] = value;
    }

    std::string get(const std::string& key) override {
        std::cout << "MockDB: Getting " << key << std::endl;
        return data.count(key) ? data[key] : "Not found";
    }

private:
    std::map<std::string, std::string> data;
};

// 正确的PessimisticTransactionDB实现
class PessimisticTransactionDBCorrect : public TransactionDB {
public:
    // ✅ 正确的方式：通过基类构造函数链初始化
    PessimisticTransactionDBCorrect(DB* db, int lock_timeout) 
        : TransactionDB(db), // 调用直接基类的构造函数
          lock_timeout_(lock_timeout) {
        std::cout << "PessimisticTransactionDBCorrect constructor, lock_timeout = " 
                  << lock_timeout_ << std::endl;
        std::cout << "db_ points to: " << db_ << std::endl;
    }

    void pessimisticLock(const std::string& key) {
        std::cout << "Acquiring pessimistic lock on " << key 
                  << " with timeout " << lock_timeout_ << "ms" << std::endl;
    }

private:
    int lock_timeout_;
};

// 错误的PessimisticTransactionDB实现 - 尝试直接初始化基类成员
class PessimisticTransactionDBError : public TransactionDB {
public:
    // ❌ 错误的方式：尝试直接初始化基类成员db_
    PessimisticTransactionDBError(DB* db, int lock_timeout) 
        : db_(db), // 编译错误：db_不是PessimisticTransactionDBError的直接成员
          lock_timeout_(lock_timeout) {
        std::cout << "This line will never be reached due to compilation error" << std::endl;
    }

private:
    int lock_timeout_;
};

int main() {
    std::cout << "=== Demonstrating Correct Inheritance Pattern ===" << std::endl;
    
    // 创建底层DB
    MockDB* mock_db = new MockDB();
    std::cout << "MockDB address: " << mock_db << std::endl;
    
    // 正确的方式：通过构造函数链
    PessimisticTransactionDBCorrect txn_db_correct(mock_db, 5000);
    txn_db_correct.put("test_key", "test_value");
    txn_db_correct.pessimisticLock("test_key");
    
    std::cout << "\n=== The following code would cause compilation error ===" << std::endl;
    std::cout << "// PessimisticTransactionDBError txn_db_error(mock_db, 5000);" << std::endl;
    std::cout << "// Error: class 'PessimisticTransactionDBError' does not have any field named 'db_'" << std::endl;

    delete mock_db;
    return 0;
}