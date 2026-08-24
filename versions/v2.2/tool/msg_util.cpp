// msg_util.cpp — 从 msg_util.h 中提取的实现
// 编译此文件时需添加 -fno-inline 以规避 MinGW 8.1.0 -O2 内联 bug
#include "msg_util.h"

// ---- parseKeyValueLine ----
// [v2.3优化] 直接解析整数，避免临时 string 分配
// - key：仍需创建 string（map 键类型要求）
// - value：直接在原地解析整数，不创建临时 string
static inline int64_t parseIntFast(const char* data, size_t len) {
    if (len == 0) return 0;
    int64_t result = 0;
    bool negative = false;
    size_t i = 0;
    if (data[0] == '-') { negative = true; i = 1; }
    for (; i < len; ++i) {
        char c = data[i];
        if (c < '0' || c > '9') break;
        result = result * 10 + (c - '0');
    }
    return negative ? -result : result;
}

std::unordered_map<std::string, int64_t> parseKeyValueLine(const std::string& line) {
    std::unordered_map<std::string, int64_t> dict;
    if (line.size() < 2 || line[0] != '/' || line[1] != '/') return dict;

    const char* data = line.data();
    size_t len = line.size();
    size_t pos = 2;

    while (pos < len) {
        // 跳过空白
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) pos++;
        size_t keyStart = pos;
        // 找 '='
        while (pos < len && data[pos] != '=' && data[pos] != ' ') pos++;
        if (pos >= len || data[pos] != '=' || pos == keyStart) {
            while (pos < len && data[pos] != ' ') pos++;
            continue;
        }
        // 创建 key string（不可避免，map 键类型要求）
        std::string key(data + keyStart, pos - keyStart);
        pos++; // 跳过 '='
        // 直接解析整数，不创建临时 string
        size_t valStart = pos;
        while (pos < len && data[pos] != ' ') pos++;
        if (pos > valStart) {
            dict[key] = parseIntFast(data + valStart, pos - valStart);
        }
    }
    return dict;
}

// ---- AxsbeFileReader ----
AxsbeFileReader::AxsbeFileReader(const std::string& filename)
    : file_(filename), hasNext_(false) {
    advance();
}

int AxsbeFileReader::next(AxsbeOrder& order, AxsbeExe& exe, AxsbeSnapStock& snap) {
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

void AxsbeFileReader::advance() {
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
