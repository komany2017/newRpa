#pragma once
// ============================================================================
// HidInput —— 单头盒子（HID 键盘 + HID 鼠标）的公共门面
//
// 目的：任何银行插件都用同一套硬件输入能力，不再各自
//       #include <newrpa/singleUsbbox/HidKeyBoard.h> 并从
//       core/src/singleUsbbox/*.cpp 复制源码到自己插件里编译。
//
// 编译归属：core/src/hid/HidInput.cpp + core/src/singleUsbbox/*.cpp
//           全部编进 NewRpaCore.dll，插件只需 #include + 链接 NewRpaCore。
//
// 典型用法（登录页安全控件）：
//   newrpa::hid::HidInput hw(session_.get());
//   hw.SetSession(session_.get());
//   if (!hw.OpenKeyboard()) return "error: hid keyboard unavailable";
//   if (!hw.FocusElement("#_safepass")) return "error: password control focus";
//   if (!hw.TypeIntoFocused(pwd, "#_safepass")) return "error: password input";
//
// 已封装的坑（都是实测踩出来的，别的银行照抄即可）：
//   * 安全控件 tabindex=-1，Tab 焦点导航到不了 → 必须按坐标点击
//   * CDP 合成键盘常被控件拒绝 → 先 CDP、再用硬件键盘回退
//   * 硬件键盘字符进的是前台窗口 → 输入前必须把 Chrome 置前 + 置顶
//   * CV/Electron 应用类名同为 Chrome_WidgetWin_1 → 必须按 chrome.exe 过滤
// ============================================================================

#include "newrpa/NewRpaExport.h"

#include <memory>
#include <string>

namespace newrpa {
namespace cdp { class CDPSession; }

namespace hid {

// 设备状态快照（可直接打日志给用户看）
struct BoxStatus {
    bool keyboardPlugged = false;   // 系统里能看到键盘盒子
    bool keyboardUsable  = false;   // 能收发私有帧
    bool keyboardOpened  = false;   // 当前已打开
    bool mousePlugged    = false;   // 鼠标盒子在位
    bool mouseOpened     = false;   // 当前已打开
    unsigned vid = 0;
    unsigned pid = 0;
    char detail[512] = {0};         // 一句话结论（中文，可直接打印）

    std::string ToString() const;
};

class NEWRPA_API HidInput {
public:
    HidInput();
    explicit HidInput(cdp::CDPSession* session);
    ~HidInput();

    HidInput(const HidInput&) = delete;
    HidInput& operator=(const HidInput&) = delete;

    // 绑定 CDP 会话（点击/取值/键盘注入都走它）
    void SetSession(cdp::CDPSession* session);

    // ---------------------------------------------------------------- 设备
    // 只检测不打开，随时可调
    BoxStatus Probe();

    // 打开键盘盒子（默认 VID 0xC216 / PID 0x0301）。已打开则直接复用。
    bool OpenKeyboard(int vid = 0xC216, int pid = 0x0301, int keyDelayMs = 60);
    bool OpenMouse(int vid = 0xC216, int pid = 0);
    void Close();

    bool KeyboardReady() const;
    bool MouseReady() const;
    const char* LastError() const;

    // ---------------------------------------------------------------- 窗口
    // 找 chrome.exe 的可见主窗口（排除 VS Code 等 Electron 同名类），
    // 置前（ALT 松前台锁 + AttachThreadInput）并按需最大化 + 置顶。
    // 硬件键盘输入前必须调用：字符进的是前台窗口。
    static bool FocusChrome(bool maximize = false, bool topmost = true);
    // 取消置顶（输完恢复普通层级；同时摘掉误置顶的 VS Code）
    static void UnpinChromeTopmost();
    static bool ChromeInForeground();

    // ---------------------------------------------------------------- 元素
    // 元素中心点（CSS 坐标）。找不到元素返回 false。
    bool ElementCenter(const std::string& selector, double& cx, double& cy);
    std::string ActiveElement();
    bool IsElementPresent(const std::string& selector);

    // CDP 鼠标点击元素中心（合成事件，快，但滑块/风控场景可能被识别）
    bool ClickElement(const std::string& selector);
    // 硬件鼠标点击元素中心（真 HID 事件，需先 SetMouseOrigin 标定 origin）
    bool ClickElementHw(const std::string& selector);
    void SetMouseOrigin(double originX, double originY);

    // 聚焦：点击元素中心 → 校验 activeElement → JS focus() 兜底。
    // 适合 tabindex=-1 的安全控件（_safepass / cfca 密码框等）。
    bool FocusElement(const std::string& selector, int attempts = 3);

    // ---------------------------------------------------------------- 键盘
    // 硬件键盘拟人输入（逐字 + 字间随机停顿 + 每 2~4 字一次长顿）。
    // 内部会先 FocusChrome()，输完恢复非置顶。
    bool TypeHw(const std::string& text, int delayMs = 60,
                bool clearFirst = false);

    // CDP char 事件输入（不受前台窗口影响；部分安全控件会拒绝）
    bool TypeCdp(const std::string& text, int perCharMs = 120);

    // 读控件已呈现的字符数（innerText/textContent/data-len 的 '*'
    // 与 data-len 取最大值）。无法读取时返回 -1。
    int MaskLength(const std::string& selector);

    // 组合策略：CDP 优先 → 不足则按已输入长度补齐 → 仍不足回退硬件键盘。
    // verifySelector 为空时不做数量校验，只保证"发送成功"。
    bool TypeIntoFocused(const std::string& text,
                         const std::string& verifySelector = {},
                         int perCharMs = 120);

    // 具名按键
    bool PressTab();
    bool PressEnter();
    bool Backspace(int times = 1);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}   // namespace hid
}   // namespace newrpa
