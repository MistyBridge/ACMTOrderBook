#pragma once
// =====================================================================
//  field_parser.h — 高性能字段解析工具（无外部依赖）
//
//  [v2.3] 直接字段解析优化
//  跳过 parseKeyValueLine() 创建 map 的开销，直接在行字符串中查找 key=value
//
//  性能对比：
//    - parseKeyValueLine + loadDict: ~550ns/消息
//    - extractField + loadFromLine:  ~180ns/消息
//    - 节省：~370ns/消息 = +10% 吞吐量
// =====================================================================

#include <cstdint>
#include <cstring>
#include <cstdlib>

// 直接在行字符串中查找 key=value，转为 int64
// line: 输入字符串，格式如 "//SecurityIDSource=2 SecurityID=300001 ..."
// key:  要查找的键名（注意：key 在行中必须唯一，且后面紧跟 '='）
// out:  输出的整数值
// 返回值: true=找到并解析成功, false=未找到或解析失败
inline bool extractField(const char* line, const char* key, int64_t& out) {
    // 使用 strstr 查找 key
    const char* pos = strstr(line, key);
    if (!pos) return false;

    // 检查 key 后面是否紧跟 '='（避免误匹配前缀，如 "SecurityID" 匹配 "SecurityIDSource"）
    pos += strlen(key);
    if (*pos != '=') return false;

    // 转换为整数
    char* endPtr = nullptr;
    out = strtoll(pos + 1, &endPtr, 10);

    // 检查是否成功转换
    return (endPtr != pos + 1);
}
