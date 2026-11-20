// hid_box.hpp —— 键盘盒子（默认 VID_C216 / PID_0301，单头）的原生 HID 驱动
//
// 不依赖任何第三方库：
//   * 不链接 msdk.lib / msdkLib.lib / hid.lib / setupapi.lib
//   * 不加载 msdk.dll
//   * hid.dll 和 setupapi.dll 是 Windows 自带组件，运行时经 GetProcAddress 取函数
//
// 私有协议是从 msdk 的真实通信帧里逆出来的：
//
//   帧长 65 字节（含 1 字节 Report ID），走 HidD_SetFeature / HidD_GetFeature
//   [0]      Report ID        = 0
//   [1]      通道             = 0（键盘数据）
//   [4]      数据长度         = 4
//   [5]      校验             = 0x83（该结构下恒定）
//   [6]      修饰键位图        Ctrl=0x01 Shift=0x02 Alt=0x04 Win=0x08
//                             RCtrl=0x10 RShift=0x20 RAlt=0x40 RWin=0x80
//   [8..13] 普通键码（HID Usage），最多 6 个
//
//   修饰键是"状态保持"的：必须先单独发一帧建立状态，再发键码帧。
//   每次 SetFeature 之后要读响应，读到 [2] != 0xCC 才算这一帧完成
//   （0xCC = 盒子还在处理上一条）。
#pragma once
#ifndef HID_BOX_HPP
#define HID_BOX_HPP

#include <windows.h>

// 修饰键位图（标准 HID 键盘报告定义）
enum HidModifier : unsigned char {
    HID_MOD_LCTRL  = 0x01,
    HID_MOD_LSHIFT = 0x02,
    HID_MOD_LALT   = 0x04,
    HID_MOD_LWIN   = 0x08,
    HID_MOD_RCTRL  = 0x10,
    HID_MOD_RSHIFT = 0x20,
    HID_MOD_RALT   = 0x40,
    HID_MOD_RWIN   = 0x80,
};

namespace hidbox {

// ---------------------------------------------------------------- 型号
struct Model {
    const char* name;
    unsigned    vid;
    unsigned    pid;
};

// 单头盒子：复合 HID 设备，mi_02 是厂商自定义接口（UsagePage 0xFFA0）
extern const Model kSingleHead;

// ---------------------------------------------------------------- 探测
// 只读枚举，不改设备状态、不发任何帧，随时可调用（用于"盒子插了没"的判断）
struct DeviceInfo
{
    bool present    = false;   // 系统里有这个 VID/PID 的设备
    bool usable     = false;   // 有能收发私有帧的厂商接口
    bool busy       = false;   // 找到设备但打不开（被占用 / 权限不足）
    int  interfaces = 0;       // 匹配 VID/PID 的 HID 接口数
    unsigned short reportLen = 0;
    char path[MAX_PATH] = {0}; // 可用接口的设备路径（定长 buffer，跨 DLL 边界安全）
    char detail[512] = {0};    // 一句话结论，可直接打给用户看
    unsigned vid = 0;
    unsigned pid = 0;
};

// 探测。返回值等价于 out->usable（能用则 true）
bool probe(unsigned vid, unsigned pid, DeviceInfo* out = nullptr);

// ---------------------------------------------------------------- 设备
class Box
{
public:
    Box() = default;
    ~Box() { close(); }

    Box(const Box&) = delete;
    Box& operator=(const Box&) = delete;

    // 枚举 HID 设备，打开厂商自定义接口。已经打开时直接返回 true。
    bool open(unsigned vid = kSingleHead.vid, unsigned pid = kSingleHead.pid);
    void close();
    bool isOpen() const { return m_dev != INVALID_HANDLE_VALUE; }

    // 句柄是否还有效：拔线后 HidD_GetAttributes 会失败
    bool isAlive();

    // 轮询等待设备出现（热插拔）。返回 false 表示超时
    bool waitForDevice(int timeoutMs, int pollMs = 200);

    // 单击一个键。modifiers 是 HidModifier 的位组合，0 表示不带修饰。
    bool tap(int hidCode, unsigned char modifiers = 0);

    // 组合键（Ctrl+V 之类）：按住修饰 + 按键，再全部放开
    bool chord(int hidCode, unsigned char modifiers);

    // 按下 / 弹起分离，供需要长按的场景使用
    bool keyDown(int hidCode, unsigned char modifiers = 0);
    bool release();

    // 掉线后自动重连（默认开）。关掉后发送失败就立即返回 false。
    void setReconnect(bool on) { m_reconnect = on; }

    const char* error() const { return m_err; }

private:
    bool writeFrame(unsigned char mods, unsigned char key);
    bool send(unsigned char mods, unsigned char key);
    void setError(const char* fmt, ...);

    HANDLE m_dev = INVALID_HANDLE_VALUE;
    unsigned short m_reportLen = 65;
    unsigned m_vid = kSingleHead.vid;   // 记住打开参数，断了能按原样重连
    unsigned m_pid = kSingleHead.pid;
    bool m_reconnect = true;
    char m_err[512] = {0};
};

} // namespace hidbox

#endif // HID_BOX_HPP
