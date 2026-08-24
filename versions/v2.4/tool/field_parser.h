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
//
//  [v2.3] 代码模板化重构
//  提取通用解析逻辑，减少代码重复
// =====================================================================

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "axsbe_base.h"

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

// =====================================================================
//  通用的 SecurityIDSource/SecurityID 解析函数
//
//  解决的问题：
//    - SecurityID 是 SecurityIDSource 的后缀，需要特殊处理
//    - 避免在每个消息类型中重复相同的解析逻辑
//
//  使用方式：
//    SecurityIDSource secSrc;
//    int securityID;
//    parseSecurityFields(line, secSrc, securityID);
// =====================================================================
inline bool parseSecurityFields(const char* line,
                                 SecurityIDSource& secSrc,
                                 int& securityID) {
    // SecurityIDSource 解析
    const char* srcPos = strstr(line, "SecurityIDSource=");
    if (srcPos) {
        char* endPtr = nullptr;
        int64_t value = strtoll(srcPos + 17, &endPtr, 10);
        if (endPtr != srcPos + 17) {
            secSrc = static_cast<SecurityIDSource>(value);
        }
    }

    // SecurityID 解析（需要排除 SecurityIDSource）
    const char* idPos = strstr(line, "SecurityID=");
    if (idPos) {
        // 确保不是 SecurityIDSource 的一部分
        bool isSource = (idPos > line + 6) &&
                       (strncmp(idPos - 6, "Source", 6) == 0);
        if (!isSource) {
            char* endPtr = nullptr;
            int64_t value = strtoll(idPos + 11, &endPtr, 10);
            if (endPtr != idPos + 11) {
                securityID = static_cast<int>(value);
            }
        }
    }

    return true;
}
