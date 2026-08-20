// ch_client.h — ClickHouse HTTP 客户端 (最小实现, WinHTTP / POSIX 原生套接字)
#pragma once
#include <string>
#include <memory>

namespace ch {

// 执行一条查询 (POST 到 /, 返回 TSV 文本), 失败抛 std::runtime_error。
// 全量物化: 结果整体驻留内存, 适合小结果集。
std::string query(const std::string& host, int port,
                  const std::string& user, const std::string& password,
                  const std::string& sql);

// 流式查询读取器: 构造即建立连接并发送 SQL, 随后逐行取结果 (行不含换行符)。
// 服务端边算边发, 本端边收边取 — 峰值内存与结果集大小无关。
// HTTP 错误 (非 200) 在首行读取时抛出 std::runtime_error。
// 不可拷贝 (独占连接); 析构关闭连接。
class QueryReader {
public:
    QueryReader(const std::string& host, int port,
                const std::string& user, const std::string& password,
                const std::string& sql);
    ~QueryReader();
    QueryReader(const QueryReader&) = delete;
    QueryReader& operator=(const QueryReader&) = delete;

    // 取下一行; 返回 false = 结果流结束
    bool nextLine(std::string& line);

    // 等待数据可读, 最多 timeoutMs 毫秒; true = 可读 (随后 nextLine 不会阻塞),
    // false = 超时。供读者线程轮询停止信号并保持服务端缓冲被持续消费。
    // Windows 端 WinHTTP 无干净的非阻塞探测, 恒真。
    bool waitData(int timeoutMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ch
