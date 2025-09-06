//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/utilities/secondary_index.h"

namespace faiss {
struct IndexIVF;
}

namespace ROCKSDB_NAMESPACE {

// EXPERIMENTAL - 实验性功能
//
// SecondaryIndex的实现，封装了基于FAISS倒排文件的索引。
// 使用给定的预训练faiss::IndexIVF对象对指定主列中的嵌入向量进行索引。
// 可用于执行K近邻查询。
//
// A SecondaryIndex implementation that wraps a FAISS inverted file based index.
// Indexes the embedding in the specified primary column using the given
// pre-trained faiss::IndexIVF object. Can be used to perform
// K-nearest-neighbors queries.

class FaissIVFIndex : public SecondaryIndex {
 public:
  // Constructs a FaissIVFIndex object. Takes ownership of the given faiss::IndexIVF instance.
  // PRE: index is not nullptr
  FaissIVFIndex(std::unique_ptr<faiss::IndexIVF>&& index,
                std::string primary_column_name);
  ~FaissIVFIndex() override;

  // 继承自SecondaryIndex的基本方法
  void SetPrimaryColumnFamily(ColumnFamilyHandle* column_family) override;
  void SetSecondaryColumnFamily(ColumnFamilyHandle* column_family) override;

  ColumnFamilyHandle* GetPrimaryColumnFamily() const override;
  ColumnFamilyHandle* GetSecondaryColumnFamily() const override;

  Slice GetPrimaryColumnName() const override;

  // primary_column_value是embedding，updated_column_value是聚簇id，primary_key未使用
  Status UpdatePrimaryColumnValue(
      const Slice& primary_key, const Slice& primary_column_value,
      std::optional<std::variant<Slice, std::string>>* updated_column_value)
      const override;

  // 把primary_column_value的值赋值给secondary_key_prefix
  Status GetSecondaryKeyPrefix(
      const Slice& primary_key, const Slice& primary_column_value,
      std::variant<Slice, std::string>* secondary_key_prefix) const override;

  // 啥都没干，返回OK
  Status FinalizeSecondaryKeyPrefix(
      std::variant<Slice, std::string>* secondary_key_prefix) const override;

  // 获取二级索引值（FAISS编码后的向量）
  Status GetSecondaryValue(const Slice& primary_key,
                           const Slice& primary_column_value,
                           const Slice& original_column_value,
                           std::optional<std::variant<Slice, std::string>>*
                               secondary_value) const override;

  // 使用给定的二级索引迭代器对目标执行K近邻向量相似性搜索，
  // 其中K由参数neighbors给出，要搜索的倒排列表数量由参数probes给出。
  // 结果主键和距离在result输出参数中返回。
  // 注意：如果探测的倒排列表在找到K个项目之前耗尽，搜索可能返回少于请求数量的结果。
  //
  // 参数it应为非nullptr且指向与此索引对应的二级索引迭代器。
  // 搜索目标应具有正确的维度（即target.size() == dim * sizeof(float)，
  // 其中dim是索引的维度），neighbors和probes应为正数，result应为非nullptr。
  //
  // 成功时返回OK，如果不满足上述前提条件则返回InvalidArgument，
  // 或者如果搜索期间发生错误则返回其他非OK状态。
  //
  // Performs a K-nearest-neighbors vector similarity search for the target
  // using the given secondary index iterator, where K is given by the parameter
  // neighbors and the number of inverted lists to search is given by the
  // parameter probes. The resulting primary keys and distances are returned in
  // the result output parameter. Note that the search may return less than the
  // requested number of results if the inverted lists probed are exhausted
  // before finding K items.
  //
  // The parameter it should be non-nullptr and point to a secondary index
  // iterator corresponding to this index. The search target should be of the
  // correct dimension (i.e. target.size() == dim * sizeof(float), where dim is
  // the dimensionality of the index), neighbors and probes should be positive,
  // and result should be non-nullptr.
  //
  // Returns OK on success, InvalidArgument if the preconditions above are not
  // met, or some other non-OK status if there is an error during the search.
  Status FindKNearestNeighbors(
      SecondaryIndexIterator* it, const Slice& target, size_t neighbors,
      size_t probes, std::vector<std::pair<std::string, float>>* result) const;

 private:
  struct KNNContext;  // K近邻搜索上下文
  class Adapter;      // FAISS倒排列表适配器。可以认为是FaissIVFIndex的内部类，只不过这个内部类在类外定义的

  std::unique_ptr<Adapter> adapter_;              // 适配器，适配InvertedLists，它IndexIVF的一个成员变量，内部维护了 nlist 个独立的列表，是向量ID和编码后数据的最终存储位置
  std::unique_ptr<faiss::IndexIVF> index_;        // FAISS IVF索引实例
  std::string primary_column_name_;               // 告诉RocksDB要为主数据的哪一列建立索引
  ColumnFamilyHandle* primary_column_family_{};   // 告诉RocksDB主数据存储在哪个列族中
  ColumnFamilyHandle* secondary_column_family_{}; // 告诉RocksDB二级索引数据存储在哪个列族中
};


// Helper methods to convert embeddings from a span of floats to Slice or vice versa

// Convert the given span of floats of size dim to a Slice.
// PRE: embedding points to a contiguous span of floats of size dim
inline Slice ConvertFloatsToSlice(const float* embedding, size_t dim) {
  return Slice(reinterpret_cast<const char*>(embedding), dim * sizeof(float));  // IEEE编码表示一个float占4个字节，所以不是存十进制
}


// Convert the given Slice to a span of floats of size dim.
// PRE: embedding.size() == dim * sizeof(float)
// Returns nullptr if the precondition is violated.
inline const float* ConvertSliceToFloats(const Slice& embedding, size_t dim) {
  if (embedding.size() != dim * sizeof(float)) {
    return nullptr;
  }

  return reinterpret_cast<const float*>(embedding.data());
}

}  // namespace ROCKSDB_NAMESPACE
