//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <charconv>
#include <memory>
#include <string>
#include <vector>

#include "faiss/IndexFlat.h"
#include "faiss/IndexIVFFlat.h"
#include "faiss/utils/random.h"
#include "rocksdb/utilities/secondary_index_faiss.h"
#include "rocksdb/utilities/transaction_db.h"
#include "test_util/testharness.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

// FAISS IVF 索引的基本功能测试
TEST(FaissIVFIndexTest, Basic) {
  constexpr size_t dim = 128;  // 向量维度
  // 创建量化器：IndexFlatL2 是一个扁平索引，使用 L2 距离计算相似度
  auto quantizer = std::make_unique<faiss::IndexFlatL2>(dim);

  constexpr size_t num_lists = 16;  // IVF 索引的聚类中心数量
  // 创建 IVF 索引：IndexIVFFlat 是倒排文件索引，将向量空间分为多个区域
  auto index =
      std::make_unique<faiss::IndexIVFFlat>(quantizer.get(), dim, num_lists);

  constexpr faiss::idx_t num_vectors = 1024;  // 测试向量数量
  std::vector<float> embeddings(dim * num_vectors);  // 存储所有向量的数组
  faiss::float_rand(embeddings.data(), dim * num_vectors, 42);  // 42是种子，生成随机向量数据

  // 对于 RocksDB 集成的 FAISS IVF 索引，训练完成后就可以接收向量数据并建立倒排索引结构了。
  // train() 方法使用输入向量训练索引，确定聚类中心
  index->train(num_vectors, embeddings.data());

  const std::string primary_column_name = "embedding";  // 主列族中存储向量的列名
  // 创建 RocksDB 的 FAISS IVF 索引包装器
  auto faiss_ivf_index =
      std::make_shared<FaissIVFIndex>(std::move(index), primary_column_name);

  // 获取测试数据库路径
  const std::string db_name = test::PerThreadDBPath("faiss_ivf_index_test");
  // 销毁可能存在的旧数据库
  EXPECT_OK(DestroyDB(db_name, Options()));

  Options options;
  options.create_if_missing = true;  // 如果数据库不存在则创建

  TransactionDBOptions txn_db_options;
  txn_db_options.secondary_indices.emplace_back(faiss_ivf_index); // 可以有多个二级索引，这里添加 FAISS 索引

  TransactionDB* db = nullptr;
  // 打开事务数据库，支持二级索引
  ASSERT_OK(TransactionDB::Open(options, txn_db_options, db_name, &db));
  // 创建一个名为 db_guard 的 std::unique_ptr 智能指针对象，并让它接管（获得所有权）原始指针 db所指向的那个 TransactionDB 对象。
  std::unique_ptr<TransactionDB> db_guard(db);

  ColumnFamilyOptions cf1_opts;
  ColumnFamilyHandle* cfh1 = nullptr;
  // 创建列族 cf1，用于存储主数据（向量数据）
  ASSERT_OK(db->CreateColumnFamily(cf1_opts, "cf1", &cfh1));
  std::unique_ptr<ColumnFamilyHandle> cfh1_guard(cfh1);

  ColumnFamilyOptions cf2_opts;
  ColumnFamilyHandle* cfh2 = nullptr;
  // 创建列族 cf2，用于存储二级索引数据
  ASSERT_OK(db->CreateColumnFamily(cf2_opts, "cf2", &cfh2));
  std::unique_ptr<ColumnFamilyHandle> cfh2_guard(cfh2);

  const auto& secondary_index = txn_db_options.secondary_indices.back();  // 获取vector容器的最后一个元素
  // 设置二级索引的主列族和辅助列族
  secondary_index->SetPrimaryColumnFamily(cfh1);    // 主数据存储在 cf1
  secondary_index->SetSecondaryColumnFamily(cfh2);  // 索引数据存储在 cf2

  // Write the embeddings to the primary column family, indexing them in the
  // process
  // 将向量数据写入主列族，同时构建二级索引
  {
    // 多态写法
    // return new SecondaryIndexMixin<WriteCommittedTxn>(...);
    // 开始一个事务，所有写操作都在事务中进行
    std::unique_ptr<Transaction> txn(db->BeginTransaction(WriteOptions()));

    for (faiss::idx_t i = 0; i < num_vectors; ++i) {
      const std::string primary_key = std::to_string(i);  // 主键为数字字符串

      // PutEntity: 存储宽列实体，这里存储向量数据到指定的列
      ASSERT_OK(txn->PutEntity(
          cfh1, primary_key,
          WideColumns{
              {primary_column_name,
               ConvertFloatsToSlice(embeddings.data() + i * dim, dim)}}));  // 将浮点数组转换为 Slice
    }

    // 提交事务，确保所有数据写入数据库
    ASSERT_OK(txn->Commit());
  }

  // Verify the raw index data in the secondary column family
  // 验证辅助列族中的原始索引数据
  {
    size_t num_found = 0;  // 找到的条目数

    // 创建迭代器遍历辅助列族（存储索引数据）
    std::unique_ptr<Iterator> it(db->NewIterator(ReadOptions(), cfh2));

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      Slice key = it->key();
      faiss::idx_t label = -1;
      // GetVarsignedint64: 从 key 中解析出聚类标签（cluster label）
      ASSERT_TRUE(GetVarsignedint64(&key, &label));
      ASSERT_GE(label, 0);
      ASSERT_LT(label, num_lists);  // 标签应该在有效范围内

      faiss::idx_t id = -1;
      // std::from_chars: 从剩余的 key 中解析出向量 ID
      ASSERT_EQ(std::from_chars(key.data(), key.data() + key.size(), id).ec,
                std::errc());
      ASSERT_GE(id, 0);
      ASSERT_LT(id, num_vectors);

      // Since we use IndexIVFFlat, there is no fine quantization, so the code
      // is actually just the original embedding
      // 因为使用 IndexIVFFlat，没有精细量化，所以存储的就是原始向量
      ASSERT_EQ(it->value(),
                ConvertFloatsToSlice(embeddings.data() + id * dim, dim));

      ++num_found;
    }

    ASSERT_OK(it->status());
    ASSERT_EQ(num_found, num_vectors);  // 确保所有向量都被索引了
  }

  // Query the index with some of the original embeddings
  // 使用原始向量查询索引
  std::unique_ptr<Iterator> underlying_it(db->NewIterator(ReadOptions(), cfh2));
  // 创建二级索引迭代器，用于向量相似性搜索
  auto secondary_it = std::make_unique<SecondaryIndexIterator>(
      faiss_ivf_index.get(), std::move(underlying_it));

  // Lambda 函数：从 Slice 格式的 key 中解析出向量 ID
  auto get_id = [](const Slice& key) -> faiss::idx_t {
    faiss::idx_t id = -1;

    if (std::from_chars(key.data(), key.data() + key.size(), id).ec !=
        std::errc()) {
      return -1;
    }

    return id;
  };

  constexpr size_t neighbors = 8;  // 查询最近的 8 个邻居

  // Lambda 函数：验证给定向量 ID 的搜索结果
  auto verify = [&](faiss::idx_t id) {
    // Search for a vector from the original set; we expect to find the vector
    // itself as the closest match, since we're performing an exhaustive search
    // 搜索原始集合中的向量；由于执行穷尽搜索，我们期望找到向量本身作为最接近的匹配
    std::vector<std::pair<std::string, float>> result;
    // FindKNearestNeighbors: 查找 K 个最近邻居
    ASSERT_OK(faiss_ivf_index->FindKNearestNeighbors(
        secondary_it.get(),
        ConvertFloatsToSlice(embeddings.data() + id * dim, dim), neighbors,
        num_lists, &result));

    ASSERT_EQ(result.size(), neighbors);

    const faiss::idx_t first_id = get_id(result[0].first);

    ASSERT_GE(first_id, 0);
    ASSERT_LT(first_id, num_vectors);
    ASSERT_EQ(first_id, id);  // 第一个结果应该是查询向量本身

    ASSERT_EQ(result[0].second, 0.0f);  // 与自身的距离应该为 0

    // Iterate over the rest of the results
    // 遍历其余结果，验证距离单调递增
    for (size_t i = 1; i < neighbors; ++i) {
      const faiss::idx_t other_id = get_id(result[i].first);
      ASSERT_GE(other_id, 0);
      ASSERT_LT(other_id, num_vectors);
      ASSERT_NE(other_id, id);  // 不应该是查询向量本身

      ASSERT_GE(result[i].second, result[i - 1].second);  // 距离应该单调递增
    }
  };

  // 测试几个不同的向量 ID
  verify(0);
  verify(16);
  verify(32);
  verify(64);

  // Sanity checks
  // 边界条件和错误输入测试
  {
    // No secondary index iterator
    // 测试空的二级索引迭代器
    constexpr SecondaryIndexIterator* bad_secondary_it = nullptr;
    std::vector<std::pair<std::string, float>> result;
    ASSERT_TRUE(faiss_ivf_index
                    ->FindKNearestNeighbors(
                        bad_secondary_it,
                        ConvertFloatsToSlice(embeddings.data(), dim), neighbors,
                        num_lists, &result)
                    .IsInvalidArgument());
  }

  {
    // Invalid target
    // 测试无效的目标向量
    std::vector<std::pair<std::string, float>> result;
    ASSERT_TRUE(faiss_ivf_index
                    ->FindKNearestNeighbors(secondary_it.get(), "foo",
                                            neighbors, num_lists, &result)
                    .IsInvalidArgument());
  }

  {
    // Invalid value for neighbors
    // 测试无效的邻居数量值
    constexpr size_t bad_neighbors = 0;
    std::vector<std::pair<std::string, float>> result;
    ASSERT_TRUE(faiss_ivf_index
                    ->FindKNearestNeighbors(
                        secondary_it.get(),
                        ConvertFloatsToSlice(embeddings.data(), dim),
                        bad_neighbors, num_lists, &result)
                    .IsInvalidArgument());
  }

  {
    // Invalid value for neighbors
    // 测试无效的探测数量值
    constexpr size_t bad_probes = 0;
    std::vector<std::pair<std::string, float>> result;
    ASSERT_TRUE(faiss_ivf_index
                    ->FindKNearestNeighbors(
                        secondary_it.get(),
                        ConvertFloatsToSlice(embeddings.data(), dim), neighbors,
                        bad_probes, &result)
                    .IsInvalidArgument());
  }

  {
    // No result parameter
    // 测试空的结果参数
    constexpr std::vector<std::pair<std::string, float>>* bad_result = nullptr;
    ASSERT_TRUE(faiss_ivf_index
                    ->FindKNearestNeighbors(
                        secondary_it.get(),
                        ConvertFloatsToSlice(embeddings.data(), dim), neighbors,
                        num_lists, bad_result)
                    .IsInvalidArgument());
  }
}

// FAISS IVF 索引对比测试：与原生 FAISS 索引进行结果对比
TEST(FaissIVFIndexTest, Compare) {
  // Train two copies of the same index; hand over one to FaissIVFIndex and use
  // the other one as a baseline for comparison
  // 训练两个相同的索引副本；一个交给 FaissIVFIndex，另一个作为对比基准
  constexpr size_t dim = 128;
  auto quantizer_cmp = std::make_unique<faiss::IndexFlatL2>(dim);  // 对比用的量化器
  auto quantizer = std::make_unique<faiss::IndexFlatL2>(dim);      // RocksDB 用的量化器

  constexpr size_t num_lists = 16;
  auto index_cmp = std::make_unique<faiss::IndexIVFFlat>(quantizer_cmp.get(),
                                                         dim, num_lists);  // 对比用的索引
  auto index =
      std::make_unique<faiss::IndexIVFFlat>(quantizer.get(), dim, num_lists);  // RocksDB 用的索引

  {
    constexpr faiss::idx_t num_train = 1024;  // 训练数据数量，不是数据库向量数量
    std::vector<float> embeddings_train(dim * num_train);
    faiss::float_rand(embeddings_train.data(), dim * num_train, 42);

    // 使用相同的训练数据训练两个索引
    index_cmp->train(num_train, embeddings_train.data());
    index->train(num_train, embeddings_train.data());
  }

  // 使用默认宽列名创建 RocksDB 的 FAISS 索引
  auto faiss_ivf_index = std::make_shared<FaissIVFIndex>(
      std::move(index), kDefaultWideColumnName.ToString()); // 从"embedding"改为"kDefaultWideColumnName"

  const std::string db_name = test::PerThreadDBPath("faiss_ivf_index_test");
  EXPECT_OK(DestroyDB(db_name, Options()));

  Options options;
  options.create_if_missing = true;

  TransactionDBOptions txn_db_options;
  txn_db_options.secondary_indices.emplace_back(faiss_ivf_index);

  TransactionDB* db = nullptr;
  ASSERT_OK(TransactionDB::Open(options, txn_db_options, db_name, &db));

  std::unique_ptr<TransactionDB> db_guard(db);

  ColumnFamilyOptions cf1_opts;
  ColumnFamilyHandle* cfh1 = nullptr;
  ASSERT_OK(db->CreateColumnFamily(cf1_opts, "cf1", &cfh1));
  std::unique_ptr<ColumnFamilyHandle> cfh1_guard(cfh1);

  ColumnFamilyOptions cf2_opts;
  ColumnFamilyHandle* cfh2 = nullptr;
  ASSERT_OK(db->CreateColumnFamily(cf2_opts, "cf2", &cfh2));
  std::unique_ptr<ColumnFamilyHandle> cfh2_guard(cfh2);

  const auto& secondary_index = txn_db_options.secondary_indices.back();
  secondary_index->SetPrimaryColumnFamily(cfh1);
  secondary_index->SetSecondaryColumnFamily(cfh2);

  // Add the same set of database vectors to both indices
  // 向两个索引添加相同的数据库向量集合
  constexpr faiss::idx_t num_db = 4096;  // 数据库向量数量，大于训练数量1024

  {
    std::vector<float> embeddings_db(dim * num_db);
    faiss::float_rand(embeddings_db.data(), dim * num_db, 123);

    for (faiss::idx_t i = 0; i < num_db; ++i) {
      const float* const embedding = embeddings_db.data() + i * dim;

      // 向原生 FAISS 索引添加向量
      index_cmp->add(1, embedding);

      const std::string primary_key = std::to_string(i);
      // 向 RocksDB 添加向量数据（这会自动更新二级索引）
      ASSERT_OK(db->Put(WriteOptions(), cfh1, primary_key,
                        ConvertFloatsToSlice(embedding, dim))); // 没有用事务和宽列
    }
  }

  // Search both indices with the same set of query vectors and make sure the
  // results match
  // 使用相同的查询向量集合搜索两个索引，确保结果匹配
  {
    constexpr faiss::idx_t num_query = 32;  // 查询向量数量
    std::vector<float> embeddings_query(dim * num_query);
    faiss::float_rand(embeddings_query.data(), dim * num_query, 456);

    std::unique_ptr<Iterator> underlying_it(
        db->NewIterator(ReadOptions(), cfh2));
    auto secondary_it = std::make_unique<SecondaryIndexIterator>(
        faiss_ivf_index.get(), std::move(underlying_it));

    auto get_id = [](const Slice& key) -> faiss::idx_t {
      faiss::idx_t id = -1;

      if (std::from_chars(key.data(), key.data() + key.size(), id).ec !=
          std::errc()) {
        return -1;
      }

      return id;
    };

    // 测试不同的邻居数量和探测数量组合
    for (size_t neighbors : {1, 2, 4}) {
      for (size_t probes : {1, 2, 4}) {
        for (faiss::idx_t i = 0; i < num_query; ++i) {
          const float* const embedding = embeddings_query.data() + i * dim;

          std::vector<float> distances(neighbors, 0.0f);  // 存储距离结果
          std::vector<faiss::idx_t> ids(neighbors, -1);   // 存储 ID 结果

          faiss::SearchParametersIVF params;
          params.nprobe = probes;  // 设置探测的聚类数量

          // 在原生 FAISS 索引中搜索
          index_cmp->search(1, embedding, neighbors, distances.data(),
                            ids.data(), &params);

          size_t result_size_cmp = 0;
          for (faiss::idx_t id_cmp : ids) {
            if (id_cmp < 0) {
              break;
            }

            ++result_size_cmp;
          }

          std::vector<std::pair<std::string, float>> result;
          // 在 RocksDB 的 FAISS 索引中搜索
          ASSERT_OK(faiss_ivf_index->FindKNearestNeighbors(
              secondary_it.get(), ConvertFloatsToSlice(embedding, dim),
              neighbors, probes, &result));

          // 验证两个索引的结果数量相同
          ASSERT_EQ(result.size(), result_size_cmp);

          // 验证每个结果的 ID 和距离都相同
          for (size_t j = 0; j < result.size(); ++j) {
            const faiss::idx_t id = get_id(result[j].first);
            ASSERT_GE(id, 0);
            ASSERT_LT(id, num_db);
            ASSERT_EQ(id, ids[j]);                        // ID 应该相同
            ASSERT_EQ(result[j].second, distances[j]);    // 距离应该相同
          }
        }
      }
    }
  }
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  // 安装堆栈跟踪处理器，用于调试崩溃
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  // 初始化 Google Test 框架
  ::testing::InitGoogleTest(&argc, argv);
  // 运行所有测试
  return RUN_ALL_TESTS();
}
