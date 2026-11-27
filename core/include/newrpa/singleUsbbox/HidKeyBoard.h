// SkKeyBoard.h —— 硬件键盘盒子（默认 VID_C216 / PID_0301，单头）的输入封装
//
// 依赖：只有 Windows 自带的 hid.dll / setupapi.dll / user32（运行时解析）。
//       不链接任何 .lib，不加载 msdk.dll。
//
// 三层职责：
//   hid_box.hpp    设备层：探测 / 打开 / 收发 65 字节私有帧（会自动重连）
//   ime_guard.hpp  输入法层：输入前把目标窗口切到英文布局，输完切回
//   本类           文本层：把一段文本拆成"扫描码段"和"剪贴板段"分别送出去
#pragma once
#ifndef SKKEYBOARD_H
#define SKKEYBOARD_H

#include <windows.h>

#include <string>

#include "hid_box.hpp"

class HidKeyBoard
{
public:
    // 文本编码。Auto：先按 UTF-8 严格校验，不通过再按系统 ANSI（简体中文即 GBK）
    enum class Encoding { Auto, Utf8, Ansi };

    // 具名按键，配合 pressKey() 使用
    enum Key {
        KeyEnter, KeyTab, KeyEsc, KeySpace, KeyBackspace, KeyDelete, KeyInsert,
        KeyHome, KeyEnd, KeyPageUp, KeyPageDown,
        KeyUp, KeyDown, KeyLeft, KeyRight,
    };

    // 设备状态快照（detect / status 的返回值）
    struct Status {
        bool plugged   = false;   // 系统里有这个盒子
        bool usable    = false;   // 能收发私有帧（真正可用）
        bool connected = false;   // 当前进程已打开
        int  interfaces = 0;      // 匹配 VID/PID 的 HID 接口数
        unsigned vid = 0;
        unsigned pid = 0;
        char path[MAX_PATH] = {0};  // 设备路径（定长 buffer，跨 DLL 安全）
        char detail[512] = {0};     // 一句话结论，可直接打给用户看
    };

    HidKeyBoard() = default;
    ~HidKeyBoard() = default;

    HidKeyBoard(const HidKeyBoard&) = delete;
    HidKeyBoard& operator=(const HidKeyBoard&) = delete;

    // ---------------------------------------------------------------- 检测
    // 只看不动：不发帧、不改变设备状态，随时可调用
    static Status detect(int vid = 0, int pid = 0);
    Status status() const;
    bool isPlugged() const { return status().usable; }
    bool isOpen() const { return m_box.isOpen(); }

    // ---------------------------------------------------------------- 连接
    // 打不开时会按 hotplugWait 等待插入（默认不等）。vid/pid 传 0 表示用默认型号。
    bool open(int vid = 0xC216, int pid = 0x0301);
    void close();
    bool waitForDevice(int timeoutMs);

    // ---------------------------------------------------------------- 输入
    // 输入一段文本，自动选通道：
    //   * 可打印 ASCII / 换行 / Tab / 退格 -> 逐个发 HID 扫描码（真键盘行为）
    //   * 中文、emoji、全角符号等 -> 走剪贴板 + Ctrl+V
    //   * 混排文本按连续段切开，两种通道交替使用
    // 输入前自动把目标窗口切到英文布局（可在选项里关掉）。
    // delayMs 是每个按键之间的基准间隔，内部会加随机抖动。
    bool type(const char* text, int delayMs = 60);
    bool type(const wchar_t* text, int delayMs = 60);
    bool type(const std::wstring& text, int delayMs = 60);

    // 强制整段走剪贴板（中文/emoji 最稳），会临时占用剪贴板并还原
    bool paste(const char* text, int delayMs = 120);
    bool paste(const wchar_t* text, int delayMs = 120);

    // 只发按键，不输入文本
    bool press(int hidCode, int times = 1);
    bool pressKey(Key key, int times = 1);
    bool enter();
    bool del();
    bool backspace(int times = 1);

    // ---------------------------------------------------------------- 选项
    void setEncoding(Encoding e) { m_encoding = e; }
    void setImeIsolation(bool on) { m_imeIsolation = on; }   // 默认 true
    void setAutoPaste(bool on) { m_autoPaste = on; }         // 默认 true
    void setHotplugWait(int ms) { m_hotplugWait = ms; }      // 默认 0
    void setKeyDelay(int ms) { m_keyDelay = ms; }
    void setPasteChord(int hidCode, unsigned char mods);     // 默认 Ctrl+V

    // 失败原因（"没检测到设备"/"打不开厂商接口"/"含不支持的字符"…）
    const char* lastError() const { return m_error; }

    // -------------------------------------------------------- 兼容旧接口
    // 保留原签名给旧调用方：input/input2 会先随机等一下、按 20 次退格清空输入框，
    // 再按系统 ANSI（GBK）解释文本。
    bool input(int vid = 0xC216, int pid = 0x0301, char* lpbuf = nullptr, int delay = 200);
    bool input2(int vid, int pid, char* lpbuf, int delay = 100);
    bool ClickEnter(int vid = 0xC216, int pid = 0x0301);
    bool ClickDelete(int vid = 0xC216, int pid = 0x0301);

private:
    bool ensureReady();
    std::wstring decode(const char* text) const;
    bool typeSegments(const std::wstring& text, int delayMs);
    bool typeRun(const wchar_t* s, size_t n, int delayMs);
    bool pasteRun(const std::wstring& text, int delayMs);
    void setError(const char* fmt, ...);

    hidbox::Box m_box;

    Encoding m_encoding   = Encoding::Auto;
    bool m_imeIsolation   = true;
    bool m_autoPaste      = true;
    int  m_hotplugWait    = 0;
    int  m_keyDelay       = 60;
    int  m_pasteKey       = 25;                // HID 用法 25 = V
    unsigned char m_pasteMods = HID_MOD_LCTRL; // Ctrl+V

    mutable char m_error[512] = {0};
};

#endif // SKKEYBOARD_H
