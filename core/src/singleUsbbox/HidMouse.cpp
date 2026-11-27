// HidMouse.cpp —— 单头盒子硬件鼠标（逆向 msdk.dll 后的纯 HID 实现，零厂商 DLL 依赖）
#include "HidMouse.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

// 借用 setupapi 的类型定义（SP_DEVICE_INTERFACE_DATA / HDEVINFO 等），
// 函数仍走动态加载，不链接 setupapi.lib / hid.lib。
#include <setupapi.h>

namespace {

template <class Fn>
void bindFn(HMODULE mod, Fn& slot, const char* name)
{
    FARPROC p = GetProcAddress(mod, name);
    static_assert(sizeof(Fn) == sizeof(FARPROC), "函数指针尺寸不一致");
    if (p) std::memcpy(&slot, &p, sizeof(slot));
}

// 动态加载的 API 指针
typedef void   (WINAPI* PFN_HidGuid)(LPGUID);
typedef BOOLEAN(WINAPI* PFN_HidAttr)(HANDLE, PVOID);
typedef BOOLEAN(WINAPI* PFN_HidFeature)(HANDLE, PVOID, ULONG);
typedef HDEVINFO(WINAPI* PFN_GetClassDevs)(LPGUID, LPCSTR, HWND, DWORD);
typedef BOOL   (WINAPI* PFN_EnumIf)(HDEVINFO, PSP_DEVINFO_DATA, LPGUID, DWORD, PSP_DEVICE_INTERFACE_DATA);
// 真实签名 6 个参数：(hdevinfo, ifd, detail, detailSize, requiredSize, deviceInfoData)
typedef BOOL   (WINAPI* PFN_GetIfDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                         PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL   (WINAPI* PFN_DestroyList)(HDEVINFO);

struct HidApi {
    HMODULE hid = nullptr, setup = nullptr;
    PFN_HidGuid pGuid = nullptr;
    PFN_HidAttr pAttr = nullptr;
    PFN_HidFeature pSet = nullptr, pGet = nullptr;
    PFN_GetClassDevs pGetClass = nullptr;
    PFN_EnumIf pEnum = nullptr;
    PFN_GetIfDetail pDetail = nullptr;
    PFN_DestroyList pDestroy = nullptr;

    bool load() {
        if (hid && setup) return true;
        hid = LoadLibraryA("hid.dll");
        setup = LoadLibraryA("setupapi.dll");
        if (!hid || !setup) return false;
        bindFn(hid, pGuid, "HidD_GetHidGuid");
        bindFn(hid, pAttr, "HidD_GetAttributes");
        bindFn(hid, pSet,  "HidD_SetFeature");
        bindFn(hid, pGet,  "HidD_GetFeature");
        bindFn(setup, pGetClass, "SetupDiGetClassDevsA");
        bindFn(setup, pEnum,     "SetupDiEnumDeviceInterfaces");
        bindFn(setup, pDetail,   "SetupDiGetDeviceInterfaceDetailA");
        bindFn(setup, pDestroy,  "SetupDiDestroyDeviceInfoList");
        return pGuid && pAttr && pSet && pGetClass && pEnum && pDetail && pDestroy;
    }
    void free() {
        if (hid) FreeLibrary(hid);
        if (setup) FreeLibrary(setup);
        hid = nullptr;
        setup = nullptr;
        pGuid = nullptr;
        pAttr = nullptr;
        pSet = nullptr;
        pGet = nullptr;
        pGetClass = nullptr;
        pEnum = nullptr;
        pDetail = nullptr;
        pDestroy = nullptr;
    }
};

// HIDD_ATTRIBUTES 结构（Size, VendorID, ProductID, VersionNumber）
struct DevAttr { ULONG Size; USHORT VendorID; USHORT ProductID; USHORT VersionNumber; };

// 枚举 HID 设备，找指定 VID（pid=0 表示任意）。返回已打开的设备句柄。
HANDLE openBox(HidApi& api, int vid, int pid, char* err, size_t errCap)
{
    GUID guid{};
    api.pGuid(&guid);
    HDEVINFO info = api.pGetClass(&guid, nullptr, nullptr,
                                  0x00000002 | 0x00000010 /*DIGCF_PRESENT|DEVICEINTERFACE*/);
    if (info == reinterpret_cast<HDEVINFO>(INVALID_HANDLE_VALUE)) {
        std::snprintf(err, errCap, "SetupDiGetClassDevs 失败");
        return INVALID_HANDLE_VALUE;
    }
    HANDLE best = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; api.pEnum(info, nullptr, &guid, i, &ifd); ++i) {
        DWORD need = 0;
        api.pDetail(info, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(std::malloc(need));
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!api.pDetail(info, &ifd, detail, need, &need, nullptr)) { std::free(detail); continue; }
        HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        std::free(detail);
        if (h == INVALID_HANDLE_VALUE) continue;
        DevAttr a{}; a.Size = sizeof(a);
        if (api.pAttr(h, &a) && a.VendorID == static_cast<USHORT>(vid)) {
            if (pid == 0 || a.ProductID == static_cast<USHORT>(pid)) {
                best = h; // 命中：优先最后一个（多接口时 mi_02 通常靠后）
                if (pid != 0) break;
            }
        }
        if (best == INVALID_HANDLE_VALUE) CloseHandle(h);
        else if (h != best) CloseHandle(h);
    }
    api.pDestroy(info);
    if (best == INVALID_HANDLE_VALUE)
        std::snprintf(err, errCap, "找不到 VID=%04X 的盒子（USB 是否插好？）", vid);
    return best;
}

} // namespace

// ================================================================ DPI

void HidMouse::enableDpiAwareness()
{
    HMODULE sh = LoadLibraryA("shcore.dll");
    if (sh) {
        typedef HRESULT(WINAPI* PFN)(int);
        PFN set = nullptr;
        bindFn(sh, set, "SetProcessDpiAwareness");
        if (set) set(2);
        FreeLibrary(sh);
    }
}

// ================================================================ 生命周期

HidMouse::~HidMouse()
{
    close();
}

void HidMouse::setError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m_error, sizeof(m_error), fmt, ap);
    va_end(ap);
}

bool HidMouse::open(int vid, int pid)
{
    if (isOpen()) return true;
    m_error[0] = 0;
    enableDpiAwareness();

    // 系统 DPI
    HMODULE u32 = LoadLibraryA("user32.dll");
    if (u32) {
        typedef UINT(WINAPI* PFN_GetDpi)();
        PFN_GetDpi getDpi = nullptr;
        bindFn(u32, getDpi, "GetDpiForSystem");
        if (getDpi) m_dpi = static_cast<int>(getDpi());
        FreeLibrary(u32);
    }
    if (m_dpi <= 0) m_dpi = 96;
    m_scale = static_cast<double>(m_dpi) / 96.0;

    HidApi api;
    if (!api.load()) {
        setError("加载 hid.dll / setupapi.dll 失败");
        return false;
    }
    m_hid = api.hid; m_setup = api.setup; // 转移所有权，close() 里释放

    m_h = openBox(api, vid, pid, m_error, sizeof(m_error));
    if (m_h == INVALID_HANDLE_VALUE) {
        close();
        return false;
    }

    // 初始化报告（抓包 M_Open  sequences）
    std::memset(m_report, 0, sizeof(m_report));
    m_report[1] = 0xFF; m_report[4] = 0x01; m_report[5] = 0x55;
    if (!writeReport()) { close(); return false; }
    Sleep(150);

    // 松开所有键，避免上次残留
    releaseAll();
    m_ready = true;
    return true;
}

void HidMouse::close()
{
    if (m_h != INVALID_HANDLE_VALUE) {
        // 尽量松开按键再关闭
        std::memset(m_report, 0, sizeof(m_report));
        m_report[4] = 0x05; m_report[5] = 0x04; m_report[6] = 0x00;
        writeReport();
        CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }
    m_ready = false;
    if (m_hid) FreeLibrary(m_hid);
    if (m_setup) FreeLibrary(m_setup);
    m_hid = m_setup = nullptr;
}

bool HidMouse::writeReport()
{
    if (m_h == INVALID_HANDLE_VALUE || !m_hid) {
        setError("盒子未打开");
        return false;
    }
    HidApi api;
    api.hid = m_hid; api.setup = m_setup;
    bindFn(m_hid, api.pSet, "HidD_SetFeature");
    if (!api.pSet) { setError("HidD_SetFeature 不可用"); return false; }
    BOOLEAN ok = api.pSet(m_h, m_report, static_cast<ULONG>(sizeof(m_report)));
    return ok ? true : false;
}

// ================================================================ 坐标

void HidMouse::screenSize(int& w, int& h) const
{
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
}

void HidMouse::currentPos(int& x, int& y)
{
    POINT p{};
    GetCursorPos(&p);
    x = p.x; y = p.y;
}

void HidMouse::drain()
{
    if (m_h == INVALID_HANDLE_VALUE || !m_hid) return;
    typedef BOOLEAN(WINAPI* PFN_GetFeature)(HANDLE, PVOID, ULONG);
    PFN_GetFeature pGet = nullptr;
    FARPROC p = GetProcAddress(m_hid, "HidD_GetFeature");
    if (p) memcpy(&pGet, &p, sizeof(pGet));
    if (!pGet) return;
    BYTE r[65] = {0};
    Sleep(15);
    for (int i = 0; i < 60; ++i) {
        if (!pGet(m_h, r, sizeof(r))) break;
        if (r[2] != 0xCC) break;
        Sleep(3);
    }
    Sleep(12);
}

void HidMouse::drainFast()
{
    if (m_h == INVALID_HANDLE_VALUE || !m_hid) return;
    typedef BOOLEAN(WINAPI* PFN_GetFeature)(HANDLE, PVOID, ULONG);
    PFN_GetFeature pGet = nullptr;
    FARPROC p = GetProcAddress(m_hid, "HidD_GetFeature");
    if (p) memcpy(&pGet, &p, sizeof(pGet));
    if (!pGet) return;
    BYTE r[65] = {0};
    // 短轮询，不 Sleep：单次 GetFeature 通常 <1ms，最多 6 帧把邮箱读空。
    // 偶发固件重复帧由 TrackRel 的 GetCursorPos 闭环吸收。
    for (int i = 0; i < 6; ++i) {
        if (!pGet(m_h, r, sizeof(r))) break;
        if (r[2] != 0xCC) break;
    }
}

// ================================================================ 动作

bool HidMouse::moveRel(int dx, int dy)
{
    if (!m_ready) { setError("盒子未打开"); return false; }
    auto clip8 = [](int v) -> BYTE {
        if (v < -127) v = -127;
        if (v > 127) v = 127;
        return static_cast<BYTE>(static_cast<signed char>(v));
    };
    std::memset(m_report, 0, sizeof(m_report));
    m_report[4] = 0x05;
    m_report[5] = 0x04;
    m_report[6] = m_button;   // 保留按住状态：按住左键拖动时不能清成 0，否则一移动就松开
    m_report[7] = clip8(dx);
    m_report[8] = clip8(dy);
    return writeReport();
}

bool HidMouse::moveTo(int physX, int physY)
{
    if (!m_ready) { setError("盒子未打开"); return false; }
    const int MAX_ITER = 30;
    for (int it = 0; it < MAX_ITER; ++it) {
        int x = 0, y = 0;
        currentPos(x, y);
        int dx = physX - x, dy = physY - y;
        if (std::abs(dx) <= 2 && std::abs(dy) <= 2) return true;
        int sx = static_cast<int>(dx / m_moveGain);
        int sy = static_cast<int>(dy / m_moveGain);
        if (sx > 60) sx = 60; if (sx < -60) sx = -60;
        if (sy > 60) sy = 60; if (sy < -60) sy = -60;
        if (sx == 0 && dx != 0) sx = dx > 0 ? 1 : -1;
        if (sy == 0 && dy != 0) sy = dy > 0 ? 1 : -1;
        if (!moveRel(sx, sy)) return false;
        Sleep(40);
    }
    int x = 0, y = 0;
    currentPos(x, y);
    setError("闭环定位未收敛: cur=(%d,%d) target=(%d,%d)", x, y, physX, physY);
    return false;
}

bool HidMouse::moveToCss(double cssX, double cssY)
{
    return moveTo(static_cast<int>(cssToPhysX(cssX)),
                  static_cast<int>(cssToPhysY(cssY)));
}

void HidMouse::btn(int code, bool down)
{
    BYTE bit = (code == 2) ? 0x02 : 0x01;   // 1=左键 2=右键
    if (down) m_button |= bit;
    else      m_button &= static_cast<BYTE>(~bit);
    std::memset(m_report, 0, sizeof(m_report));
    m_report[4] = 0x05;
    m_report[5] = 0x04;
    m_report[6] = m_button;
    writeReport();
}

bool HidMouse::leftDown()  { if (!m_ready) return false; btn(1, true);  return true; }
bool HidMouse::leftUp()    { if (!m_ready) return false; btn(1, false); return true; }
bool HidMouse::rightDown() { if (!m_ready) return false; btn(2, true);  return true; }
bool HidMouse::rightUp()   { if (!m_ready) return false; btn(2, false); return true; }

bool HidMouse::click(int physX, int physY)
{
    if (!moveTo(physX, physY)) return false;
    Sleep(60);
    leftDown();
    Sleep(50);
    return leftUp();
}

bool HidMouse::releaseAll()
{
    if (!m_ready && m_h == INVALID_HANDLE_VALUE) return false;
    m_button = 0;
    std::memset(m_report, 0, sizeof(m_report));
    m_report[4] = 0x05; m_report[5] = 0x04; m_report[6] = 0x00;
    return writeReport();
}

bool HidMouse::dragRelativeCss(double startCssX, double startCssY, double deltaCss,
                               int steps, int minDelayMs, int maxDelayMs)
{
    if (!m_ready) { setError("盒子未打开"); return false; }
    if (steps < 2) steps = 2;
    if (minDelayMs < 0) minDelayMs = 0;
    if (maxDelayMs < minDelayMs) maxDelayMs = minDelayMs;

    // 先精确移到起点并按下
    if (!moveToCss(startCssX, startCssY)) return false;
    Sleep(120);
    leftDown();
    Sleep(120);

    // ease-in-out 相对位移 + 随机抖动（纵向 ±0.8 CSS px）
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        double ease = t * t * (3.0 - 2.0 * t);
        double jitterX = (static_cast<double>(std::rand()) / RAND_MAX - 0.5) * 1.0;
        double jitterY = (static_cast<double>(std::rand()) / RAND_MAX - 0.5) * 1.6;
        double dx = deltaCss * ease + jitterX;
        double dy = jitterY;
        int px = static_cast<int>(cssToPhysX(startCssX + dx));
        int py = static_cast<int>(cssToPhysY(startCssY + dy));
        if (!moveTo(px, py)) { leftUp(); return false; }
        int delay = minDelayMs;
        if (maxDelayMs > minDelayMs)
            delay += std::rand() % (maxDelayMs - minDelayMs + 1);
        Sleep(delay);
    }
    Sleep(120);
    // 终点精确补偿
    int ex = static_cast<int>(cssToPhysX(startCssX + deltaCss));
    int ey = static_cast<int>(cssToPhysY(startCssY));
    moveTo(ex, ey);
    Sleep(80);
    leftUp();
    Sleep(200);
    return true;
}
