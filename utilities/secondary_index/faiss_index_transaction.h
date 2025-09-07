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

namespace ROCKSDB_NAMESPACE {
  template <typename Txn>
  class FaissIndexTransaction : public Txn {

   public:
    // 构造函数
    template <typename... Args>
    explicit FaissIndexTransaction(
        std::shared_ptr<SecondaryIndex> secondary_indices,
        Args&&... args);
    
    Status Put(ColumnFamilyHandle* column_family, const Slice& key,
               const Slice& value, const bool assume_tracked = false) override;
    Status PutEntity(ColumnFamilyHandle* column_family, const Slice& key,
                     const WideColumns& columns,
                     bool assume_tracked = false) override;

    Status Delete(ColumnFamilyHandle* column_family, const Slice& key,
                  const bool assume_tracked = false) override;
   private:
    // 给Put/Delete等操作提供保存点机制
    template <typename Operation>
    Status PerformWithSavePoint(Operation&& operation);

    template <typename Value>
    Status PutWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                   const Slice& key,
                                   const Value& value_or_columns,
                                   bool do_validate);
    Status DeleteWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                      const Slice& key, bool do_validate);

    Status GetPrimaryEntryForUpdate(ColumnFamilyHandle* column_family,
                                    const Slice& primary_key,
                                    PinnableWideColumns* existing_primary_columns,
                                    bool do_validate);
    Status RemoveSecondaryEntries(ColumnFamilyHandle* column_family,
                                  const Slice& primary_key,
                                  const WideColumns& existing_primary_columns);                                    
    Status AddPrimaryEntry(ColumnFamilyHandle* column_family,
                          const Slice& primary_key,
                          const WideColumns& primary_columns);                                                                  
    Status AddSecondaryEntries(const Slice& primary_key,
                               const WideColumns& secondary_columns,
                               const std::shared_ptr<SecondaryIndex>& applicable_indices);
   private:
    std::shared_ptr<SecondaryIndex> secondary_index_;
  };

  template <typename Txn>
  template <typename... Args>
  FaissIndexTransaction<Txn>::FaissIndexTransaction(
      std::shared_ptr<SecondaryIndex> secondary_index,
      Args&&... args)
      : Txn(std::forward<Args>(args)...) {
    if (secondary_index != nullptr) {
      secondary_index_ = secondary_index;
      assert(secondary_index_ != nullptr);
    }
  }

  template <typename Txn>
  Status FaissIndexTransaction<Txn>::Put(ColumnFamilyHandle* column_family,
                                       const Slice& key, const Slice& value,
                                       const bool assume_tracked = false) {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return PutWithSecondaryIndices(column_family, key, value, do_validate);
    });    
  }

  template <typename Txn>
  Status FaissIndexTransaction<Txn>::PutEntity(ColumnFamilyHandle* column_family,
                                           const Slice& key,
                                           const WideColumns& columns,
                                           bool assume_tracked = false) {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return PutWithSecondaryIndices(column_family, key, columns, do_validate);
    });
  }

  template <typename Txn>
  Status FaissIndexTransaction<Txn>::Delete(ColumnFamilyHandle* column_family,
                                          const Slice& key,
                                          const bool assume_tracked = false) {
    return PerformWithSavePoint([&]() {
      const bool do_validate = !assume_tracked;
      return DeleteWithSecondaryIndices(column_family, key, do_validate);
    });
  }
  template <typename Txn>
  template <typename Operation>
  Status FaissIndexTransaction<Txn>::PerformWithSavePoint(Operation&& operation) {
    // 保存点
    Txn::SetSavePoint();

    // 执行操作
    const Status s = operation();
    if (!s.ok()) {
      const Status st = Txn::RollbackToSavePoint();
      assert(st.ok());
      // 失败时回滚，这会释放锁
      return s;
    }

    const Status st = Txn::PopSavePoint();
    assert(st.ok());
    // 成功时提交，锁在事务完成后释放
    return Status::OK();
  }

  template <typename Txn>
  template <typename Value>
  Status FaissIndexTransaction<Txn>::PutWithSecondaryIndices(
      ColumnFamilyHandle* column_family, 
      const Slice& key,
      const Value& value_or_columns, bool do_validate) {
    
    if (!column_family) { // 传入的是cfh1
      column_family = Txn::DefaultColumnFamily();
    }
    const Slice& primary_key = key;
    {
      PinnableWideColumns existing_primary_columns;
      const Status s = GetPrimaryEntryForUpdate(
          column_family, primary_key, &existing_primary_columns, do_validate);
      if (!s.ok()) {
        if (!s.IsNotFound()) {  // Status::NotFound();
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

    {
      const Status s =
          AddPrimaryEntry(column_family, primary_key, value_or_columns);  // 内部会调用 RocksDB 的 Put 或者 Merge，以 primary_key 作为 key，value_or_columns 作为 value
      if (!s.ok()) {
        return s;
      }
    }

    {
      const Status s = AddSecondaryEntries(primary_key, value_or_columns, secondary_index_);  // 更新二级索引表
      if (!s.ok()) {
        return s;
      }
    }

    return Status::OK();
  }

  template <typename Txn>
  Status FaissIndexTransaction<Txn>::DeleteWithSecondaryIndices(ColumnFamilyHandle* column_family,
                                    const Slice& key, bool do_validate) {
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
      constexpr bool assume_tracked = true;
      const Status s = Txn::Delete(column_family, key, assume_tracked);
      if (!s.ok()) {
        return s;
      }
    }

    return Status::OK();
  }


}
