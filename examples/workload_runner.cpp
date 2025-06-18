#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <chrono> // Required for timing

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/iterator.h"


int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <database_path> <workload_file_path>" << std::endl;
        return 1;
    }

    std::string db_path = argv[1];
    std::string workload_file_path = argv[2];

    rocksdb::DB* db;
    rocksdb::Options options;
    rocksdb::WriteOptions write_options;
    rocksdb::ReadOptions read_options;
    options.create_if_missing = true;              // Create the database if it doesn't exist

    std::string line;
    char op_type;
    long long line_number = 0;
    long long operations_processed = 0;
    long long insert_count = 0;
    long long update_count = 0;
    long long delete_count = 0;
    long long range_delete_count = 0; // For range delete operations
    long long point_query_count = 0; // For point query operations
    long long range_query_count = 0; // For range query operations
    long long total_data_size_bytes = 0; // For calculating throughput
    

    rocksdb::Status s = rocksdb::DestroyDB(db_path, options); // TODO，无法调试
    if (!s.ok()) {
        std::cerr << "DestroyDB failed: " << s.ToString() << std::endl;
        return 1;
    }

    std::cout << "Default compression: " << static_cast<int>(options.compression) << std::endl;

    options.write_buffer_size = 16 * 1024 * 1024; // 设置memtable大小为16MB
    options.target_file_size_base = 16 * 1024 * 1024; // 设置SST文件大小为16MB

    rocksdb::Status status = rocksdb::DB::Open(options, db_path, &db);  // 要获取db，通过std::move(db_impl)得到
    if (!status.ok()) {
        std::cerr << "Error opening database " << db_path << ": " << status.ToString() << std::endl;
        return 1;
    }
    std::cout << "Database opened successfully: " << db_path << std::endl;

    rocksdb::Iterator *it = db->NewIterator(read_options); // 创建一个新的迭代器用于范围读取

    std::ifstream workload_file(workload_file_path);
    if (!workload_file.is_open()) {
        std::cerr << "Error opening workload file: " << workload_file_path << std::endl;
        delete db;
        return 1;
    }
    std::cout << "Processing workload file: " << workload_file_path << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now(); // Start timer

    while (std::getline(workload_file, line)) {
        line_number++;
        char instruction;
        std::string key, start_key, end_key, value;
        workload_file >> instruction;
        switch(instruction) {
        case 'I':
            workload_file >> key >> value;
            s = db->Put(write_options, key, value);
            if (!s.ok()) std::cerr << s.ToString() << std::endl;
            assert(s.ok());
            insert_count++;
            total_data_size_bytes += key.size() + value.size();
            break;
        case 'U':
            workload_file >> key >> value;
            s = db->Put(write_options, key, value);
            if (!s.ok()) std::cerr << s.ToString() << std::endl;
            assert(s.ok());
            insert_count++;
            total_data_size_bytes += key.size() + value.size();
            break;
        case 'D':
            workload_file >> key;
            s = db->Delete(write_options, key);
            if (!s.ok()) std::cerr << s.ToString() << std::endl;
            assert(s.ok());
            delete_count++;
            // Optionally, you could account for the size of the key in deletes for total "processed" bytes
            // total_data_size_bytes += key.size();
            break;
        case 'R':   // range delete 
            workload_file >> start_key >> end_key;
            s = db->DeleteRange(write_options, start_key, end_key);
            assert(s.ok());
            range_delete_count++;
            break;
        case 'Q':   // probe: point query
            workload_file >> key;
            s = db->Get(rocksdb::ReadOptions(), key, &value);
            if (!s.ok() && !s.IsNotFound()) {
                std::cerr << "Error getting key " << key << ": " << s.ToString() << std::endl;
            }
            point_query_count++;
            break;
        case 'S':
            workload_file >> start_key >> end_key;
            it->Refresh();
            assert(it->status().ok());
            for (it->Seek(start_key); it->Valid(); it->Next()) {
                //std::cout << "found key = " << it->key().ToString() << std::endl;
                if(it->key() == end_key) {
                    break;
                }
            }
            if (!it->status().ok()) {
              std::cerr << it->status().ToString() << std::endl;
            }
            range_query_count++;
            break;
        default:
            std::cerr << "ERROR: Case match NOT found !!" << std::endl;
            break;
        }
        if (line_number% 100000 == 0) {
            std::cout << "Processed " << line_number << " operations ("
                      << insert_count << " inserts, " << delete_count << " deletes, " << update_count << " update, "
                      << range_delete_count << " range_delete_count, " << point_query_count << " point_query_count, " 
                      << range_query_count << " range_query_count)" << std::endl;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now(); // End timer
    auto duration_s = std::chrono::duration<double>(end_time - start_time).count();
    double ops = 0;
    if (duration_s > 0) {
        ops = line_number / duration_s;
    }
    double throughput_mb_s = 0;
    if (duration_s > 0) {
        throughput_mb_s = (static_cast<double>(total_data_size_bytes) / (1024 * 1024)) / duration_s;
    }


    std::cout << "Finished processing workload file." << std::endl;
    std::cout << "Total lines read: " << line_number << std::endl;
    std::cout << "Total inserts: " << insert_count << std::endl;
    std::cout << "Total updates: " << update_count << std::endl;
    std::cout << "Total deletes: " << delete_count << std::endl;
    std::cout << "Total range deletes: " << range_delete_count << std::endl;
    std::cout << "Total point queries: " << point_query_count << std::endl;
    std::cout << "Total range queries: " << range_query_count << std::endl;
    std::cout << "Total data written (Inserts K+V): " << static_cast<double>(total_data_size_bytes) / (1024 * 1024) << " MB" << std::endl;
    std::cout << "Execution time: " << duration_s << " seconds" << std::endl;
    std::cout << "Operations per second (OPS): " << ops << std::endl;
    std::cout << "Throughput (MB/s): " << throughput_mb_s << std::endl;


    workload_file.close();
    delete db; // Close the database

    std::cout << "Database closed." << std::endl;

    return 0;
}