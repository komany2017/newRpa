#pragma once

#include "newrpa/NewRpaExport.h"
// ============================================================================
// CDP Domain 封装 — 让业务代码方便操作 Chrome
//
// 所有 Domain 都挂在 CDPSession 上，CDPSession 已经 Connect() 好。
// ============================================================================

#include "newrpa/cdp/CDPSession.h"
#include <string>
#include <vector>
#include <functional>

namespace newrpa {
namespace cdp {

// 取返回结果里的 "root" / "nodeId" 等嵌套字段的便捷辅助
inline json::value_t NodeType(int id) { return json::value_t::discarded; }

// ----------------------------------------------------------------------------
// Page Domain
// ----------------------------------------------------------------------------
class NEWRPA_API PageDomain {
public:
    explicit PageDomain(CDPSession* s) : s_(s) {}

    // 导航到 URL，等 loadEventFired（最多 timeoutMs）
    bool Navigate(const std::string& url, int timeoutMs = 15000);

    // 截全屏 PNG（base64）
    std::string CaptureScreenshotBase64();

    // 截 JPEG（base64）
    std::string CaptureScreenshotJpeg(int quality = 80);

    // 设置视口
    void SetViewport(int w, int h, double dpr = 1.0, bool mobile = false);

    // 关闭某个 frame / page（用 targetId）
    void CloseTarget(const std::string& targetId);

private:
    CDPSession* s_;
};

// ----------------------------------------------------------------------------
// Runtime Domain
// ----------------------------------------------------------------------------
class NEWRPA_API RuntimeDomain {
public:
    explicit RuntimeDomain(CDPSession* s) : s_(s) {}

    // 在 main frame 执行 JS，返回求值结果（json）
    json Evaluate(const std::string& expression,
                  const std::string& returnByValue = "true",
                  int timeoutMs = 10000,
                  bool awaitPromise = false);

    // 简化版：直接把 JS 当字符串运行，取 returnValue.value
    std::string EvalString(const std::string& js, int timeoutMs = 3000);

    // 异步版：JS 返回 Promise，等待 resolve 后取 value（加载图片等场景）
    std::string EvalStringAsync(const std::string& js, int timeoutMs = 10000);
    bool        EvalBool(const std::string& js, int timeoutMs = 3000);
    double      EvalNumber(const std::string& js, int timeoutMs = 3000);

    // callFunctionOn —— 对特定执行上下文（frame）里的 objectId 调用方法
    json CallFunctionOn(const std::string& functionDeclaration,
                        const json& args = json::array(),
                        const std::string& objectId = {});

private:
    CDPSession* s_;
};

// ----------------------------------------------------------------------------
// DOM Domain
// ----------------------------------------------------------------------------
class NEWRPA_API DomDomain {
public:
    explicit DomDomain(CDPSession* s) : s_(s) {}

    // 拿文档根 nodeId
    int GetDocumentNodeId(int depth = -1);

    // CSS Selector 查询，返回 nodeId 数组
    std::vector<int> QuerySelector(int rootNodeId, const std::string& selector);
    // 便捷版：直接用选择器（底层先 GetDocumentNodeId）
    std::vector<int> QuerySelector(const std::string& selector);

    // 取节点属性
    std::string GetAttribute(int nodeId, const std::string& name);

    // 设节点属性
    void SetAttributeValue(int nodeId, const std::string& name,
                           const std::string& value);

    // 取节点 outerHTML
    std::string GetOuterHTML(int nodeId);
    // 取节点 innerText
    std::string GetInnerText(int nodeId);

    // 获取节点的 bounding box（page 坐标）
    // 返回 {x, y, width, height}，找不到返回空
    json GetBoxModel(int nodeId);

    // 根据 nodeId 生成 backendNodeId（给 Input 用）
    int DescribeNode(int nodeId);

private:
    CDPSession* s_;
};

// ----------------------------------------------------------------------------
// Input Domain
// ----------------------------------------------------------------------------
class NEWRPA_API InputDomain {
public:
    explicit InputDomain(CDPSession* s) : s_(s) {}

    // 在指定坐标点击
    void MouseClick(double x, double y, const std::string& button = "left");

    // 滚动（正数向下，负数向上）
    void Wheel(double deltaX, double deltaY);

    // 输入文本（不触发中文输入法，适合纯 ASCII）
    void Type(const std::string& text);

    // 按键：name = "Enter" / "Tab" / "Control" / "KeyA" ...
    void KeyPress(const std::string& key,
                  const std::string& code = {},
                  const std::string& keyCode = {});

    // 组合键：Ctrl+A 之类
    void KeyCombo(const std::vector<std::string>& keys);

private:
    CDPSession* s_;
};

// ----------------------------------------------------------------------------
// Network Domain
// ----------------------------------------------------------------------------
class NEWRPA_API NetworkDomain {
public:
    explicit NetworkDomain(CDPSession* s) : s_(s) {}

    void Enable();
    void Disable();

    void SetUserAgent(const std::string& ua,
                      const std::string& platform = "Win32");
    void SetExtraHTTPHeaders(const std::string& name, const std::string& value);
    void ClearExtraHTTPHeaders();

    // 拦截下一个 URL 匹配的请求，回调返回 fulfill 的 JSON
    void InterceptRequest(const std::string& urlPattern,
                          std::function<json(const json&)> handler);

    // 拿某个 responseId 的响应 body（base64 编码自动解码）
    // 返回 pair{content, filename_hint}
    struct ResponseBody {
        std::string content;       // 已解码内容（可能是二进制）
        std::string contentType;   // Content-Type
        std::string filename;      // 从 URL/Content-Disposition 猜的文件名
        std::string requestId;     // CDP requestId
        bool isBase64 = false;
    };
    ResponseBody GetResponseBody(const std::string& requestId);

    // 开始捕获下载文件（监听 responseReceived）
    // urlKeyword: 响应 URL 必须包含的关键词（如 "download"/"export"/"excel"）
    // contentTypeKeyword: Content-Type 必须包含的关键词（如 "spreadsheet"/"csv"/"octet-stream"）
    void StartCaptureDownload(const std::string& urlKeyword = "",
                              const std::string& contentTypeKeyword = "spreadsheet");
    // 停止捕获，返回捕获到的所有下载
    std::vector<ResponseBody> StopCaptureDownload(int timeoutMs = 10000);

private:
    CDPSession* s_;
};

} // namespace cdp
} // namespace newrpa
