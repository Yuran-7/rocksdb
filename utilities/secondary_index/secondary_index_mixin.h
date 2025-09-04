//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "db/wide/wide_columns_helper.h"
#include "rocksdb/options.h"
#include "rocksdb/utilities/secondary_index.h"
#include "rocksdb/wide_columns.h"
#include "util/autovector.h"
#include "utilities/secondary_index/secondary_index_helper.h"

// 此头文件，只被utilities/transactions/pessimistic_transaction_db.cc一个源文件引用
namespace ROCKSDB_NAMESPACE {

// 二级索引混入类：为事务类添加自动维护二级索引的能力
// 通过模板继承机制，包装原有事务接口，自动处理二级索引的增删改操作
template <typename Txn>
class SecondaryIndexMixin : public Txn {
 public:
  // 构造函数：接收二级索引列表和基类构造参数
  template <typename... Args>
  explicit SecondaryIndexMixin(
      const std::vector<std::shared_ptr<SecondaryIndex>>* secondary_indices,
      Args&&... args)
      : Txn(std::forward<Args>(args)...),
        secondary_indices_(secondary_indices) {
    assert(secondary_indices_);
    assert(!secondary_indices_->empty());
  }

  // 重写Put方法：自动维护二级索引
  using Txn::Put;
  Status Put(ColumnFamilyHandle* column_family, const Slice& key,
             const Slice& value, const bool assume_tracked = false) override {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return PutWithSecondaryIndices(column_family, key, value, do_validate);
    });
  }
  // SliceParts版本的Put
  Status Put(ColumnFamilyHandle* column_family, const SliceParts& key,
             const SliceParts& value,
             const bool assume_tracked = false) override {
    std::string key_str;
    const Slice key_slice(key, &key_str);

    std::string value_str;
    const Slice value_slice(value, &value_str);

    return Put(column_family, key_slice, value_slice, assume_tracked);
  }

  // 宽列版本的Put
  Status PutEntity(ColumnFamilyHandle* column_family, const Slice& key,
                   const WideColumns& columns,
                   bool assume_tracked = false) override {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return PutEntityWithSecondaryIndices(column_family, key, columns,
                                           do_validate);
    });
  }

  // Merge操作暂不支持二级索引
  using Txn::Merge;
  Status Merge(ColumnFamilyHandle* /* column_family */, const Slice& /* key */,
               const Slice& /* value */,
               const bool /* assume_tracked */ = false) override {
    return Status::NotSupported(
        "Merge with secondary indices not yet supported");
  }

  // 重写Delete方法：自动清理二级索引
  using Txn::Delete;
  Status Delete(ColumnFamilyHandle* column_family, const Slice& key,
                const bool assume_tracked = false) override {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return DeleteWithSecondaryIndices(column_family, key, do_validate);
    });
  }
  // SliceParts版本的Delete
  Status Delete(ColumnFamilyHandle* column_family, const SliceParts& key,
                const bool assume_tracked = false) override {
    std::string key_str;
    const Slice key_slice(key, &key_str);

    return Delete(column_family, key_slice, assume_tracked);
  }

  // SingleDelete版本
  using Txn::SingleDelete;
  Status SingleDelete(ColumnFamilyHandle* column_family, const Slice& key,
                      const bool assume_tracked = false) override {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return SingleDeleteWithSecondaryIndices(column_family, key, do_validate);
    });
  }
  Status SingleDelete(ColumnFamilyHandle* column_family, const SliceParts& key,
                      const bool assume_tracked = false) override {
    std::string key_str;
    const Slice key_slice(key, &key_str);

    return SingleDelete(column_family, key_slice, assume_tracked);
  }

  // 非跟踪版本的Put操作
  using Txn::PutUntracked;
  Status PutUntracked(ColumnFamilyHandle* column_family, const Slice& key,
                      const Slice& value) override {
    return PerformWithSavePoint([&]() {
      constexpr bool do_validate = false;
      return PutWithSecondaryIndices(column_family, key, value, do_validate);
    });
  }
  Status PutUntracked(ColumnFamilyHandle* column_family, const SliceParts& key,
                      const SliceParts& value) override {
    std::string key_str;
    const Slice key_slice(key, &key_str);

    std::string value_str;
    const Slice value_slice(value, &value_str);

    return PutUntracked(column_family, key_slice, value_slice);
  }

  // 非跟踪版本的宽列Put
  Status PutEntityUntracked(ColumnFamilyHandle* column_family, const Slice& key,
                            const WideColumns& columns) override {
    return PerformWithSavePoint([&]() {
      constexpr bool do_validate = false; // 完全 跳过 validate 逻辑，适用于一些调用场景下，调用者已经保证了 key/索引等状态是安全的，不需要额外检查
      return PutEntityWithSecondaryIndices(column_family, key, columns,
                                           do_validate);
    });
  }

  // 非跟踪版本的Merge（暂不支持）
  using Txn::MergeUntracked;
  Status MergeUntracked(ColumnFamilyHandle* /* column_family */,
                        const Slice& /* key */,
                        const Slice& /* value */) override {
    return Status::NotSupported(
        "MergeUntracked with secondary indices not yet supported");
  }

  // 非跟踪版本的Delete操作
  using Txn::DeleteUntracked;
  Status DeleteUntracked(ColumnFamilyHandle* column_family,
                         const Slice& key) override {
    return PerformWithSavePoint([&]() {
      constexpr bool do_validate = false;
      return DeleteWithSecondaryIndices(column_family, key, do_validate);
    });
  }
  Status DeleteUntracked(ColumnFamilyHandle* column_family,
                         const SliceParts& key) override {
    std::string key_str;
    const Slice key_slice(key, &key_str);

    return DeleteUntracked(column_family, key_slice);
  }

  // 非跟踪版本的SingleDelete
  using Txn::SingleDeleteUntracked;
  Status SingleDeleteUntracked(ColumnFamilyHandle* column_family,
                               const Slice& key) override {
    return PerformWithSavePoint([&]() {
      constexpr bool do_validate = false;
      return SingleDeleteWithSecondaryIndices(column_family, key, do_validate);
    });
  }

 private:
  // 索引数据封装类：管理单个索引的列值更新
  class IndexData {
   public:
    IndexData(const SecondaryIndex* index, const Slice& previous_column_value)
        : index_(index), previous_column_value_(previous_column_value) {
      assert(index_);
    }

    // 访问器方法
    const SecondaryIndex* index() const { return index_; }
    const Slice& previous_column_value() const {
      return previous_column_value_;
    }
    std::optional<std::variant<Slice, std::string>>& updated_column_value() {
      return updated_column_value_;
    }
    // 获取最终的主列值（更新后或原值）
    Slice primary_column_value() const {
      return updated_column_value_.has_value()
                 ? SecondaryIndexHelper::AsSlice(*updated_column_value_)
                 : previous_column_value_;
    }

   private:
    const SecondaryIndex* index_;
    Slice previous_column_value_;
    std::optional<std::variant<Slice, std::string>> updated_column_value_;
  };

  // 带保存点执行操作：失败时自动回滚
  template <typename Operation>
  Status PerformWithSavePoint(Operation&& operation) {   // 转发引用的语法，条件1: 这是一个函数模板， 条件2: 参数形式为 T&&
    // utilities/transactions/transaction_base.cc内的void TransactionBaseImpl::SetSavePoint()函数
    // 要加::，不然编译会报错，详情请看examples/demo1.cpp
    Txn::SetSavePoint();

    const Status s = operation(); // 调用lambda函数

    if (!s.ok()) {
      [[maybe_unused]] const Status st = Txn::RollbackToSavePoint();
      assert(st.ok());

      return s;
    }

    [[maybe_unused]] const Status st = Txn::PopSavePoint();
    assert(st.ok());

    return Status::OK();
  }

  // 用主表的primary_key搜索，看看是否有数据，如果有数据，就把数据存到existing_primary_columns（加排他锁）
  Status GetPrimaryEntryForUpdate(ColumnFamilyHandle* column_family,
                                  const Slice& primary_key,
                                  PinnableWideColumns* existing_primary_columns,
                                  bool do_validate) {
    assert(column_family);
    assert(existing_primary_columns);

    constexpr bool exclusive = true;

    return Txn::GetEntityForUpdate(ReadOptions(), column_family, primary_key,
                                   existing_primary_columns, exclusive,
                                   do_validate);  // WriteCommittedTxn::GetEntityForUpdate
  }

  // 删除二级索引的单个条目
  Status RemoveSecondaryEntry(const SecondaryIndex* secondary_index,
                              const Slice& primary_key,
                              const Slice& existing_primary_column_value) {
    assert(secondary_index);

    std::variant<Slice, std::string> secondary_key_prefix;

    {
      const Status s = secondary_index->GetSecondaryKeyPrefix(
          primary_key, existing_primary_column_value, &secondary_key_prefix); // 把聚类的簇的id，从existing_primary_column_value复制到secondary_key_prefix
      if (!s.ok()) {
        return s;
      }
    }

    {
      const Status s =
          secondary_index->FinalizeSecondaryKeyPrefix(&secondary_key_prefix);
      if (!s.ok()) {
        return s;
      }
    }

    const std::string secondary_key =
        SecondaryIndexHelper::AsString(secondary_key_prefix) +
        primary_key.ToString(); // secondary_key = 聚类的簇的id + primary_key

    return Txn::SingleDelete(secondary_index->GetSecondaryColumnFamily(),
                             secondary_key);
  }

  // 添加主表条目（标量值版本）
  Status AddPrimaryEntry(ColumnFamilyHandle* column_family,
                         const Slice& primary_key, const Slice& primary_value) {
    assert(column_family);

    constexpr bool assume_tracked = true;

    return Txn::Put(column_family, primary_key, primary_value, assume_tracked);
  }

  // 添加主表条目（宽列版本）
  // 将经过UpdatePrimaryColumnValues处理后的数据写入主数据列族（cf1）。
  //
  // 以FaissIVFIndex为例：
  // 写入的数据是：
  // - Key: 原始主键 (e.g., "42")
  // - Value (宽列): {"embedding": <聚类中心ID>}
  //
  // 注意：这里写入的不再是原始的向量数据，而是它所属的聚类ID。
  // 原始向量数据被保存在了 applicable_indices 中，用于下一步生成二级索引。
  Status AddPrimaryEntry(ColumnFamilyHandle* column_family,
                         const Slice& primary_key,
                         const WideColumns& primary_columns) {
    assert(column_family);

    constexpr bool assume_tracked = true;

    return Txn::PutEntity(column_family, primary_key, primary_columns,
                          assume_tracked);  // 调用链：WriteCommittedTxn::PutEntity -> WriteCommittedTxn::PutEntityImpl -> WriteCommittedTxn::Operate
  }

  // 添加单个二级索引条目
  Status AddSecondaryEntry(const SecondaryIndex* secondary_index,
                           const Slice& primary_key,
                           const Slice& primary_column_value,
                           const Slice& previous_column_value) {
    assert(secondary_index);

    std::variant<Slice, std::string> secondary_key_prefix;

    {
      const Status s = secondary_index->GetSecondaryKeyPrefix(
          primary_key, primary_column_value, &secondary_key_prefix);  // 把聚类的簇的id，从primary_column_value复制到secondary_key_prefix
      if (!s.ok()) {
        return s;
      }
    }

    {
      const Status s =
          secondary_index->FinalizeSecondaryKeyPrefix(&secondary_key_prefix); // 啥都不干
      if (!s.ok()) {
        return s;
      }
    }

    std::optional<std::variant<Slice, std::string>> secondary_value;

    {
      const Status s = secondary_index->GetSecondaryValue(
          primary_key, primary_column_value, previous_column_value,
          &secondary_value);
      if (!s.ok()) {
        return s;
      }
    }

    {
      const std::string secondary_key =
          SecondaryIndexHelper::AsString(secondary_key_prefix) +
          primary_key.ToString(); // secondary_key = 聚类的簇的id + primary_key

      const Status s =
          Txn::Put(secondary_index->GetSecondaryColumnFamily(), secondary_key,
                   secondary_value.has_value()
                       ? SecondaryIndexHelper::AsSlice(*secondary_value)
                       : Slice());
      if (!s.ok()) {
        return s;
      }
    }

    return Status::OK();
  }

  // 批量删除所有相关的二级索引条目
  Status RemoveSecondaryEntries(ColumnFamilyHandle* column_family,
                                const Slice& primary_key,
                                const WideColumns& existing_columns) {  // cf1，lable(id)，宽列数据
    assert(column_family);

    for (const auto& secondary_index : *secondary_indices_) {
      assert(secondary_index);

      if (secondary_index->GetPrimaryColumnFamily() != column_family) { // GetPrimaryColumnFamily获取id所在的列簇
        continue;
      }

      const auto it = WideColumnsHelper::Find(
          existing_columns.cbegin(), existing_columns.cend(),
          secondary_index->GetPrimaryColumnName()); // GetPrimaryColumnName()是embedding，it是执行宽列中的embedding列
      if (it == existing_columns.cend()) {
        continue;
      }

      const Status st =
          RemoveSecondaryEntry(secondary_index.get(), primary_key, it->value());  // secondary_index是智能指针，get()获取裸指针；primary_key是label(id)；it->value()是embedding列的值
      if (!st.ok()) {
        return st;
      }
    } // for

    return Status::OK();
  }

  // 更新主列值（标量值版本）：调用各索引的UpdatePrimaryColumnValue
  Status UpdatePrimaryColumnValues(ColumnFamilyHandle* column_family,
                                   const Slice& primary_key,
                                   Slice& primary_value,
                                   autovector<IndexData>& applicable_indices) {
    assert(column_family);
    assert(applicable_indices.empty());

    applicable_indices.reserve(secondary_indices_->size());

    for (const auto& secondary_index : *secondary_indices_) {
      assert(secondary_index);

      if (secondary_index->GetPrimaryColumnFamily() != column_family) {
        continue;
      }

      if (secondary_index->GetPrimaryColumnName() != kDefaultWideColumnName) {
        continue;
      }

      applicable_indices.emplace_back(
          IndexData(secondary_index.get(), primary_value));

      auto& index_data = applicable_indices.back();

      const Status s = secondary_index->UpdatePrimaryColumnValue(
          primary_key, index_data.previous_column_value(),
          &index_data.updated_column_value());
      if (!s.ok()) {
        return s;
      }

      primary_value = index_data.primary_column_value();
    }

    return Status::OK();
  }

  Status UpdatePrimaryColumnValues(ColumnFamilyHandle* column_family,
                                   const Slice& primary_key,
                                   WideColumns& primary_columns,
                                   autovector<IndexData>& applicable_indices) {
    assert(column_family);
    assert(applicable_indices.empty());

    // TODO: as an optimization, we can avoid calling SortColumns a second time
    // in WriteBatchInternal::PutEntity
    WideColumnsHelper::SortColumns(primary_columns);

    // 遍历所有二级索引，为本次Put操作预处理数据，并将受影响的索引信息存入
    // applicable_indices，以便后续创建索引条目。
    //
    // 以FaissIVFIndex为例：
    // 1. 调用 secondary_index->UpdatePrimaryColumnValue。
    // 2. 在该函数内部，使用FAISS的粗量化器为输入的向量（it->value()）找到其所属的聚类中心ID（label）。
    // 3. 将这个聚类ID序列化后，通过 updated_column_value 返回。
    // 4. Mixin将这个返回的聚类ID覆盖掉宽列中原始的向量值（it->value() = ...）。
    // 5. 将这个索引的元数据（包括原始向量值）存入 applicable_indices。
    //
    // 结果：primary_columns 中的向量值被替换为聚类ID，为下一步写入主表做好了准备。
    applicable_indices.reserve(secondary_indices_->size());

    for (const auto& secondary_index : *secondary_indices_) {
      assert(secondary_index);

      if (secondary_index->GetPrimaryColumnFamily() != column_family) {
        continue;
      }

      const auto it = WideColumnsHelper::Find(
          primary_columns.begin(), primary_columns.end(),
          secondary_index->GetPrimaryColumnName());
      if (it == primary_columns.end()) {
        continue;
      }

      applicable_indices.emplace_back(
          IndexData(secondary_index.get(), it->value())); // 构造函数传递两个参数，const SecondaryIndex* index, const Slice& previous_column_value

      auto& index_data = applicable_indices.back();

      const Status s = secondary_index->UpdatePrimaryColumnValue(
          primary_key, index_data.previous_column_value(),
          &index_data.updated_column_value());  // previous_column_value()是向量，updated_column_value()是聚类ID
      if (!s.ok()) {
        return s;
      }

      it->value() = index_data.primary_column_value();  // index_data中如果有updated_column_value_，就返回updated_column_value_，所以这里相当于把it->value()替换成了聚类ID，实实在在会影响传入的参数 WideColumns& primary_columns
    }

    return Status::OK();
  }

  // 批量添加二级索引条目
  // 遍历 applicable_indices 列表，为每个受影响的索引在二级索引列族（cf2）中创建一条记录。
  //
  // 以FaissIVFIndex为例，内部调用的 AddSecondaryEntry 会：
  // 1. 调用 secondary_index->GetSecondaryKeyPrefix，传入聚类ID，直接返回该ID作为Key前缀。
  // 2. 调用 secondary_index->GetSecondaryValue，传入聚类ID和原始向量值。
  //    此函数计算向量与聚类中心的残差，并生成细量化编码（fine-quantized code）作为二级索引的Value。
  // 3. 构造二级索引的Key: [聚类ID] + [原始主键]。
  // 4. 将 Key 和 Value（细量化编码）写入二级索引列族 cf2。
  //
  // 结果：在cf2中创建了倒排列表，实现了 "聚类ID -> 属于该聚类的向量列表" 的映射。
  Status AddSecondaryEntries(const Slice& primary_key,
                             const autovector<IndexData>& applicable_indices) {
    for (const auto& index_data : applicable_indices) {
      const Status s = AddSecondaryEntry(index_data.index(), primary_key,
                                         index_data.primary_column_value(),
                                         index_data.previous_column_value());
      if (!s.ok()) {
        return s;
      }
    }

    return Status::OK();
  }

  // Put操作的通用实现模板：支持标量值和宽列
  template <typename Value>
  Status PutWithSecondaryIndicesImpl(ColumnFamilyHandle* column_family,
                                     const Slice& key,
                                     const Value& value_or_columns,
                                     bool do_validate) {
    // TODO: we could avoid removing and recreating secondary entries for
    // which neither the secondary key prefix nor the value has changed

    if (!column_family) { // 传入的是cfh1
      column_family = Txn::DefaultColumnFamily();
    }

    const Slice& primary_key = key;

    {
      PinnableWideColumns existing_primary_columns;

      const Status s = GetPrimaryEntryForUpdate(
          column_family, primary_key, &existing_primary_columns, do_validate);  // s.ok()表示找到了，s.IsNotFound()表示没找到
      if (!s.ok()) {
        if (!s.IsNotFound()) {  // Status::NotFound();
          return s;
        }
      } else {  // s.IsNotFound() = true → 不会进入 else，只是跳过，继续后续逻辑
        const Status st = RemoveSecondaryEntries(
            column_family, primary_key, existing_primary_columns.columns());  // 删除与该主键关联的所有二级索引条目
        if (!st.ok()) {
          return st;
        }
      }
    }

    auto primary_value_or_columns = value_or_columns;
    autovector<IndexData> applicable_indices;

    {
      // 核心函数
      const Status s = UpdatePrimaryColumnValues(column_family, primary_key,
                                                 primary_value_or_columns,  // cfh1，label(id)，宽列数据
                                                 applicable_indices);
      if (!s.ok()) {  
        return s;
      }
    }

    {
      const Status s =
          AddPrimaryEntry(column_family, primary_key, primary_value_or_columns);  // 内部会调用 RocksDB 的 Put 或者 Merge，以 primary_key 作为 key，primary_value_or_columns 作为 value
      if (!s.ok()) {
        return s;
      }
    }

    {
      const Status s = AddSecondaryEntries(primary_key, applicable_indices);  // 更新二级索引表
      if (!s.ok()) {
        return s;
      }
    }

    return Status::OK();
  }

  Status PutWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                 const Slice& key, const Slice& value,
                                 bool do_validate) {
    return PutWithSecondaryIndicesImpl(column_family, key, value, do_validate);
  }

  Status PutEntityWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                       const Slice& key,
                                       const WideColumns& columns,
                                       bool do_validate) {
    return PutWithSecondaryIndicesImpl(column_family, key, columns,
                                       do_validate);
  }

  template <typename Operation>
  Status DeleteWithSecondaryIndicesImpl(ColumnFamilyHandle* column_family,
                                        const Slice& key, bool do_validate,
                                        Operation&& operation) {
    if (!column_family) {
      column_family = Txn::DefaultColumnFamily();
    }

    {
      PinnableWideColumns existing_primary_columns;

      const Status s = GetPrimaryEntryForUpdate(
          column_family, key, &existing_primary_columns, do_validate);
      if (!s.ok()) {
        if (!s.IsNotFound()) {
          return s;
        }

        return Status::OK();
      } else {
        const Status st = RemoveSecondaryEntries(
            column_family, key, existing_primary_columns.columns());
        if (!st.ok()) {
          return st;
        }
      }
    }

    {
      const Status s = operation(column_family, key);
      if (!s.ok()) {
        return s;
      }
    }

    return Status::OK();
  }

  Status DeleteWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                    const Slice& key, bool do_validate) {
    return DeleteWithSecondaryIndicesImpl(
        column_family, key, do_validate,
        [&](ColumnFamilyHandle* cfh, const Slice& primary_key) {
          assert(cfh);

          constexpr bool assume_tracked = true;

          return Txn::Delete(cfh, primary_key, assume_tracked);
        });
  }

  Status SingleDeleteWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                          const Slice& key, bool do_validate) {
    return DeleteWithSecondaryIndicesImpl(
        column_family, key, do_validate,
        [&](ColumnFamilyHandle* cfh, const Slice& primary_key) {
          assert(cfh);

          constexpr bool assume_tracked = true;

          return Txn::SingleDelete(cfh, primary_key, assume_tracked);
        });
  }

  const std::vector<std::shared_ptr<SecondaryIndex>>* secondary_indices_;
};

}  // namespace ROCKSDB_NAMESPACE
