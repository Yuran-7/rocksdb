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

  // 获取主表的primary_key，看看是否有数据，如果有数据，就把数据存到existing_primary_columns（加排他锁）
  Status GetPrimaryEntryForUpdate(ColumnFamilyHandle* column_family,
                                  const Slice& primary_key,
                                  PinnableWideColumns* existing_primary_columns,
                                  bool do_validate) {
    assert(column_family);
    assert(existing_primary_columns);

    constexpr bool exclusive = true;

    return Txn::GetEntityForUpdate(ReadOptions(), column_family, primary_key,
                                   existing_primary_columns, exclusive,
                                   do_validate);
  }

  // 删除单个二级索引条目
  Status RemoveSecondaryEntry(const SecondaryIndex* secondary_index,
                              const Slice& primary_key,
                              const Slice& existing_primary_column_value) {
    assert(secondary_index);

    std::variant<Slice, std::string> secondary_key_prefix;

    {
      const Status s = secondary_index->GetSecondaryKeyPrefix(
          primary_key, existing_primary_column_value, &secondary_key_prefix);
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
        primary_key.ToString();

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
  Status AddPrimaryEntry(ColumnFamilyHandle* column_family,
                         const Slice& primary_key,
                         const WideColumns& primary_columns) {
    assert(column_family);

    constexpr bool assume_tracked = true;

    return Txn::PutEntity(column_family, primary_key, primary_columns,
                          assume_tracked);
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
          primary_key, primary_column_value, &secondary_key_prefix);
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
          primary_key.ToString();

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
                                const WideColumns& existing_columns) {
    assert(column_family);

    for (const auto& secondary_index : *secondary_indices_) {
      assert(secondary_index);

      if (secondary_index->GetPrimaryColumnFamily() != column_family) {
        continue;
      }

      const auto it = WideColumnsHelper::Find(
          existing_columns.cbegin(), existing_columns.cend(),
          secondary_index->GetPrimaryColumnName());
      if (it == existing_columns.cend()) {
        continue;
      }

      const Status st =
          RemoveSecondaryEntry(secondary_index.get(), primary_key, it->value());
      if (!st.ok()) {
        return st;
      }
    }

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

  // 更新主列值（宽列版本）：处理多列的索引更新
  Status UpdatePrimaryColumnValues(ColumnFamilyHandle* column_family,
                                   const Slice& primary_key,
                                   WideColumns& primary_columns,
                                   autovector<IndexData>& applicable_indices) {
    assert(column_family);
    assert(applicable_indices.empty());

    // TODO: as an optimization, we can avoid calling SortColumns a second time
    // in WriteBatchInternal::PutEntity
    WideColumnsHelper::SortColumns(primary_columns);

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
          IndexData(secondary_index.get(), it->value()));

      auto& index_data = applicable_indices.back();

      const Status s = secondary_index->UpdatePrimaryColumnValue(
          primary_key, index_data.previous_column_value(),
          &index_data.updated_column_value());
      if (!s.ok()) {
        return s;
      }

      it->value() = index_data.primary_column_value();
    }

    return Status::OK();
  }

  // 批量添加二级索引条目
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
        if (!s.IsNotFound()) {
          return s;
        }
      } else {
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
      const Status s = UpdatePrimaryColumnValues(column_family, primary_key,
                                                 primary_value_or_columns,
                                                 applicable_indices); // 哪些二级索引需要修改（填充到 applicable_indices）
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
