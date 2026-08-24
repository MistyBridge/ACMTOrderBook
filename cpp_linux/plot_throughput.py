# -*- coding: utf-8 -*-
# 吞吐量性能曲线 — 全版本本机实测 (T1 系统端到端)
# 数据来源: versions/ 各版本在真实 000001 2022-04-22 数据上的实测 (MinGW -O3 -march=native, 单次重放)
# 输出: 仓库根目录 throughput.png
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

# 中文字体
for f in ["Microsoft YaHei", "SimHei", "Noto Sans CJK SC"]:
    try:
        font_manager.findfont(f, fallback_to_default=False)
        plt.rcParams["font.sans-serif"] = [f]
        break
    except Exception:
        continue
plt.rcParams["axes.unicode_minus"] = False

# ---- 本机实测 T1 系统端到端吞吐 (000001 2022-04-22, 233,875 条, 单次重放) ----
labels = ["Python", "C++ v1", "C++ v2", "C++ v2.1", "C++ v2.2",
          "C++ v2.3", "C++ v2.4", "C++ v2.7", "C++ v2.8"]
vals = [1575, 102573, 106066, 102918, 267175,
        853527, 922082, 1226486, 1292270]

fig, ax = plt.subplots(figsize=(12, 6.5))
x = range(len(labels))
ax.plot(x, vals, "o-", color="#e74c3c", lw=2.2, ms=8,
        label="本机实测 T1 系统端到端 (000001 2022-04-22, 233,875 条)")
for xi, v in zip(x, vals):
    ax.annotate(f"{v:,}", (xi, v), textcoords="offset points",
                xytext=(0, 10), ha="center", fontsize=9, fontweight="bold",
                color="#e74c3c")

ax.set_xticks(list(x))
ax.set_xticklabels(labels, rotation=30, ha="right", fontsize=9)
ax.set_yscale("log")
ax.set_ylim(1000, 3e6)
ax.set_ylabel("吞吐量 (msg/s, 对数轴)", fontsize=11)
ax.set_title("ACMTOrderBook 版本演进实测 (T1 系统端到端)", fontsize=14, pad=14)
ax.grid(True, which="major", ls="--", alpha=0.4)
ax.legend(loc="upper left", fontsize=10)

fig.tight_layout()
fig.savefig("throughput.png", dpi=150)
print("OK: throughput.png 已生成")
