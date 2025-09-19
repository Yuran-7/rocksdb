import numpy as np
import pandas as pd
import pywt

# =========================
# 1. 模拟数据库表 WebLog
# =========================
data = pd.DataFrame({
    "RequestID": range(1, 9),
    "Size": [200, 210, 190, 8000, 8050, 200, 195, 202]
})

print("原始数据:")
print(data)

# =========================
# 2. 构建频率向量 (直方图表示分布)
# =========================
domain_max = 8192       # 假设最大请求大小
bin_size = 64           # 分桶间隔
bins = np.arange(0, domain_max + bin_size, bin_size)

hist, _ = np.histogram(data["Size"], bins=bins)
signal = hist.astype(float)

# =========================
# 3. Haar 小波变换 + 压缩
# =========================
coeffs = pywt.wavedec(signal, "haar")

# 展平成一维数组，找阈值
flat_coeffs = np.concatenate([c for c in coeffs])
k = 10  # 只保留前 k 个重要系数
threshold = np.partition(np.abs(flat_coeffs), -k)[-k]

# 压缩：小于阈值的系数置 0
compressed_coeffs = [np.where(np.abs(c) >= threshold, c, 0) for c in coeffs]

# =========================
# 4. 小波逆变换（恢复分布）
# =========================
reconstructed_signal = pywt.waverec(compressed_coeffs, "haar")

# =========================
# 5. 对比原始与恢复分布
# =========================
df_compare = pd.DataFrame({
    "bin_start": bins[:-1],
    "bin_end": bins[1:],
    "original_count": signal,
    "reconstructed_count": np.round(reconstructed_signal[:len(signal)], 2)
})

print("\n原始分布 vs 压缩小波恢复分布 (前20行):")
print(df_compare.head(20))

# =========================
# 6. 模拟 SQL 查询 (范围查询基数估计)
# =========================
query_min, query_max = 100, 300

# 原始数据真实结果
true_count = ((data["Size"] >= query_min) & (data["Size"] <= query_max)).sum()

# 从直方图估计
bin_mask = (bins[:-1] >= query_min) & (bins[1:] <= query_max)
estimate_original = signal[bin_mask].sum()
estimate_reconstructed = reconstructed_signal[bin_mask].sum()

print(f"\nSQL 查询: SELECT * FROM WebLog WHERE Size BETWEEN {query_min} AND {query_max}")
print(f"真实结果行数       = {true_count}")
print(f"基于原始分布估计   = {estimate_original}")
print(f"基于压缩小波估计   = {estimate_reconstructed:.2f}")
