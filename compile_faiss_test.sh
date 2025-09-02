#!/bin/bash

# 编译 FAISS IVF 索引测试的脚本
# 使用你的自定义 FAISS 库路径

set -e  # 遇到错误时退出

echo "=== 开始编译 FAISS IVF 索引测试 ==="

# 检查 RocksDB 调试库是否存在
if [ ! -f "librocksdb_debug.a" ]; then
    echo "错误: librocksdb_debug.a 不存在，请先运行: make static_lib DEBUG_LEVEL=1"
    exit 1
fi

# 检查 FAISS 库是否存在
FAISS_LIB_PATH="/home/ysh/faiss/build/faiss/libfaiss.so"
FAISS_HEADER_PATH="/home/ysh/faiss"

if [ ! -f "$FAISS_LIB_PATH" ]; then
    echo "错误: FAISS 库不存在: $FAISS_LIB_PATH"
    exit 1
fi

if [ ! -d "$FAISS_HEADER_PATH/faiss" ]; then
    echo "错误: FAISS 头文件目录不存在: $FAISS_HEADER_PATH/faiss"
    exit 1
fi

echo "1. 编译 FAISS IVF 索引库..."
g++ -c -o faiss_ivf_index.o utilities/secondary_index/faiss_ivf_index.cc \
    -I./include \
    -I. \
    -I"$FAISS_HEADER_PATH" \
    -std=c++17 -g -W -Wextra -Wall -Wsign-compare -Wshadow -Wunused-parameter \
    -faligned-new -DHAVE_ALIGNED_NEW -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX \
    -DOS_LINUX -fno-builtin-memcmp -DROCKSDB_FALLOCATE_PRESENT -DSNAPPY -DGFLAGS=1 \
    -DZLIB -DBZIP2 -DNUMA -DTBB -DROCKSDB_MALLOC_USABLE_SIZE -DROCKSDB_PTHREAD_ADAPTIVE_MUTEX \
    -DROCKSDB_BACKTRACE -DROCKSDB_RANGESYNC_PRESENT -DROCKSDB_SCHED_GETCPU_PRESENT \
    -DROCKSDB_AUXV_GETAUXVAL_PRESENT -march=native -DHAVE_UINT128_EXTENSION \
    -DROCKSDB_JEMALLOC -DJEMALLOC_NO_DEMANGLE -O2 -fno-omit-frame-pointer \
    -momit-leaf-frame-pointer -Woverloaded-virtual -Wnon-virtual-dtor \
    -Wno-missing-field-initializers -Wno-invalid-offsetof -DROCKSDB_USE_RTTI -fPIC

# echo "2. 编译 GTest 和测试框架..."

# 编译 gtest
# g++ -c -o third-party/gtest-1.8.1/fused-src/gtest/gtest-all.o \
#     third-party/gtest-1.8.1/fused-src/gtest/gtest-all.cc \
#     -I./include -I. -Ithird-party/gtest-1.8.1/fused-src -std=c++17 -g

# # 编译 testutil
# g++ -c -o test_util/testutil.o test_util/testutil.cc \
#     -I./include -I. -Ithird-party/gtest-1.8.1/fused-src \
#     -std=c++17 -g -W -Wextra -Wall -Wsign-compare -Wshadow -Wunused-parameter \
#     -faligned-new -DHAVE_ALIGNED_NEW -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX \
#     -DOS_LINUX -fno-builtin-memcmp -DROCKSDB_FALLOCATE_PRESENT -DSNAPPY -DGFLAGS=1 \
#     -DZLIB -DBZIP2 -DNUMA -DTBB -DROCKSDB_MALLOC_USABLE_SIZE -DROCKSDB_PTHREAD_ADAPTIVE_MUTEX \
#     -DROCKSDB_BACKTRACE -DROCKSDB_RANGESYNC_PRESENT -DROCKSDB_SCHED_GETCPU_PRESENT \
#     -DROCKSDB_AUXV_GETAUXVAL_PRESENT -march=native -DHAVE_UINT128_EXTENSION \
#     -DROCKSDB_JEMALLOC -DJEMALLOC_NO_DEMANGLE -O2 -fno-omit-frame-pointer \
#     -momit-leaf-frame-pointer -Woverloaded-virtual -Wnon-virtual-dtor \
#     -Wno-missing-field-initializers -Wno-invalid-offsetof -DROCKSDB_USE_RTTI -fPIC

# # 编译 testharness
# g++ -c -o test_util/testharness.o test_util/testharness.cc \
#     -I./include -I. -Ithird-party/gtest-1.8.1/fused-src \
#     -std=c++17 -g -W -Wextra -Wall -Wsign-compare -Wshadow -Wunused-parameter \
#     -faligned-new -DHAVE_ALIGNED_NEW -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX \
#     -DOS_LINUX -fno-builtin-memcmp -DROCKSDB_FALLOCATE_PRESENT -DSNAPPY -DGFLAGS=1 \
#     -DZLIB -DBZIP2 -DNUMA -DTBB -DROCKSDB_MALLOC_USABLE_SIZE -DROCKSDB_PTHREAD_ADAPTIVE_MUTEX \
#     -DROCKSDB_BACKTRACE -DROCKSDB_RANGESYNC_PRESENT -DROCKSDB_SCHED_GETCPU_PRESENT \
#     -DROCKSDB_AUXV_GETAUXVAL_PRESENT -march=native -DHAVE_UINT128_EXTENSION \
#     -DROCKSDB_JEMALLOC -DJEMALLOC_NO_DEMANGLE -O2 -fno-omit-frame-pointer \
#     -momit-leaf-frame-pointer -Woverloaded-virtual -Wnon-virtual-dtor \
#     -Wno-missing-field-initializers -Wno-invalid-offsetof -DROCKSDB_USE_RTTI -fPIC

# # 最终链接
# echo "3. 编译测试文件..."
# g++ -g3 -O0 -fno-omit-frame-pointer -std=c++17 \
#     -I./include \
#     -I. \
#     -I/home/ysh/faiss \
#     -Ithird-party/gtest-1.8.1/fused-src \
#     -DFAISS_USE_LAPACK \
#     -DHAVE_ALIGNED_NEW -faligned-new \
#     -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX \
#     -DOS_LINUX -fno-builtin-memcmp -DROCKSDB_FALLOCATE_PRESENT \
#     -DSNAPPY -DGFLAGS=1 -DZLIB -DBZIP2 -DLZ4 -DZSTD -DNUMA -DTBB \
#     -DFAISS_ENABLE_GPU=OFF -DFAISS_ENABLE_RAFT=OFF \
#     utilities/secondary_index/faiss_ivf_index_test.cc \
#     faiss_ivf_index.o \
#     third-party/gtest-1.8.1/fused-src/gtest/gtest-all.o \
#     test_util/testutil.o \
#     test_util/testharness.o \
#     librocksdb_debug.a \
#     /home/ysh/faiss/build/faiss/libfaiss.so \
#     -Wl,-rpath=/home/ysh/faiss/build/faiss \
#     -lz -lbz2 -lsnappy -llz4 -lzstd -lnuma -ltbb \
#     -llapack -lblas -lgfortran \
#     -lpthread -ldl \
#     -o faiss_ivf_index_test

# echo "=== 编译完成 ==="
# echo "运行测试: ./faiss_ivf_index_test"

# echo "=== 编译完成 ==="
# echo "运行测试: ./faiss_ivf_index_test"


