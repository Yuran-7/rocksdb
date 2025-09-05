#include <iostream>
#include <charconv>
#include <memory>
#include <vector>


#include "faiss/IndexFlat.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/utils/random.h"
#include "rocksdb/utilities/secondary_index_faiss.h"
#include "rocksdb/utilities/transaction_db.h"
#include "util/coding.h"


using namespace ROCKSDB_NAMESPACE;

int main() {
  constexpr size_t dim = 128;
  auto quantizer_cmp = std::make_unique<faiss::IndexFlatL2>(dim);
  auto quantizer = std::make_unique<faiss::IndexFlatL2>(dim);

  constexpr size_t num_lists = 16;
  auto index_cmp = std::make_unique<faiss::IndexIVFFlat>(quantizer_cmp.get(), dim, num_lists);
  auto index = std::make_unique<faiss::IndexIVFFlat>(quantizer.get(), dim, num_lists);

  {
    constexpr faiss::idx_t num_train = 1024;
    std::vector<float> embeddings_train(dim * num_train);
    faiss::float_rand(embeddings_train.data(), dim * num_train, 42);

    index_cmp->train(num_train, embeddings_train.data());
    index->train(num_train, embeddings_train.data());
  }

  auto faiss_ivf_index = std::make_shared<FaissIVFIndex>(
      std::move(index), kDefaultWideColumnName.ToString());

  const std::string db_name = "/home/ysh/LSM/rocksdb/examples/faiss_search_db";
  DestroyDB(db_name, Options());

  Options options;
  options.create_if_missing = true;

  TransactionDBOptions txn_db_options;
  txn_db_options.secondary_indices.emplace_back(faiss_ivf_index);

  TransactionDB* db = nullptr;
  Status s = TransactionDB::Open(options, txn_db_options, db_name, &db);
  if (!s.ok()) {
    std::cerr << "DB open failed: " << s.ToString() << "\n";
    return 1;
  }
  std::unique_ptr<TransactionDB> db_guard(db);

  ColumnFamilyHandle* cfh1 = nullptr;
  s = db->CreateColumnFamily(ColumnFamilyOptions(), "cf1", &cfh1);
  if (!s.ok()) { std::cerr << "Create CF1 failed\n"; return 1; }
  std::unique_ptr<ColumnFamilyHandle> cfh1_guard(cfh1);

  ColumnFamilyHandle* cfh2 = nullptr;
  s = db->CreateColumnFamily(ColumnFamilyOptions(), "cf2", &cfh2);
  if (!s.ok()) { std::cerr << "Create CF2 failed\n"; return 1; }
  std::unique_ptr<ColumnFamilyHandle> cfh2_guard(cfh2);

  const auto& secondary_index = txn_db_options.secondary_indices.back();
  secondary_index->SetPrimaryColumnFamily(cfh1);
  secondary_index->SetSecondaryColumnFamily(cfh2);

  constexpr faiss::idx_t num_db = 1000000;
  {
    std::vector<float> embeddings_db(dim * num_db);
    faiss::float_rand(embeddings_db.data(), dim * num_db, 123);

    auto start_time = std::chrono::high_resolution_clock::now();
    for (faiss::idx_t i = 0; i < num_db; ++i) {
      const float* const embedding = embeddings_db.data() + i * dim;
      index_cmp->add(1, embedding);
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "Pure Faiss: " << duration.count() << " 毫秒\n";

    start_time = std::chrono::high_resolution_clock::now();
    for (faiss::idx_t i = 0; i < num_db; ++i) {
      const float* const embedding = embeddings_db.data() + i * dim;

      const std::string primary_key = std::to_string(i);
      s = db->Put(WriteOptions(), cfh1, primary_key,
                  ConvertFloatsToSlice(embedding, dim));
      if (!s.ok()) {
        std::cerr << "Put failed at i=" << i << "\n";
        return 1;
      }
    }
    end_time = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "RocksDB + Faiss: " << duration.count() << " 毫秒\n";    
  }

  constexpr faiss::idx_t num_query = 32;
  std::vector<float> embeddings_query(dim * num_query);
  faiss::float_rand(embeddings_query.data(), dim * num_query, 456);

  std::unique_ptr<Iterator> underlying_it(db->NewIterator(ReadOptions(), cfh2));
  auto secondary_it = std::make_unique<SecondaryIndexIterator>(
      faiss_ivf_index.get(), std::move(underlying_it));

  auto get_id = [](const Slice& key) -> faiss::idx_t {
    faiss::idx_t id = -1;
    if (std::from_chars(key.data(), key.data() + key.size(), id).ec != std::errc()) {
      return -1;
    }
    return id;
  };

  for (size_t neighbors : {1, 2, 4}) {
    for (size_t probes : {1, 2, 4}) {
      for (faiss::idx_t i = 0; i < num_query; ++i) {
        const float* const embedding = embeddings_query.data() + i * dim;

        std::vector<float> distances(neighbors, 0.0f);
        std::vector<faiss::idx_t> ids(neighbors, -1);

        faiss::SearchParametersIVF params;
        params.nprobe = probes;

        index_cmp->search(1, embedding, neighbors, distances.data(), ids.data(), &params);

        size_t result_size_cmp = 0;
        for (faiss::idx_t id_cmp : ids) {
          if (id_cmp < 0) break;
          ++result_size_cmp;
        }

        std::vector<std::pair<std::string, float>> result;
        s = faiss_ivf_index->FindKNearestNeighbors(
            secondary_it.get(), ConvertFloatsToSlice(embedding, dim),
            neighbors, probes, &result);
        if (!s.ok()) {
          std::cerr << "FindKNN failed: " << s.ToString() << "\n";
          return 1;
        }

        if (result.size() != result_size_cmp) {
          std::cerr << "Mismatch result size at query " << i
                    << " neighbors=" << neighbors
                    << " probes=" << probes << "\n";
          return 1;
        }

        for (size_t j = 0; j < result.size(); ++j) {
          const faiss::idx_t id = get_id(result[j].first);
          if (id < 0 || id >= num_db) {
            std::cerr << "Invalid ID returned\n";
            return 1;
          }
          if (id != ids[j]) {
            std::cerr << "ID mismatch: " << id << " vs " << ids[j] << "\n";
            return 1;
          }
          if (result[j].second != distances[j]) {
            std::cerr << "Distance mismatch: " << result[j].second
                      << " vs " << distances[j] << "\n";
            return 1;
          }
        }
      }
    }
  }

  std::cout << "All checks passed!\n";
  return 0;
}


/*
编译命令:

g++ -g3 -O0  -fno-omit-frame-pointer -std=c++17 \
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
    -llapack -lblas  \
    -lpthread -ldl \
    -o examples/faiss_search
*/