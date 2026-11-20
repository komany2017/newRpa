#pragma once
// ============================================================================
// CDPClient — 基于 WebSocket++ 的 Chrome DevTools Protocol 客户端
//
// CDP 协议格式：
//   请求:  {"id": 1, "method": "Page.navigate", "params": {...}}
//   响应:  {"id": 1, "result": {...}}   或   {"id": 1, "error": {...}}
//   事件:  {"method": "Page.loadEventFired", "params": {...}}
//
// 银行 RPA 业务流程是顺序的（登录→查余额→查流水），所以 CDPSession
// 封装成"发命令→阻塞→拿到响应"的同步模型，事件通过回调推送。
// ============================================================================

#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <thread>
#include <memory>

#include "newrpa/NewRpaExport.h"
#include <nlohmann/json.hpp>

namespace newrpa {
namespace cdp {

using json = nlohmann::json;

// CDP 事件回调类型
using EventCallback = std::function<void(const json& params)>;

// 一个 Chrome Target（页面/tab）的会话
class NEWRPA_API CDPSession {
public:
    explicit CDPSession(const std::string& wsUrl);
    ~CDPSession();

    // mutex / condition_variable 不可移动，显式 delete
    CDPSession(const CDPSession&) = delete;
    CDPSession& operator=(const CDPSession&) = delete;
    CDPSession(CDPSession&&) noexcept = delete;
    CDPSession& operator=(CDPSession&&) noexcept = delete;

    // 建立 WebSocket 连接
    bool Connect();
    void Disconnect();
    bool IsConnected() const { return connected_.load(); }

    // ----- 核心 CDP 调用 -----

    // 同步调用 CDP 命令，等待最多 timeoutMs 毫秒返回
    // 返回 result 字段（成功）或 throw std::runtime_error（失败）
    json Call(const std::string& method,
              const json& params = json::object(),
              int timeoutMs = 10000);

    // 仅发命令不关心结果（比如 Page.enable）
    void CallNoWait(const std::string& method,
                    const json& params = json::object());

    // 注册 CDP 事件回调。返回的 handle 用于取消注册。
    // 事件名形如 "Page.loadEventFired"
    int OnEvent(const std::string& eventName, EventCallback cb);
    void OffEvent(const std::string& eventName, int handle);

private:
    void RunLoop();                     // WebSocket++ 运行线程
    void OnMessage(const std::string&); // 收到一条 CDP 消息
    json WaitForResponse(int id, int timeoutMs);

    std::string wsUrl_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    // 下一个请求 id
    std::atomic<int> nextId_{1};

    // 等待中的请求：id → {cv, mutex, response}
    struct PendingResponse {
        std::condition_variable cv;
        std::mutex mtx;
        json response;        // 收到的 result 或 error
        bool received = false; // 是否收到了响应（成功或错误都算）
    };
    std::unordered_map<int, std::shared_ptr<PendingResponse>> pendings_;
    std::mutex pendingsMutex_;

    // 事件分发：eventName → { handle → callback }
    std::unordered_map<std::string,
                       std::unordered_map<int, EventCallback>> eventCbs_;
    std::mutex eventMutex_;
    int nextEventHandle_{1};

    // WebSocket++ 内部状态（pimpl，避免头文件暴露 wspp 细节）
    struct WsImpl;
    std::unique_ptr<WsImpl> ws_;
    std::thread wsThread_;
};

} // namespace cdp
} // namespace newrpa
