#include <iostream>
#include <cassert>
#include "rocksdb/db.h"
#include "rocksdb/options.h"

int main() {
    rocksdb::DB* db;
    rocksdb::Options options;
    options.create_if_missing = true;

    // 明确指定使用 Snappy 压缩
    options.compression = rocksdb::kZSTD;

    rocksdb::Status status = rocksdb::DB::Open(options, "./test_snappy_db", &db);
    assert(status.ok());

    std::string key = "foo";
    std::string value = "this is a long string that snappy will attempt to compress...";
    status = db->Put(rocksdb::WriteOptions(), key, value);
    assert(status.ok());

    std::string retrieved;
    status = db->Get(rocksdb::ReadOptions(), key, &retrieved);
    assert(status.ok());

    std::cout << "Retrieved value: " << retrieved << std::endl;

    delete db;
    return 0;
}
