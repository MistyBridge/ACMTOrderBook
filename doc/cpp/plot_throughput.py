import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# 版本数据
versions = ['Python', 'C++ v1', 'v2.1', 'v2.2', 'v2.3', 'v2.4', 'v2.5', 'v2.6', 'v2.7', 'v2.8']
throughput = [4109, 64613, 223410, 433332, 1102511, 1087261, 1065053, 1240251, 1224407, 1339869]

# 创建图形
fig, ax = plt.subplots(figsize=(14, 8))

# 绘制折线图
x = np.arange(len(versions))
ax.plot(x, throughput, 'b-o', linewidth=2, markersize=8, label='Throughput')

# 填充区域
ax.fill_between(x, throughput, alpha=0.3, color='blue')

# 设置标题和标签
ax.set_title('ACMTOrderBook Performance Evolution\nThroughput (msg/s)', fontsize=16, fontweight='bold')
ax.set_xlabel('Version', fontsize=12)
ax.set_ylabel('Throughput (msg/s)', fontsize=12)

# 设置x轴刻度
ax.set_xticks(x)
ax.set_xticklabels(versions, rotation=45, ha='right')

# 设置y轴格式
ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, p: format(int(x), ',')))

# 添加网格
ax.grid(True, linestyle='--', alpha=0.7)

# 添加数据标签
for i, (v, t) in enumerate(zip(versions, throughput)):
    ax.annotate(f'{t:,}', (i, t), textcoords="offset points", xytext=(0, 15), 
                ha='center', fontsize=9, fontweight='bold')

# 添加性能提升注释
ax.annotate('+32,509%\n(325x)', xy=(9, 1339869), xytext=(7, 1400000),
            arrowprops=dict(arrowstyle='->', color='red'),
            fontsize=12, fontweight='bold', color='red')

# 设置y轴范围
ax.set_ylim(0, 1500000)

# 添加图例
ax.legend(loc='upper left')

# 调整布局
plt.tight_layout()

# 保存图片
plt.savefig('doc/cpp/throughput_evolution.png', dpi=150, bbox_inches='tight')
print("图片已保存: doc/cpp/throughput_evolution.png")

# 显示图片
plt.show()
