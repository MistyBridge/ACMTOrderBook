// ch_client.cpp — ClickHouse HTTP 客户端
//   Windows: WinHTTP; Linux/POSIX: 原生套接字 (HTTP/1.1, 最小实现)
//
// 两个层次:
//   query()       全量物化 (旧接口, 小结果集)
//   QueryReader   流式逐行 (大结果集: 边收边取, 峰值内存与结果集大小无关)
#include "ch_client.h"
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <utility>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <winhttp.h>
    #pragma comment(lib, "winhttp.lib")
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <poll.h>
    #include <cerrno>
#endif

namespace ch {

// ---- base64 (Basic 认证) ----
static std::string base64(const std::string& in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = (unsigned char)in[i] << 16;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

#ifdef _WIN32

// ==================== Windows: WinHTTP ====================

std::string query(const std::string& host, int port,
                  const std::string& user, const std::string& password,
                  const std::string& sql)
{
    std::wstring whost(host.begin(), host.end());

    HINTERNET hSession = WinHttpOpen(L"ACMTOrderBook/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) throw std::runtime_error("WinHttpOpen failed");

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); throw std::runtime_error("WinHttpConnect failed"); }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/", nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    std::wstring wuser(user.begin(), user.end());
    std::wstring wpass(password.begin(), password.end());
    WinHttpSetCredentials(hRequest, WINHTTP_AUTH_TARGET_SERVER,
                          WINHTTP_AUTH_SCHEME_BASIC, wuser.c_str(), wpass.c_str(), nullptr);

    std::wstring whdr = L"Content-Type: text/plain\r\n";
    BOOL ok = WinHttpSendRequest(hRequest, whdr.c_str(), (DWORD)-1,
                                 (LPVOID)sql.data(), (DWORD)sql.size(),
                                 (DWORD)sql.size(), 0);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpSendRequest failed");
    }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error("WinHttpReceiveResponse failed");
    }

    std::string body;
    body.reserve(4 * 1024 * 1024);
    char buf[256 * 1024];
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        DWORD want = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, buf, want, &got) || got == 0) break;
        body.append(buf, got);
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (status != 200) {
        std::ostringstream oss;
        oss << "ClickHouse query failed, HTTP " << status;
        if (body.size() > 300) body.resize(300);
        oss << ": " << body;
        throw std::runtime_error(oss.str());
    }
    return body;
}

struct QueryReader::Impl {
    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    std::string buf;      // 已接收未消费字节
    size_t pos = 0;       // buf 内已消费偏移 (行首)
    bool eof = false;
};

QueryReader::QueryReader(const std::string& host, int port,
                         const std::string& user, const std::string& password,
                         const std::string& sql)
    : impl_(new Impl)
{
    Impl& im = *impl_;
    std::wstring whost(host.begin(), host.end());

    im.hSession = WinHttpOpen(L"ACMTOrderBook/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!im.hSession) throw std::runtime_error("WinHttpOpen failed");

    im.hConnect = WinHttpConnect(im.hSession, whost.c_str(), (INTERNET_PORT)port, 0);
    if (!im.hConnect) throw std::runtime_error("WinHttpConnect failed");

    im.hRequest = WinHttpOpenRequest(im.hConnect, L"POST", L"/", nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!im.hRequest) throw std::runtime_error("WinHttpOpenRequest failed");

    std::wstring wuser(user.begin(), user.end());
    std::wstring wpass(password.begin(), password.end());
    WinHttpSetCredentials(im.hRequest, WINHTTP_AUTH_TARGET_SERVER,
                          WINHTTP_AUTH_SCHEME_BASIC, wuser.c_str(), wpass.c_str(), nullptr);

    std::wstring whdr = L"Content-Type: text/plain\r\n";
    if (!WinHttpSendRequest(im.hRequest, whdr.c_str(), (DWORD)-1,
                            (LPVOID)sql.data(), (DWORD)sql.size(), (DWORD)sql.size(), 0))
        throw std::runtime_error("WinHttpSendRequest failed");

    if (!WinHttpReceiveResponse(im.hRequest, nullptr))
        throw std::runtime_error("WinHttpReceiveResponse failed");

    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(im.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        std::string body;
        char tmp[1024];
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(im.hRequest, &avail) && avail > 0 && body.size() < 300) {
            DWORD want = avail > (DWORD)sizeof(tmp) ? (DWORD)sizeof(tmp) : avail;
            DWORD got = 0;
            if (!WinHttpReadData(im.hRequest, tmp, want, &got) || got == 0) break;
            body.append(tmp, got);
        }
        std::ostringstream oss;
        oss << "ClickHouse query failed, HTTP " << status << ": " << body;
        throw std::runtime_error(oss.str());
    }
}

QueryReader::~QueryReader() {
    Impl& im = *impl_;
    if (im.hRequest) WinHttpCloseHandle(im.hRequest);
    if (im.hConnect) WinHttpCloseHandle(im.hConnect);
    if (im.hSession) WinHttpCloseHandle(im.hSession);
}

bool QueryReader::nextLine(std::string& line) {
    Impl& im = *impl_;
    for (;;) {
        size_t p = im.buf.find('\n', im.pos);
        if (p != std::string::npos) {
            line = im.buf.substr(im.pos, p - im.pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            im.pos = p + 1;
            return true;
        }
        if (im.pos > 0) { im.buf.erase(0, im.pos); im.pos = 0; }
        if (im.eof) {
            // 末尾残缺行照常吐出 (CH 每行以 \n 结尾, 正常不会走到)
            if (!im.buf.empty()) {
                line = std::move(im.buf);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                im.buf.clear();
                return true;
            }
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(im.hRequest, &avail))
            throw std::runtime_error("WinHttpQueryDataAvailable failed");
        if (avail == 0) { im.eof = true; continue; }
        char tmp[65536];
        DWORD want = avail > (DWORD)sizeof(tmp) ? (DWORD)sizeof(tmp) : avail;
        DWORD got = 0;
        if (!WinHttpReadData(im.hRequest, tmp, want, &got) || got == 0) {
            im.eof = true;
            continue;
        }
        im.buf.append(tmp, got);
    }
}

bool QueryReader::waitData(int) {
    // WinHTTP 无非阻塞可读探测 (WinHttpQueryDataAvailable 会阻塞), 恒真 —
    // 读者线程直接进入阻塞读, 停止信号依赖关闭句柄 (见 clickhouse_source)
    return true;
}

#else  // ==================== POSIX (Linux) ====================

std::string query(const std::string& host, int port,
                  const std::string& user, const std::string& password,
                  const std::string& sql)
{
    // 解析地址
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res)
        throw std::runtime_error("getaddrinfo failed for " + host);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); throw std::runtime_error("socket failed"); }
    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        ::close(fd);
        throw std::runtime_error("connect failed to " + host);
    }
    freeaddrinfo(res);

    std::string auth = "Authorization: Basic " + base64(user + ":" + password) + "\r\n";
    std::string req;
    req.reserve(sql.size() + 512);
    // HTTP/1.0: ClickHouse 对 1.1 默认返回 chunked 传输编码, 1.0 则返回原始正文
    req += "POST / HTTP/1.0\r\n";
    req += "Host: " + host + ":" + portStr + "\r\n";
    req += "Content-Type: text/plain\r\n";
    req += auth;
    req += "Content-Length: " + std::to_string(sql.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += sql;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { ::close(fd); throw std::runtime_error("send failed"); }
        sent += (size_t)n;
    }

    std::string body;
    body.reserve(4 * 1024 * 1024);
    char buf[256 * 1024];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, (size_t)n);
    }
    ::close(fd);

    // 分离 HTTP 头与正文
    size_t pos = body.find("\r\n\r\n");
    if (pos == std::string::npos) throw std::runtime_error("bad HTTP response");
    std::string head = body.substr(0, pos);
    std::string content = body.substr(pos + 4);

    if (head.find(" 200 ") == std::string::npos) {
        std::ostringstream oss;
        oss << "ClickHouse query failed: " << head.substr(0, head.find("\r\n"));
        if (content.size() > 300) content.resize(300);
        oss << ": " << content;
        throw std::runtime_error(oss.str());
    }
    return content;
}

struct QueryReader::Impl {
    int fd = -1;
    std::string buf;      // 已接收未消费字节
    size_t pos = 0;       // buf 内已消费偏移 (行首)
    bool headerDone = false;
    bool eof = false;
};

QueryReader::QueryReader(const std::string& host, int port,
                         const std::string& user, const std::string& password,
                         const std::string& sql)
    : impl_(new Impl)
{
    Impl& im = *impl_;
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res)
        throw std::runtime_error("getaddrinfo failed for " + host);

    im.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (im.fd < 0) { freeaddrinfo(res); throw std::runtime_error("socket failed"); }
    if (connect(im.fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        ::close(im.fd);
        im.fd = -1;
        throw std::runtime_error("connect failed to " + host);
    }
    freeaddrinfo(res);

    std::string auth = "Authorization: Basic " + base64(user + ":" + password) + "\r\n";
    std::string req;
    req.reserve(sql.size() + 512);
    // HTTP/1.0 + Connection: close: CH 返回原始正文 (无 chunked), 连接关闭即流结束,
    // 正文可边收边解析。
    req += "POST / HTTP/1.0\r\n";
    req += "Host: " + host + ":" + portStr + "\r\n";
    req += "Content-Type: text/plain\r\n";
    req += auth;
    req += "Content-Length: " + std::to_string(sql.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += sql;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = ::send(im.fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { ::close(im.fd); im.fd = -1; throw std::runtime_error("send failed"); }
        sent += (size_t)n;
    }
}

QueryReader::~QueryReader() {
    Impl& im = *impl_;
    if (im.fd >= 0) ::close(im.fd);
}

bool QueryReader::waitData(int timeoutMs) {
    Impl& im = *impl_;
    if (im.eof || !im.headerDone) return true;   // 头未收齐时视为可读, 交给 nextLine
    struct pollfd p{ im.fd, POLLIN, 0 };
    int r = ::poll(&p, 1, timeoutMs);
    return (r > 0 && (p.revents & POLLIN));
}

bool QueryReader::nextLine(std::string& line) {
    Impl& im = *impl_;
    for (;;) {
        // 1) 剥 HTTP 头 (仅首段数据)
        if (!im.headerDone) {
            size_t m = im.buf.find("\r\n\r\n");
            if (m == std::string::npos) {
                // 头未收齐 → 继续收
                char tmp[65536];
                ssize_t n = ::recv(im.fd, tmp, sizeof(tmp), 0);
                if (n <= 0) throw std::runtime_error("ClickHouse: 响应头不完整或连接中断");
                im.buf.append(tmp, (size_t)n);
                continue;
            }
            std::string head = im.buf.substr(0, m);
            if (head.find(" 200 ") == std::string::npos) {
                std::string rest = im.buf.substr(m + 4);
                if (rest.size() > 300) rest.resize(300);
                throw std::runtime_error("ClickHouse query failed: " +
                                         head.substr(0, head.find("\r\n")) + ": " + rest);
            }
            im.buf.erase(0, m + 4);
            im.headerDone = true;
            // 落到行扫描
        }
        // 2) 行扫描
        size_t p = im.buf.find('\n', im.pos);
        if (p != std::string::npos) {
            line = im.buf.substr(im.pos, p - im.pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            im.pos = p + 1;
            return true;
        }
        // 3) 无完整行: 压缩已消费部分, 再收一批
        if (im.pos > 0) { im.buf.erase(0, im.pos); im.pos = 0; }
        if (im.eof) {
            // 末尾残缺行照常吐出 (CH 每行以 \n 结尾, 正常不会走到)
            if (!im.buf.empty()) {
                line = std::move(im.buf);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                im.buf.clear();
                return true;
            }
            return false;
        }
        char tmp[65536];
        ssize_t n = ::recv(im.fd, tmp, sizeof(tmp), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error("recv failed");
        if (n == 0) { im.eof = true; continue; }
        im.buf.append(tmp, (size_t)n);
    }
}

#endif  // _WIN32

} // namespace ch
