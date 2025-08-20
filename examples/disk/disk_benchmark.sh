#!/bin/bash

# 磁盘性能测试脚本
# 用于测试服务器磁盘的读写速度
# Usage: ./disk_benchmark.sh [test_directory]

# 设置测试参数
TEST_DIR="${1:-./disk_test}"
TEST_FILE="$TEST_DIR/testfile"
FILE_SIZE="1G"        # 测试文件大小
BLOCK_SIZE="1M"       # 块大小
SYNC_MODE="oflag=dsync"  # 同步写入模式

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}      磁盘性能测试脚本${NC}"
echo -e "${BLUE}========================================${NC}"

# 获取系统信息
echo -e "${YELLOW}系统信息:${NC}"
echo "操作系统: $(uname -a)"
echo "CPU信息: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d':' -f2 | xargs)"
echo "内存信息: $(free -h | grep 'Mem:' | awk '{print $2}')"
echo "测试目录: $TEST_DIR"
echo "测试文件大小: $FILE_SIZE"
echo

# 创建测试目录
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

# 获取磁盘信息
DISK_INFO=$(df -h . | tail -1)
echo -e "${YELLOW}磁盘信息:${NC}"
echo "$DISK_INFO"
echo

# 清理缓存函数
clear_cache() {
    echo "清理系统缓存..."
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || echo "注意：需要sudo权限清理缓存"
    sleep 2
}

# 测试函数
run_test() {
    local test_name="$1"
    local command="$2"
    echo -e "${GREEN}正在执行: $test_name${NC}"
    echo "命令: $command"
    
    # 运行测试并捕获输出
    result=$(eval $command 2>&1)
    echo "$result"
    
    # 提取速度信息
    speed=$(echo "$result" | grep -E "(MB/s|GB/s)" | tail -1)
    if [ -n "$speed" ]; then
        echo -e "${BLUE}结果: $speed${NC}"
    fi
    echo
}

echo -e "${YELLOW}开始磁盘性能测试...${NC}"
echo

# 1. 顺序写入测试
echo -e "${GREEN}=== 1. 顺序写入测试 ===${NC}"
clear_cache
run_test "顺序写入 (无缓存)" "dd if=/dev/zero of=testfile_write bs=$BLOCK_SIZE count=1024 $SYNC_MODE"

# 2. 顺序读取测试
echo -e "${GREEN}=== 2. 顺序读取测试 ===${NC}"
clear_cache
run_test "顺序读取 (无缓存)" "dd if=testfile_write of=/dev/null bs=$BLOCK_SIZE"

# 3. 缓存写入测试
echo -e "${GREEN}=== 3. 缓存写入测试 ===${NC}"
run_test "缓存写入" "dd if=/dev/zero of=testfile_cached bs=$BLOCK_SIZE count=1024"

# 4. 缓存读取测试
echo -e "${GREEN}=== 4. 缓存读取测试 ===${NC}"
run_test "缓存读取" "dd if=testfile_cached of=/dev/null bs=$BLOCK_SIZE"

# 5. 随机读写测试 (如果有fio工具)
if command -v fio &> /dev/null; then
    echo -e "${GREEN}=== 5. 随机读写测试 (使用fio) ===${NC}"
    
    # 随机写入测试
    clear_cache
    echo -e "${BLUE}随机写入测试:${NC}"
    fio --name=random_write --ioengine=libaio --iodepth=4 --rw=randwrite --bs=4k --direct=1 --size=512m --numjobs=1 --runtime=30 --group_reporting --filename=fio_test_file

    # 随机读取测试
    clear_cache
    echo -e "${BLUE}随机读取测试:${NC}"
    fio --name=random_read --ioengine=libaio --iodepth=4 --rw=randread --bs=4k --direct=1 --size=512m --numjobs=1 --runtime=30 --group_reporting --filename=fio_test_file

    # 混合读写测试
    clear_cache
    echo -e "${BLUE}混合读写测试 (70% 读, 30% 写):${NC}"
    fio --name=mixed_rw --ioengine=libaio --iodepth=4 --rw=randrw --rwmixread=70 --bs=4k --direct=1 --size=512m --numjobs=1 --runtime=30 --group_reporting --filename=fio_test_file
else
    echo -e "${YELLOW}注意: 未安装fio工具，跳过随机读写测试${NC}"
    echo "安装命令: sudo apt-get install fio (Ubuntu/Debian) 或 sudo yum install fio (CentOS/RHEL)"
fi

# 6. IOPS测试 (小块随机读写)
echo -e "${GREEN}=== 6. IOPS测试 (4K随机读写) ===${NC}"
clear_cache
run_test "4K随机写入" "dd if=/dev/zero of=testfile_4k bs=4k count=10000 $SYNC_MODE"
clear_cache
run_test "4K随机读取" "dd if=testfile_4k of=/dev/null bs=4k"

# 7. 延迟测试
echo -e "${GREEN}=== 7. 磁盘延迟测试 ===${NC}"
if command -v ioping &> /dev/null; then
    echo "测试磁盘延迟 (10次测试):"
    ioping -c 10 .
else
    echo "使用dd测试写入延迟:"
    for i in {1..5}; do
        echo "测试 $i:"
        time dd if=/dev/zero of=latency_test_$i bs=4k count=1 oflag=dsync 2>&1 | grep real
    done
fi

# 清理测试文件
echo -e "${YELLOW}清理测试文件...${NC}"
rm -f testfile_write testfile_cached fio_test_file testfile_4k latency_test_*

# 生成测试报告
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}      测试完成${NC}"
echo -e "${BLUE}========================================${NC}"

# 显示磁盘使用情况
echo -e "${YELLOW}当前磁盘使用情况:${NC}"
df -h .

echo
echo -e "${GREEN}性能测试建议:${NC}"
echo "1. 顺序读写速度反映大文件传输性能"
echo "2. 随机读写速度反映数据库等应用性能"
echo "3. IOPS (每秒输入/输出操作数) 反映小文件处理能力"
echo "4. 延迟反映响应速度"
echo
echo "对比不同服务器时，请在相同负载条件下进行测试"
echo "建议运行多次测试取平均值以获得更准确的结果"
