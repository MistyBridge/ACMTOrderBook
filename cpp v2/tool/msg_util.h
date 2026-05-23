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

// ---- CYB 价格笼子公式（参数为内部精度，2位小数）----
inline int32_t cybCageUpper(int32_t refPx) {
    return refPx <= 24 ? refPx + 1 : (refPx * 102 + 50) / 100;
}

inline int32_t cybCageLower(int32_t refPx) {
    return refPx <= 25 ? refPx - 1 : (refPx * 98 + 50) / 100;
}

inline int32_t cybMatchUpper(int32_t refPx) {
    return (refPx * 110 + 50) / 100;
}

inline int32_t cybMatchLower(int32_t refPx) {
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
