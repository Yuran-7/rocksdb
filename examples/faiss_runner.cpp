#include <charconv>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <filesystem>

#include "faiss/IndexFlat.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/utils/random.h"
#include "rocksdb/utilities/secondary_index_faiss.h"
#include "rocksdb/utilities/transaction_db.h"

using namespace rocksdb;

int main(int argc, char* argv[]) {

    std::string db_path = "/home/ysh/LSM/rocksdb/examples/faiss_db";
    constexpr size_t dim = 128;  // 向量维度
    // 创建量化器：IndexFlatL2 是一个扁平索引，使用 L2 距离计算相似度
    auto quantizer = std::make_unique<faiss::IndexFlatL2>(dim);

    constexpr size_t num_lists = 16;  // IVF 索引的聚类中心数量
    // 创建 IVF 索引：IndexIVFFlat 是倒排文件索引，将向量空间分为多个区域
    auto index = std::make_unique<faiss::IndexIVFFlat>(quantizer.get(), dim, num_lists);

    constexpr faiss::idx_t num_vectors = 1000;  // 测试向量数量
    std::vector<float> embeddings(dim * num_vectors);  // 存储所有向量的数组
    faiss::float_rand(embeddings.data(), dim * num_vectors, 42);  // 42是种子，生成随机向量数据

    // 对于 RocksDB 集成的 FAISS IVF 索引，训练完成后就可以接收向量数据并建立倒排索引结构了。
    // train() 方法使用输入向量训练索引，确定聚类中心
    auto start_time = std::chrono::high_resolution_clock::now();
    index->train(num_vectors, embeddings.data());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "Training time: " << duration.count() << " 毫秒\n";

    const std::string primary_column_name = "embedding";  // 主列族中存储向量的列名
    // 创建 RocksDB 的 FAISS IVF 索引包装器
    auto faiss_ivf_index = std::make_shared<FaissIVFIndex>(std::move(index), primary_column_name);
    
    // 检查数据库是否存在，如果存在就删除
    if (std::filesystem::exists(db_path)) {
        std::cout << "Database exists at " << db_path << ", deleting it...\n";
        try {
            std::filesystem::remove_all(db_path);
            std::cout << "Database deleted successfully.\n";
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error deleting database: " << e.what() << std::endl;
            return -1;
        }
    }
    
    Options options;
    options.create_if_missing = true;  // 如果数据库不存在则创建

    TransactionDBOptions txn_db_options;
    txn_db_options.secondary_indices.emplace_back(faiss_ivf_index); // 可以有多个二级索引，这里添加 FAISS 索引

    TransactionDB* db = nullptr;
    TransactionDB::Open(options, txn_db_options, db_path, &db); // 最终db应该是WriteCommittedTxnDB类型的
    std::unique_ptr<TransactionDB> db_guard(db);

    ColumnFamilyOptions cf1_opts;
    ColumnFamilyHandle* cfh1 = nullptr;
    db->CreateColumnFamily(cf1_opts, "cf1", &cfh1);
    std::unique_ptr<ColumnFamilyHandle> cfh1_guard(cfh1);

    ColumnFamilyOptions cf2_opts;
    ColumnFamilyHandle* cfh2 = nullptr;
    db->CreateColumnFamily(cf2_opts, "cf2", &cfh2);
    std::unique_ptr<ColumnFamilyHandle> cfh2_guard(cfh2);

    const auto& secondary_index = txn_db_options.secondary_indices.back();

    secondary_index->SetPrimaryColumnFamily(cfh1);    // 主数据存储在 cf1
    secondary_index->SetSecondaryColumnFamily(cfh2);  // 索引数据存储在 cf2

    {
        start_time = std::chrono::high_resolution_clock::now();
        // 在 TransactionDB 的基类声明的BeginTransaction函数有三个参数，其中两个有默认参数，所以这里只需要填一个参数即可
        // 此时txn是SecondaryIndexMixin<WriteCommittedTxn>类型的，SecondaryIndexMixin继承WriteCommittedTxn
        std::unique_ptr<Transaction> txn(db->BeginTransaction(WriteOptions()));

        for (faiss::idx_t i = 0; i < num_vectors; ++i) {
            const std::string primary_key = std::to_string(i);  // 主键为数字字符串
            WideColumns w = {
                {primary_column_name,
                 ConvertFloatsToSlice(embeddings.data() + i * dim, dim)}};
            // PutEntity在最早的基类Transaction中就是纯虚函数，后续经过很多个子类的重写，最后是SecondaryIndexMixin类的实现
            // SecondaryIndexMixin -> WriteCommittedTxn -> 
            txn->PutEntity(cfh1, primary_key, std::move(w));
        }

        txn->Commit();
        end_time = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "Put time: " << duration.count()/1000.0 << " 秒\n";

    }

}

/*
编译

g++ -g3 -O0  -fno-omit-frame-pointer -std=c++17 \
    -I./include \
    -I. \
    -I/home/ysh/faiss \
    -DROCKSDB_PLATFORM_POSIX \
    -DOS_LINUX \
    examples/faiss_runner.cpp \
    utilities/secondary_index/faiss_ivf_index.cc \
    librocksdb_debug.a \
    /home/ysh/faiss/build/faiss/libfaiss.so \
    -Wl,-rpath=/home/ysh/faiss/build/faiss \
    -lz -lbz2 -lsnappy -llz4 -lzstd -lnuma -ltbb \
    -llapack -lblas  \
    -lpthread -ldl \
    -o examples/faiss_runner
*/