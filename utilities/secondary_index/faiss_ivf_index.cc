//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <cassert>
#include <optional>
#include <stdexcept>
#include <utility>

#include "faiss/IndexIVF.h"
#include "faiss/invlists/InvertedLists.h"
#include "rocksdb/utilities/secondary_index_faiss.h"
#include "util/autovector.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

namespace {

// 将FAISS标签序列化为字符串（用作RocksDB键前缀）
std::string SerializeLabel(faiss::idx_t label) {  // using idx_t = int64_t
  std::string label_str;
  PutVarsignedint64(&label_str, label);

  return label_str;
}

// 从字符串反序列化FAISS标签
faiss::idx_t DeserializeLabel(Slice label_slice) {
  faiss::idx_t label = -1;
  [[maybe_unused]] const bool ok = GetVarsignedint64(&label_slice, &label);
  assert(ok);

  return label;
}

}  // namespace

// K近邻搜索上下文，存储迭代器和主键映射。KNNContext属于声明在FaissIVFIndex类内部，定义在类外的内部类
struct FaissIVFIndex::KNNContext {
  SecondaryIndexIterator* it;   // 二级索引迭代器
  autovector<std::string> keys; // 主键列表（FAISS ID到主键的映射）
};

// FAISS倒排列表适配器类：将RocksDB的存储接口适配为FAISS的倒排列表接口
class FaissIVFIndex::Adapter : public faiss::InvertedLists {  // InvertedLists是一个抽象基类，纯虚函数必须重写
 public:
  Adapter(size_t num_lists, size_t code_size)
      : faiss::InvertedLists(num_lists, code_size) {
    use_iterator = true;  // 启用基于迭代器的访问模式
  }

  // 非基于迭代器的读取接口；由于use_iterator为true，这些方法不会被使用
  // Non-iterator-based read interface; not implemented/used since use_iterator
  // is true
  size_t list_size(size_t /* list_no */) const override {
    assert(false);  // 不应该被调用
    return 0;
  }

  const uint8_t* get_codes(size_t /* list_no */) const override {
    assert(false);  // 不应该被调用
    return nullptr;
  }

  const faiss::idx_t* get_ids(size_t /* list_no */) const override {
    assert(false);  // 不应该被调用
    return nullptr;
  }

  // 基于迭代器的读取接口：为指定的倒排列表创建迭代器
  // Iterator-based read interface
  faiss::InvertedListsIterator* get_iterator(
      size_t list_no, void* inverted_list_context = nullptr) const override {
    KNNContext* const knn_context =
        static_cast<KNNContext*>(inverted_list_context);
    assert(knn_context);

    return new IteratorAdapter(knn_context, list_no, code_size);
  }

  // 写入接口；目前只实现了add_entry，这是唯一需要的方法
  // Write interface; only add_entry is implemented/required for now
  size_t add_entry(size_t /* list_no */, faiss::idx_t /* id */,
                   const uint8_t* code,
                   void* inverted_list_context = nullptr) override {
    std::string* const code_str =
        static_cast<std::string*>(inverted_list_context);
    assert(code_str);

    // 将FAISS编码复制到字符串中
    code_str->assign(reinterpret_cast<const char*>(code), code_size);

    return 0;
  }

  // 批量添加条目（未实现）
  size_t add_entries(size_t /* list_no */, size_t /* num_entries */,
                     const faiss::idx_t* /* ids */,
                     const uint8_t* /* code */) override {
    assert(false);  // 不支持
    return 0;
  }

  // 更新单个条目（未实现）
  void update_entry(size_t /* list_no */, size_t /* offset */,
                    faiss::idx_t /* id */, const uint8_t* /* code */) override {
    assert(false);  // 不支持
  }

  // 批量更新条目（未实现）
  void update_entries(size_t /* list_no */, size_t /* offset */,
                      size_t /* num_entries */, const faiss::idx_t* /* ids */,
                      const uint8_t* /* code */) override {
    assert(false);  // 不支持
  }

  // 调整列表大小（未实现）
  void resize(size_t /* list_no */, size_t /* new_size */) override {
    assert(false);  // 不支持
  }

 private:
  // 迭代器适配器：将RocksDB迭代器适配为FAISS倒排列表迭代器
  class IteratorAdapter : public faiss::InvertedListsIterator { // InvertedListsIterator是独立于InvertedLists的一个抽象基类
   public:
    // 构造迭代器适配器
    IteratorAdapter(KNNContext* knn_context, size_t list_no, size_t code_size)
        : knn_context_(knn_context),
          it_(knn_context_->it),
          code_size_(code_size) {
      assert(knn_context_);
      assert(it_);

      // 定位到指定的倒排列表（通过序列化的标签）
      const std::string label = SerializeLabel(list_no);
      it_->Seek(label);
      Update();
    }

    // 检查迭代器是否指向有效数据
    bool is_available() const override { return id_and_codes_.has_value(); }

    // 移动到下一个条目
    void next() override {
      it_->Next();
      Update();
    }

    // 获取当前条目的ID和编码数据
    std::pair<faiss::idx_t, const uint8_t*> get_id_and_codes() override {
      assert(is_available());

      return *id_and_codes_;
    }

   private:
    // 更新当前迭代器状态和ID映射
    void Update() {
      id_and_codes_.reset();

      // 检查迭代器状态
      const Status status = it_->status();
      if (!status.ok()) {
        throw std::runtime_error(status.ToString());
      }

      // 检查迭代器是否有效
      if (!it_->Valid()) {
        return;
      }

      // 准备值数据
      if (!it_->PrepareValue()) {
        throw std::runtime_error(
            "Failed to prepare value during iteration in FaissIVFIndex");
      }

      // 获取编码数据并验证大小
      const Slice value = it_->value();
      if (value.size() != code_size_) {
        throw std::runtime_error(
            "Code with unexpected size encountered during iteration in "
            "FaissIVFIndex");
      }

      // 为当前条目分配ID并记录主键
      const faiss::idx_t id = knn_context_->keys.size();
      knn_context_->keys.emplace_back(it_->key().ToString());

      // 设置ID和编码数据的配对
      id_and_codes_.emplace(id, reinterpret_cast<const uint8_t*>(value.data()));
    }

    KNNContext* knn_context_;        // K近邻搜索上下文
    SecondaryIndexIterator* it_;     // RocksDB二级索引迭代器
    size_t code_size_;               // 编码数据大小
    std::optional<std::pair<faiss::idx_t, const uint8_t*>> id_and_codes_; // 当前ID和编码数据
  };
};

// FaissIVFIndex构造函数：初始化FAISS索引和适配器
FaissIVFIndex::FaissIVFIndex(std::unique_ptr<faiss::IndexIVF>&& index,
                             std::string primary_column_name)
    : adapter_(std::make_unique<Adapter>(index->nlist, index->code_size)),
      index_(std::move(index)),
      primary_column_name_(std::move(primary_column_name)) {
  assert(index_);
  assert(index_->quantizer);

  // 禁用并行模式（避免线程安全问题）
  index_->parallel_mode = 0;
  // 这一步很关键，index_是IndexIVF类型的，InvertedLists* invlists是它的一个成员变量，
  // 这里将我们重写了InvertedLists的适配器adapter_作为IndexIVF的成员变量
  index_->replace_invlists(adapter_.get());
}

FaissIVFIndex::~FaissIVFIndex() = default;

// 设置主列族
void FaissIVFIndex::SetPrimaryColumnFamily(ColumnFamilyHandle* column_family) {
  assert(column_family);
  primary_column_family_ = column_family;
}

// 设置二级索引列族
void FaissIVFIndex::SetSecondaryColumnFamily(
    ColumnFamilyHandle* column_family) {
  assert(column_family);
  secondary_column_family_ = column_family;
}

// 获取主列族
ColumnFamilyHandle* FaissIVFIndex::GetPrimaryColumnFamily() const {
  return primary_column_family_;
}

// 获取二级索引列族
ColumnFamilyHandle* FaissIVFIndex::GetSecondaryColumnFamily() const {
  return secondary_column_family_;
}

// 获取主列名
Slice FaissIVFIndex::GetPrimaryColumnName() const {
  return primary_column_name_;
}

// 更新主列值：将向量通过FAISS粗量化器分配到聚类
Status FaissIVFIndex::UpdatePrimaryColumnValue(
    const Slice& /* primary_key */, const Slice& primary_column_value,
    std::optional<std::variant<Slice, std::string>>* updated_column_value)
    const { 
  assert(updated_column_value);

  // 将Slice转换为float数组
  const float* const embedding =
      ConvertSliceToFloats(primary_column_value, index_->d);
  if (!embedding) {
    return Status::InvalidArgument(
        "Incorrectly sized vector passed to FaissIVFIndex");
  }

  constexpr faiss::idx_t n = 1;
  faiss::idx_t label = -1;

  try {
    // index_是IndexIVF类型的，quantizer是IndexIVF内部的struct Index*，assign是Index的方法，本质上是搜索
    // assign函数还有一个参数k表示topk，默认是1
    // 函数的目的就是获取向量最近的k个簇的编号，放入label中
    index_->quantizer->assign(n, embedding, &label);
  } catch (const std::exception& e) {
    return Status::InvalidArgument(e.what());
  }

  // 验证标签有效性
  if (label < 0 || label >= index_->nlist) {
    return Status::InvalidArgument(
        "Unexpected label returned by coarse quantizer");
  }

  // 序列化标签作为更新后的列值
  updated_column_value->emplace(SerializeLabel(label));

  return Status::OK();
}

// 获取二级索引键前缀：直接使用聚类标签作为前缀
Status FaissIVFIndex::GetSecondaryKeyPrefix(
    const Slice& /* primary_key */, const Slice& primary_column_value,
    std::variant<Slice, std::string>* secondary_key_prefix) const {
  assert(secondary_key_prefix);

  // 反序列化标签并验证
  [[maybe_unused]] const faiss::idx_t label =
      DeserializeLabel(primary_column_value);
  assert(label >= 0);
  assert(label < index_->nlist);

  // 使用聚类标签作为二级索引键前缀
  *secondary_key_prefix = primary_column_value;

  return Status::OK();
}

// 最终确定二级索引键前缀：对于FAISS IVF，不需要额外处理
Status FaissIVFIndex::FinalizeSecondaryKeyPrefix(
    std::variant<Slice, std::string>* /* secondary_key_prefix */) const {
  return Status::OK();
}

// 获取二级索引值：使用FAISS细量化器对向量进行编码
Status FaissIVFIndex::GetSecondaryValue(
    const Slice& /* primary_key */, const Slice& primary_column_value,
    const Slice& original_column_value,
    std::optional<std::variant<Slice, std::string>>* secondary_value) const {
  assert(secondary_value);

  // 反序列化聚类标签
  const faiss::idx_t label = DeserializeLabel(primary_column_value);
  assert(label >= 0);
  assert(label < index_->nlist);

  constexpr faiss::idx_t n = 1;

  // 将原始列值转换为float数组
  const float* const embedding =
      ConvertSliceToFloats(original_column_value, index_->d);
  assert(embedding);

  constexpr faiss::idx_t* xids = nullptr;   // 是外部ID数组，用于为添加的向量指定自定义ID
  std::string code_str;

  try {
    // 使用FAISS细量化器对向量进行编码
    index_->add_core(n, embedding, xids, &label, &code_str);
  } catch (const std::exception& e) {
    return Status::Corruption(e.what());
  }

  // 验证编码大小
  if (code_str.size() != index_->code_size) {
    return Status::Corruption(
        "Code with unexpected size returned by fine quantizer");
  }

  // 返回编码作为二级索引值
  secondary_value->emplace(std::move(code_str));

  return Status::OK();
}

// 执行K近邻搜索：使用FAISS进行向量相似度搜索
Status FaissIVFIndex::FindKNearestNeighbors(
    SecondaryIndexIterator* it, const Slice& target, size_t neighbors,
    size_t probes, std::vector<std::pair<std::string, float>>* result) const {
  // 参数验证
  if (!it) {
    return Status::InvalidArgument("Secondary index iterator must be provided");
  }

  // 将目标向量转换为float数组
  const float* const embedding = ConvertSliceToFloats(target, index_->d);
  if (!embedding) {
    return Status::InvalidArgument(
        "Incorrectly sized vector passed to FaissIVFIndex");
  }

  if (!neighbors) {
    return Status::InvalidArgument("Invalid number of neighbors");
  }

  if (!probes) {
    return Status::InvalidArgument("Invalid number of probes");
  }

  if (!result) {
    return Status::InvalidArgument("Result parameter must be provided");
  }

  result->clear();

  // 准备搜索结果缓冲区
  std::vector<float> distances(neighbors, 0.0f);
  std::vector<faiss::idx_t> ids(neighbors, -1);

  // 创建K近邻搜索上下文
  KNNContext knn_context{it, {}};

  // 设置FAISS搜索参数
  faiss::SearchParametersIVF params;
  params.nprobe = probes;  // 探测的倒排列表数量
  params.inverted_list_context = &knn_context;  // 传递上下文给适配器

  constexpr faiss::idx_t n = 1;

  try {
    // index_是IndexIVF类型的
    index_->search(n, embedding, neighbors, distances.data(), ids.data(),
                   &params);
  } catch (const std::exception& e) {
    return Status::Corruption(e.what());
  }

  // 构造结果：将FAISS ID映射回主键并附上距离
  result->reserve(neighbors);

  for (size_t i = 0; i < neighbors; ++i) {
    if (ids[i] < 0) {
      break;  // 没有更多结果
    }

    // 验证ID有效性
    if (ids[i] >= knn_context.keys.size()) {
      result->clear();
      return Status::Corruption("Unexpected id returned by FAISS");
    }

    // 添加主键和距离到结果中
    result->emplace_back(knn_context.keys[ids[i]], distances[i]);
  }

  return Status::OK();
}

}  // namespace ROCKSDB_NAMESPACE
