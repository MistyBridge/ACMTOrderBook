# 订单簿重建算法 Python 模型

用 Python 实现的订单簿重建引擎（对应 C++ 引擎 `cpp v2/behave/` 的算法原型）。

## 结构

* `main.py` — 入口：读 L2 历史文件并回放重建订单簿
* `behave/` — 订单簿引擎核心（axob.py 等）
* `tool/` — 行情解析工具（axsbe_*.py 消息结构、msg_util.py 读取 L2 历史文件、ch_to_axsbe.py 从 ClickHouse 导出）

## 执行

```bash
cd py
python main.py ../data/20220422/AX_sbe_szse_000001.log 1
```

## 与 C++ 版关系

`cpp v1/`、`cpp v2/` 为本算法的 C++ 重写；`cpp_linux/` 为 ClickHouse 直连回放版
（性能对比见仓库根 README）。
