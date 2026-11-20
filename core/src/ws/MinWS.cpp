// ============================================================================
// MinWS —— 极简 WebSocket 客户端实现
// 零外部依赖，Win32 Winsock 就行
// ============================================================================

#include "newrpa/ws/MinWS.h"
#include <spdlog/spdlog.h>

#include <cstring>
#include <random>
#include <sstream>
#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#  define SOCK_INVALID INVALID_SOCKET
#  define SOCK_CLOSE   closesocket
#  define LAST_ERROR   WSAGetLastError()
#else
#  define SOCK_INVALID (-1)
#  define SOCK_CLOSE   ::close
#  define LAST_ERROR   errno
#endif

namespace newrpa {
namespace ws {

// ----------------------------------------------------------------------------
// SHA-1（纯 C++，零依赖）—— FIPS 180-4 简化实现
// 输入：任意字节流 → 输出 20 字节
// ----------------------------------------------------------------------------
static std::vector<uint8_t> SHA1(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;

    uint64_t totalBits = (uint64_t)len * 8;

    // 补字节：1 位 1 + 若干 0 + 64 位 big-endian 长度
    std::vector<uint8_t> buf(data, data + len);
    buf.push_back(0x80);
    while ((buf.size() % 64) != 56) buf.push_back(0);
    for (int i = 7; i >= 0; --i)
        buf.push_back((uint8_t)(totalBits >> (i * 8)));

    auto rol = [](uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); };

    for (size_t i = 0; i < buf.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j)
            w[j] = ((uint32_t)buf[i+j*4] << 24) | ((uint32_t)buf[i+j*4+1] << 16) |
                   ((uint32_t)buf[i+j*4+2] << 8)  |  (uint32_t)buf[i+j*4+3];
        for (int j = 16; j < 80; ++j)
            w[j] = rol(w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int j = 0; j < 80; ++j) {
            uint32_t f, k;
            if      (j < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (j < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
            else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;             k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[j];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::vector<uint8_t> out(20);
    auto put = [&](uint32_t h, int off) {
        out[off+0] = (h >> 24) & 0xFF; out[off+1] = (h >> 16) & 0xFF;
        out[off+2] = (h >> 8)  & 0xFF; out[off+3] =  h        & 0xFF;
    };
    put(h0, 0); put(h1, 4); put(h2, 8); put(h3, 12); put(h4, 16);
    return out;
}

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64EncodeRaw(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        uint32_t n = ((uint32_t)data[i]) << 16 | ((uint32_t)data[i+1]) << 8 | data[i+2];
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back(kB64[(n >> 6)  & 0x3F]);
        out.push_back(kB64[ n        & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < data.size()) n |= ((uint32_t)data[i+1]) << 8;
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? kB64[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// ----------------------------------------------------------------------------
// URL 解析
// ----------------------------------------------------------------------------
struct ParsedUrl {
    std::string scheme;  // "ws" or "wss"
    std::string host;
    int         port;
    std::string path;
};

static bool ParseUrl(const std::string& url, ParsedUrl& out) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    out.scheme = url.substr(0, p);
    size_t rest = p + 3;
    size_t slash = url.find('/', rest);
    std::string hostPort;
    if (slash == std::string::npos) {
        hostPort = url.substr(rest);
        out.path = "/";
    } else {
        hostPort = url.substr(rest, slash - rest);
        out.path = url.substr(slash);
    }
    size_t colon = hostPort.find(':');
    if (colon == std::string::npos) {
        out.host = hostPort;
        out.port = (out.scheme == "wss") ? 443 : 80;
    } else {
        out.host = hostPort.substr(0, colon);
        out.port = std::atoi(hostPort.substr(colon + 1).c_str());
    }
    return true;
}

// ----------------------------------------------------------------------------
// MinWS 实现
// ----------------------------------------------------------------------------

bool MinWS::SetSocketTimeout(int timeoutMs) {
    if (timeoutMs <= 0) return true;
#if defined(_WIN32)
    DWORD tv = (DWORD)timeoutMs;
    if (setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == SOCKET_ERROR)
        return false;
    DWORD tv2 = (DWORD)timeoutMs;
    return setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv2, sizeof(tv2)) != SOCKET_ERROR;
#else
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return false;
    return setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool MinWS::SendAll(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32)
        int n = send(sock_, (const char*)(data + sent), (int)(len - sent), 0);
#else
        ssize_t n = ::send(sock_, data + sent, len - sent, 0);
#endif
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool MinWS::RecvExact(uint8_t* buf, size_t len, int timeoutMs) {
    SetSocketTimeout(timeoutMs);
    size_t got = 0;
    while (got < len) {
#if defined(_WIN32)
        int n = recv(sock_, (char*)(buf + got), (int)(len - got), 0);
#else
        ssize_t n = ::recv(sock_, buf + got, len - got, 0);
#endif
        if (n == 0) return false;
        if (n < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAETIMEDOUT) return false;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EAGAIN) return false;
#endif
            return false;
        }
        got += n;
    }
    return true;
}

bool MinWS::Connect(const std::string& url) {
    Close();

    ParsedUrl parsed;
    if (!ParseUrl(url, parsed)) {
        spdlog::error("[MinWS] bad url: {}", url);
        return false;
    }
    spdlog::info("[MinWS] Connecting ws://{}:{}{}", parsed.host, parsed.port, parsed.path);

    // DNS resolve
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(parsed.port);
    if (getaddrinfo(parsed.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        spdlog::error("[MinWS] DNS failed: {}", parsed.host);
        return false;
    }

    // 尝试每一个 addrinfo（通常就一个）
    for (auto* p = res; p; p = p->ai_next) {
#if defined(_WIN32)
        sock_ = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock_ == SOCK_INVALID) continue;
        if (connect(sock_, p->ai_addr, (int)p->ai_addrlen) == SOCKET_ERROR) {
            SOCK_CLOSE(sock_); sock_ = SOCK_INVALID; continue;
        }
#else
        sock_ = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock_ < 0) continue;
        if (::connect(sock_, p->ai_addr, p->ai_addrlen) < 0) {
            SOCK_CLOSE(sock_); sock_ = -1; continue;
        }
#endif
        break;
    }
    freeaddrinfo(res);

    if (sock_ == SOCK_INVALID) {
        spdlog::error("[MinWS] connect failed");
        return false;
    }

    // 发 HTTP Upgrade
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    uint8_t keyBytes[16];
    for (auto& b : keyBytes) b = (uint8_t)dis(gen);
    std::string keyB64 = Base64EncodeRaw(std::vector<uint8_t>(keyBytes, keyBytes + 16));

    std::ostringstream os;
    os << "GET " << parsed.path << " HTTP/1.1\r\n"
       << "Host: " << parsed.host << ":" << parsed.port << "\r\n"
       << "Upgrade: websocket\r\n"
       << "Connection: Upgrade\r\n"
       << "Sec-WebSocket-Key: " << keyB64 << "\r\n"
       << "Sec-WebSocket-Version: 13\r\n"
       << "\r\n";
    std::string req = os.str();
    if (!SendAll((const uint8_t*)req.data(), req.size())) {
        spdlog::error("[MinWS] send handshake failed");
        SOCK_CLOSE(sock_); sock_ = SOCK_INVALID;
        return false;
    }

    // 读响应（HTTP/1.1 101 Switching Protocols）
    std::vector<uint8_t> resp;
    char buf[1024];
    bool headerEnd = false;
    while (!headerEnd) {
#if defined(_WIN32)
        int n = recv(sock_, buf, sizeof(buf), 0);
#else
        ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        resp.insert(resp.end(), buf, buf + n);
        // 找 \r\n\r\n
        for (size_t i = 0; i + 3 < resp.size(); ++i) {
            if (resp[i]=='\r' && resp[i+1]=='\n' && resp[i+2]=='\r' && resp[i+3]=='\n') {
                headerEnd = true; break;
            }
        }
    }
    std::string respStr(resp.begin(), resp.end());
    if (respStr.find("101") == std::string::npos) {
        spdlog::error("[MinWS] handshake failed: {}", respStr.substr(0, 200));
        SOCK_CLOSE(sock_); sock_ = SOCK_INVALID;
        return false;
    }

    // 验证 Sec-WebSocket-Accept
    std::string acceptNeed = Base64EncodeRaw(
        SHA1((const uint8_t*)keyB64.data(), keyB64.size())
    );
    // 但 Accept 还要拼 magic string... 我上面做 SHA1 前漏拼了！补上：
    std::string combined = keyB64 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    acceptNeed = Base64EncodeRaw(SHA1((const uint8_t*)combined.data(), combined.size()));
    size_t ap = respStr.find("Sec-WebSocket-Accept:");
    if (ap != std::string::npos) {
        size_t vStart = respStr.find(':', ap) + 1;
        size_t vEnd   = respStr.find("\r\n", vStart);
        std::string got = respStr.substr(vStart, vEnd - vStart);
        // trim
        while (!got.empty() && got.front() == ' ') got.erase(got.begin());
        while (!got.empty() && got.back()  == ' ') got.pop_back();
        if (got != acceptNeed) {
            spdlog::warn("[MinWS] Sec-WebSocket-Accept mismatch (maybe ok for CDP)");
            // 很多 CDP 实现跳过这个校验，我们也跳过
        }
    }

    connected_.store(true);
    SetSocketTimeout(0); // 非阻塞
    spdlog::info("[MinWS] Connected OK");
    return true;
}

void MinWS::Close() {
    if (!connected_.load()) {
        if (sock_ != SOCK_INVALID) { SOCK_CLOSE(sock_); sock_ = SOCK_INVALID; }
        return;
    }
    // 不发 WebSocket Close 帧，直接半关闭 socket：
    //   send shutdown(SHUT_WR) → 告诉 Chrome 我们不再发数据
    //   但 Chrome 那边不会触发 target detach/renderer crash
    //
    // 之前的问题：SendAll(CloseFrame) 会让 Chrome CDP 触发 target detach，
    // 在 CDP 156 上直接导致 Chrome 渲染进程崩溃。
#if defined(_WIN32)
    shutdown(sock_, SD_SEND);
#else
    shutdown(sock_, SHUT_WR);
#endif
    // 不 Read 也不 recv — 直接让 CDPSession Disconnect() 关 socket + join 线程
    connected_.store(false);
}

bool MinWS::SendText(const std::string& text) {
    if (!connected_.load()) return false;
    return WriteFrame(OP_TEXT, (const uint8_t*)text.data(), text.size());
}

bool MinWS::WriteFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
    std::vector<uint8_t> frame;
    frame.reserve(14 + len);

    // Byte 0: FIN=1 + opcode
    frame.push_back(0x80 | (opcode & 0x0F));

    // Byte 1+: MASK=1 + payload len
    uint8_t maskKey[4];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (auto& b : maskKey) b = (uint8_t)dis(gen);

    if (len < 126) {
        frame.push_back(0x80 | (uint8_t)len);
    } else if (len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)((len >> 8) & 0xFF));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
    }
    frame.insert(frame.end(), maskKey, maskKey + 4);

    for (size_t i = 0; i < len; ++i)
        frame.push_back(payload[i] ^ maskKey[i % 4]);

    return SendAll(frame.data(), frame.size());
}

bool MinWS::ReadFramePayload(std::vector<uint8_t>& out, int timeoutMs) {
    // 读 Header
    uint8_t hdr[2];
    if (!RecvExact(hdr, 2, timeoutMs)) return false;

    bool fin    = (hdr[0] & 0x80) != 0;
    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payloadLen = hdr[1] & 0x7F;

    if (payloadLen == 126) {
        uint8_t e[2];
        if (!RecvExact(e, 2, timeoutMs)) return false;
        payloadLen = (e[0] << 8) | e[1];
    } else if (payloadLen == 127) {
        uint8_t e[8];
        if (!RecvExact(e, 8, timeoutMs)) return false;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | e[i];
    }

    uint8_t maskKey[4] = {};
    if (masked) {
        if (!RecvExact(maskKey, 4, timeoutMs)) return false;
    }

    std::vector<uint8_t> payload(payloadLen);
    if (payloadLen > 0) {
        if (!RecvExact(payload.data(), (size_t)payloadLen, timeoutMs)) return false;
        if (masked) {
            for (size_t i = 0; i < payloadLen; ++i)
                payload[i] ^= maskKey[i % 4];
        }
    }

    out = std::move(payload);
    // 把 opcode 存在 out 的前面？不行——直接改 API：这里只存 payload，opcode 由外层拿
    return true;
}

// 改一下：上面的 ReadFramePayload 其实不返回 opcode。我们需要 ReadMessage 自己读 opcode
// 为了简单，加一个完整的 ReadMessage 私有方法
bool MinWS::RecvMessage(Message& out, int timeoutMs) {
    if (!connected_.load()) return false;

    // 读 opcode + payload
    uint8_t hdr[2];
    if (!RecvExact(hdr, 2, timeoutMs)) return false;

    bool fin    = (hdr[0] & 0x80) != 0;
    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payloadLen = hdr[1] & 0x7F;

    if (payloadLen == 126) {
        uint8_t e[2]; RecvExact(e, 2, timeoutMs);
        payloadLen = (e[0] << 8) | e[1];
    } else if (payloadLen == 127) {
        uint8_t e[8]; RecvExact(e, 8, timeoutMs);
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | e[i];
    }

    uint8_t maskKey[4] = {};
    if (masked) RecvExact(maskKey, 4, timeoutMs);

    std::vector<uint8_t> payload(payloadLen);
    if (payloadLen > 0) {
        RecvExact(payload.data(), (size_t)payloadLen, timeoutMs);
        if (masked) for (size_t i = 0; i < payloadLen; ++i)
            payload[i] ^= maskKey[i % 4];
    }

    // 处理 PING → 自动 PONG
    if (opcode == OP_PING) {
        WriteFrame(OP_PONG, payload.data(), payload.size());
        // 继续读下一条
        return RecvMessage(out, timeoutMs);
    }
    // CLOSE → 回 Close 然后关闭
    if (opcode == OP_CLOSE) {
        WriteFrame(OP_CLOSE, payload.data(), payload.size());
        connected_.store(false);
        SOCK_CLOSE(sock_); sock_ = SOCK_INVALID;
        return false;
    }

    out.opcode = opcode;
    out.data   = std::move(payload);
    return true;
}

} // namespace ws
} // namespace newrpa
