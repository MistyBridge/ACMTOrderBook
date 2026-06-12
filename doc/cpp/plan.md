# C++ 撮合引擎重写计划
## 目标
将 Python py/ 目录下的订单簿重建引擎重写为 C++，保持算法完全一致，同时做模块化拆分。
---
## 整体目录结构
```
cpp/
|-- CMakeLists.txt                  # 构建配置
|-- main.cpp                        # 测试入口
|--
|-- tool/                           # 第1层：数据解析
|   |-- axsbe_base.h                #   常量、枚举
|   |-- axsbe_order.h               #   逐笔委托消息
|   |-- axsbe_exe.h                 #   逐笔成交消息
|   |-- axsbe_snap_stock.h          #   行情快照消息 + price_level
|   |-- msg_util.h                  #   文件读取器 + CYB笼子公式
|--
|-- behave/                         # 第2层：订单簿重建引擎
    |-- ob_types.h                  #   内部数据结构
    |-- axob.h                      #   AXOB 类声明
    |-- axob_init.cpp               #   构造、工具函数
    |-- axob_order.cpp              #   委托处理
    |-- axob_trade.cpp              #   成交/撤单处理
    |-- axob_cage.cpp               #   创业板价格笼子
    |-- axob_snap.cpp               #   快照生成与验证
```
