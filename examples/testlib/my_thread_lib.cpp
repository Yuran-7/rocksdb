#include <stdio.h>
#include <pthread.h>
#include <string>   // C++ 字符串，Snappy 库常用
#include <snappy.h> // Snappy 压缩库的头文件
#include "my_thread_lib.h" // 包含我们自己的头文件

// 使用 extern "C" 确保这些函数具有 C 语言的链接特性，
// 这样 C 语言的 main.c 文件才能正确调用它们。
#ifdef __cplusplus
extern "C" {
#endif

void *thread_function(void *arg) {
    printf("Hello from the thread! Argument: %s\n", (char *)arg);
    pthread_exit(NULL);
}

void create_my_thread(const char *message) {
    pthread_t tid;
    printf("Creating a thread...\n");
    if (pthread_create(&tid, NULL, thread_function, (void *)message) != 0) {
        perror("Failed to create thread");
        return;
    }
    // For demonstration, we'll join the thread to ensure its completion
    if (pthread_join(tid, NULL) != 0) {
        perror("Failed to join thread");
    }
    printf("Thread finished.\n");
}

// 新增的 Snappy 压缩函数
void compress_data_snappy(const char *input_data) {
    std::string input_str(input_data);
    std::string compressed_str;
    std::string uncompressed_str;
    size_t uncompressed_length;

    printf("\n--- Snappy Compression Demo ---\n");
    printf("Original data size: %zu bytes\n", input_str.length());
    printf("Original data: \"%s\"\n", input_str.c_str());

    // 使用 Snappy 压缩数据
    snappy::Compress(input_str.data(), input_str.length(), &compressed_str);
    printf("Compressed data size: %zu bytes\n", compressed_str.length());

    // 使用 Snappy 解压缩数据
    if (snappy::Uncompress(compressed_str.data(), compressed_str.length(), &uncompressed_str)) {
        printf("Uncompressed data size: %zu bytes\n", uncompressed_str.length());
        printf("Uncompressed data: \"%s\"\n", uncompressed_str.c_str());
        if (input_str == uncompressed_str) {
            printf("Compression and decompression successful and data matches!\n");
        } else {
            printf("Error: Decompressed data does NOT match original!\n");
        }
    } else {
        printf("Error: Failed to uncompress data.\n");
    }
    printf("-------------------------------\n");
}

#ifdef __cplusplus
}
#endif