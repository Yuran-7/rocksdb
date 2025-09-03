#include <iostream>
using namespace std;

// 模拟 RocksDB 的 TransactionBaseImpl
class TransactionBaseImpl {
public:
    void SetSavePoint() {
        cout << "TransactionBaseImpl::SetSavePoint() called" << endl;
    }
};

// 模拟 RocksDB 的 WriteCommittedTxnDB
// 实际上它继承自 TransactionBaseImpl
class WriteCommittedTxnDB : public TransactionBaseImpl {
    // 注意这里没有重新声明 SetSavePoint
};

// SecondaryIndexMixin 模板类
template <typename Txn>
class SecondaryIndexMixin: public Txn {
public:
    void PerformWithSavePoint() {
        // ✅ 正确写法：加上 ::
        Txn::SetSavePoint();

        // ❌ 错误写法：不加 ::
        // 如果你把上面一行注释掉，改成下面这行，就会报错
        // SetSavePoint();
    }
};

int main() {
    SecondaryIndexMixin<WriteCommittedTxnDB> mixin;
    mixin.PerformWithSavePoint();
    return 0;
}
