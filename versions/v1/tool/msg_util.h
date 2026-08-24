#pragma once
#include "axsbe_base.h"
#include "axsbe_order.h"
#include "axsbe_exe.h"
#include "axsbe_snap_stock.h"
#include <fstream>
#include <string>
#include <sstream>
#include <unordered_map>

// =====================================================================
//  文件读取器 + CYB 价格笼子公式
//  对应 Python tool/msg_util.py
// =====================================================================

// ---- CYB 价格笼子公式（参数为内部精度，2位小数）----
inline int32_t cybCageUpper(int32_t refPx) {
    // 买方笼子上限：大于此值被隐藏
    return refPx <= 24 ? refPx + 1 : (refPx * 102 + 50) / 100;
}

inline int32_t cybCageLower(int32_t refPx) {
    // 卖方笼子下限：小于此值被隐藏
    return refPx <= 25 ? refPx - 1 : (refPx * 98 + 50) / 100;
}

inline int32_t cybMatchUpper(int32_t refPx) {
    return (refPx * 110 + 50) / 100;
}

inline int32_t cybMatchLower(int32_t refPx) {
    return (refPx * 90 + 50) / 100;
}

// ---- 解析 //Key=Value 行 ----
inline std::unordered_map<std::string, int64_t> parseKeyValueLine(const std::string& line) {
    std::unordered_map<std::string, int64_t> dict;
    if (line.size() < 2 || line[0] != '/' || line[1] != '/') return dict;

    size_t pos = 2;
    while (pos < line.size()) {
        // 跳过空白
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
        size_t keyStart = pos;
        // 找 '='
        while (pos < line.size() && line[pos] != '=' && line[pos] != ' ') pos++;
        if (pos >= line.size() || line[pos] != '=' || pos == keyStart) {
            // 没有 '='，跳过这个 token
            while (pos < line.size() && line[pos] != ' ') pos++;
            continue;
        }
        std::string key = line.substr(keyStart, pos - keyStart);
        pos++; // 跳过 '='
        // 读取 value（整数）
        size_t valStart = pos;
        while (pos < line.size() && line[pos] != ' ') pos++;
        if (pos > valStart) {
            std::string valStr = line.substr(valStart, pos - valStart);
            try {
                dict[key] = std::stoll(valStr);
            } catch (...) {
                // 不是数字则跳过
            }
        }
    }
    return dict;
}

// =====================================================================
//  AxsbeFileReader — 逐行读取 .log 文件，返回消息对象
//  对应 Python axsbe_file() 生成器
// =====================================================================

class AxsbeFileReader {
public:
    AxsbeFileReader(const std::string& filename)
        : file_(filename), hasNext_(false) {
        advance();
    }

    bool hasNext() const { return hasNext_; }

    // 返回 MsgType，调用者根据类型取对应结构体
    // type=192 -> order 有效
    // type=191 -> exe   有效
    // type=111 -> snap  有效
    int next(AxsbeOrder& order, AxsbeExe& exe, AxsbeSnapStock& snap) {
        int type = currentType_;
        if (type == MsgType_order) {
            order = currentOrder_;
        } else if (type == MsgType_exe) {
            exe = currentExe_;
        } else if (type == MsgType_snap) {
            snap = currentSnap_;
        }
        advance();
        return type;
    }

private:
    std::ifstream file_;
    bool hasNext_;
    int currentType_;
    AxsbeOrder     currentOrder_;
    AxsbeExe       currentExe_;
    AxsbeSnapStock currentSnap_;

    void advance() {
        hasNext_ = false;
        std::string line;
        while (std::getline(file_, line)) {
            if (line.size() < 2 || line[0] != '/' || line[1] != '/') continue;

            auto dict = parseKeyValueLine(line);
            if (dict.find("MsgType") == dict.end()) continue;

            int msgType = static_cast<int>(dict["MsgType"]);
            currentType_ = msgType;

            if (msgType == MsgType_order) {
                currentOrder_ = AxsbeOrder{};
                currentOrder_.loadDict(dict);
                hasNext_ = true;
                return;
            } else if (msgType == MsgType_exe) {
                currentExe_ = AxsbeExe{};
                currentExe_.loadDict(dict);
                hasNext_ = true;
                return;
            } else if (msgType == MsgType_snap) {
                currentSnap_ = AxsbeSnapStock{};
                currentSnap_.loadDict(dict);
                hasNext_ = true;
                return;
            }
            // 其他 MsgType（如 11, 12）跳过
        }
    }
};
