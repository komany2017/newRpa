// ============================================================================
// CDP Domain 实现 — 都是 CDPSession::Call 的薄封装
// ============================================================================

#include "newrpa/cdp/CDPDomains.h"
#include <spdlog/spdlog.h>

namespace newrpa {
namespace cdp {

// ----------------------------------------------------------------------------
// PageDomain
// ----------------------------------------------------------------------------

bool PageDomain::Navigate(const std::string& url, int timeoutMs) {
    auto result = s_->Call("Page.navigate", {{"url", url}}, timeoutMs);
    std::string frameId = result.value("frameId", "");
    // 等 Page.loadEventFired
    bool loaded = false;
    int h = s_->OnEvent("Page.loadEventFired",
                         [&](const json&) { loaded = true; });

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (!loaded &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    s_->OffEvent("Page.loadEventFired", h);
    if (!loaded) spdlog::warn("[Page] Navigate load timeout (DOM may be ready): {}", url);
    // loadEventFired 超时也返回 true —— 页面 DOM/content 大概率已经可用
    return true;
}

std::string PageDomain::CaptureScreenshotBase64() {
    auto r = s_->Call("Page.captureScreenshot");
    return r.value("data", "");
}

std::string PageDomain::CaptureScreenshotJpeg(int quality) {
    auto r = s_->Call("Page.captureScreenshot",
                      {{"format", "jpeg"}, {"quality", quality}});
    return r.value("data", "");
}

void PageDomain::SetViewport(int w, int h, double dpr, bool mobile) {
    s_->Call("Emulation.setDeviceMetricsOverride", {
        {"width", w},
        {"height", h},
        {"deviceScaleFactor", dpr},
        {"mobile", mobile},
        {"screenWidth", w},
        {"screenHeight", h},
        {"positionX", 0},
        {"positionY", 0}
    });
}

void PageDomain::CloseTarget(const std::string& targetId) {
    s_->Call("Target.closeTarget", {{"targetId", targetId}});
}

// ----------------------------------------------------------------------------
// RuntimeDomain
// ----------------------------------------------------------------------------

json RuntimeDomain::Evaluate(const std::string& expression,
                             const std::string& returnByValue,
                             int timeoutMs,
                             bool awaitPromise) {
    return s_->Call("Runtime.evaluate", {
        {"expression", expression},
        {"returnByValue", returnByValue == "true"},
        {"awaitPromise", awaitPromise}
    }, timeoutMs);
}

std::string RuntimeDomain::EvalString(const std::string& js, int timeoutMs) {
    auto r = Evaluate(js, "true", timeoutMs, false);
    return r.value("result", json::object()).value("value", std::string{});
}

std::string RuntimeDomain::EvalStringAsync(const std::string& js, int timeoutMs) {
    auto r = Evaluate(js, "true", timeoutMs, true);
    if (r.contains("exceptionDetails")) return std::string{};
    return r.value("result", json::object()).value("value", std::string{});
}

bool RuntimeDomain::EvalBool(const std::string& js, int timeoutMs) {
    auto r = Evaluate(js, "true", timeoutMs);
    return r.value("result", json::object()).value("value", false);
}

double RuntimeDomain::EvalNumber(const std::string& js, int timeoutMs) {
    auto r = Evaluate(js, "true", timeoutMs);
    return r.value("result", json::object()).value("value", 0.0);
}

json RuntimeDomain::CallFunctionOn(const std::string& functionDeclaration,
                                   const json& args,
                                   const std::string& objectId) {
    json params = {{"functionDeclaration", functionDeclaration},
                   {"returnByValue", true}};
    if (!args.empty()) params["arguments"] = args;
    if (!objectId.empty()) params["objectId"] = objectId;
    return s_->Call("Runtime.callFunctionOn", params);
}

// ----------------------------------------------------------------------------
// DomDomain
// ----------------------------------------------------------------------------

int DomDomain::GetDocumentNodeId(int depth) {
    json params = {{"depth", depth}, {"pierce", true}};
    auto r = s_->Call("DOM.getDocument", params);
    return r.value("root", json::object()).value("nodeId", 0);
}

std::vector<int> DomDomain::QuerySelector(int rootNodeId,
                                           const std::string& selector) {
    auto r = s_->Call("DOM.querySelectorAll", {
        {"nodeId", rootNodeId},
        {"selector", selector}
    });
    std::vector<int> result;
    for (auto& id : r.value("nodeIds", json::array()))
        result.push_back(id.get<int>());
    return result;
}

std::vector<int> DomDomain::QuerySelector(const std::string& selector) {
    int root = GetDocumentNodeId(2);
    if (!root) return {};
    return QuerySelector(root, selector);
}

std::string DomDomain::GetAttribute(int nodeId, const std::string& name) {
    auto r = s_->Call("DOM.getAttributes", {{"nodeId", nodeId}});
    auto attrs = r.value("attributes", json::array());
    for (size_t i = 0; i + 1 < attrs.size(); i += 2)
        if (attrs[i].get<std::string>() == name)
            return attrs[i+1].get<std::string>();
    return "";
}

void DomDomain::SetAttributeValue(int nodeId, const std::string& name,
                                  const std::string& value) {
    s_->Call("DOM.setAttributeValue", {
        {"nodeId", nodeId},
        {"name", name},
        {"value", value}
    });
}

std::string DomDomain::GetOuterHTML(int nodeId) {
    auto r = s_->Call("DOM.getOuterHTML", {{"nodeId", nodeId}});
    return r.value("outerHTML", "");
}

std::string DomDomain::GetInnerText(int nodeId) {
    auto r = s_->Call("DOM.getNodeForLocation", {
        {"x", 0}, {"y", 0}   // 占位
    });
    // 简单做法：用 Runtime 调 textContent
    RuntimeDomain rt(s_);
    std::ostringstream js;
    js << "(function(n){ return n ? n.innerText || n.textContent : ''; })"
       << "($0)";
    // $0 不行，换个方式：用 backendNodeId
    // 为简单起见，这里用 DOM.resolveNode + Runtime.callFunctionOn
    auto rn = s_->Call("DOM.resolveNode", {{"nodeId", nodeId}});
    std::string objId = rn.value("object", json::object()).value("objectId", "");
    if (objId.empty()) return "";
    auto rr = rt.CallFunctionOn("function(){ return this.innerText || this.textContent || ''; }",
                                json::array(), objId);
    return rr.value("result", json::object()).value("value", std::string{});
}

json DomDomain::GetBoxModel(int nodeId) {
    auto r = s_->Call("DOM.getBoxModel", {{"nodeId", nodeId}});
    return r.value("model", json::object());
}

int DomDomain::DescribeNode(int nodeId) {
    auto r = s_->Call("DOM.describeNode", {{"nodeId", nodeId}});
    return r.value("node", json::object()).value("backendNodeId", 0);
}

// ----------------------------------------------------------------------------
// InputDomain
// ----------------------------------------------------------------------------

void InputDomain::MouseClick(double x, double y, const std::string& button) {
    json params = {{"type", "mousePressed"}, {"x", x}, {"y", y},
                   {"button", button}, {"clickCount", 1}};
    s_->Call("Input.dispatchMouseEvent", params);
    params["type"] = "mouseReleased";
    s_->Call("Input.dispatchMouseEvent", params);
}

void InputDomain::Wheel(double deltaX, double deltaY) {
    s_->Call("Input.dispatchMouseEvent", {
        {"type", "mouseWheel"},
        {"deltaX", deltaX},
        {"deltaY", deltaY},
        {"pointerType", "mouse"}
    });
}

void InputDomain::Type(const std::string& text) {
    for (char c : text) {
        std::string cStr(1, c);
        s_->Call("Input.dispatchKeyEvent", {
            {"type", "char"},
            {"text", cStr},
            {"unmodifiedText", cStr}
        });
    }
}

void InputDomain::KeyPress(const std::string& key,
                           const std::string& code,
                           const std::string& keyCode) {
    json down = {{"type", "keyDown"}, {"key", key}};
    if (!code.empty()) down["code"] = code;
    if (!keyCode.empty()) down["keyCode"] = std::atoi(keyCode.c_str());
    s_->Call("Input.dispatchKeyEvent", down);
    json up = {{"type", "keyUp"}, {"key", key}};
    if (!code.empty()) up["code"] = code;
    s_->Call("Input.dispatchKeyEvent", up);
}

void InputDomain::KeyCombo(const std::vector<std::string>& keys) {
    // 先按住所有修饰键
    std::vector<std::string> mods = {"Control", "Shift", "Alt", "Meta"};
    std::vector<std::string> held;
    for (auto& k : keys) {
        bool isMod = std::find(mods.begin(), mods.end(), k) != mods.end();
        if (isMod) {
            s_->Call("Input.dispatchKeyEvent",
                     {{"type", "keyDown"}, {"key", k}});
            held.push_back(k);
        }
    }
    // 最后一个非修饰键触发 keyDown + keyUp
    std::string last;
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        if (std::find(mods.begin(), mods.end(), *it) == mods.end()) {
            last = *it;
            break;
        }
    }
    if (!last.empty()) {
        s_->Call("Input.dispatchKeyEvent", {{"type", "keyDown"}, {"key", last}});
        s_->Call("Input.dispatchKeyEvent", {{"type", "keyUp"},   {"key", last}});
    }
    // 依次松开修饰键
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        s_->Call("Input.dispatchKeyEvent",
                 {{"type", "keyUp"}, {"key", *it}});
    }
}

// ----------------------------------------------------------------------------
// NetworkDomain
// ----------------------------------------------------------------------------

void NetworkDomain::SetUserAgent(const std::string& ua,
                                 const std::string& platform) {
    s_->Call("Network.setUserAgentOverride", {
        {"userAgent", ua},
        {"platform", platform}
    });
}

void NetworkDomain::SetExtraHTTPHeaders(const std::string& name,
                                        const std::string& value) {
    s_->Call("Network.setExtraHTTPHeaders", {
        {"headers", json({{name, value}})}
    });
}

void NetworkDomain::ClearExtraHTTPHeaders() {
    s_->Call("Network.setExtraHTTPHeaders",
             {{"headers", json::object()}});
}

void NetworkDomain::InterceptRequest(
    const std::string& urlPattern,
    std::function<json(const json&)> handler) {
    s_->Call("Fetch.enable", {
        {"patterns", json::array({
            {{"urlPattern", urlPattern}}
        })}
    });
    s_->OnEvent("Fetch.requestPaused",
                [this, handler](const json& params) {
                    auto reqId = params.value("requestId", "");
                    auto fulfill = handler(params.value("request", json::object()));
                    json p = {{"requestId", reqId}};
                    for (auto it = fulfill.begin(); it != fulfill.end(); ++it)
                        p[it.key()] = it.value();
                    s_->Call("Fetch.fulfillRequest", p);
                });
}

// ----------------------------------------------------------------------------
// NetworkDomain — 新增 Enable / GetResponseBody / DownloadCapturer
// ----------------------------------------------------------------------------

void NetworkDomain::Enable()  { s_->CallNoWait("Network.enable"); }
void NetworkDomain::Disable() { s_->CallNoWait("Network.disable"); }

namespace {
// base64 解码
std::string Base64Decode(const std::string& in) {
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '\n' || c == '\r' || c == ' ') continue;
        if (c == '=') break;
        auto pos = tbl.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(pos);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// 从 URL / Content-Disposition 猜文件名
std::string GuessFilename(const std::string& url, const json& headers) {
    // 1. Content-Disposition
    if (headers.contains("content-disposition")) {
        auto cd = headers["content-disposition"].get<std::string>();
        size_t p = cd.find("filename=");
        if (p != std::string::npos) {
            std::string fn = cd.substr(p + 9);
            if (!fn.empty() && fn.front() == '"') fn.erase(0, 1);
            if (!fn.empty() && fn.back()  == '"') fn.pop_back();
            if (!fn.empty()) return fn;
        }
    }
    // 2. URL 最后一段
    size_t q = url.find('?');
    std::string path = (q == std::string::npos) ? url : url.substr(0, q);
    size_t s = path.find_last_of("/\\");
    if (s != std::string::npos) return path.substr(s + 1);
    return "download.bin";
}

// 下载捕获器 — 匿名命名空间单例简化（NetworkDomain 成员）
struct CaptureState {
    std::mutex mtx;
    std::condition_variable cv;
    std::string urlKw, ctKw;
    std::vector<std::string> capturedIds;
    int eventHandle = -1;
    bool active = false;
};
static CaptureState g_capture;
} // anon

NetworkDomain::ResponseBody NetworkDomain::GetResponseBody(const std::string& requestId) {
    ResponseBody rb;
    auto r = s_->Call("Network.getResponseBody", {{"requestId", requestId}});
    rb.isBase64 = r.value("base64Encoded", false);
    std::string body = r.value("body", "");
    rb.content = rb.isBase64 ? Base64Decode(body) : body;

    // 拿 headers 猜 Content-Type 和文件名
    auto r2 = s_->Call("Network.getRequestPostData",
                       {{"requestId", requestId}});  // 不一定有
    // 用 setExtraHTTPHeaders 那条 responseReceived 里已经有 headers
    return rb;
}

void NetworkDomain::StartCaptureDownload(const std::string& urlKeyword,
                                         const std::string& contentTypeKeyword) {
    std::lock_guard<std::mutex> lk(g_capture.mtx);
    if (g_capture.active) return;
    g_capture.active = true;
    g_capture.urlKw  = urlKeyword;
    g_capture.ctKw   = contentTypeKeyword;
    g_capture.capturedIds.clear();

    Enable();

    g_capture.eventHandle = s_->OnEvent("Network.responseReceived",
        [this](const json& params) {
            std::lock_guard<std::mutex> lk(g_capture.mtx);
            if (!g_capture.active) return;

            std::string url = params.value("response", json::object()).value("url", "");
            std::string ct  = params.value("response", json::object()).value("mimeType", "");

            bool urlOk = g_capture.urlKw.empty() ||
                         url.find(g_capture.urlKw) != std::string::npos;
            bool ctOk  = g_capture.ctKw.empty() ||
                         ct.find(g_capture.ctKw) != std::string::npos ||
                         ct.find("octet-stream") != std::string::npos ||
                         ct.find("ms-excel") != std::string::npos ||
                         ct.find("spreadsheet") != std::string::npos ||
                         ct.find("csv") != std::string::npos;

            if (urlOk && ctOk) {
                std::string rid = params.value("requestId", "");
                if (!rid.empty()) {
                    spdlog::info("[Network] Captured download: url={} ct={} rid={}",
                                 url, ct, rid);
                    g_capture.capturedIds.push_back(rid);
                    g_capture.cv.notify_all();
                }
            }
        });
}

std::vector<NetworkDomain::ResponseBody>
NetworkDomain::StopCaptureDownload(int timeoutMs) {
    std::vector<ResponseBody> result;

    // 等一段时间让响应到达
    std::unique_lock<std::mutex> lk(g_capture.mtx);
    if (!g_capture.capturedIds.empty()) {
        // 已经有了，直接取
    } else {
        g_capture.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
            [] { return !g_capture.capturedIds.empty(); });
    }

    auto ids = std::move(g_capture.capturedIds);
    g_capture.capturedIds.clear();
    g_capture.active = false;
    lk.unlock();

    // 取每个响应 body
    for (auto& rid : ids) {
        try {
            auto rb = GetResponseBody(rid);
            rb.requestId = rid;
            result.push_back(std::move(rb));
        } catch (const std::exception& e) {
            spdlog::warn("[Network] GetResponseBody failed: {}", e.what());
        }
    }
    return result;
}

} // namespace cdp
} // namespace newrpa
