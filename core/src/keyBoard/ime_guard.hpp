// ime_guard.hpp —— 输入法隔离（跨进程）
//
// 为什么不用 ImmSetOpenStatus：
//   ImmGetContext 只对"调用线程自己创建的窗口"有效。跨进程拿别的窗口句柄去调，
//   一律返回 NULL（本机实测：direct / AttachThreadInput 两条路都是 0），
//   所以那种写法看起来在关输入法，其实是空操作。
//
// 可行的办法是切键盘布局：
//   LoadKeyboardLayoutW(L"00000409") 把美式键盘载进来（不写入 HKCU\Keyboard Layout\Preload，
//   实测无残留），再给目标窗口 PostMessage(WM_INPUTLANGCHANGEREQUEST)，目标线程就会
//   切到该布局。英文布局下没有 IME 拦截，ASCII 必定原样上屏；用完再切回去。
#pragma once
#ifndef IME_GUARD_HPP
#define IME_GUARD_HPP

#include <windows.h>

namespace ime {

// 是否"纯英文键盘布局"（这种布局下 ASCII 一定按原样上屏）
inline bool isPlainLayout(HKL hkl)
{
    // 低 16 位是语言 ID，0x09 结尾的都是英语系：0x0409 en-US / 0x0809 en-GB / 0x0C09 en-AU。
    // 不用 ImmIsIME 判断：本机上它对 0x0409 也返回 TRUE，不可靠。
    const WORD lang = LOWORD(reinterpret_cast<ULONG_PTR>(hkl));
    return (lang & 0x00FF) == 0x0009;
}

// 窗口所在线程当前用的布局
inline HKL layoutOf(HWND hwnd)
{
    if (!hwnd) return nullptr;
    return GetKeyboardLayout(GetWindowThreadProcessId(hwnd, nullptr));
}

// 语言 ID -> 便于阅读的名字（只覆盖常见的几种）
inline const char* layoutName(HKL hkl)
{
    switch (LOWORD(reinterpret_cast<ULONG_PTR>(hkl))) {
        case 0x0409: return "英文(美国)";
        case 0x0804: return "中文(简体)";
        case 0x0404: return "中文(繁体)";
        case 0x0411: return "日文";
        case 0x0412: return "韩文";
        default:     return "其它";
    }
}

// 作用域内把目标窗口切到英文布局，析构时切回。
class Bypass
{
public:
    Bypass() : Bypass(true, nullptr) {}
    explicit Bypass(HWND target) : Bypass(true, target) {}

    // enable=false 时完全不动输入法（只是站在那儿，析构也不做事）
    Bypass(bool enable, HWND target)
        : m_hwnd(target ? target : GetForegroundWindow())
    {
        if (!enable) {
            m_note = "已关闭输入法隔离";
            return;
        }
        if (!m_hwnd) {
            m_note = "没有前台窗口，跳过输入法处理";
            return;
        }

        m_tid = GetWindowThreadProcessId(m_hwnd, nullptr);
        m_old = GetKeyboardLayout(m_tid);

        if (isPlainLayout(m_old)) {          // 本来就是英文布局，不用动
            m_engaged = true;
            m_note = "已是英文布局";
            return;
        }

        // KLF_NOTELLSHELL：不惊动语言栏，减少闪烁
        m_us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
        if (!m_us) {
            m_note = "系统里没有可用的英文布局（LoadKeyboardLayout 失败）";
            return;
        }

        if (!PostMessageW(m_hwnd, WM_INPUTLANGCHANGEREQUEST, 0,
                          reinterpret_cast<LPARAM>(m_us))) {
            m_us = nullptr;
            m_note = "无法向目标窗口请求切换输入法";
            return;
        }

        for (int i = 0; i < 25; ++i) {       // 最多等 500ms
            if (GetKeyboardLayout(m_tid) == m_us) {
                m_engaged = true;
                m_note = "已切到英文布局";
                return;
            }
            Sleep(20);
        }
        m_note = "目标窗口没有响应输入法切换";
    }

    ~Bypass() { restore(); }

    Bypass(const Bypass&) = delete;
    Bypass& operator=(const Bypass&) = delete;

    // 是否真的切过去了。false 时调用方应改用不经过键盘的通道。
    bool engaged() const { return m_engaged; }
    const char* note() const { return m_note; }

    void restore()
    {
        if (!m_engaged || !m_us || !m_old || m_old == m_us) return;
        if (!IsWindow(m_hwnd)) { m_us = nullptr; return; }

        PostMessageW(m_hwnd, WM_INPUTLANGCHANGEREQUEST, 0,
                     reinterpret_cast<LPARAM>(m_old));
        for (int i = 0; i < 25; ++i) {
            if (GetKeyboardLayout(m_tid) == m_old) break;
            Sleep(20);
        }
        m_us = nullptr;          // 只还原一次
    }

private:
    HWND  m_hwnd = nullptr;
    DWORD m_tid  = 0;
    HKL   m_old  = nullptr;
    HKL   m_us   = nullptr;
    bool  m_engaged = false;
    const char* m_note = "";
};

} // namespace ime

#endif // IME_GUARD_HPP
