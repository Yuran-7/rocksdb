# 磁盘性能测试工具使用说明

本工具包含两个版本的磁盘性能测试脚本，用于测试和对比不同服务器的磁盘性能。

## 文件说明

1. **disk_benchmark.sh** - Bash版本，功能更全面
2. **disk_benchmark.py** - Python版本，跨平台，输出格式化
3. **README_disk_benchmark.md** - 本说明文档

## 快速使用

### 方法1：使用Bash脚本 (推荐)
```bash
# 基本测试
./disk_benchmark.sh

# 指定测试目录
./examples/disk/disk_benchmark.sh /home/ysh/disk_test
```

### 方法2：使用Python脚本
```bash
# 基本测试
python3 ./examples/disk/disk_benchmark.py

# 指定参数
python3 ./examples/disk/disk_benchmark.py --test-dir /home/ysh/disk_test --file-size 1024 --save-results
```

## 测试项目

### 1. 顺序读写测试
- **顺序写入 (同步)**：测试大文件写入性能，使用fsync确保数据写入磁盘
- **顺序读取**：测试大文件读取性能
- **顺序写入 (缓存)**：测试使用缓存的写入性能
- **缓存读取**：测试从缓存读取的性能

### 2. 随机读写测试 (需要fio工具)
- **随机写入**：4K块随机写入测试
- **随机读取**：4K块随机读取测试
- **混合读写**：70%读取 + 30%写入的混合测试

### 3. IOPS测试
- **4K随机写入IOPS**：每秒输入/输出操作数
- **4K随机读取IOPS**：小文件处理能力

### 4. 延迟测试
- **磁盘延迟**：单次IO操作的响应时间

## 性能指标说明

### 吞吐量 (MB/s)
- **顺序读写**：反映大文件传输能力，重要用于视频、备份等场景
- **随机读写**：反映数据库、虚拟机等随机访问场景的性能

### IOPS (每秒操作数)
- **高IOPS**：适合数据库、高并发Web应用
- **低IOPS**：可能影响系统响应速度

### 延迟 (毫秒)
- **低延迟**：响应快，用户体验好
- **高延迟**：可能导致应用卡顿

## 对比建议

### 同等条件测试
1. **相同测试环境**：相同的文件大小、测试目录位置
2. **相同系统负载**：测试时关闭其他重IO应用
3. **多次测试**：运行3-5次取平均值，避免偶然因素

### 关键指标对比
```
服务器A vs 服务器B

顺序写入:    XXX MB/s  vs  XXX MB/s
顺序读取:    XXX MB/s  vs  XXX MB/s
4K随机写入:  XXX IOPS  vs  XXX IOPS
4K随机读取:  XXX IOPS  vs  XXX IOPS
平均延迟:    XXX ms    vs  XXX ms
```

### 硬件影响因素
1. **磁盘类型**：SSD > HDD，NVMe SSD > SATA SSD
2. **RAID配置**：RAID 0 > RAID 1 > RAID 5
3. **控制器**：硬件RAID > 软件RAID
4. **接口**：NVMe > SATA 3.0 > SATA 2.0

## 安装依赖 (可选)

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install fio ioping
```

### CentOS/RHEL
```bash
sudo yum install epel-release
sudo yum install fio ioping
```

## 注意事项

1. **权限要求**：清理缓存需要sudo权限
2. **磁盘空间**：确保测试目录有足够空间 (默认需要2GB)
3. **测试时间**：完整测试大约需要5-10分钟
4. **数据安全**：测试会创建临时文件，测试完成后自动清理

## 示例输出

```
========================================
      磁盘性能测试脚本
========================================
系统信息:
操作系统: Linux server1 5.4.0-74-generic
CPU信息: Intel(R) Xeon(R) CPU E5-2678 v3 @ 2.50GHz
内存信息: 16G
测试目录: ./disk_test

=== 1. 顺序写入测试 ===
正在执行: 顺序写入 (无缓存)
结果: 120.5 MB/s

=== 2. 顺序读取测试 ===
正在执行: 顺序读取 (无缓存)
结果: 145.2 MB/s

测试摘要:
----------------------------------------
顺序写入 (同步): 120.50 MB/s
顺序读取:        145.20 MB/s
4K随机写入:      2340.50 IOPS
4K随机读取:      3120.80 IOPS
```

## 故障排除

### 权限错误
```bash
# 如果出现权限错误，给脚本添加执行权限
chmod +x disk_benchmark.sh
chmod +x disk_benchmark.py
```

### 缓存清理失败
```bash
# 手动清理缓存
sync
sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
```

### Python版本要求
- Python 3.6 或更高版本
- 无需额外安装依赖包

通过这些测试，您可以客观地对比两台服务器的磁盘性能差异，确定是否为硬件问题。
