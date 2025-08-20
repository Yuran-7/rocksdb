#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
磁盘性能测试工具 (Python版本)
用于测试和对比不同服务器的磁盘性能
"""

import os
import sys
import time
import subprocess
import json
from datetime import datetime
import platform

class DiskBenchmark:
    def __init__(self, test_dir="./disk_test", file_size_mb=1024):
        self.test_dir = test_dir
        self.file_size_mb = file_size_mb
        self.file_size_bytes = file_size_mb * 1024 * 1024
        self.block_size = 1024 * 1024  # 1MB
        self.results = {}
        
        # 创建测试目录
        os.makedirs(test_dir, exist_ok=True)
        
    def get_system_info(self):
        """获取系统信息"""
        info = {
            "hostname": platform.node(),
            "os": platform.platform(),
            "python_version": platform.python_version(),
            "cpu_count": os.cpu_count(),
            "timestamp": datetime.now().isoformat()
        }
        
        # 获取内存信息
        try:
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        info["memory_total"] = line.split()[1] + " " + line.split()[2]
                        break
        except:
            info["memory_total"] = "Unknown"
            
        return info
    
    def clear_cache(self):
        """清理系统缓存"""
        try:
            os.system("sync")
            subprocess.run(["sudo", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"], 
                         capture_output=True, check=True)
            time.sleep(1)
        except:
            print("注意：清理缓存需要sudo权限，跳过缓存清理")
    
    def write_test(self, filename, use_sync=True):
        """写入测试"""
        filepath = os.path.join(self.test_dir, filename)
        
        start_time = time.time()
        
        # 使用dd命令进行写入测试
        sync_flag = "oflag=dsync" if use_sync else ""
        cmd = f"dd if=/dev/zero of={filepath} bs=1M count={self.file_size_mb} {sync_flag}"
        
        try:
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
            end_time = time.time()
            
            duration = end_time - start_time
            speed_mb_s = self.file_size_mb / duration if duration > 0 else 0
            
            return {
                "duration": duration,
                "speed_mb_s": speed_mb_s,
                "success": result.returncode == 0,
                "output": result.stderr if result.stderr else result.stdout
            }
        except Exception as e:
            return {
                "duration": 0,
                "speed_mb_s": 0,
                "success": False,
                "error": str(e)
            }
    
    def read_test(self, filename):
        """读取测试"""
        filepath = os.path.join(self.test_dir, filename)
        
        if not os.path.exists(filepath):
            return {"success": False, "error": "File not found"}
        
        start_time = time.time()
        
        cmd = f"dd if={filepath} of=/dev/null bs=1M"
        
        try:
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
            end_time = time.time()
            
            duration = end_time - start_time
            speed_mb_s = self.file_size_mb / duration if duration > 0 else 0
            
            return {
                "duration": duration,
                "speed_mb_s": speed_mb_s,
                "success": result.returncode == 0,
                "output": result.stderr if result.stderr else result.stdout
            }
        except Exception as e:
            return {
                "duration": 0,
                "speed_mb_s": 0,
                "success": False,
                "error": str(e)
            }
    
    def small_io_test(self, block_size_kb=4, num_operations=1000):
        """小块随机IO测试"""
        filename = f"small_io_test_{block_size_kb}k"
        filepath = os.path.join(self.test_dir, filename)
        
        # 写入测试
        start_time = time.time()
        try:
            cmd = f"dd if=/dev/zero of={filepath} bs={block_size_kb}k count={num_operations} oflag=dsync"
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
            write_time = time.time() - start_time
            write_iops = num_operations / write_time if write_time > 0 else 0
        except:
            write_time = 0
            write_iops = 0
        
        # 读取测试
        self.clear_cache()
        start_time = time.time()
        try:
            cmd = f"dd if={filepath} of=/dev/null bs={block_size_kb}k"
            result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
            read_time = time.time() - start_time
            read_iops = num_operations / read_time if read_time > 0 else 0
        except:
            read_time = 0
            read_iops = 0
        
        # 清理测试文件
        try:
            os.remove(filepath)
        except:
            pass
            
        return {
            "write_iops": write_iops,
            "read_iops": read_iops,
            "write_time": write_time,
            "read_time": read_time
        }
    
    def run_full_benchmark(self):
        """运行完整的磁盘性能测试"""
        print("=" * 50)
        print("磁盘性能测试开始")
        print("=" * 50)
        
        # 获取系统信息
        self.results["system_info"] = self.get_system_info()
        print(f"主机名: {self.results['system_info']['hostname']}")
        print(f"操作系统: {self.results['system_info']['os']}")
        print(f"CPU核心数: {self.results['system_info']['cpu_count']}")
        print(f"内存: {self.results['system_info']['memory_total']}")
        print()
        
        # 1. 顺序写入测试 (同步)
        print("1. 顺序写入测试 (同步)...")
        self.clear_cache()
        result = self.write_test("seq_write_sync.dat", use_sync=True)
        self.results["sequential_write_sync"] = result
        if result["success"]:
            print(f"   速度: {result['speed_mb_s']:.2f} MB/s")
        else:
            print(f"   错误: {result.get('error', 'Unknown error')}")
        
        # 2. 顺序读取测试
        print("2. 顺序读取测试...")
        self.clear_cache()
        result = self.read_test("seq_write_sync.dat")
        self.results["sequential_read"] = result
        if result["success"]:
            print(f"   速度: {result['speed_mb_s']:.2f} MB/s")
        else:
            print(f"   错误: {result.get('error', 'Unknown error')}")
        
        # 3. 顺序写入测试 (缓存)
        print("3. 顺序写入测试 (缓存)...")
        result = self.write_test("seq_write_cached.dat", use_sync=False)
        self.results["sequential_write_cached"] = result
        if result["success"]:
            print(f"   速度: {result['speed_mb_s']:.2f} MB/s")
        else:
            print(f"   错误: {result.get('error', 'Unknown error')}")
        
        # 4. 缓存读取测试
        print("4. 缓存读取测试...")
        result = self.read_test("seq_write_cached.dat")
        self.results["cached_read"] = result
        if result["success"]:
            print(f"   速度: {result['speed_mb_s']:.2f} MB/s")
        else:
            print(f"   错误: {result.get('error', 'Unknown error')}")
        
        # 5. 4K随机IO测试
        print("5. 4K随机IO测试...")
        result = self.small_io_test(block_size_kb=4, num_operations=1000)
        self.results["random_4k_io"] = result
        print(f"   4K写入IOPS: {result['write_iops']:.2f}")
        print(f"   4K读取IOPS: {result['read_iops']:.2f}")
        
        # 清理测试文件
        try:
            os.remove(os.path.join(self.test_dir, "seq_write_sync.dat"))
            os.remove(os.path.join(self.test_dir, "seq_write_cached.dat"))
        except:
            pass
        
        print()
        print("=" * 50)
        print("测试完成")
        print("=" * 50)
        
        return self.results
    
    def save_results(self, filename="disk_benchmark_results.json"):
        """保存测试结果到JSON文件"""
        filepath = os.path.join(self.test_dir, filename)
        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(self.results, f, indent=2, ensure_ascii=False)
        print(f"测试结果已保存到: {filepath}")
    
    def print_summary(self):
        """打印测试摘要"""
        print("\n测试摘要:")
        print("-" * 40)
        
        if "sequential_write_sync" in self.results:
            result = self.results["sequential_write_sync"]
            if result["success"]:
                print(f"顺序写入 (同步): {result['speed_mb_s']:.2f} MB/s")
        
        if "sequential_read" in self.results:
            result = self.results["sequential_read"]
            if result["success"]:
                print(f"顺序读取:        {result['speed_mb_s']:.2f} MB/s")
        
        if "sequential_write_cached" in self.results:
            result = self.results["sequential_write_cached"]
            if result["success"]:
                print(f"顺序写入 (缓存): {result['speed_mb_s']:.2f} MB/s")
        
        if "cached_read" in self.results:
            result = self.results["cached_read"]
            if result["success"]:
                print(f"缓存读取:        {result['speed_mb_s']:.2f} MB/s")
        
        if "random_4k_io" in self.results:
            result = self.results["random_4k_io"]
            print(f"4K随机写入:      {result['write_iops']:.2f} IOPS")
            print(f"4K随机读取:      {result['read_iops']:.2f} IOPS")

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="磁盘性能测试工具")
    parser.add_argument("--test-dir", default="./disk_test", help="测试目录 (默认: ./disk_test)")
    parser.add_argument("--file-size", type=int, default=1024, help="测试文件大小 MB (默认: 1024)")
    parser.add_argument("--save-results", action="store_true", help="保存结果到JSON文件")
    
    args = parser.parse_args()
    
    # 创建测试实例
    benchmark = DiskBenchmark(test_dir=args.test_dir, file_size_mb=args.file_size)
    
    # 运行测试
    results = benchmark.run_full_benchmark()
    
    # 打印摘要
    benchmark.print_summary()
    
    # 保存结果
    if args.save_results:
        benchmark.save_results()

if __name__ == "__main__":
    main()
