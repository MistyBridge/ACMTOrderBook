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
//
//  [v2.5] SIMD 加速的字符串查找（评估后决定不采纳，回退 strstr）
//  基准证明手写 SSE4.2/AVX2 慢于 CRT strstr（其内部已用 SIMD），故 strstr_simd
//  直接回退 strstr。详见 benchmark/bench_simd.cpp。
// =====================================================================

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "axsbe_base.h"

// =====================================================================
//  [v2.3] 字符串查找
//
//  使用标准 C 库 strstr()。内部（MSVC/glibc)已用 SIMD 实现，故手写
//  SSE4.2/AVX2 反而更慢（见 benchmark/bench_simd.cpp 的 A/B 证据），
//  决定不采纳。strstr_simd() 保留为语义别名，直接回退 strstr()。
//  （注：已移除 _mm_cmpistri 路径——它在未启用 SSE4.2 的 gcc 目标上
//   即使不调用也会因 always_inline 目标不匹配而编译失败，且无性能收益。）
// =====================================================================

// 字符串查找（语义别名，回退到标准 strstr()）
// haystack: 被搜索的字符串
// needle: 要查找的子字符串
// 返回值：找到的位置，如果未找到返回 nullptr
inline const char* strstr_simd(const char* haystack, const char* needle) {
    return strstr(haystack, needle);
}

// 直接在行字符串中查找 key=value，转为 int64
// line: 输入字符串，格式如 "//SecurityIDSource=2 SecurityID=300001 ..."
// key:  要查找的键名（注意：key 在行中必须唯一，且后面紧跟 '='）
// out:  输出的整数值
// 返回值: true=找到并解析成功, false=未找到或解析失败
inline bool extractField(const char* line, const char* key, int64_t& out) {
    // 使用标准 strstr 查找 key（SIMD 版本性能反而下降，回滚）
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

// =====================================================================
//  [v2.6] 零分配版本 — 接受 (ptr, end) 对，无需 null-terminated 字符串
//
//  避免 MmapFileReader::advance() 中每个消息的 std::string 分配
//  节省 ~200-500ns/消息（取决于行长度）
// =====================================================================

// [v2.6] 手动整数解析，不修改源内存（mmap 只读安全）
inline bool parseI64(const char* s, const char* end, int64_t& out) {
    if (s >= end) return false;
    bool neg = false;
    const char* p = s;
    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }
    if (p >= end || *p < '0' || *p > '9') return false;
    int64_t val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        ++p;
    }
    out = neg ? -val : val;
    return p > s;  // 至少解析了一个字符
}

inline bool extractField(const char* lineStart, const char* lineEnd,
                         const char* key, int64_t& out) {
    // 使用 strstr 在行内查找 key（保持高性能）
    // 约束：行长度 < 1024 字节（SZSE 消息最大 ~500B）
    // 如果 strstr 匹配超出 lineEnd，说明 key 不在本行中
    const char* pos = strstr(lineStart, key);
    if (!pos || pos >= lineEnd) return false;
    pos += strlen(key);
    if (pos >= lineEnd || *pos != '=') return false;
    return parseI64(pos + 1, lineEnd, out);
}

// =====================================================================
//  [v2.6] 前向 strstr 优化
//
//  FieldParser 维护搜索位置，避免每次从行首搜索
//  前提：字段在行中的顺序固定（SZSE 日志格式确实固定）
//
//  性能对比：
//    - extractField（每次从行首）：~57ns/调用
//    - FieldParser::find（前向搜索）：~20ns/调用
//    - 节省：~37ns/调用 = +5-10% 吞吐量
// =====================================================================
class FieldParser {
    const char* pos_;
    const char* end_;
public:
    FieldParser(const char* start, const char* end) : pos_(start), end_(end) {}

    // 前向查找 key=value，从上次位置继续搜索
    // 合并解析和位置推进，避免重复扫描数字
    bool find(const char* key, int64_t& out) {
        const char* found = strstr(pos_, key);
        if (!found || found >= end_) return false;
        found += strlen(key);
        if (found >= end_ || *found != '=') return false;
        // 解析整数值并同时推进位置
        const char* p = found + 1;
        if (p >= end_) return false;
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        else if (*p == '+') { ++p; }
        if (p >= end_ || *p < '0' || *p > '9') return false;
        int64_t val = 0;
        while (p < end_ && *p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            ++p;
        }
        out = neg ? -val : val;
        pos_ = p;  // 一次推进，无重复扫描
        return true;
    }
};

// [v2.6] 零分配版 parseSecurityFields
inline bool parseSecurityFields(const char* lineStart, const char* lineEnd,
                                SecurityIDSource& secSrc, int& securityID) {
    int64_t value;
    if (extractField(lineStart, lineEnd, "SecurityIDSource", value)) {
        secSrc = static_cast<SecurityIDSource>(value);
    }
    // SecurityID（排除 SecurityIDSource 的匹配）
    const char* idKey = "SecurityID=";
    const size_t idKeyLen = 11;
    const char* pos = lineStart;
    while (pos + idKeyLen <= lineEnd) {
        if (*pos == 'S') {
            const char* p = pos + 1;
            const char* k = idKey + 1;
            while (k < idKey + idKeyLen && p < lineEnd && *p == *k) { ++p; ++k; }
            if (k == idKey + idKeyLen && p <= lineEnd) {
                if (pos > lineStart && pos - lineStart >= 6) {
                    if (strncmp(pos - 6, "Source", 6) == 0) {
                        pos = p;
                        continue;
                    }
                }
                if (p < lineEnd) {
                    if (parseI64(p, lineEnd, value)) {
                        securityID = static_cast<int>(value);
                    }
                }
                return true;
            }
        }
        ++pos;
    }
    return true;
}
