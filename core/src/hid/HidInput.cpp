// ============================================================================
// HidInput 实现 —— 公共硬件输入门面（键盘盒子 + 鼠标盒子 + 焦点/点击/输入策略）
//
// 只依赖 Windows 自带 DLL（hid.dll / setupapi.dll / user32 / shcore），
// 与具体银行无关；所有厂商协议细节都在 singleUsbbox 层。
// ============================================================================

#include "newrpa/hid/HidInput.h"

#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/singleUsbbox/HidKeyBoard.h"
#include "newrpa/singleUsbbox/HidMouse.hpp"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace newrpa {
namespace hid {

namespace {

using Clock = std::chrono::steady_clock;

void SleepMs(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// UTF-8 -> 宽字符（Windows 侧统一走 Unicode）
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

bool IsAsciiOnly(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

// 进程名是否等于 name（如 L"chrome.exe"）。Electron 应用（VS Code 等）
// 窗口类名同样是 Chrome_WidgetWin_1，必须靠进程名区分。
bool ProcessImageIs(HWND h, const wchar_t* name) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;
    wchar_t exe[MAX_PATH] = {0};
    DWORD len = MAX_PATH;
    bool is = false;
    if (QueryFullProcessImageNameW(hp, 0, exe, &len)) {
        const wchar_t* base = wcsrchr(exe, L'\\');
        base = base ? base + 1 : exe;
        is = (_wcsicmp(base, name) == 0);
    }
    CloseHandle(hp);
    return is;
}

struct FindCtx {
    HWND best = nullptr;
    int  bestLen = 0;
};

BOOL CALLBACK EnumChromeProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 256);
    if (wcsstr(cls, L"Chrome_WidgetWin_1") == nullptr) return TRUE;
    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    if ((r.right - r.left) < 600 || (r.bottom - r.top) < 400) return TRUE;
    if (!ProcessImageIs(h, L"chrome.exe")) return TRUE;   // 排掉 VS Code 等
    int n = GetWindowTextLengthW(h);
    if (n > ctx->bestLen) { ctx->bestLen = n; ctx->best = h; }
    return TRUE;
}

HWND FindChromeWindow() {
    FindCtx ctx;
    EnumWindows(EnumChromeProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// 把误置顶的 VS Code（Electron，同类名）摘下来，避免残留 TOPMOST 盖住别人
void UnpinVsCodeTopmost() {
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (!(ex & WS_EX_TOPMOST)) return TRUE;
        wchar_t cls[128] = {0};
        GetClassNameW(h, cls, 128);
        if (wcsstr(cls, L"Chrome_WidgetWin_1") == nullptr) return TRUE;
        if (!ProcessImageIs(h, L"Code.exe")) return TRUE;
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return TRUE;
    }, 0);
}

}  // namespace

// ----------------------------------------------------------------------------
// BoxStatus
// ----------------------------------------------------------------------------
std::string BoxStatus::ToString() const {
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "keyboard{plugged=%d,usable=%d,opened=%d} "
                  "mouse{plugged=%d,opened=%d} vid=0x%04X pid=0x%04X detail=%s",
                  keyboardPlugged ? 1 : 0, keyboardUsable ? 1 : 0,
                  keyboardOpened ? 1 : 0, mousePlugged ? 1 : 0,
                  mouseOpened ? 1 : 0, vid, pid, detail);
    return std::string(buf);
}

// ----------------------------------------------------------------------------
// Impl
// ----------------------------------------------------------------------------
struct HidInput::Impl {
    cdp::CDPSession* session = nullptr;

    HidKeyBoard kb;
    HidMouse    mouse;
    bool kbOpened = false;
    bool mouseOpened = false;
    int  keyDelay = 60;

    mutable char error[512] = {0};

    void SetError(const char* msg) {
        std::snprintf(error, sizeof(error), "%s", msg ? msg : "");
    }

    // ---- CDP 小工具 ----
    std::string EvalStr(const std::string& js, int timeoutMs = 5000) {
        if (!session) return {};
        try {
            cdp::RuntimeDomain rt(session);
            return rt.EvalString(js, timeoutMs);
        } catch (const std::exception& e) {
            SetError(e.what());
            return {};
        } catch (...) {
            return {};
        }
    }

    bool EvalBool(const std::string& js, int timeoutMs = 3000) {
        std::string r = EvalStr(js, timeoutMs);
        return r == "true" || r == "1";
    }

    // 逐字符发 CDP char 事件（不受前台窗口影响）
    bool SendCdpChars(const std::string& text, size_t from, size_t to,
                      int perCharMs) {
        if (!session) return false;
        for (size_t i = from; i < to; ++i) {
            std::string ch(1, text[i]);
            nlohmann::json p = {{"type", "char"},
                                {"text", ch},
                                {"unmodifiedText", ch}};
            try {
                session->Call("Input.dispatchKeyEvent", p, 5000);
            } catch (const std::exception& e) {
                SetError(e.what());
                return false;
            } catch (...) {
                return false;
            }
            SleepMs(perCharMs);
        }
        return true;
    }
};

HidInput::HidInput() : d_(new Impl()) {}
HidInput::HidInput(cdp::CDPSession* session) : d_(new Impl()) {
    d_->session = session;
}
HidInput::~HidInput() { Close(); }

void HidInput::SetSession(cdp::CDPSession* session) { d_->session = session; }

const char* HidInput::LastError() const { return d_->error; }
bool HidInput::KeyboardReady() const { return d_->kbOpened; }
bool HidInput::MouseReady() const { return d_->mouseOpened; }

// ----------------------------------------------------------------------------
// 设备
// ----------------------------------------------------------------------------
BoxStatus HidInput::Probe() {
    BoxStatus st;
    auto ks = HidKeyBoard::detect();
    st.keyboardPlugged = ks.plugged;
    st.keyboardUsable  = ks.usable;
    st.vid = ks.vid;
    st.pid = ks.pid;
    std::snprintf(st.detail, sizeof(st.detail), "%s", ks.detail);

    // 鼠标与键盘通常同一个盒子；用键盘探测结果做在位判断，
    // 再叠加当前进程的打开状态。
    st.mousePlugged = ks.plugged;
    st.keyboardOpened = d_->kbOpened;
    st.mouseOpened = d_->mouseOpened;
    return st;
}

bool HidInput::OpenKeyboard(int vid, int pid, int keyDelayMs) {
    d_->keyDelay = keyDelayMs;
    if (d_->kbOpened) return true;
    d_->kb.setImeIsolation(true);
    d_->kb.setKeyDelay(keyDelayMs);
    if (!d_->kb.open(vid, pid)) {
        d_->SetError(d_->kb.lastError());
        spdlog::error("[HID] 键盘盒子打开失败: {}", d_->kb.lastError());
        return false;
    }
    d_->kbOpened = true;
    spdlog::info("[HID] 键盘盒子已打开 (keyDelay={}ms)", keyDelayMs);
    return true;
}

bool HidInput::OpenMouse(int vid, int pid) {
    if (d_->mouseOpened) return true;
    if (!d_->mouse.open(vid, pid)) {
        d_->SetError(d_->mouse.lastError());
        spdlog::error("[HID] 鼠标盒子打开失败: {}", d_->mouse.lastError());
        return false;
    }
    d_->mouseOpened = true;
    spdlog::info("[HID] 鼠标盒子已打开");
    return true;
}

void HidInput::Close() {
    if (d_->kbOpened) { d_->kb.close(); d_->kbOpened = false; }
    if (d_->mouseOpened) { d_->mouse.close(); d_->mouseOpened = false; }
}

// ----------------------------------------------------------------------------
// 窗口
// ----------------------------------------------------------------------------
bool HidInput::FocusChrome(bool maximize, bool topmost) {
    UnpinVsCodeTopmost();
    HWND h = FindChromeWindow();
    if (!h) {
        spdlog::error("[HID] 没找到 chrome.exe 窗口");
        return false;
    }
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (maximize) ShowWindow(h, SW_MAXIMIZE);
    if (topmost) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (GetForegroundWindow() == h) return true;

    // ALT 敲一下解除系统前台锁，再 AttachThreadInput 强抢
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD myTid = GetCurrentThreadId();
    bool attached = false;
    if (fgTid && fgTid != myTid)
        attached = (AttachThreadInput(myTid, fgTid, TRUE) != 0);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    SleepMs(200);

    bool ok = (GetForegroundWindow() == h);
    if (!ok) spdlog::warn("[HID] Chrome 未能置前");
    return ok;
}

void HidInput::UnpinChromeTopmost() {
    HWND h = FindChromeWindow();
    if (h) SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UnpinVsCodeTopmost();
}

bool HidInput::ChromeInForeground() {
    HWND h = FindChromeWindow();
    return h && GetForegroundWindow() == h;
}

// ----------------------------------------------------------------------------
// 元素
// ----------------------------------------------------------------------------
bool HidInput::ElementCenter(const std::string& selector, double& cx,
                             double& cy) {
    std::string js =
        "(function(){try{var e=document.querySelector(" +
        nlohmann::json(selector).dump() +
        ");if(!e)return '';var r=e.getBoundingClientRect();"
        "if(r.width<=0&&r.height<=0)return '';"
        "return JSON.stringify({x:r.left+r.width/2,y:r.top+r.height/2});}"
        "catch(err){return ''}})()";
    std::string s = d_->EvalStr(js);
    if (s.empty()) return false;
    try {
        auto j = nlohmann::json::parse(s);
        cx = j.value("x", 0.0);
        cy = j.value("y", 0.0);
        return true;
    } catch (...) {
        return false;
    }
}

std::string HidInput::ActiveElement() {
    return d_->EvalStr(
        "document.activeElement ? "
        "(document.activeElement.id||document.activeElement.tagName) : 'none'");
}

bool HidInput::IsElementPresent(const std::string& selector) {
    return d_->EvalBool("(function(){try{return !!document.querySelector(" +
                        nlohmann::json(selector).dump() + ")}catch(e){return false}})()");
}

bool HidInput::ClickElement(const std::string& selector) {
    double x = 0, y = 0;
    if (!ElementCenter(selector, x, y)) {
        d_->SetError("element not found or has no box");
        return false;
    }
    if (!d_->session) {
        d_->SetError("no cdp session");
        return false;
    }
    try {
        cdp::InputDomain in(d_->session);
        in.MouseClick(x, y);
    } catch (const std::exception& e) {
        d_->SetError(e.what());
        return false;
    } catch (...) {
        return false;
    }
    SleepMs(300);
    return true;
}

void HidInput::SetMouseOrigin(double originX, double originY) {
    d_->mouse.setOrigin(originX, originY);
}

bool HidInput::ClickElementHw(const std::string& selector) {
    if (!d_->mouseOpened) {
        d_->SetError("hid mouse not opened");
        return false;
    }
    double x = 0, y = 0;
    if (!ElementCenter(selector, x, y)) {
        d_->SetError("element not found or has no box");
        return false;
    }
    if (!FocusChrome(false, true)) {
        d_->SetError("chrome window not focused");
        return false;
    }
    // 先闭环移到元素中心，再按键（click() 自带 moveTo 绝对坐标，这里不能直接复用）
    bool ok = d_->mouse.moveToCss(x, y);
    if (ok) {
        SleepMs(80);
        ok = d_->mouse.leftDown();
        SleepMs(50);
        ok = d_->mouse.leftUp() && ok;
    }
    if (!ok) d_->SetError(d_->mouse.lastError());
    d_->mouse.releaseAll();
    UnpinChromeTopmost();
    return ok;
}

bool HidInput::FocusElement(const std::string& selector, int attempts) {
    if (attempts < 1) attempts = 1;
    const std::string wantId =
        d_->EvalStr("(function(){try{var e=document.querySelector(" +
                    nlohmann::json(selector).dump() +
                    ");return e?(e.id||e.tagName):''}catch(x){return ''}})()");

    for (int i = 0; i < attempts; ++i) {
        // 1) 按坐标点击（tabindex=-1 的控件只能这样进焦点）
        double x = 0, y = 0;
        if (ElementCenter(selector, x, y) && d_->session) {
            try {
                cdp::InputDomain in(d_->session);
                in.MouseClick(x, y);
            } catch (...) {
                // 点击失败不致命，下面 JS focus 兜底
            }
            SleepMs(400);
        }
        std::string active = ActiveElement();
        if (!wantId.empty() && active == wantId) return true;
        // 兼容控件把焦点落在外层 id 上的情况（如 _safepass / safepass）
        if (!active.empty() && active != "none" && active != "BODY" &&
            active != "HTML" &&
            (wantId.find(active) != std::string::npos ||
             active.find(wantId) != std::string::npos)) {
            return true;
        }

        // 2) JS focus() 兜底
        d_->EvalStr("(function(){try{var e=document.querySelector(" +
                    nlohmann::json(selector).dump() +
                    ");if(e){e.focus();return 'ok'}}catch(x){}return 'no'})()");
        SleepMs(250);
        active = ActiveElement();
        if (!wantId.empty() && active == wantId) return true;
    }
    spdlog::warn("[HID] 焦点未落到 {}（active={}）", selector, ActiveElement());
    return false;
}

// ----------------------------------------------------------------------------
// 键盘
// ----------------------------------------------------------------------------
bool HidInput::TypeHw(const std::string& text, int delayMs, bool clearFirst) {
    if (!OpenKeyboard(0xC216, 0x0301, delayMs)) return false;
    if (!FocusChrome(false, true)) return false;

    if (clearFirst) {
        d_->kb.backspace(32);
        SleepMs(300);
    }

    std::wstring w = Utf8ToWide(text);
    if (w.empty()) {
        UnpinChromeTopmost();
        d_->SetError("empty text");
        return false;
    }

    std::mt19937 rng{static_cast<unsigned>(Clock::now()
                          .time_since_epoch().count())};
    auto rnd = [&rng](int lo, int hi) {
        return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1));
    };

    bool ok = true;
    if (IsAsciiOnly(text)) {
        // ASCII：逐字发真实扫描码，字间随机停顿 + 每 2~4 字一次长顿
        int sinceLong = 0;
        int nextLong = rnd(2, 4);
        for (size_t i = 0; i < w.size(); ++i) {
            if (!d_->kb.type(std::wstring(1, w[i]), delayMs)) {
                d_->SetError(d_->kb.lastError());
                ok = false;
                break;
            }
            SleepMs(rnd(40, 160));
            ++sinceLong;
            if (sinceLong >= nextLong && i + 1 < w.size()) {
                SleepMs(rnd(200, 600));
                sinceLong = 0;
                nextLong = rnd(2, 4);
            }
        }
    } else {
        // 含中文/emoji：交给 HidKeyBoard 走剪贴板通道
        ok = d_->kb.type(w, delayMs);
        if (!ok) d_->SetError(d_->kb.lastError());
    }

    UnpinChromeTopmost();
    spdlog::info("[HID] 硬件键盘输入 chars={} ok={}", w.size(), ok);
    return ok;
}

bool HidInput::TypeCdp(const std::string& text, int perCharMs) {
    if (!d_->session) {
        d_->SetError("no cdp session");
        return false;
    }
    return d_->SendCdpChars(text, 0, text.size(), perCharMs);
}

int HidInput::MaskLength(const std::string& selector) {
    std::string js =
        "(function(){try{var e=document.querySelector(" +
        nlohmann::json(selector).dump() +
        ");if(!e)return '-1';"
        "var t=e.innerText||'',c=e.textContent||'';"
        "var a=(t.match(/[*]/g)||[]).length,b=(c.match(/[*]/g)||[]).length;"
        "var n=0;try{n=parseInt(e.getAttribute('data-len')||'0',10)||0}catch(x){}"
        "return String(Math.max(a,b,n))}catch(err){return '-1'}})()";
    std::string r = d_->EvalStr(js);
    if (r.empty()) return -1;
    try {
        return std::stoi(r);
    } catch (...) {
        return -1;
    }
}

bool HidInput::TypeIntoFocused(const std::string& text,
                               const std::string& verifySelector,
                               int perCharMs) {
    if (text.empty()) {
        d_->SetError("empty text");
        return false;
    }
    const int want = static_cast<int>(text.size());

    // --- 1) CDP 优先：不受前台窗口影响，也不会串到别的 Chrome 实例 ---
    bool cdpSent = false;
    if (d_->session) {
        cdpSent = d_->SendCdpChars(text, 0, text.size(), perCharMs);
    }

    int entered = -1;
    if (cdpSent && !verifySelector.empty()) {
        for (int round = 0; round < 3; ++round) {
            SleepMs(400);
            entered = MaskLength(verifySelector);
            if (entered >= want) break;
            if (entered < 0) break;           // 控件不呈现可读长度，无法校验
            if (entered == 0) break;          // 一个字符都没进：控件拒绝合成键盘
            if (!d_->SendCdpChars(text, static_cast<size_t>(entered),
                                  text.size(), perCharMs))
                break;
        }
        if (entered >= want) {
            spdlog::info("[HID] CDP 键盘输入完成 ({}/{})", entered, want);
            return true;
        }
        if (entered < 0) {
            // 控件不呈现可读的字符数（既无星号也无 data-len）→ 无法校验，
            // 直接采信 CDP 的发送结果，避免回退硬件键盘导致重复输入
            spdlog::warn("[HID] 控件无法读取输入长度，按 CDP 结果采信");
            return cdpSent;
        }
    } else if (cdpSent && verifySelector.empty()) {
        return true;
    }

    spdlog::warn("[HID] CDP 键盘未生效 (entered={}/{}), 回退硬件键盘",
                 entered, want);

    // --- 2) 硬件键盘回退（真扫描码，绝大多数安全控件都接受）---
    if (!TypeHw(text, d_->keyDelay, false)) return false;
    if (verifySelector.empty()) return true;

    SleepMs(600);
    entered = MaskLength(verifySelector);
    if (entered >= want) {
        spdlog::info("[HID] 硬件键盘输入完成 ({}/{})", entered, want);
        return true;
    }
    d_->SetError("typed length mismatch");
    spdlog::error("[HID] 输入长度不足 ({}/{})", entered, want);
    return false;
}

bool HidInput::PressTab() {
    if (!OpenKeyboard(0xC216, 0x0301, d_->keyDelay)) return false;
    if (!FocusChrome(false, true)) return false;
    bool ok = d_->kb.pressKey(HidKeyBoard::KeyTab);
    UnpinChromeTopmost();
    if (!ok) d_->SetError(d_->kb.lastError());
    return ok;
}

bool HidInput::PressEnter() {
    if (!OpenKeyboard(0xC216, 0x0301, d_->keyDelay)) return false;
    if (!FocusChrome(false, true)) return false;
    bool ok = d_->kb.pressKey(HidKeyBoard::KeyEnter);
    UnpinChromeTopmost();
    if (!ok) d_->SetError(d_->kb.lastError());
    return ok;
}

bool HidInput::Backspace(int times) {
    if (!OpenKeyboard(0xC216, 0x0301, d_->keyDelay)) return false;
    if (!FocusChrome(false, true)) return false;
    bool ok = d_->kb.backspace(times);
    UnpinChromeTopmost();
    if (!ok) d_->SetError(d_->kb.lastError());
    return ok;
}

}  // namespace hid
}  // namespace newrpa
