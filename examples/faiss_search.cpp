#include <charconv>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <random>

#include "faiss/IndexFlat.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/utils/random.h"
#include "rocksdb/utilities/secondary_index_faiss.h"
#include "rocksdb/utilities/transaction_db.h"
#include "util/coding.h"

using namespace rocksdb;

int main(int argc, char* argv[]) {
    std::string db_path = "/home/ysh/LSM/rocksdb/examples/faiss_db";
    constexpr size_t dim = 128;  // 向量维度
    constexpr size_t num_lists = 16;  // IVF 索引的聚类中心数量
    const std::string primary_column_name = "embedding";

    // 检查数据库是否存在
    if (!std::filesystem::exists(db_path)) {
        std::cerr << "Database does not exist at " << db_path << std::endl;
        std::cerr << "Please run faiss_runner first to create the database." << std::endl;
        return -1;
    }

    std::cout << "Loading existing FAISS database..." << std::endl;

    // 重新生成与 faiss_runner 相同的原始向量数据
    constexpr faiss::idx_t num_vectors = 1000000;
    std::vector<float> embeddings(dim * num_vectors);
    faiss::float_rand(embeddings.data(), dim * num_vectors, 42);

    // 创建并训练 FAISS 索引（与 faiss_runner 中相同的配置）
    auto quantizer = std::make_unique<faiss::IndexFlatL2>(dim);
    auto index = std::make_unique<faiss::IndexIVFFlat>(quantizer.get(), dim, num_lists);
    
    std::cout << "Training FAISS index..." << std::endl;
    auto train_start = std::chrono::high_resolution_clock::now();
    index->train(num_vectors, embeddings.data());
    auto train_end = std::chrono::high_resolution_clock::now();
    auto train_duration = std::chrono::duration_cast<std::chrono::milliseconds>(train_end - train_start);
    std::cout << "Training time: " << train_duration.count() << " ms" << std::endl;
    
    // 创建 RocksDB 的 FAISS IVF 索引包装器
    auto faiss_ivf_index = std::make_shared<FaissIVFIndex>(std::move(index), primary_column_name);
    
    // 打开现有数据库
    std::vector<std::string> cf_names;
    Options options;
    DB::ListColumnFamilies(options, db_path, &cf_names);
    
    std::vector<ColumnFamilyDescriptor> cf_descriptors;
    for (const auto& name : cf_names) {
        cf_descriptors.emplace_back(name, ColumnFamilyOptions());
    }
    
    TransactionDBOptions txn_db_options;
    txn_db_options.secondary_indices.emplace_back(faiss_ivf_index);

    std::vector<ColumnFamilyHandle*> cf_handles;
    TransactionDB* db = nullptr;
    Status s = TransactionDB::Open(DBOptions(options), txn_db_options, db_path, cf_descriptors, &cf_handles, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open database: " << s.ToString() << std::endl;
        return -1;
    }
    std::unique_ptr<TransactionDB> db_guard(db);

    // 找到 cf1 和 cf2
    ColumnFamilyHandle* cfh1 = nullptr;
    ColumnFamilyHandle* cfh2 = nullptr;
    
    for (size_t i = 0; i < cf_handles.size(); ++i) {
        if (cf_handles[i]->GetName() == "cf1") {
            cfh1 = cf_handles[i];
        } else if (cf_handles[i]->GetName() == "cf2") {
            cfh2 = cf_handles[i];
        }
    }

    if (!cfh1 || !cfh2) {
        std::cerr << "Could not find cf1 or cf2 column families" << std::endl;
        return -1;
    }

    // 设置二级索引的列族
    faiss_ivf_index->SetPrimaryColumnFamily(cfh1);
    faiss_ivf_index->SetSecondaryColumnFamily(cfh2);

    std::cout << "Database opened successfully" << std::endl;

    // 从数据库中读取一些键用于查询测试
    std::cout << "Loading sample keys for queries..." << std::endl;
    std::vector<std::vector<float>> sample_vectors;
    std::vector<std::string> sample_keys;
    
    {
        std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions(), cfh1));
        size_t count = 0;
        for (it->SeekToFirst(); it->Valid() && count < 1000; it->Next(), ++count) {
            Slice key = it->key();
            sample_keys.emplace_back(key.ToString());
            
            // 从键解析出ID，然后使用对应的原始向量
            faiss::idx_t id = -1;
            if (std::from_chars(key.data(), key.data() + key.size(), id).ec == std::errc() && 
                id >= 0 && id < num_vectors) {
                sample_vectors.emplace_back(embeddings.data() + id * dim, embeddings.data() + (id + 1) * dim);
            }
        }
    }
    
    std::cout << "Loaded " << sample_vectors.size() << " sample vectors" << std::endl;

    // 创建二级索引迭代器
    std::unique_ptr<Iterator> underlying_it(db->NewIterator(ReadOptions(), cfh2));
    auto secondary_it = std::make_unique<SecondaryIndexIterator>(
        faiss_ivf_index.get(), std::move(underlying_it));

    // Lambda 函数：从 Slice 格式的 key 中解析出向量 ID
    auto get_id = [](const Slice& key) -> faiss::idx_t {
        faiss::idx_t id = -1;
        if (std::from_chars(key.data(), key.data() + key.size(), id).ec != std::errc()) {
            return -1;
        }
        return id;
    };

    // 性能测试参数
    constexpr size_t neighbors = 10;  // 查询最近的邻居数量
    constexpr size_t num_queries = 100;  // 测试查询数量
    constexpr size_t probes = 4;  // IVF 探测的聚类数量

    std::cout << "\n=== FAISS Search Performance Test ===" << std::endl;
    std::cout << "Query parameters:" << std::endl;
    std::cout << "  Neighbors: " << neighbors << std::endl;
    std::cout << "  Probes: " << probes << std::endl;
    std::cout << "  Test queries: " << num_queries << std::endl;

    // 随机选择查询向量
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, sample_vectors.size() - 1);

    std::vector<double> query_times;
    std::vector<size_t> result_counts;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    for (size_t q = 0; q < num_queries; ++q) {
        size_t query_idx = dis(gen);
        const auto& query_vector = sample_vectors[query_idx];
        const auto& query_key = sample_keys[query_idx];
        
        auto query_start = std::chrono::high_resolution_clock::now();
        
        std::vector<std::pair<std::string, float>> result;
        Status search_status = faiss_ivf_index->FindKNearestNeighbors(
            secondary_it.get(),
            ConvertFloatsToSlice(query_vector.data(), dim),
            neighbors,
            probes,
            &result
        );
        
        auto query_end = std::chrono::high_resolution_clock::now();
        
        if (!search_status.ok()) {
            std::cerr << "Search failed for query " << q << ": " << search_status.ToString() << std::endl;
            continue;
        }
        
        double query_time_ms = std::chrono::duration<double, std::milli>(query_end - query_start).count();
        query_times.push_back(query_time_ms);
        result_counts.push_back(result.size());
        
        // 验证第一个结果（应该是查询向量本身）
        if (!result.empty()) {
            const std::string& first_key = result[0].first;
            if (first_key == query_key && result[0].second < 1e-5) {
                // 验证成功
            } else {
                std::cout << "Warning: Query " << q << " (key=" << query_key 
                         << ") first result key=" << first_key 
                         << " distance=" << result[0].second << std::endl;
            }
        }
        
        // 每10个查询显示进度
        if ((q + 1) % 10 == 0) {
            std::cout << "Completed " << (q + 1) << "/" << num_queries << " queries" << std::endl;
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time = std::chrono::duration<double>(end_time - start_time).count();

    // 计算统计信息
    if (!query_times.empty()) {
        double avg_time = 0.0, min_time = query_times[0], max_time = query_times[0];
        for (double t : query_times) {
            avg_time += t;
            min_time = std::min(min_time, t);
            max_time = std::max(max_time, t);
        }
        avg_time /= query_times.size();

        double avg_results = 0.0;
        for (size_t c : result_counts) {
            avg_results += c;
        }
        avg_results /= result_counts.size();

        std::cout << "\n=== Performance Results ===" << std::endl;
        std::cout << "Total time: " << total_time << " seconds" << std::endl;
        std::cout << "Successful queries: " << query_times.size() << "/" << num_queries << std::endl;
        std::cout << "Query time statistics (ms):" << std::endl;
        std::cout << "  Average: " << avg_time << std::endl;
        std::cout << "  Min: " << min_time << std::endl;
        std::cout << "  Max: " << max_time << std::endl;
        std::cout << "Queries per second: " << query_times.size() / total_time << std::endl;
        std::cout << "Average results per query: " << avg_results << std::endl;
    }

    // 详细测试几个特定的查询
    std::cout << "\n=== Detailed Test Results ===" << std::endl;
    
    auto detailed_test = [&](size_t test_idx, const std::string& description) {
        if (test_idx >= sample_vectors.size()) {
            std::cout << "\nSkipping " << description << " (index out of range)" << std::endl;
            return;
        }
        
        std::cout << "\nTesting " << description << " (Key: " << sample_keys[test_idx] << ")" << std::endl;
        
        auto query_start = std::chrono::high_resolution_clock::now();
        std::vector<std::pair<std::string, float>> result;
        Status search_status = faiss_ivf_index->FindKNearestNeighbors(
            secondary_it.get(),
            ConvertFloatsToSlice(sample_vectors[test_idx].data(), dim),
            neighbors,
            probes,
            &result
        );
        auto query_end = std::chrono::high_resolution_clock::now();
        
        if (!search_status.ok()) {
            std::cout << "  Search failed: " << search_status.ToString() << std::endl;
            return;
        }
        
        double query_time = std::chrono::duration<double, std::milli>(query_end - query_start).count();
        std::cout << "  Query time: " << query_time << " ms" << std::endl;
        std::cout << "  Results found: " << result.size() << std::endl;
        
        std::cout << "  Top results:" << std::endl;
        for (size_t i = 0; i < std::min(size_t(5), result.size()); ++i) {
            std::cout << "    " << i+1 << ". Key: " << result[i].first 
                     << ", Distance: " << result[i].second << std::endl;
        }
    };

    detailed_test(0, "first vector");
    if (sample_vectors.size() > 1) {
        detailed_test(sample_vectors.size() / 2, "middle vector");
        detailed_test(sample_vectors.size() - 1, "last vector");
    }

    // 清理列族句柄
    for (auto* cfh : cf_handles) {
        if (cfh != db->DefaultColumnFamily()) {
            delete cfh;
        }
    }

    std::cout << "\nFAISS search test completed successfully!" << std::endl;
    return 0;
}

/*
编译命令:

g++ -g3 -O0 -fno-omit-frame-pointer -std=c++17 \
    -I./include \
    -I. \
    -I/home/ysh/faiss \
    -DROCKSDB_PLATFORM_POSIX \
    -DOS_LINUX \
    examples/faiss_search.cpp \
    utilities/secondary_index/faiss_ivf_index.cc \
    librocksdb_debug.a \
    /home/ysh/faiss/build/faiss/libfaiss.so \
    -Wl,-rpath=/home/ysh/faiss/build/faiss \
    -lz -lbz2 -lsnappy -llz4 -lzstd -lnuma -ltbb \
    -llapack -lblas \
    -lpthread -ldl \
    -o examples/faiss_search

运行方法:
1. 首先运行 faiss_runner 创建数据库
2. 然后运行 faiss_search 进行查询性能测试
*/