#include <gtest/gtest.h>
#include "tool/field_parser.h"
#include <cstdint>
#include <cstring>

TEST(FieldParser, ExtractFieldCString) {
    // 注意：extractField 用 strstr 找 key 并校验紧跟在 '=' 后。
    // "SecurityID" 是 "SecurityIDSource" 的前缀，真实行里 Source 在前，会命中前缀而非目标，
    // 因此 SecurityID 需走 parseSecurityFields（见 ParseSecurityFields）。此处只用唯一键。
    const char* line = "//MsgType=192 SecurityIDSource=102 SecurityID=300001 ApplSeqNum=123 Price=1054";
    int64_t v = 0;
    EXPECT_TRUE(extractField(line, "MsgType", v));
    EXPECT_EQ(v, 192);
    EXPECT_TRUE(extractField(line, "ApplSeqNum", v));
    EXPECT_EQ(v, 123);
    EXPECT_TRUE(extractField(line, "Price", v));
    EXPECT_EQ(v, 1054);
}

TEST(FieldParser, ExtractFieldPointerVersion) {
    const char* line = "MsgType=191 ChannelNo=1 ApplSeqNum=9";
    const char* end = line + std::strlen(line);
    int64_t v = 0;
    EXPECT_TRUE(extractField(line, end, "MsgType", v));
    EXPECT_EQ(v, 191);
    EXPECT_TRUE(extractField(line, end, "ApplSeqNum", v));
    EXPECT_EQ(v, 9);
}

TEST(FieldParser, ForwardParser) {
    const char* line = "ApplSeqNum=7 Price=1054 OrderQty=100";
    const char* end = line + std::strlen(line);
    FieldParser p(line, end);
    int64_t v = 0;
    EXPECT_TRUE(p.find("ApplSeqNum", v));
    EXPECT_EQ(v, 7);
    EXPECT_TRUE(p.find("Price", v));
    EXPECT_EQ(v, 1054);
    EXPECT_TRUE(p.find("OrderQty", v));
    EXPECT_EQ(v, 100);
}

TEST(FieldParser, ParseI64) {
    int64_t v = 0;
    EXPECT_TRUE(parseI64("12345", "12345" + 5, v));
    EXPECT_EQ(v, 12345);
    EXPECT_TRUE(parseI64("-42", "-42" + 3, v));
    EXPECT_EQ(v, -42);
    EXPECT_FALSE(parseI64("", "" + 0, v));
}

TEST(FieldParser, ParseSecurityFields) {
    const char* line = "SecurityIDSource=102 SecurityID=300001";
    const char* end = line + std::strlen(line);
    SecurityIDSource src = SecurityIDSource_NULL;
    int id = 0;
    parseSecurityFields(line, end, src, id);
    EXPECT_EQ(src, SecurityIDSource_SZSE);
    EXPECT_EQ(id, 300001);
}

TEST(FieldParser, SimdStrstr) {
    const char* hay = "//SecurityIDSource=102 SecurityID=300001";
    const char* needle = "SecurityID=";
    const char* r = strstr_simd(hay, needle);
    ASSERT_NE(r, nullptr);
    // must not match the "SecurityIDSource" suffix, i.e. positioned at the real SecurityID=
    EXPECT_EQ(std::strncmp(r, needle, std::strlen(needle)), 0);
}
