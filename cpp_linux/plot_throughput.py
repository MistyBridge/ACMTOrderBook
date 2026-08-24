# -*- coding: utf-8 -*-
# 吞吐量性能曲线 (历史基准 + 本机实测 T1 系统端到端)
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

# ---- 历史基准 (原项目, py 仪表盘 Windows 环境, 未复测) ----
hist_labels = ["Python", "C++ v1", "v2.1", "v2.2", "v2.3", "v2.4",
               "v2.5", "v2.6", "v2.7", "v2.8"]
hist_vals = [4109, 64613, 223410, 433332, 1102511, 1087261,
             1065053, 1240251, 1224407, 1339869]

# ---- 本机实测 (T1 系统端到端吞吐, 000001 2022-04-22 233,875 条) ----
cur_labels = ["Python 实测", "C++ v1 实测", "C++ v2 实测"]
cur_vals = [1529, 159434, 1330000]

fig, ax = plt.subplots(figsize=(12, 6.5))
x = range(len(hist_labels))
ax.plot(x, hist_vals, "o-", color="#4472c4", lw=1.8, ms=6,
        label="历史基准 (原项目, py 仪表盘 Windows 环境, 未复测)")
for xi, v in zip(x, hist_vals):
    ax.annotate(f"{v:,}", (xi, v), textcoords="offset points",
                xytext=(0, 8), ha="center", fontsize=8, color="#4472c4")

# 实测段
cx = [len(hist_labels), len(hist_labels) + 1, len(hist_labels) + 2]
ax.plot(cx, cur_vals, "s-", color="#e74c3c", lw=2.2, ms=8,
        label="本机实测 T1 系统端到端 (000001 2022-04-22, 233,875 条)")
for xi, v in zip(cx, cur_vals):
    ax.annotate(f"{v:,}", (xi, v), textcoords="offset points",
                xytext=(0, 10), ha="center", fontsize=10, fontweight="bold",
                color="#e74c3c")

all_labels = hist_labels + cur_labels
ax.set_xticks(list(x) + list(cx))
ax.set_xticklabels(all_labels, rotation=30, ha="right", fontsize=9)
ax.set_yscale("log")
ax.set_ylim(1000, 2e7)
ax.set_ylabel("吞吐量 (msg/s, 对数轴)", fontsize=11)
ax.set_title("ACMTOrderBook 吞吐量性能曲线 (T1 系统端到端)", fontsize=14, pad=14)
ax.grid(True, which="major", ls="--", alpha=0.4)
ax.legend(loc="upper left", fontsize=10)

fig.tight_layout()
fig.savefig("throughput.png", dpi=150)
print("OK: throughput.png 已生成")
