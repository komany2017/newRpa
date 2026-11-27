// SkKeyBoard.cpp —— 硬件键盘盒子输入实现
//
// 设计要点：
//   1. 零第三方依赖：只经 hid_box.hpp 走 Windows 原生 HID API，
//      没有 msdk.dll，也没有任何 .lib
//   2. 表驱动：ASCII 0x20..0x7E 全部压进一张 95 项常量表，
//      加字符只改表，不改逻辑
//   3. 分段输入：能发扫描码的连成一段逐键敲；中文/emoji 连成一段走剪贴板，
//      混排文本（"账号 abc123，备注：张三"）自动交替
//   4. 输入法隔离：发扫描码前把目标窗口切到英文布局，输完切回
//      （ImmSetOpenStatus 跨进程无效，这里用 WM_INPUTLANGCHANGEREQUEST）

#include "HidKeyBoard.h"
#include "ime_guard.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// ---------------------------------------------------------------- HID 键码
// USB HID Usage Table, Keyboard/Keypad page (0x07)。
// 命名按键位物理位置，不按字符 —— 一个键能出两个字符（如 1 和 !），
// 具体出哪个由修饰键决定，这也正是 kAscii 表里要存 shift 的原因。
enum : unsigned char {
    kA = 4, kB, kC, kD, kE, kF, kG, kH, kI, kJ, kK, kL, kM,
    kN, kO, kP, kQ, kR, kS, kT, kU, kV, kW, kX, kY, kZ,        // 4..29
    k1 = 30, k2, k3, k4, k5, k6, k7, k8, k9, k0,               // 30..39
    kEnter = 40, kEsc, kBackspace, kTab, kSpace,
    kMinus, kEqual, kLBracket, kRBracket, kBackslash,
    kSemicolon = 51, kQuote, kBackquote, kComma, kDot, kSlash, // 50 号位未定义
    kCapsLock = 57,
    kF1 = 58, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11, kF12,
    kPrintScreen, kScrollLock, kPause,
    kInsert, kHome, kPageUp, kDelete, kEnd, kPageDown,
    kRight, kLeft, kDown, kUp,                                  // 79..82
    kNumLock, kKeypadSlash, kKeypadStar, kKeypadMinus, kKeypadPlus, kKeypadEnter,
    kLeftCtrl = 224, kLeftShift, kLeftAlt, kLeftWin,
    kRightCtrl, kRightShift, kRightAlt, kRightWin,
};

struct Stroke {
    unsigned char code;   // HID 键码
    bool          shift;  // 是否需要同时按住 Shift
};

// ---------------------------------------------------------------- ASCII 表
// 下标 = 字符 - 0x20，覆盖可打印 ASCII 全集（0x20 空格 ~ 0x7E 波浪号）共 95 项。
constexpr Stroke kAscii[95] = {
    // 0x20
    {kSpace,     false},  // ' '
    {k1,         true },  // '!'  Shift+1
    {kQuote,     true },  // '"'
    {k3,         true },  // '#'
    {k4,         true },  // '$'
    {k5,         true },  // '%'
    {k7,         true },  // '&'
    {kQuote,     false},  // '\''
    {k9,         true },  // '('
    {k0,         true },  // ')'
    {k8,         true },  // '*'
    {kEqual,     true },  // '+'
    {kComma,     false},  // ','
    {kMinus,     false},  // '-'
    {kDot,       false},  // '.'
    {kSlash,     false},  // '/'
    // 0x30 数字行（不带 Shift 时出数字，带 Shift 时出上面的符号）
    {k0, false}, {k1, false}, {k2, false}, {k3, false}, {k4, false},
    {k5, false}, {k6, false}, {k7, false}, {k8, false}, {k9, false},
    // 0x3A
    {kSemicolon, true },  // ':'
    {kSemicolon, false},  // ';'
    {kComma,     true },  // '<'
    {kEqual,     false},  // '='
    {kDot,       true },  // '>'
    {kSlash,     true },  // '?'
    {k2,         true },  // '@'  Shift+2
    // 0x41 大写字母
    {kA, true}, {kB, true}, {kC, true}, {kD, true}, {kE, true}, {kF, true},
    {kG, true}, {kH, true}, {kI, true}, {kJ, true}, {kK, true}, {kL, true},
    {kM, true}, {kN, true}, {kO, true}, {kP, true}, {kQ, true}, {kR, true},
    {kS, true}, {kT, true}, {kU, true}, {kV, true}, {kW, true}, {kX, true},
    {kY, true}, {kZ, true},
    // 0x5B
    {kLBracket,  false},  // '['
    {kBackslash, false},  // '\\'
    {kRBracket,  false},  // ']'
    {k6,         true },  // '^'
    {kMinus,     true },  // '_'
    {kBackquote, false},  // '`'
    // 0x61 小写字母
    {kA, false}, {kB, false}, {kC, false}, {kD, false}, {kE, false}, {kF, false},
    {kG, false}, {kH, false}, {kI, false}, {kJ, false}, {kK, false}, {kL, false},
    {kM, false}, {kN, false}, {kO, false}, {kP, false}, {kQ, false}, {kR, false},
    {kS, false}, {kT, false}, {kU, false}, {kV, false}, {kW, false}, {kX, false},
    {kY, false}, {kZ, false},
    // 0x7B
    {kLBracket,  true },  // '{'
    {kBackslash, true },  // '|'
    {kRBracket,  true },  // '}'
    {kBackquote, true },  // '~'
};

// 一个字符怎么敲。code == 0 表示键盘打不出来（中文、emoji、全角符号…）。
Stroke strokeFor(wchar_t ch)
{
    switch (ch) {
        case L'\n': case L'\r': return {kEnter,     false};
        case L'\t':             return {kTab,       false};
        case L'\b':             return {kBackspace, false};
        default: break;
    }
    if (ch >= 0x20 && ch <= 0x7E) return kAscii[ch - 0x20];
    return Stroke{0, false};
}

bool isTypeable(wchar_t ch) { return strokeFor(ch).code != 0; }

int hidCodeOf(HidKeyBoard::Key k)
{
    switch (k) {
        case HidKeyBoard::KeyEnter:     return kEnter;
        case HidKeyBoard::KeyTab:       return kTab;
        case HidKeyBoard::KeyEsc:       return kEsc;
        case HidKeyBoard::KeySpace:     return kSpace;
        case HidKeyBoard::KeyBackspace: return kBackspace;
        case HidKeyBoard::KeyDelete:    return kDelete;
        case HidKeyBoard::KeyInsert:    return kInsert;
        case HidKeyBoard::KeyHome:      return kHome;
        case HidKeyBoard::KeyEnd:       return kEnd;
        case HidKeyBoard::KeyPageUp:    return kPageUp;
        case HidKeyBoard::KeyPageDown:  return kPageDown;
        case HidKeyBoard::KeyUp:        return kUp;
        case HidKeyBoard::KeyDown:      return kDown;
        case HidKeyBoard::KeyLeft:      return kLeft;
        case HidKeyBoard::KeyRight:     return kRight;
    }
    return 0;
}

// ---------------------------------------------------------------- 编码工具
// 整串都是合法 UTF-8、且至少含一个 3 字节或 4 字节序列时才认定是 UTF-8。
// 只判"合法"不够：GBK 双字节经常恰好落在 UTF-8 的二字组合法区间
// （"我" 的 GBK 是 CE D2，正好能解析成 U+03D2），要靠 3/4 字节序列这个强特征区分。
// 4 字节必须算进来：漏了它，纯 emoji 文本会被当成 GBK，😀 变成"馃榾"。
bool looksLikeUtf8(const char* s)
{
    bool hasWideSeq = false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        if (*p < 0x80) { ++p; continue; }

        int need;
        unsigned char lo = 0x80, hi = 0xBF;
        if (*p >= 0xC2 && *p <= 0xDF) {
            need = 1;
        } else if (*p >= 0xE0 && *p <= 0xEF) {
            need = 2;
            hasWideSeq = true;
            if (*p == 0xE0) lo = 0xA0;   // 排除过长编码
            if (*p == 0xED) hi = 0x9F;   // 排除代理区
        } else if (*p >= 0xF0 && *p <= 0xF4) {
            need = 3;
            hasWideSeq = true;
            if (*p == 0xF0) lo = 0x90;
            if (*p == 0xF4) hi = 0x8F;
        } else {
            return false;
        }
        ++p;
        for (int i = 0; i < need; ++i, ++p) {
            if (*p < lo || *p > hi) return false;
            lo = 0x80; hi = 0xBF;        // 续字节回到普通区间
        }
    }
    return hasWideSeq;
}

// 指定代码页 -> UTF-16
std::wstring toWide(const char* s, UINT cp)
{
    int wlen = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return std::wstring();

    std::wstring w(static_cast<size_t>(wlen), L'\0');
    if (!MultiByteToWideChar(cp, 0, s, -1, &w[0], wlen)) return std::wstring();
    w.resize(static_cast<size_t>(wlen) - 1);   // 去掉结尾的 L'\0'
    return w;
}

// ---------------------------------------------------------------- 剪贴板
bool clipboardReadText(std::wstring& out)
{
    out.clear();
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

bool clipboardWriteText(const std::wstring& text)
{
    // 剪贴板可能被别的程序短暂占用，重试几次
    for (int i = 0; i < 5; ++i) {
        if (!OpenClipboard(nullptr)) { Sleep(20); continue; }

        EmptyClipboard();
        bool ok = false;
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(mem)) {
                std::memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                if (SetClipboardData(CF_UNICODETEXT, mem)) ok = true;
                else GlobalFree(mem);
            } else {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
        if (ok) return true;
        Sleep(20);
    }
    return false;
}

// 按键之间的随机延时，模仿人手
void jitter(int baseMs)
{
    if (baseMs <= 0) return;
    const int lo = baseMs / 2 > 0 ? baseMs / 2 : 1;
    const int span = baseMs - lo + 1;
    Sleep(static_cast<DWORD>(lo + (span > 1 ? std::rand() % span : 0)));
}

} // namespace

// ================================================================ 状态

HidKeyBoard::Status HidKeyBoard::detect(int vid, int pid)
{
    if (vid == 0) vid = 0xC216;
    if (pid == 0) pid = 0x0301;

    hidbox::DeviceInfo info;
    hidbox::probe(static_cast<unsigned>(vid), static_cast<unsigned>(pid), &info);

    Status st;
    st.vid        = info.vid;
    st.pid        = info.pid;
    st.plugged    = info.present;
    st.usable     = info.usable;
    st.interfaces = info.interfaces;
    std::snprintf(st.path,   sizeof(st.path),   "%s", info.path);
    std::snprintf(st.detail, sizeof(st.detail), "%s", info.detail);
    return st;
}

HidKeyBoard::Status HidKeyBoard::status() const
{
    Status st = detect();
    st.connected = m_box.isOpen();
    return st;
}

// ================================================================ 连接

void HidKeyBoard::setError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m_error, sizeof(m_error), fmt, ap);
    va_end(ap);
}

void HidKeyBoard::setPasteChord(int hidCode, unsigned char mods)
{
    m_pasteKey = hidCode;
    m_pasteMods = mods;
}

bool HidKeyBoard::open(int vid, int pid)
{
    if (m_box.isOpen()) return true;
    if (vid == 0) vid = 0xC216;
    if (pid == 0) pid = 0x0301;

    if (m_box.open(static_cast<unsigned>(vid), static_cast<unsigned>(pid))) {
        std::srand(static_cast<unsigned>(GetTickCount()));
        return true;
    }

    // 没插上时可以等一会儿（热插拔场景），但错误信息要保留第一次的原因
    if (m_hotplugWait > 0 && m_box.waitForDevice(m_hotplugWait)) {
        std::srand(static_cast<unsigned>(GetTickCount()));
        return true;
    }

    setError("%s", m_box.error());
    return false;
}

void HidKeyBoard::close()
{
    m_box.close();
}

bool HidKeyBoard::waitForDevice(int timeoutMs)
{
    if (m_box.waitForDevice(timeoutMs)) return true;
    setError("%s", m_box.error());
    return false;
}

// 每次输入前都重新确认一遍：盒子在不在、句柄还有没有效
bool HidKeyBoard::ensureReady()
{
    if (m_box.isOpen() && m_box.isAlive()) return true;

    const Status st = detect();
    if (!st.usable) {
        if (m_hotplugWait > 0 && waitForDevice(m_hotplugWait)) return true;
        setError("%s", st.detail);
        return false;
    }
    return open();
}

// ================================================================ 文本

std::wstring HidKeyBoard::decode(const char* text) const
{
    switch (m_encoding) {
        case Encoding::Utf8: return toWide(text, CP_UTF8);
        case Encoding::Ansi: return toWide(text, CP_ACP);
        case Encoding::Auto: break;
    }
    return toWide(text, looksLikeUtf8(text) ? CP_UTF8 : CP_ACP);
}

bool HidKeyBoard::type(const char* text, int delayMs)
{
    if (!text || !*text) {
        setError("没有要输入的文本");
        return false;
    }
    const std::wstring w = decode(text);
    if (w.empty()) {
        setError("文本解码失败：编码不对？用 setEncoding() 指明 UTF-8 或 ANSI(GBK)");
        return false;
    }
    return type(w, delayMs);
}

bool HidKeyBoard::type(const wchar_t* text, int delayMs)
{
    if (!text || !*text) {
        setError("没有要输入的文本");
        return false;
    }
    return type(std::wstring(text), delayMs);
}

bool HidKeyBoard::type(const std::wstring& text, int delayMs)
{
    if (text.empty()) {
        setError("没有要输入的文本");
        return false;
    }
    if (!ensureReady()) return false;
    return typeSegments(text, delayMs < 0 ? m_keyDelay : delayMs);
}

bool HidKeyBoard::typeSegments(const std::wstring& raw, int delayMs)
{
    // \r\n -> \n，否则会打两个回车
    std::wstring text;
    text.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r') {
            if (i + 1 < raw.size() && raw[i + 1] == L'\n') continue;
            text.push_back(L'\n');
        } else {
            text.push_back(raw[i]);
        }
    }

    bool hasKeys = false;
    for (size_t i = 0; i < text.size() && !hasKeys; ++i) hasKeys = isTypeable(text[i]);

    // 要发扫描码就得先切断输入法。切不动就地降级成剪贴板，保证内容正确。
    HWND target = hasKeys ? GetForegroundWindow() : nullptr;
    ime::Bypass imeGuard(m_imeIsolation && hasKeys, target);
    const bool keysSafe = !hasKeys || !m_imeIsolation || imeGuard.engaged();

    size_t i = 0;
    while (i < text.size()) {
        const bool keyable = keysSafe && isTypeable(text[i]);

        size_t j = i;
        while (j < text.size() && (keysSafe && isTypeable(text[j])) == keyable) ++j;

        const std::wstring chunk = text.substr(i, j - i);
        if (keyable) {
            if (!typeRun(chunk.data(), chunk.size(), delayMs)) return false;
        } else {
            if (!m_autoPaste) {
                setError("字符 U+%04X 键盘打不出来，而自动剪贴板通道已关闭",
                         static_cast<unsigned>(chunk[0]));
                return false;
            }
            if (!pasteRun(chunk, delayMs)) return false;
        }
        i = j;
    }
    return true;
}

// 一段"能敲"的文本：逐字符发扫描码
bool HidKeyBoard::typeRun(const wchar_t* s, size_t n, int delayMs)
{
    for (size_t i = 0; i < n; ++i) {
        const Stroke st = strokeFor(s[i]);
        if (st.code == 0) {
            setError("字符 U+%04X 没有对应的键位", static_cast<unsigned>(s[i]));
            return false;
        }
        if (!m_box.tap(st.code, st.shift ? HID_MOD_LSHIFT : 0)) {
            setError("%s", m_box.error());
            return false;
        }
        jitter(delayMs);
    }
    return true;
}

// 一段"敲不出来"的文本：整段写进剪贴板，再 Ctrl+V 粘上去
bool HidKeyBoard::pasteRun(const std::wstring& text, int delayMs)
{
    // 先备份原剪贴板文本，粘完再放回去，尽量不给用户留副作用
    std::wstring backup;
    const bool hadText = clipboardReadText(backup);

    if (!clipboardWriteText(text)) {
        setError("写剪贴板失败，可能被其它程序占用");
        return false;
    }

    const bool ok = m_box.chord(m_pasteKey, m_pasteMods);

    // 等目标窗口真正取走剪贴板内容，再还原，否则可能粘出旧内容
    Sleep(static_cast<DWORD>(delayMs > 0 ? delayMs : 120));

    if (hadText) clipboardWriteText(backup);

    if (!ok) {
        setError("%s", m_box.error());
        return false;
    }
    return true;
}

bool HidKeyBoard::paste(const char* text, int delayMs)
{
    if (!text || !*text) {
        setError("没有要输入的文本");
        return false;
    }
    const std::wstring w = decode(text);
    if (w.empty()) {
        setError("文本解码失败：编码不对？用 setEncoding() 指明 UTF-8 或 ANSI(GBK)");
        return false;
    }
    if (!ensureReady()) return false;
    return pasteRun(w, delayMs);
}

bool HidKeyBoard::paste(const wchar_t* text, int delayMs)
{
    if (!text || !*text) {
        setError("没有要输入的文本");
        return false;
    }
    if (!ensureReady()) return false;
    return pasteRun(std::wstring(text), delayMs);
}

// ================================================================ 单键

bool HidKeyBoard::press(int hidCode, int times)
{
    if (hidCode <= 0) {
        setError("无效的键码 %d", hidCode);
        return false;
    }
    if (!ensureReady()) return false;

    const int n = times < 1 ? 1 : times;
    for (int i = 0; i < n; ++i) {
        if (!m_box.tap(hidCode)) {
            setError("%s", m_box.error());
            return false;
        }
        jitter(m_keyDelay);
    }
    return true;
}

bool HidKeyBoard::pressKey(Key key, int times)
{
    const int code = hidCodeOf(key);
    if (code == 0) {
        setError("无效的具名按键");
        return false;
    }
    return press(code, times);
}

bool HidKeyBoard::enter()              { return press(kEnter); }
bool HidKeyBoard::del()                { return press(kDelete); }
bool HidKeyBoard::backspace(int times) { return press(kBackspace, times); }

// ================================================================ 旧接口
// 行为与原实现一致：先随机等一会儿，再按 20 次退格清空输入框，然后输入。
// 老调用方传进来的是 GBK，这里固定按 ANSI 解释，不受 setEncoding 影响。

bool HidKeyBoard::input(int vid, int pid, char* lpbuf, int delay)
{
    return input2(vid, pid, lpbuf, delay);
}

bool HidKeyBoard::input2(int vid, int pid, char* lpbuf, int delay)
{
    if (!lpbuf) {
        setError("文本指针为空");
        return false;
    }
    if (!open(vid, pid)) return false;

    const Encoding saved = m_encoding;
    m_encoding = Encoding::Ansi;

    jitter(800);
    const bool ok = backspace(20) && type(lpbuf, delay);

    m_encoding = saved;
    return ok;
}

bool HidKeyBoard::ClickEnter(int vid, int pid)
{
    if (!open(vid, pid)) return false;

    jitter(900);
    // 用小键盘回车（88）：银行类客户端对回车的接收方式不统一，兼容面更广。
    // 想要主键盘回车用 enter()。
    if (!press(kKeypadEnter)) return false;

    close();
    return true;
}

bool HidKeyBoard::ClickDelete(int vid, int pid)
{
    if (!open(vid, pid)) return false;

    jitter(900);
    if (!press(kDelete)) return false;

    close();
    return true;
}
