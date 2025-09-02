//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "rocksdb/iterator.h"
#include "rocksdb/rocksdb_namespace.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

class ColumnFamilyHandle;

// EXPERIMENTAL - 实验性功能
//
// 二级索引是构建在主键值对集合之上的额外数据结构，使得可以通过值而不是键来高效查询键值对。
// 普通键值对和宽列键值对都可以被索引，后者按列为单位进行索引。
// 二级索引维护从（列）值到具有相应值的主键列表的映射（在给定列中）。
//
// A secondary index is an additional data structure built over a set of primary
// key-values that enables efficiently querying key-values by value instead of
// key. Both plain and wide-column key-values can be indexed, the latter on a
// per-column basis. The secondary index then maintains a mapping from (column)
// value to the list of primary keys that have the corresponding value (in the
// given column).
//
// 主键值对和二级键值对可以存储在相同或不同的列族中。应用程序有责任避免冲突和歧义
// （例如，通过使用前缀创建单独的键空间或为每个二级索引使用专用列族）。
// 另外，请注意应用程序不应直接操作二级索引条目。
//
// The primary and secondary key-value pairs can be stored in either the same
// column family or different ones. It is the application's responsibility to
// avoid conflicts and ambiguities (for example, by using prefixes to create
// separate key spaces or using a dedicated column family for each secondary
// index). Also, note that applications are not expected to manipulate secondary
// index entries directly.
//
// 在有并发写入的一般情况下，维护二级索引需要事务语义和并发控制。
// 因此，二级索引仅通过事务层支持。使用二级索引时，每当通过事务插入、更新或删除
// （主）键值对时（无论是显式还是隐式事务），RocksDB将根据主列族和列名调用任何适用的
// SecondaryIndex对象，并将自动根据需要添加或删除任何二级索引条目（使用相同的事务）。
//
// In the general case where there are concurrent writers, maintaining a
// secondary index requires transactional semantics and concurrency control.
// Because of this, secondary indices are only supported via the transaction
// layer. With secondary indices, whenever a (primary) key-value is inserted,
// updated, or deleted via a transaction (regardless of whether it is an
// explicit or implicit one), RocksDB will invoke any applicable SecondaryIndex
// objects based on primary column family and column name, and it will
// automatically add or remove any secondary index entries as needed (using
// the same transaction).
//
// 注意：除了Set{Primary,Secondary}ColumnFamily（不应在初始化后调用）之外，
// SecondaryIndex实现的方法应该是线程安全的。
//
// Note: the methods of SecondaryIndex implementations are expected to be
// thread-safe with the exception of Set{Primary,Secondary}ColumnFamily (which
// are not expected to be called after initialization).

class SecondaryIndex {
 public:
  virtual ~SecondaryIndex() = default;

  // 设置主列族句柄
  virtual void SetPrimaryColumnFamily(ColumnFamilyHandle* column_family) = 0;
  // 设置二级索引列族句柄  
  virtual void SetSecondaryColumnFamily(ColumnFamilyHandle* column_family) = 0;

  // 获取主列族句柄
  virtual ColumnFamilyHandle* GetPrimaryColumnFamily() const = 0;
  // 获取二级索引列族句柄
  virtual ColumnFamilyHandle* GetSecondaryColumnFamily() const = 0;

  // 要为主数据的哪一列建立索引。普通键值对可以通过指定kDefaultWideColumnName来索引。
  // The name of the primary column to index. Plain key-values can be indexed by
  // specifying kDefaultWideColumnName.
  virtual Slice GetPrimaryColumnName() const = 0;

  // 在插入或更新主键值对期间可选地更新主列值。
  // 在主键值对写入被添加到事务之前由事务层调用。
  // 返回非OK状态会回滚事务中与此主键值对相关的所有操作。
  // Optionally update the primary column value during an insert or update of a
  // primary key-value. Called by the transaction layer before the primary
  // key-value write is added to the transaction. Returning a non-OK status
  // rolls back all operations in the transaction related to this primary
  // key-value.
  virtual Status UpdatePrimaryColumnValue(
      const Slice& primary_key, const Slice& primary_column_value,
      std::optional<std::variant<Slice, std::string>>* updated_column_value)
      const = 0;

  // 获取给定主键值对的二级索引键前缀。
  // 此方法在添加或删除二级索引条目（格式为<secondary_key_prefix><primary_key> -> <secondary_value>）
  // 时由事务层调用，应该是确定性的。
  // 输出参数secondary_key_prefix应基于primary_key和/或primary_column_value。
  // 返回非OK状态会回滚事务中与此主键值对相关的所有操作。
  // Get the secondary key prefix for a given primary key-value. This method is
  // called by the transaction layer when adding or removing secondary index
  // entries (which have the form <secondary_key_prefix><primary_key> ->
  // <secondary_value>) and should be deterministic. The output parameter
  // secondary_key_prefix is expected to be based on primary_key and/or
  // primary_column_value. Returning a non-OK status rolls back all operations
  // in the transaction related to this primary key-value.
  virtual Status GetSecondaryKeyPrefix(
      const Slice& primary_key, const Slice& primary_column_value,
      std::variant<Slice, std::string>* secondary_key_prefix) const = 0;

  // 最终确定二级索引键前缀，例如通过添加一些元数据来防止歧义（如索引ID或长度指示符）。
  // 此方法在添加或删除二级索引条目（格式为<secondary_key_prefix><primary_key> -> <secondary_value>）
  // 以及查询索引时（在这种情况下用搜索目标调用）由事务层调用。
  // 方法应该是确定性的。返回非OK状态会回滚事务中与此主键值对相关的所有操作。
  // Finalize the secondary key prefix, for instance by adding some metadata to
  // prevent ambiguities (e.g. index id or length indicator). This method is
  // called by the transaction layer when adding or removing secondary index
  // entries (which have the form <secondary_key_prefix><primary_key> ->
  // <secondary_value>) and also when querying the index (in which case it is
  // called with the search target). The method should be deterministic.
  // Returning a non-OK status rolls back all operations in the transaction
  // related to this primary key-value.
  virtual Status FinalizeSecondaryKeyPrefix(
      std::variant<Slice, std::string>* secondary_key_prefix) const = 0;

  // 获取给定主键值对的可选二级索引值。
  // 此方法在添加二级索引条目（格式为<secondary_key_prefix><primary_key> -> <secondary_value>）
  // 时由事务层调用。
  // previous_column_value包含主列的先前值，以防它被UpdatePrimaryColumnValue更改。
  // 返回非OK状态会回滚事务中与此主键值对相关的所有操作。
  // Get the optional secondary value for a given primary key-value. This method
  // is called by the transaction layer when adding secondary index
  // entries (which have the form <secondary_key_prefix><primary_key> ->
  // <secondary_value>). previous_column_value contains the previous value of
  // the primary column in case it was changed by UpdatePrimaryColumnValue.
  // Returning a non-OK status rolls back all operations in the transaction
  // related to this primary key-value.
  virtual Status GetSecondaryValue(
      const Slice& primary_key, const Slice& primary_column_value,
      const Slice& previous_column_value,
      std::optional<std::variant<Slice, std::string>>* secondary_value)
      const = 0;
};

// SecondaryIndexIterator可用于查找给定搜索目标的主键。
// 它可以按原样使用或作为构建块。其接口镜像大部分Iterator API，
// 但不包括SeekToFirst、SeekToLast和SeekForPrev，这些对二级索引不适用，因此不存在。
// 可以通过调用返回的迭代器的Seek API并使用搜索目标来查询索引，
// 然后使用Next（可能还有Prev）来遍历匹配的索引条目。
// 迭代器暴露主键，即从索引条目中剥离二级索引键前缀。
//
// SecondaryIndexIterator can be used to find the primary keys for a given
// search target. It can be used as-is or as a building block. Its interface
// mirrors most of the Iterator API, with the exception of SeekToFirst,
// SeekToLast, and SeekForPrev, which are not applicable to secondary indices
// and thus not present. Querying the index can be performed by calling the
// returned iterator's Seek API with a search target, and then using Next (and
// potentially Prev) to iterate through the matching index entries. The iterator
// exposes primary keys, that is, the secondary key prefix is stripped from the
// index entries.

class SecondaryIndexIterator {
 public:
  // 构造SecondaryIndexIterator。SecondaryIndexIterator获取底层迭代器的所有权。
  // 前提条件：index不为nullptr
  // 前提条件：underlying_it不为nullptr且指向索引的二级列族上的迭代器
  // Constructs a SecondaryIndexIterator. The SecondaryIndexIterator takes
  // ownership of the underlying iterator.
  // PRE: index is not nullptr
  // PRE: underlying_it is not nullptr and points to an iterator over the
  // index's secondary column family
  SecondaryIndexIterator(const SecondaryIndex* index,
                         std::unique_ptr<Iterator>&& underlying_it);

  // 返回迭代器是否有效，即是否定位在与搜索目标匹配的二级索引条目上。
  // Returns whether the iterator is valid, i.e. whether it is positioned on a
  // secondary index entry matching the search target.
  bool Valid() const;

  // 返回迭代器的状态，如果迭代器有效则保证为OK。
  // 否则，可能是非OK（表示错误），或OK（表示迭代器已到达适用二级索引条目的末尾）。
  // Returns the status of the iterator, which is guaranteed to be OK if the
  // iterator is valid. Otherwise, it might be non-OK, which indicates an error,
  // or OK, which means that the iterator has reached the end of the applicable
  // secondary index entries.
  Status status() const;

  // 使用给定的搜索目标查询索引。
  // Query the index with the given search target.
  void Seek(const Slice& target);

  // 将迭代器移动到下一个条目。
  // 前提条件：Valid()
  // Move the iterator to the next entry.
  // PRE: Valid()
  void Next();

  // 将迭代器移回到上一个条目。
  // 前提条件：Valid()
  // Move the iterator back to the previous entry.
  // PRE: Valid()
  void Prev();

  // 准备当前条目的值。如果底层迭代器是用读选项allow_unprepared_value设置为true构造的，
  // 则应在调用value()或columns()之前调用此方法。成功时返回true。失败时返回false并将状态设置为非OK。
  // 前提条件：Valid()
  // Prepare the value of the current entry. Should be called before calling
  // value() or columns() if the underlying iterator was constructed with the
  // read option allow_unprepared_value set to true. Returns true upon success.
  // Returns false and sets the status to non-OK upon failure.
  // PRE: Valid()
  bool PrepareValue();

  // 从当前二级索引条目返回主键。
  // 前提条件：Valid()
  // Returns the primary key from the current secondary index entry.
  // PRE: Valid()
  Slice key() const;

  // 返回当前二级索引条目的值。
  // 前提条件：Valid()
  // Returns the value of the current secondary index entry.
  // PRE: Valid()
  Slice value() const;

  // 将当前二级索引条目的值作为宽列结构返回。
  // 前提条件：Valid()
  // Returns the value of the current secondary index entry as a wide-column
  // structure.
  // PRE: Valid()
  const WideColumns& columns() const;

  // 返回当前二级索引条目的时间戳。
  // 前提条件：Valid()
  // Returns the timestamp of the current secondary index entry.
  // PRE: Valid()
  Slice timestamp() const;

  // 查询底层迭代器的给定属性。成功时返回OK，失败时返回非OK。
  // 前提条件：Valid()
  // Queries the given property of the underlying iterator. Returns OK on
  // success, non-OK on failure.
  // PRE: Valid()
  Status GetProperty(std::string prop_name, std::string* prop) const;

 private:
  const SecondaryIndex* index_;
  std::unique_ptr<Iterator> underlying_it_;
  Status status_;
  std::string prefix_;
};

}  // namespace ROCKSDB_NAMESPACE
