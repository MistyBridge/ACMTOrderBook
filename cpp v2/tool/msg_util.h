#pragma once
#include "axsbe_base.h"
#include "axsbe_order.h"
#include "axsbe_exe.h"
#include "axsbe_snap_stock.h"
#include <fstream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <cstring>
#include <cstdlib>

// =====================================================================
//  文件读取器 + CYB 价格笼子公式
//  对应 Python tool/msg_util.py
// =====================================================================

// ---- CYB 价格笼子公式 (参数/返回均为内部价格精度 ×10^6; 笼子仅发生在深市创业板) ----
// 交易所规则 (深交所创业板连续竞价有效竞价范围):
//   买: min(基准价 × 102%, 基准价 + 0.01 元) 取上限侧 — 低价股按"加一分"兜底;
//   卖: max(基准价 × 98%,  基准价 - 0.01 元)。
// 原实现把 0.24/0.25 元写成字面量 24/25 (绑定 ×10^2 精度), 统一定点后
// 必须用 TICK_1CENT 表达, 否则低价分支恒真, 笼子退化为 ±1 个最小单位。
// 深市价格原生精度 ×10^6 (内部统一) → 1 分钱 (0.01 元) = 100000。
constexpr int64_t TICK_1CENT = 100000;   // 0.01 元 (×10^6 精度域)

// 阈值: 基准价低于此值时, ±2% 的幅度不足 1 分, 改用"加/减一分"。
// 0.01/0.02 = 0.5 元 → 上界阈值 0.24 元 (含) 沿用交易所口径。
constexpr int64_t CAGE_LOW_PX_UP = 24 * TICK_1CENT;     // 0.24 元
constexpr int64_t CAGE_LOW_PX_DN = 25 * TICK_1CENT;     // 0.25 元

inline int64_t cybCageUpper(int64_t refPx) {
    return refPx <= CAGE_LOW_PX_UP ? refPx + TICK_1CENT
                                   : (refPx * 102 + 50) / 100;
}

inline int64_t cybCageLower(int64_t refPx) {
    return refPx <= CAGE_LOW_PX_DN ? refPx - TICK_1CENT
                                   : (refPx * 98 + 50) / 100;
}

inline int64_t cybMatchUpper(int64_t refPx) {
    return (refPx * 110 + 50) / 100;
}

inline int64_t cybMatchLower(int64_t refPx) {
    return (refPx * 90 + 50) / 100;
}

// ---- 解析 //Key=Value 行（定义在 msg_util.cpp）----
std::unordered_map<std::string, int64_t> parseKeyValueLine(const std::string& line);

// =====================================================================
//  AxsbeFileReader — 逐行读取 .log 文件，返回消息对象
//  方法定义在 msg_util.cpp 中，避免 MinGW 8.1.0 -O2 内联 bug
// =====================================================================

class AxsbeFileReader {
public:
    AxsbeFileReader(const std::string& filename);
    bool hasNext() const { return hasNext_; }
    int next(AxsbeOrder& order, AxsbeExe& exe, AxsbeSnapStock& snap);
    void close() { file_.close(); }  // 显式关闭，规避 MinGW -O2 析构挂死

private:
    std::ifstream file_;
    bool hasNext_;
    int currentType_;
    AxsbeOrder     currentOrder_;
    AxsbeExe       currentExe_;
    AxsbeSnapStock currentSnap_;

    void advance();
};
