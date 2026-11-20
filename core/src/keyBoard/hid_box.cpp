// hid_box.cpp —— 原生 HID 驱动实现
//
// 系统 API 全部运行时解析：编译不链接 hid.lib / setupapi.lib，
// 运行只依赖 Windows 自带的 hid.dll 和 setupapi.dll。

#include "hid_box.hpp"

#include <setupapi.h>
#include <hidsdi.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------- 系统函数
// 用显式函数指针类型声明，不引用导入库里的任何符号
typedef BOOLEAN (WINAPI *PFN_GetHidGuid)(LPGUID);
typedef BOOLEAN (WINAPI *PFN_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (WINAPI *PFN_GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA*);
typedef NTSTATUS(WINAPI *PFN_HidP_GetCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
typedef BOOLEAN (WINAPI *PFN_FreePreparsedData)(PHIDP_PREPARSED_DATA);
typedef BOOLEAN (WINAPI *PFN_SetFeature)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (WINAPI *PFN_GetFeature)(HANDLE, PVOID, ULONG);

typedef HDEVINFO(WINAPI *PFN_GetClassDevs)(const GUID*, PCSTR, HWND, DWORD);
typedef BOOL    (WINAPI *PFN_EnumInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL    (WINAPI *PFN_GetInterfaceDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                                 PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL    (WINAPI *PFN_DestroyDevInfo)(HDEVINFO);

struct Api
{
    HMODULE hid = nullptr;
    HMODULE setup = nullptr;

    PFN_GetHidGuid        GetHidGuid        = nullptr;
    PFN_GetAttributes     GetAttributes     = nullptr;
    PFN_GetPreparsedData  GetPreparsedData  = nullptr;
    PFN_HidP_GetCaps      GetCaps           = nullptr;
    PFN_FreePreparsedData FreePreparsedData = nullptr;
    PFN_SetFeature        SetFeature        = nullptr;
    PFN_GetFeature        GetFeature        = nullptr;

    PFN_GetClassDevs        GetClassDevs        = nullptr;
    PFN_EnumInterfaces      EnumInterfaces      = nullptr;
    PFN_GetInterfaceDetail  GetInterfaceDetail  = nullptr;
    PFN_DestroyDevInfo      DestroyDevInfo      = nullptr;

    bool ok = false;
    char err[256] = {0};

    Api() { load(); }

    // FARPROC 转函数指针：用 memcpy 避开 -Wcast-function-type
    template <class Fn>
    static void bind(HMODULE mod, Fn& slot, const char* name)
    {
        FARPROC p = GetProcAddress(mod, name);
        static_assert(sizeof(Fn) == sizeof(FARPROC), "函数指针尺寸不一致");
        std::memcpy(&slot, &p, sizeof(slot));
    }

    void load()
    {
        hid   = LoadLibraryA("hid.dll");
        setup = LoadLibraryA("setupapi.dll");
        if (!hid || !setup) {
            std::snprintf(err, sizeof(err), "加载系统 HID 组件失败（错误码 %lu）",
                          static_cast<unsigned long>(GetLastError()));
            return;
        }

        bind(hid, GetHidGuid,        "HidD_GetHidGuid");
        bind(hid, GetAttributes,     "HidD_GetAttributes");
        bind(hid, GetPreparsedData,  "HidD_GetPreparsedData");
        bind(hid, GetCaps,           "HidP_GetCaps");
        bind(hid, FreePreparsedData, "HidD_FreePreparsedData");
        bind(hid, SetFeature,        "HidD_SetFeature");
        bind(hid, GetFeature,        "HidD_GetFeature");

        bind(setup, GetClassDevs,       "SetupDiGetClassDevsA");
        bind(setup, EnumInterfaces,     "SetupDiEnumDeviceInterfaces");
        bind(setup, GetInterfaceDetail, "SetupDiGetDeviceInterfaceDetailA");
        bind(setup, DestroyDevInfo,     "SetupDiDestroyDeviceInfoList");

        ok = GetHidGuid && GetAttributes && GetPreparsedData && GetCaps &&
             FreePreparsedData && SetFeature && GetFeature &&
             GetClassDevs && EnumInterfaces && GetInterfaceDetail && DestroyDevInfo;
        if (!ok)
            std::snprintf(err, sizeof(err),
                          "hid.dll / setupapi.dll 缺少必要的导出函数，系统组件可能已损坏");
    }
};

Api& api()
{
    static Api instance;   // C++11 magic static，首次调用时初始化
    return instance;
}

// ---------------------------------------------------------------- 枚举
// 厂商自定义页：单头盒子是 0xFFA0，这里放宽到整个 0xFFxx
bool isVendorPage(unsigned short page) { return (page & 0xFF00) == 0xFF00; }

bool pathHasTag(const char* path, const char* tag)
{
    const size_t n = std::strlen(tag);
    for (const char* p = path; *p; ++p) {
        size_t i = 0;
        while (i < n && std::tolower(static_cast<unsigned char>(p[i])) == tag[i]) ++i;
        if (i == n) return true;
    }
    return false;
}

// 设备路径里带 vid_xxxx&pid_yyyy，不打开设备也能认出来
bool pathMatches(const char* path, unsigned vid, unsigned pid)
{
    char tag[16];
    std::snprintf(tag, sizeof(tag), "vid_%04x", vid);
    if (!pathHasTag(path, tag)) return false;
    std::snprintf(tag, sizeof(tag), "pid_%04x", pid);
    return pathHasTag(path, tag);
}

struct Candidate
{
    std::string path;
    bool opened = false;          // CreateFile 成功
    bool vendor = false;          // 是厂商自定义页（能收发私有帧）
    unsigned short reportLen = 0;
};

// 枚举出所有 VID/PID 匹配的 HID 接口。只读，打开后立即关闭。
void collect(unsigned vid, unsigned pid, std::vector<Candidate>& out)
{
    Api& a = api();
    if (!a.ok) return;

    GUID guid;
    a.GetHidGuid(&guid);

    HDEVINFO devs = a.GetClassDevs(&guid, nullptr, nullptr,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifc;
    std::memset(&ifc, 0, sizeof(ifc));
    ifc.cbSize = sizeof(ifc);

    for (DWORD i = 0; a.EnumInterfaces(devs, nullptr, &guid, i, &ifc); ++i) {
        DWORD need = 0;
        a.GetInterfaceDetail(devs, &ifc, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<char> buf(need);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_A>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!a.GetInterfaceDetail(devs, &ifc, det, need, nullptr, nullptr)) continue;
        if (!pathMatches(det->DevicePath, vid, pid)) continue;

        Candidate c;
        c.path = det->DevicePath;

        HANDLE h = CreateFileA(det->DevicePath,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            c.opened = true;

            HIDD_ATTRIBUTES at;
            std::memset(&at, 0, sizeof(at));
            at.Size = sizeof(at);

            PHIDP_PREPARSED_DATA pd = nullptr;
            HIDP_CAPS caps;
            std::memset(&caps, 0, sizeof(caps));
            const bool gotCaps = a.GetPreparsedData(h, &pd) && pd &&
                                 a.GetCaps(pd, &caps) == HIDP_STATUS_SUCCESS;
            if (pd) a.FreePreparsedData(pd);

            if (gotCaps && isVendorPage(caps.UsagePage)) {
                c.vendor = true;
                c.reportLen = caps.OutputReportByteLength;
            }
            CloseHandle(h);
        }
        out.push_back(c);
    }

    a.DestroyDevInfo(devs);
}

// ---------------------------------------------------------------- 帧参数
constexpr unsigned short kDefaultReportLen = 65;
constexpr unsigned char  kFrameLen    = 0x04;   // 数据长度
constexpr unsigned char  kFrameCheck  = 0x83;   // 校验（结构固定时恒定）
constexpr unsigned char  kBusy        = 0xCC;   // 盒子还在处理上一条
constexpr int            kSettleMs    = 15;     // 发完这一帧到开始轮询的等待
constexpr int            kPollMax     = 60;     // 最多轮询次数
constexpr int            kPollStepMs  = 3;
constexpr int            kTailMs      = 12;     // 轮询完成后的余量，防丢字符

} // namespace

// ================================================================ 探测

namespace hidbox {

const Model kSingleHead = {"单头键盘盒子", 0xC216, 0x0301};

// 一句话结论，直接写到 info->detail 里
static void fillSummary(DeviceInfo* info)
{
    const bool isKnown = (info->vid == kSingleHead.vid && info->pid == kSingleHead.pid);
    const char* name = isKnown ? kSingleHead.name : "键盘盒子";

    if (info->usable) {
        std::snprintf(info->detail, sizeof(info->detail),
                      "%s 已插好（%d 个接口，报告长度 %u 字节）",
                      name, info->interfaces,
                      static_cast<unsigned>(info->reportLen));
    } else if (info->present) {
        std::snprintf(info->detail, sizeof(info->detail),
                      "检测到 %s（VID=%04X PID=%04X），但打不开厂商接口——"
                      "可能被其它程序占用，或固件不是这个型号",
                      name, info->vid, info->pid);
    } else {
        std::snprintf(info->detail, sizeof(info->detail),
                      "没检测到 %s（VID=%04X PID=%04X）——检查 USB 是否插好",
                      name, info->vid, info->pid);
    }
}

bool probe(unsigned vid, unsigned pid, DeviceInfo* out)
{
    DeviceInfo info;
    info.vid = vid;
    info.pid = pid;

    std::vector<Candidate> list;
    collect(vid, pid, list);

    info.present    = !list.empty();
    info.interfaces = static_cast<int>(list.size());

    // 优先 0xFFA0 那一组；退一步接受任何 0xFFxx 厂商页
    for (const Candidate& c : list) {
        if (c.vendor && c.reportLen >= 8) {
            info.usable = true;
            info.reportLen = c.reportLen;
            std::snprintf(info.path, sizeof(info.path), "%s", c.path.c_str());
            break;
        }
    }
    if (!info.usable) {
        for (const Candidate& c : list) {
            if (!c.opened) info.busy = true;
        }
    }

    fillSummary(&info);
    if (out) *out = info;
    return info.usable;
}

// ================================================================ 设备

void Box::setError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m_err, sizeof(m_err), fmt, ap);
    va_end(ap);
}

bool Box::open(unsigned vid, unsigned pid)
{
    if (isOpen()) return true;
    m_vid = vid;
    m_pid = pid;

    Api& a = api();
    if (!a.ok) {
        setError("%s", a.err);
        return false;
    }

    std::vector<Candidate> list;
    collect(vid, pid, list);

    bool matchedVidPid = !list.empty();
    bool openedButNoVendor = false;

    for (const Candidate& c : list) {
        if (!c.vendor) { if (c.opened) openedButNoVendor = true; continue; }

        HANDLE h = CreateFileA(c.path.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        m_dev = h;
        m_reportLen = c.reportLen > 0 ? c.reportLen : kDefaultReportLen;
        return true;
    }

    if (matchedVidPid) {
        if (openedButNoVendor)
            setError("找到 VID=%04X PID=%04X 的设备，但它的接口不是厂商自定义页"
                     "（UsagePage 0xFFxx）——固件不匹配", vid, pid);
        else
            setError("找到 VID=%04X PID=%04X 的设备，但打不开厂商接口——"
                     "可能被其它程序独占，或 USB 权限不足", vid, pid);
    } else {
        setError("没检测到 %s（VID=%04X PID=%04X）——检查 USB 是否插好",
                 (vid == kSingleHead.vid && pid == kSingleHead.pid) ? kSingleHead.name : "键盘盒子",
                 vid, pid);
    }
    return false;
}

void Box::close()
{
    if (m_dev != INVALID_HANDLE_VALUE) {
        CloseHandle(m_dev);
        m_dev = INVALID_HANDLE_VALUE;
    }
}

bool Box::isAlive()
{
    if (m_dev == INVALID_HANDLE_VALUE) return false;
    Api& a = api();
    if (!a.ok) return false;

    HIDD_ATTRIBUTES at;
    std::memset(&at, 0, sizeof(at));
    at.Size = sizeof(at);
    if (a.GetAttributes(m_dev, &at)) return true;

    close();          // 拔线了，句柄已经废了
    return false;
}

bool Box::waitForDevice(int timeoutMs, int pollMs)
{
    if (isAlive()) return true;
    if (pollMs < 50) pollMs = 50;

    for (int waited = 0; ; waited += pollMs) {
        if (open(m_vid, m_pid)) return true;
        if (waited >= timeoutMs) break;
        Sleep(static_cast<DWORD>(pollMs));
    }
    setError("%d 毫秒内没等到 %s（VID=%04X PID=%04X）",
             timeoutMs, (m_vid == kSingleHead.vid && m_pid == kSingleHead.pid)
                            ? kSingleHead.name : "键盘盒子", m_vid, m_pid);
    return false;
}

bool Box::writeFrame(unsigned char mods, unsigned char key)
{
    Api& a = api();
    unsigned char f[256];
    std::memset(f, 0, sizeof(f));
    f[0] = 0;              // Report ID
    f[1] = 0;              // 通道 0 = 键盘数据
    f[4] = kFrameLen;      // 数据长度
    f[5] = kFrameCheck;    // 校验
    f[6] = mods;           // 修饰键位图
    f[8] = key;            // 第一个普通键

    if (!a.SetFeature(m_dev, f, m_reportLen)) return false;

    // 盒子是邮箱式的：不把响应读走，下一条命令就不会被接收。
    // 帧间必须留出处理时间，否则连续发送会丢字符。
    Sleep(kSettleMs);
    for (int i = 0; i < kPollMax; ++i) {
        unsigned char r[256];
        std::memset(r, 0, sizeof(r));
        if (!a.GetFeature(m_dev, r, m_reportLen)) break;
        if (r[2] != kBusy) break;
        Sleep(kPollStepMs);
    }
    Sleep(kTailMs);
    return true;
}

bool Box::send(unsigned char mods, unsigned char key)
{
    Api& a = api();
    if (!a.ok) {
        setError("%s", a.err);
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (m_dev == INVALID_HANDLE_VALUE) {
            // 掉线重连：拔了再插回来时，下一次发送自动接上
            if (!m_reconnect || !open(m_vid, m_pid)) {
                if (m_err[0] == 0) setError("设备未打开");
                return false;
            }
        }
        if (writeFrame(mods, key)) return true;

        setError("发送按键帧失败（HidD_SetFeature 返回 false）");
        if (!m_reconnect) return false;

        close();      // 可能是拔线了，重开一次再试
    }
    return false;
}

bool Box::tap(int hidCode, unsigned char modifiers)
{
    if (modifiers) {
        // 修饰键是状态保持的：单独的"建立状态"帧不能省
        if (!send(modifiers, 0)) return false;
        if (!send(modifiers, static_cast<unsigned char>(hidCode))) return false;
        if (!send(modifiers, 0)) return false;
        return send(0, 0);             // 清掉修饰，否则会一直按着
    }

    if (!send(0, static_cast<unsigned char>(hidCode))) return false;
    return send(0, 0);
}

bool Box::chord(int hidCode, unsigned char modifiers)
{
    if (!keyDown(hidCode, modifiers)) return false;
    return release();
}

bool Box::keyDown(int hidCode, unsigned char modifiers)
{
    if (modifiers && !send(modifiers, 0)) return false;
    return send(modifiers, static_cast<unsigned char>(hidCode));
}

bool Box::release()
{
    if (m_dev == INVALID_HANDLE_VALUE) return true;
    return send(0, 0);
}

} // namespace hidbox
