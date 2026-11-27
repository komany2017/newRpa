// HidMouse.hpp —— 单头盒子硬件鼠标（纯 HID，无厂商 DLL 依赖）
//
// 逆向 msdk.dll 后的零依赖实现：直接用 SetupAPI + HidD_SetFeature 与盒子通讯，
// 不再加载 msdk.dll。盒子是标准 USB HID 设备（VID=0xC216，如 PID=0x0301 mi_02）。
//
// 为什么用盒子硬件鼠标而不是 CDP / SendInput：
//   * 盒子发出的是真 USB HID 鼠标事件，JS 里 isTrusted=true，能绕过顶象/ BJCA 的
//     自动化检测；CDP Input.dispatchMouseEvent 是合成事件，容易被识别。
//   * 不加载厂商 DLL，部署只需系统自带 hid.dll/setupapi.dll，无中文路径依赖。
//
// 逆向得到的 HID 报告协议（65 字节 = 1 字节 reportID + 64 字节 payload）：
//   * 初始化:        [1]=0xFF [4]=0x01 [5]=0x55
//   * 相对移动:      [4]=0x05 [5]=0x04 [6]=0x00 [7]=dx(int8) [8]=dy(int8)
//   * 左键按下/松开: [4]=0x05 [5]=0x04 [6]=0x01 / 0x00
//   * 右键:          [6]=0x02 / 0x00（同为 keycode 位）
//   * MoveR 是"强度"语义（发 100 实际位移随加速曲线波动），不是像素。因此
//     moveTo 用闭环：发 MoveR -> GetCursorPos 读回 -> 按误差迭代逼近目标。
//
// 坐标与 DPI：
//   * 进程需 SetProcessDpiAwareness(2)，鼠标/窗口坐标才是物理像素。
//   * scale = GetDpiForSystem()/96（本机 120/96 = 1.25）。
//   * CSS->物理：phys = origin + css*scale。origin 由调用方标定后 setOrigin() 传入。
//
// 典型用法：
//   HidMouse m;
//   if (!m.open()) { log(m.lastError()); return; }
//   m.setOrigin(10, 250);                 // 由 CDP 注入 mousemove + 反推得到
//   m.moveToCss(sliderCssX, sliderCssY);  // 闭环精确移到滑块
//   m.leftDown();
//   m.dragRelativeCss(startX, startY, targetDeltaCss);  // 拟人相对拖动
//   m.leftUp();
#pragma once
#ifndef HID_MOUSE_HPP
#define HID_MOUSE_HPP

#include <windows.h>

#include <string>
#include <vector>

class HidMouse
{
public:
    HidMouse() = default;
    ~HidMouse();
    HidMouse(const HidMouse&) = delete;
    HidMouse& operator=(const HidMouse&) = delete;

    // ---------------------------------------------------------------- 设备
    // 打开盒子（按 VID=0xC216 枚举，优先 PID=0x0301）。无 msdk.dll 依赖。
    bool open(int vid = 0xC216, int pid = 0);
    void close();
    bool isOpen() const { return m_h != INVALID_HANDLE_VALUE; }

    // ---------------------------------------------------------------- DPI/坐标
    static void enableDpiAwareness();
    int  dpi() const { return m_dpi; }
    double scale() const { return m_scale; }

    void screenSize(int& w, int& h) const;

    void setOrigin(double ox, double oy) { m_ox = ox; m_oy = oy; }
    double originX() const { return m_ox; }
    double originY() const { return m_oy; }

    double cssToPhysX(double cssX) const { return m_ox + cssX * m_scale; }
    double cssToPhysY(double cssY) const { return m_oy + cssY * m_scale; }

    // ---------------------------------------------------------------- 鼠标动作（物理坐标）
    bool moveTo(int physX, int physY);          // 闭环精确定位（误差 ±2px）
    bool moveRel(int dx, int dy);               // 原始相对移动（强度语义，不保证像素）
    bool leftDown();
    bool leftUp();
    bool rightDown();
    bool rightUp();
    bool click(int physX, int physY);
    bool releaseAll();

    // ---------------------------------------------------------------- 鼠标动作（CSS 坐标，需先 setOrigin）
    bool moveToCss(double cssX, double cssY);

    // 拟人相对拖动：从 (startCssX,startCssY) 按住，按 ease-in-out 曲线相对右移 deltaCss 像素，
    // 每步带随机抖动和随机停顿，最后精确落到终点再松开。
    bool dragRelativeCss(double startCssX, double startCssY, double deltaCss,
                         int steps = 40, int minDelayMs = 25, int maxDelayMs = 60);

    static void currentPos(int& x, int& y);

    // 邮箱清空：SetFeature 后调用（Sleep15ms + GetFeature 轮询到响应非 0xCC + Sleep12ms）。
    // 不清邮箱时部分固件会把上一帧重复应用——实测 rel(30,0) 走出 219px 且带 y 漂移。
    void drain();

    // 低开销邮箱清空：不 Sleep，只轮询几帧 GetFeature。
    // 用于高频拟人轨迹（8~12ms/帧）；偶发重复帧由调用方 GetCursorPos 闭环吸收。
    void drainFast();

    const char* lastError() const { return m_error; }

private:
    void setError(const char* fmt, ...);
    bool writeReport();                 // 把 m_report 写入盒子（HidD_SetFeature）
    void btn(int code, bool down);      // 按键报告（code: 1=左 2=右）

    HANDLE  m_h = INVALID_HANDLE_VALUE;
    BYTE    m_report[65] = {0};         // 复用的报告缓冲
    BYTE    m_button = 0;               // 当前按住的鼠标键位 [6]（0=松开 1=左 2=右），移动时保留

    // 系统 DLL 动态加载（不链接 setupapi.lib/hid.lib）
    HMODULE m_hid = nullptr;
    HMODULE m_setup = nullptr;

    bool m_ready = false;
    int  m_dpi = 96;
    double m_scale = 1.0;
    double m_ox = 0.0, m_oy = 0.0;
    double m_moveGain = 2.47;           // MoveR 强度->像素比例，闭环用

    mutable char m_error[512] = {0};
};

#endif // HID_MOUSE_HPP
