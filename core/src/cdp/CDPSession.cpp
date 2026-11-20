// ============================================================================
// CDPSession 实现 — MinWS (零依赖 WebSocket) 作为底层，桥接到同步 API
// ============================================================================

#include "newrpa/cdp/CDPSession.h"
#include "newrpa/ws/MinWS.h"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <chrono>
#include <sstream>

namespace newrpa {
namespace cdp {

struct CDPSession::WsImpl {
    ws::MinWS sock;
    CDPSession* parent = nullptr;
    std::mutex sendMtx;
    std::atomic<bool> recvRunning{false};
};

// ----------------------------------------------------------------------------

CDPSession::CDPSession(const std::string& wsUrl)
    : wsUrl_(wsUrl)
    , ws_(std::make_unique<WsImpl>())
{
    ws_->parent = this;
}

CDPSession::~CDPSession() {
    Disconnect();
}

// ----------------------------------------------------------------------------

bool CDPSession::Connect() {
    if (connected_.load()) return true;

    spdlog::info("[CDP] Connecting to {}", wsUrl_);
    if (!ws_->sock.Connect(wsUrl_)) {
        spdlog::error("[CDP] MinWS connect failed");
        return false;
    }
    connected_.store(true);

    // 启收包线程
    ws_->recvRunning.store(true);
    wsThread_ = std::thread([this]() {
        spdlog::info("[CDP] Recv thread started");
        while (ws_->recvRunning.load() && connected_.load()) {
            ws::Message msg;
            if (!ws_->sock.RecvMessage(msg, 3000)) {
                if (!connected_.load()) break;
                if (!ws_->recvRunning.load()) break;
                // timeout 正常，循环继续
                continue;
            }
            std::string text(msg.data.begin(), msg.data.end());
            OnMessage(text);
        }
        spdlog::info("[CDP] Recv thread exited");
    });

    // 基本 CDP Domain 启用
    CallNoWait("Page.enable");
    CallNoWait("Runtime.enable");
    CallNoWait("DOM.enable");
    CallNoWait("Network.enable");
    CallNoWait("Input.enable");
    // [FIX] 不要在连接时无条件 Fetch.enable：
    // Fetch 开启后会暂停所有网络请求，而这里没有 requestPaused 放行处理器，
    // 导致 Page.navigate 后页面资源全部挂起、导航卡死（登录超时）。
    // 需要请求拦截时由 NetworkDomain::InterceptRequest() 按需开启并处理放行。
    // CallNoWait("Fetch.enable");

    return true;
}

void CDPSession::Disconnect() {
    if (!connected_.load() && !running_.load()) return;
    ws_->recvRunning.store(false);
    connected_.store(false);
    ws_->sock.Close();
    if (wsThread_.joinable()) wsThread_.join();
    running_.store(false);
}

// ----------------------------------------------------------------------------

json CDPSession::Call(const std::string& method,
                      const json& params,
                      int timeoutMs) {
    if (!connected_.load())
        throw std::runtime_error("CDPSession not connected");

    int id = nextId_.fetch_add(1);
    json msg = {{"id", id}, {"method", method}};
    if (!params.is_null() && !params.empty()) msg["params"] = params;

    auto pending = std::make_shared<PendingResponse>();
    {
        std::lock_guard<std::mutex> lk(pendingsMutex_);
        pendings_[id] = pending;
    }

    // 发
    {
        std::lock_guard<std::mutex> lk(ws_->sendMtx);
        if (!ws_->sock.SendText(msg.dump())) {
            pendings_.erase(id);
            throw std::runtime_error("CDP send failed");
        }
    }

    auto result = WaitForResponse(id, timeoutMs);

    {
        std::lock_guard<std::mutex> lk(pendingsMutex_);
        pendings_.erase(id);
    }

    if (result.contains("error")) {
        std::ostringstream oss;
        oss << "CDP " << method << " error: "
            << result["error"].dump();
        throw std::runtime_error(oss.str());
    }
    if (!result.contains("result"))
        return json::object();
    return result["result"];
}

void CDPSession::CallNoWait(const std::string& method, const json& params) {
    if (!connected_.load()) return;
    int id = nextId_.fetch_add(1);
    json msg = {{"id", id}, {"method", method}};
    if (!params.empty()) msg["params"] = params;
    try {
        std::lock_guard<std::mutex> lk(ws_->sendMtx);
        ws_->sock.SendText(msg.dump());
    } catch (...) {}
}

json CDPSession::WaitForResponse(int id, int timeoutMs) {
    std::shared_ptr<PendingResponse> pending;
    {
        std::lock_guard<std::mutex> lk(pendingsMutex_);
        auto it = pendings_.find(id);
        if (it == pendings_.end())
            throw std::runtime_error("Pending response lost");
        pending = it->second;
    }

    std::unique_lock<std::mutex> lk(pending->mtx);
    if (!pending->cv.wait_for(lk,
                              std::chrono::milliseconds(timeoutMs),
                              [&] { return pending->received; })) {
        throw std::runtime_error("CDP call timeout (id=" +
                                 std::to_string(id) + ")");
    }
    return pending->response;
}

// ----------------------------------------------------------------------------

int CDPSession::OnEvent(const std::string& eventName, EventCallback cb) {
    std::lock_guard<std::mutex> lk(eventMutex_);
    int handle = nextEventHandle_++;
    eventCbs_[eventName][handle] = std::move(cb);
    return handle;
}

void CDPSession::OffEvent(const std::string& eventName, int handle) {
    std::lock_guard<std::mutex> lk(eventMutex_);
    auto it = eventCbs_.find(eventName);
    if (it != eventCbs_.end()) it->second.erase(handle);
}

// ----------------------------------------------------------------------------

void CDPSession::OnMessage(const std::string& raw) {
    json msg;
    try { msg = json::parse(raw); }
    catch (const std::exception&) {
        spdlog::warn("[CDP] Parse error: {}", raw.substr(0, 200));
        return;
    }

    if (msg.contains("id")) {
        int id = msg["id"].get<int>();
        std::shared_ptr<PendingResponse> pending;
        {
            std::lock_guard<std::mutex> lk(pendingsMutex_);
            auto it = pendings_.find(id);
            if (it != pendings_.end()) pending = it->second;
        }
        if (pending) {
            std::lock_guard<std::mutex> lk(pending->mtx);
            pending->response = std::move(msg);
            pending->received = true;
            pending->cv.notify_one();
        }
        return;
    }

    if (msg.contains("method")) {
        std::string evName = msg["method"].get<std::string>();
        json params = msg.value("params", json::object());

        spdlog::trace("[CDP] Event: {}", evName);

        std::vector<EventCallback> cbs;
        {
            std::lock_guard<std::mutex> lk(eventMutex_);
            auto it = eventCbs_.find(evName);
            if (it != eventCbs_.end()) {
                for (auto& kv : it->second) cbs.push_back(kv.second);
            }
        }
        for (auto& cb : cbs) {
            try { cb(params); }
            catch (const std::exception& e) {
                spdlog::error("[CDP] Event handler exception: {}", e.what());
            }
        }
        return;
    }

    spdlog::debug("[CDP] Unhandled message: {}", msg.dump(2));
}

} // namespace cdp
} // namespace newrpa
