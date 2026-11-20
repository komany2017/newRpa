#pragma once
// ============================================================================
// minws.h —— 极简 WebSocket 客户端（RFC 6455 子集）
//
// 专门为 Chrome DevTools Protocol 定制的最小实现：
//   - 支持 ws:// 和 wss://（通过 OpenSSL，没装就只用 ws://）
//   - 文本帧 / 二进制帧 / Ping / Pong / Close
//   - 客户端必带 mask（RFC 强制）
//   - 同步 send / 阻塞 recv
//
// 整个 CDPSession 只需要：Connect / SendText / RecvText / Close
// ============================================================================

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <stdexcept>
#include <cstdint>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#endif

#if defined(_WIN32)
static bool s_wsaInit = [](){
    WSADATA w; return WSAStartup(MAKEWORD(2,2), &w) == 0;
}();
#endif

namespace newrpa {
namespace ws {

// RFC 6455 opcode
enum Opcode : uint8_t {
    OP_CONT  = 0x0,
    OP_TEXT  = 0x1,
    OP_BIN   = 0x2,
    OP_CLOSE = 0x8,
    OP_PING  = 0x9,
    OP_PONG  = 0xA,
};

struct Message {
    uint8_t          opcode;
    std::vector<uint8_t> data;
};

class MinWS {
public:
    MinWS() = default;
    ~MinWS() { Close(); }

    MinWS(const MinWS&) = delete;
    MinWS& operator=(const MinWS&) = delete;

    // 连接到 ws://host:port/path
    // 返回 true 表示握手成功
    bool Connect(const std::string& url);

    // 关闭连接（发 Close 帧 + 关 socket）
    void Close();

    bool IsConnected() const { return connected_.load(); }

    // 发送文本帧（客户端必须 mask）
    bool SendText(const std::string& text);

    // 阻塞等待一条完整消息（处理 ping/pong 自动回复）
    // timeoutMs = 0 表示无限等
    bool RecvMessage(Message& out, int timeoutMs = 0);

private:
    // 内部 socket 封装
#if defined(_WIN32)
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::atomic<bool> connected_{false};

    // HTTP Upgrade 握手
    bool DoHandshake(const std::string& host, int port,
                     const std::string& path, bool useSsl);

    // 读写 socket
    bool SendAll(const uint8_t* data, size_t len);
    bool RecvExact(uint8_t* buf, size_t len, int timeoutMs = 5000);
    bool SetSocketTimeout(int timeoutMs);

    // 帧编解码
    bool WriteFrame(uint8_t opcode, const uint8_t* payload, size_t len);
    // 从 socket 读帧的 payload（处理 continuation / 大 payload）
    bool ReadFramePayload(std::vector<uint8_t>& out, int timeoutMs);
};

// ----------------------------------------------------------------------------
// Hash 辅助（用于 Sec-WebSocket-Accept）
// Base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
// ----------------------------------------------------------------------------
static std::string Base64EncodeRaw(const std::vector<uint8_t>& data);
static std::vector<uint8_t> SHA1(const uint8_t* data, size_t len); // 桩，后面填 OpenSSL

} // namespace ws
} // namespace newrpa
