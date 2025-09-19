#include <iostream>
#include <string>
#include <vector>
#include <cassert>

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/slice.h"
#include "db/blob/blob_index.h"
#include "db/write_batch_internal.h"
#include "db/column_family.h"

using namespace ROCKSDB_NAMESPACE;

int main() {
    const std::string db_path = "/tmp/blob_demo_db";
    system(("rm -rf " + db_path).c_str());

    DB* db = nullptr;
    Options options;
    options.create_if_missing = true;
    options.enable_blob_files = true;
    options.min_blob_size = 100;               // 100 bytes 以上的 value 会存为 blob
    options.blob_file_size = 1024 * 1024;      // 1MB
    options.enable_blob_garbage_collection = true;
    options.blob_garbage_collection_age_cutoff = 0.25;

    Status s = DB::Open(options, db_path, &db);
    if (!s.ok()) {
        std::cerr << "Failed to open db: " << s.ToString() << std::endl;
        return 1;
    }
    std::cout << "Database opened at: " << db_path << std::endl;

    WriteOptions wopts;
    ReadOptions ropts;

    // 1. 插入普通 key
    db->Put(wopts, "small_key1", "small_value");
    db->Put(wopts, "small_key2", "another_small_value");

    // 2. 插入大 value（会存成 blob）
    std::string large_value(500, 'A');
    db->Put(wopts, "large_key1", large_value);

    // 3. 插入内联 blob with TTL
    {
        uint64_t expiration = 1234567890;
        std::string blob_index;
        BlobIndex::EncodeInlinedTTL(&blob_index, expiration, "inlined_value_with_ttl");

        WriteBatch batch;
        auto cfh = db->DefaultColumnFamily();
        auto cfh_impl = static_cast<ColumnFamilyHandleImpl*>(cfh);
        WriteBatchInternal::PutBlobIndex(&batch, cfh_impl->GetID(), "inlined_ttl_key", blob_index);
        db->Write(wopts, &batch);
    }

    // 4. 插入 blob 引用
    {
        std::string blob_index;
        BlobIndex::EncodeBlob(&blob_index, 100, 1024, 256, kSnappyCompression);

        WriteBatch batch;
        auto cfh = db->DefaultColumnFamily();
        auto cfh_impl = static_cast<ColumnFamilyHandleImpl*>(cfh);
        WriteBatchInternal::PutBlobIndex(&batch, cfh_impl->GetID(), "blob_ref_key", blob_index);
        db->Write(wopts, &batch);
    }

    // 5. 插入带 TTL 的 blob 引用
    {
        uint64_t expiration = 1234567890 + 3600;
        std::string blob_index;
        BlobIndex::EncodeBlobTTL(&blob_index, expiration, 101, 2048, 512, kLZ4Compression);

        WriteBatch batch;
        auto cfh = db->DefaultColumnFamily();
        auto cfh_impl = static_cast<ColumnFamilyHandleImpl*>(cfh);
        WriteBatchInternal::PutBlobIndex(&batch, cfh_impl->GetID(), "blob_ttl_key", blob_index);
        db->Write(wopts, &batch);
    }

    std::cout << "\n=== Testing Reads ===" << std::endl;
    {
        std::vector<std::string> keys = {
            "small_key1", "large_key1", "inlined_ttl_key",
            "blob_ref_key", "blob_ttl_key", "non_existent_key"
        };

        for (auto& key : keys) {
            PinnableSlice pval;
            Status st = db->Get(ropts, key, &pval);
            if (st.ok()) {
                std::string val = pval.ToString();
                BlobIndex blob_index;
                if (blob_index.DecodeFrom(val).ok()) {
                    std::cout << "GET (BLOB): " << key << " -> " 
                              << blob_index.DebugString(false) << std::endl;
                } else {
                    std::cout << "GET: " << key << " -> " 
                              << val.substr(0, 50) 
                              << (val.size() > 50 ? "..." : "") << std::endl;
                }
            } else {
                std::cout << "GET: " << key << " -> NOT_FOUND" << std::endl;
            }
        }
    }

    std::cout << "\n=== Testing Updates ===" << std::endl;
    db->Put(wopts, "small_key1", "updated_small_value");
    {
        std::string val;
        db->Get(ropts, "small_key1", &val);
        std::cout << "Updated small_key1 -> " << val << std::endl;
    }

    std::cout << "\n=== Testing Deletes ===" << std::endl;
    db->Delete(wopts, "small_key2");
    {
        std::string val;
        Status st = db->Get(ropts, "small_key2", &val);
        std::cout << "Deleted small_key2, exists? " << st.ok() << std::endl;
    }

    std::cout << "\n=== Database Iteration ===" << std::endl;
    {
        auto it = db->NewIterator(ropts);
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            std::string val = it->value().ToString();

            BlobIndex blob_index;
            if (blob_index.DecodeFrom(val).ok()) {
                std::cout << "  " << key << " -> " << blob_index.DebugString(false) << std::endl;
            } else {
                std::cout << "  " << key << " -> " 
                          << val.substr(0, 50) 
                          << (val.size() > 50 ? "..." : "") << std::endl;
            }
        }
        delete it;
    }

    std::cout << "\n=== Database Stats ===" << std::endl;
    {
        std::string stats;
        db->GetProperty("rocksdb.stats", &stats);
        std::cout << stats << std::endl;

        std::string blob_stats;
        if (db->GetProperty("rocksdb.blob-stats", &blob_stats)) {
            std::cout << "Blob Stats:\n" << blob_stats << std::endl;
        }
    }

    delete db;
    return 0;
}
