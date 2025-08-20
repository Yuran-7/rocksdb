#ifndef MY_THREAD_LIB_H
#define MY_THREAD_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

void create_my_thread(const char *message);
void compress_data_snappy(const char *input_data); // 新增的函数声明

#ifdef __cplusplus
}
#endif

#endif // MY_THREAD_LIB_H