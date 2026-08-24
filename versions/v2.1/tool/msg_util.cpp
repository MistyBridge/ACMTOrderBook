// msg_util.cpp — 从 msg_util.h 中提取的实现
// 编译此文件时需添加 -fno-inline 以规避 MinGW 8.1.0 -O2 内联 bug
#include "msg_util.h"

// ---- parseKeyValueLine ----
std::unordered_map<std::string, int64_t> parseKeyValueLine(const std::string& line) {
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
