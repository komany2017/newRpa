// ============================================================================
// KLB 银行插件实现 — 昆仑银行企业网银（查询版）
//
// 登录: CDP 填明文 + CDP 鼠标聚焦 + 硬件键盘盒子输密码（iProtect 控件只认真键盘）
// 取数: 业务页面全部在顶层 iframe#iframepage 里，同源 JS 直取
// 回单: URMPreviewReceipt.do 直出 PDF（官方下载 zip 走行内网，外网不可达）
// ============================================================================

#include "KLBPlugin.h"
#include "newrpa/util/StringUtils.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <regex>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <random>

using namespace newrpa;
namespace fs = std::filesystem;

namespace newrpa {
namespace klb {

// ----------------------------------------------------------------------------
// DLL 导出
// ----------------------------------------------------------------------------
extern "C" {
plugin::IBankPlugin* CreatePlugin()  { return new KLBPlugin(); }
void DestroyPlugin(plugin::IBankPlugin* p) { delete p; }
}

// ----------------------------------------------------------------------------
// 构造/析构/基础
// ----------------------------------------------------------------------------

KLBPlugin::KLBPlugin()  = default;
KLBPlugin::~KLBPlugin() { Close(); }

uint32_t KLBPlugin::Capabilities() const {
    return plugin::IBankPlugin::CAP_LOGIN
         | plugin::IBankPlugin::CAP_CUR_BALANCE
         | plugin::IBankPlugin::CAP_CUR_TRANS
         | plugin::IBankPlugin::CAP_YES_TRANS
         | plugin::IBankPlugin::CAP_SLIP;
}

void KLBPlugin::SetSession(std::shared_ptr<cdp::CDPSession> s,
                            std::shared_ptr<chrome::ChromeBrowser> b) {
    session_ = std::move(s);
    browser_ = std::move(b);
    if (session_) {
        page_  = std::make_unique<cdp::PageDomain>(session_.get());
        rt_    = std::make_unique<cdp::RuntimeDomain>(session_.get());
        input_ = std::make_unique<cdp::InputDomain>(session_.get());
        // 监听 JS 弹窗（密码错误等失败提示通常走 alert，先记下内容便于定位）
        if (dialogEvHandle_) session_->OffEvent("Page.javascriptDialogOpening",
                                                dialogEvHandle_);
        dialogEvHandle_ = session_->OnEvent(
            "Page.javascriptDialogOpening", [this](const nlohmann::json& p) {
                std::lock_guard<std::mutex> lk(dialogMtx_);
                lastDialogMsg_ = p.value("message", std::string());
                spdlog::warn("[KLB] JS dialog: {}", lastDialogMsg_);
            });
    }
}

void KLBPlugin::SetConfig(const plugin::BankConfig& cfg) { cfg_ = cfg; }

void KLBPlugin::Close() { loggedIn_ = false; }

// ----------------------------------------------------------------------------
// helper
// ----------------------------------------------------------------------------

std::string KLBPlugin::EvalStr(const std::string& js, int timeoutMs) {
    if (!rt_) return "";
    return rt_->EvalString(js, timeoutMs);
}

bool KLBPlugin::EvalBool(const std::string& js, int timeoutMs) {
    if (!rt_) return false;
    return rt_->EvalBool(js, timeoutMs);
}

bool KLBPlugin::WaitBool(const std::string& js, int timeoutMs, int stepMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (EvalBool(js)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return false;
}

std::string KLBPlugin::IframeDoc() {
    return "document.getElementById('iframepage').contentDocument";
}

// 解析 Java bean toString 风格 "{k=v, k=v, ...}"
// 值里可能含逗号（如 AcPerMit=0,1,4），按 ", key=" 前瞻切分
std::map<std::string, std::string> KLBPlugin::ParseBean(const std::string& s) {
    std::map<std::string, std::string> out;
    std::string body = s;
    if (!body.empty() && body.front() == '{') body = body.substr(1);
    if (!body.empty() && body.back() == '}')  body.pop_back();

    static const std::regex keyStart(",\\s*[A-Za-z_][A-Za-z0-9_]*=");
    size_t pos = 0;
    std::string cur;
    auto flush = [&](std::string seg) {
        auto eq = seg.find('=');
        if (eq == std::string::npos) return;
        std::string k = seg.substr(0, eq);
        // 去掉 k 开头的逗号与空白
        size_t kb = k.find_first_not_of(", \t\r\n");
        if (kb == std::string::npos) return;
        k = k.substr(kb);
        // 尾部空格
        size_t ke = k.find_last_not_of(" \t\r\n");
        k = (ke == std::string::npos) ? k : k.substr(0, ke + 1);
        std::string v = seg.substr(eq + 1);
        if (!k.empty()) out[k] = v;
    };
    std::smatch m;
    std::string rest = body;
    while (std::regex_search(rest, m, keyStart)) {
        cur += m.prefix().str();
        flush(cur);
        cur = m.str().substr(1);  // 保留 "key="，去掉逗号
        rest = m.suffix().str();
    }
    cur += rest;
    flush(cur);
    return out;
}

// ----------------------------------------------------------------------------
// 硬件键盘（HidKeyBoard 直接调用，不经外部工具）
// ----------------------------------------------------------------------------

namespace {

struct FindCtx {
    std::wstring clsKey;
    HWND best = nullptr;
    int  bestLen = 0;
};

BOOL CALLBACK EnumChromeProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 256);
    if (std::wstring(cls).find(ctx->clsKey) == std::wstring::npos) return TRUE;
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return TRUE;
    RECT r{};
    GetWindowRect(h, &r);
    if ((r.right - r.left) < 600 || (r.bottom - r.top) < 400) return TRUE;
    if (n > ctx->bestLen) { ctx->bestLen = n; ctx->best = h; }
    return TRUE;
}

}  // namespace

// 按类名找 Chrome 主窗口（复用 EnumChromeProc 的过滤逻辑）
static HWND FindChromeWindow() {
    FindCtx ctx{L"Chrome_WidgetWin_1", nullptr, 0};
    EnumWindows(EnumChromeProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

bool KLBPlugin::FocusChromeWindow() {
    HWND h = FindChromeWindow();
    if (!h) {
        spdlog::error("[KLB] Chrome window not found");
        return false;
    }
    // 置顶：硬件键盘字符进的是前台窗口，输密码期间被别的窗口盖住就串了
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (GetForegroundWindow() == h) return true;
    ShowWindow(h, SW_RESTORE);
    // ALT 敲一下解除前台锁
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
    Sleep(200);
    bool ok = (GetForegroundWindow() == h);
    if (!ok) spdlog::warn("[KLB] Chrome foreground not set");
    return ok;
}

// 输完取消置顶（恢复普通层级，不干扰用户后续操作）
void KLBPlugin::UnpinChromeTopmost() {
    HWND h = FindChromeWindow();
    if (h) SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool KLBPlugin::HardwareType(const std::string& text, int delayMs) {
    if (!FocusChromeWindow()) return false;
    HidKeyBoard kb;
    kb.setImeIsolation(true);
    kb.setKeyDelay(delayMs);
    auto st = HidKeyBoard::detect();
    spdlog::info("[KLB] HW keyboard: plugged={} usable={} detail={}",
                 st.plugged, st.usable, st.detail);
    if (!kb.open()) {
        spdlog::error("[KLB] HidKeyBoard open failed: {}", kb.lastError());
        return false;
    }
    kb.backspace(32);  // 清空，防残留
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // 模拟人工输入：逐字发送，字间随机停顿，每隔 2~4 个字符还有一次
    // "想一想"的长顿（200~600ms），避免匀速机关枪式的输入
    // 注意：不能加 static —— lambda 捕获非自动存储期变量是未定义行为（C++20 起报错）
    std::mt19937 rng{
        static_cast<unsigned>(std::chrono::steady_clock::now()
                              .time_since_epoch().count())};
    auto rnd = [&rng](int lo, int hi) {
        return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1));
    };
    int sinceLong = 0;
    int nextLong = rnd(2, 4);
    for (size_t i = 0; i < text.size(); ++i) {
        if (!kb.type(std::wstring(1, static_cast<wchar_t>(text[i])), delayMs)) {
            spdlog::error("[KLB] HW type char {} failed: {}",
                          static_cast<unsigned char>(text[i]), kb.lastError());
            UnpinChromeTopmost();
            return false;
        }
        // 字间停顿 40~160ms（叠加 HidKeyBoard 内部 jitter [delayMs/2, delayMs)）
        std::this_thread::sleep_for(std::chrono::milliseconds(rnd(40, 160)));
        ++sinceLong;
        if (sinceLong >= nextLong && i + 1 < text.size()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(rnd(200, 600)));
            sinceLong = 0;
            nextLong = rnd(2, 4);
        }
    }
    spdlog::info("[KLB] HW type: ok=true chars={} (human pacing)", text.size());
    UnpinChromeTopmost();
    return true;
}

bool KLBPlugin::HardwareTab() {
    if (!FocusChromeWindow()) return false;
    HidKeyBoard kb;
    kb.setImeIsolation(true);
    kb.setKeyDelay(60);
    if (!kb.open()) {
        spdlog::error("[KLB] HidKeyBoard open failed: {}", kb.lastError());
        return false;
    }
    bool ok = kb.pressKey(HidKeyBoard::KeyTab);
    spdlog::info("[KLB] HW Tab: ok={} err={}", ok, kb.lastError());
    UnpinChromeTopmost();
    return ok;
}

// 关掉可能弹着的 JS alert/confirm（银行登录失败提示常见形态）。
// alert 挂起期间 Runtime.evaluate 全部阻塞，不关掉后续检测全是超时
void KLBPlugin::DismissJsDialog() {
    if (!session_) return;
    try {
        session_->Call("Page.handleJavaScriptDialog",
                       {{"accept", false}}, 2000);
        spdlog::info("[KLB] JS dialog dismissed");
    } catch (...) {
        // 没有弹窗时该命令会报错，忽略即可
    }
}

// ----------------------------------------------------------------------------
// 菜单导航
// ----------------------------------------------------------------------------

bool KLBPlugin::GotoMenu(const std::string& menuText, const std::string& iframeSub,
                          int timeoutMs) {
    std::string menuEsc = util::ReplaceAll(menuText, "\\", "\\\\");
    menuEsc = util::ReplaceAll(menuEsc, "\"", "\\\"");
    std::string js =
        "(function(){var els=document.querySelectorAll('a');"
        "for(var i=0;i<els.length;i++){"
        "if((els[i].innerText||'').trim()==='" + menuEsc + "')"
        "{els[i].click();return true;}}return false;})()";
    if (!EvalBool(js)) {
        spdlog::error("[KLB] menu '{}' not found", menuText);
        return false;
    }
    return WaitBool("document.getElementById('iframepage') && "
                    "document.getElementById('iframepage').src.indexOf('" +
                    iframeSub + "')>=0", timeoutMs);
}

// ----------------------------------------------------------------------------
// 登录
// ----------------------------------------------------------------------------

bool KLBPlugin::WaitLoginForm(int timeoutMs) {
    // 三重就绪：文档加载完 + 控件已替换 input + UserId 位置没被遮罩挡住
    // （Chrome 刚启动时表单元素可能先出现，但 iProtect 初始化遮罩还在，
    //  此时点击会落到 BODY 上）
    return WaitBool(
        "document.readyState==='complete' && "
        "!!document.getElementById('CifNo') && "
        "!!document.getElementById('_safepass') && "
        "(function(){var e=document.getElementById('UserId');"
        "if(!e) return false;var r=e.getBoundingClientRect();"
        "var t=document.elementFromPoint(r.x+r.width/2,r.y+r.height/2);"
        "return !!t && (t.id==='UserId'||t.tagName==='INPUT');})()",
        timeoutMs);
}

std::string KLBPlugin::LoginImpl() {
    if (!session_ || !browser_) return "error: session not ready";
    if (loggedIn_) return "already logged in";

    // 已经连续失败 2 次（密码错误等）→ 不再重试，防止反复输错锁号
    if (loginFailCount_ >= 2) {
        spdlog::error("[KLB] login already failed {} times, stop retrying "
                      "(check password / account status)", loginFailCount_);
        return "error: 登录已连续失败2次，停止重试（请检查密码或账户状态）";
    }

    std::string url = cfg_.loginUrl.empty()
        ? "https://cbank-simp.klb.cn/eweb/prelogin.do?LoginType=Q&_locale=zh_CN&BankId=9999"
        : cfg_.loginUrl;

    std::string curUrl = EvalStr("location.href");
    spdlog::info("[KLB] Current page: {}", curUrl);

    // 已登录？登录成功后顶层文档会加载 iframe#iframepage（src 是 welcome.do
    // 或任一业务页），未登录时顶层文档就是登录表单（无 iframepage）
    if (EvalBool("!!document.getElementById('iframepage')")) {
        spdlog::info("[KLB] Already logged in (iframepage exists)");
        loggedIn_ = true;
        return "already logged in";
    }

    // 登录表单没出现（可能停在 logout.do / 业务页残留）→ 导航到登录页
    if (!WaitLoginForm(3000)) {
        spdlog::info("[KLB] Not on login page, navigating to {}", url);
        page_->Navigate(url, 30000);
        if (!WaitLoginForm()) {
            spdlog::error("[KLB] Login form not found. URL={}", EvalStr("location.href"));
            return "error: login form not found";
        }
    }
    spdlog::info("[KLB] Login form ready");

    // --- 1. 填客户号 / 操作员号（普通输入框，CDP 直接赋值）---
    std::string cif = cfg_.account.empty() ? "200009642" : cfg_.account;
    std::string uid = cfg_.operatorId.empty() ? "ZL123" : cfg_.operatorId;
    EvalStr("document.getElementById('CifNo').value='" + cif + "';"
            "document.getElementById('UserId').value='" + uid + "';");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // --- 2. 聚焦密码控件：鼠标点操作员输入框 → 硬件键盘 Tab（走正常焦点
    //     导航，比直接点 _safepass 可靠）；Tab 失败再回退直接点控件 ---
    auto ClickBox = [this](const std::string& id) -> bool {
        std::string boxJson = EvalStr(
            "(function(){var e=document.getElementById('" + id + "');"
            "if(!e) return '';var r=e.getBoundingClientRect();"
            "return JSON.stringify({x:r.x+r.width/2,y:r.y+r.height/2});})()");
        if (boxJson.empty()) return false;
        try {
            auto j = nlohmann::json::parse(boxJson);
            input_->MouseClick(j.value("x", 0.0), j.value("y", 0.0));
        } catch (...) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::string a = EvalStr(
            "document.activeElement ? "
            "(document.activeElement.id||document.activeElement.tagName) : 'none'");
        spdlog::info("[KLB] click {} -> active={}", id, a);
        return a == id;
    };
    auto ActiveId = [this]() {
        return EvalStr("document.activeElement ? "
                       "(document.activeElement.id||document.activeElement.tagName) : 'none'");
    };

    bool focused = false;
    // 两轮策略：第一轮 5 次点击都落空（Chrome 刚重启的竞态、控件遮罩未退等
    // 原因）→ 重新 Navigate 登录页再来一轮，自愈
    for (int round = 0; round < 2 && !focused; ++round) {
        if (round > 0) {
            spdlog::warn("[KLB] focus failed round 1, reloading login page");
            page_->Navigate(url, 30000);
            if (!WaitLoginForm()) {
                spdlog::error("[KLB] login form not found after reload");
                break;
            }
            EvalStr("document.getElementById('CifNo').value='" + cif + "';"
                    "document.getElementById('UserId').value='" + uid + "';");
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
        for (int attempt = 0; attempt < 5 && !focused; ++attempt) {
            // 点操作员输入框
            if (!ClickBox("UserId")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // 硬件键盘 Tab → 密码控件
            HardwareTab();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::string active = ActiveId();
            spdlog::info("[KLB] after Tab: active={}", active);
            if (active == "_safepass" || active == "safepass") { focused = true; break; }

            // 回退：直接点 _safepass
            ClickBox("_safepass");
            active = ActiveId();
            if (active == "_safepass" || active == "safepass") focused = true;
        }
    }
    if (!focused) {
        spdlog::error("[KLB] safepass focus failed, active={}", ActiveId());
        return "error: safepass not focused";
    }

    // --- 3. 硬件键盘输密码（合成键盘被 iProtect 拒绝）---
    std::string pwd = cfg_.password.empty() ? "jy852963" : cfg_.password;
    // 验证-重试：控件星号数必须等于密码长度，不符就清空重输
    std::string masked;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // 输前焦点复查：不在线上就重新点（防 Tab/切换瞬间焦点漂移吃字符）
        std::string a = ActiveId();
        if (a != "_safepass" && a != "safepass") {
            spdlog::warn("[KLB] focus drifted before typing ({}), re-click", a);
            ClickBox("_safepass");
        }
        if (!HardwareType(pwd, 80)) return "error: hardware keyboard typing failed";
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        masked = EvalStr("document.getElementById('_safepass').innerText");
        int stars = std::count(masked.begin(), masked.end(), '*');
        spdlog::info("[KLB] masked stars={} expect={}", stars, pwd.size());
        if (stars == static_cast<int>(pwd.size())) break;
        if (attempt == 2) {
            spdlog::error("[KLB] password length mismatch after retries: '{}'", masked);
            return "error: password not entered (stars=" + std::to_string(stars) + ")";
        }
    }
    spdlog::info("[KLB] password entered (masked)");

    // --- 4. 点登录 → doLogin() 自动取密文+MFM 提交 ---
    EvalBool("(function(){var b=document.getElementById('Button');"
             "if(b){b.click();return true;}return false;})()");

    // 等 iframepage 出现（登录成功）；期间若弹出 alert（密码错误提示）立即判失败，
    // 不干等 30s 超时（alert 挂起时 evaluate 每次都会拖满超时）
    {
        std::lock_guard<std::mutex> lk(dialogMtx_);
        lastDialogMsg_.clear();
    }
    auto LoginOk = [this]() -> bool {
        try {
            return EvalBool("!!document.getElementById('iframepage')", 2000);
        } catch (...) { return false; }
    };
    bool ok = false;
    bool dialogHit = false;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(30000);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(dialogMtx_);
            if (!lastDialogMsg_.empty()) { dialogHit = true; break; }
        }
        if (LoginOk()) { ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    // 提交没触发（偶发）且没有弹窗 → 再点一次 Button
    if (!ok && !dialogHit) {
        spdlog::warn("[KLB] login submit timeout, retry click Button");
        EvalBool("(function(){var b=document.getElementById('Button');"
                 "if(b){b.click();return true;}return false;})()");
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(15000);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(dialogMtx_);
                if (!lastDialogMsg_.empty()) { dialogHit = true; break; }
            }
            if (LoginOk()) { ok = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }

    if (!ok) {
        // 失败：先关掉可能弹着的 alert（不关后续 evaluate 全被挂起），再计数
        DismissJsDialog();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string dlg;
        {
            std::lock_guard<std::mutex> lk(dialogMtx_);
            dlg = lastDialogMsg_;
        }
        ++loginFailCount_;
        spdlog::error("[KLB] LOGIN FAILED attempt #{}/2 dialog='{}' url={}",
                      loginFailCount_, dlg, EvalStr("location.href", 3000));
        if (loginFailCount_ >= 2) {
            return "error: 登录连续失败2次，停止重试" +
                   std::string(dlg.empty() ? "" : ("（提示: " + dlg + "）")) +
                   "，请检查密码或账户状态";
        }
        return "error: 登录失败（第1次）" +
               std::string(dlg.empty() ? "" : ("，提示: " + dlg));
    }

    loginFailCount_ = 0;   // 登录成功，失败计数清零

    std::string user = EvalStr("var e=document.getElementById('UserName'); e?e.value:''");
    spdlog::info("[KLB] LOGIN_OK user={}", user);
    loggedIn_ = true;
    return "ok";
}

// ----------------------------------------------------------------------------
// 注册账户余额
// ----------------------------------------------------------------------------

std::vector<std::map<std::string, std::string>> KLBPlugin::GetCurBalImpl() {
    std::vector<std::map<std::string, std::string>> rows;
    if (!loggedIn_) {
        std::string r = LoginImpl();
        if (r != "ok" && r != "already logged in") return rows;
    }

    if (!GotoMenu("注册账户余额", "ActQryPre.do")) return rows;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // AcNoInfo radio value = Java bean toString
    std::string arrJson = EvalStr(
        "(function(){var d=" + IframeDoc() + ";"
        "var out=[];var rs=d.querySelectorAll('input[name=AcNoInfo]');"
        "for(var i=0;i<rs.length;i++) out.push(rs[i].value);"
        "return JSON.stringify(out);})()");
    if (arrJson.empty()) {
        spdlog::error("[KLB] AcNoInfo collect failed");
        return rows;
    }
    try {
        auto arr = nlohmann::json::parse(arrJson);
        for (auto& v : arr) {
            if (!v.is_string()) continue;
            auto bean = ParseBean(v.get<std::string>());
            if (bean.empty()) continue;
            std::map<std::string, std::string> row;
            row["AcNo"]      = bean.count("AcNo")      ? bean["AcNo"]      : "";
            row["AcName"]    = bean.count("AcName")    ? bean["AcName"]    : "";
            row["Currency"]  = bean.count("Currency")  ? bean["Currency"]  : "";
            row["Balance"]   = bean.count("Balance")   ? bean["Balance"]   : "";
            row["AvailBal"]  = bean.count("AvailBal")  ? bean["AvailBal"]  : "";
            row["LastDayBal"]= bean.count("LastDayBal")? bean["LastDayBal"]: "";
            row["AcState"]   = bean.count("AcState")   ? bean["AcState"]   : "";
            row["OpenDate"]  = bean.count("OpenDate")  ? bean["OpenDate"]  : "";
            row["DeptId"]    = bean.count("DeptId")    ? bean["DeptId"]    : "";
            row["BankAcType"]= bean.count("BankAcType")? bean["BankAcType"]: "";
            row["sub_acct"]  = bean.count("sub_acct")  ? bean["sub_acct"]  : "";
            row["AcSeq"]     = bean.count("AcSeq")     ? bean["AcSeq"]     : "";
            rows.push_back(std::move(row));
        }
    } catch (const std::exception& e) {
        spdlog::error("[KLB] AcNoInfo parse failed: {}", e.what());
        return rows;
    }

    // 落盘 JSON
    nlohmann::json out;
    out["queryTime"] = NowStamp();
    out["source"] = "https://cbank-simp.klb.cn/eweb/ActQryPre.do";
    out["accountCount"] = rows.size();
    out["accounts"] = nlohmann::json::array();
    for (auto& r : rows) out["accounts"].push_back(r);
    SaveTextFile("KLB_注册账户余额.json", out.dump(2));
    spdlog::info("[KLB] CurBal: {} accounts saved", rows.size());
    return rows;
}

// ----------------------------------------------------------------------------
// 交易明细查询（ActTrsQryPre.do）
// ----------------------------------------------------------------------------

std::string KLBPlugin::QueryTransJson(const std::string& begin,
                                       const std::string& end, bool& ok) {
    ok = false;
    if (!GotoMenu("交易明细查询", "ActTrsQryPre.do")) return "";

    // 等 select[name=AcNoAlias] 出现（注意：只有 name 没有 id！）
    if (!WaitBool("!(" + IframeDoc() + ".querySelector('select[name=AcNoAlias]')==null)",
                  20000)) {
        spdlog::error("[KLB] AcNoAlias select not ready");
        return "";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 选账户（优先配置账号前缀匹配，否则 CNY 账户）+ 日期 + 查询
    std::string acno = cfg_.account;
    std::string js = "(function(){var d=" + IframeDoc() + ";"
        "var sel=d.querySelector('select[name=AcNoAlias]');"
        "if(!sel) return 'no select';"
        "var want=null;";
    if (!acno.empty()) {
        js += "for(var i=0;i<sel.options.length;i++)"
              "if(sel.options[i].value.indexOf('" + acno + "')===0) want=sel.options[i].value;";
    }
    js += "if(!want){for(var i=0;i<sel.options.length;i++)"
          "if(sel.options[i].value.indexOf('/CNY/')>=0) want=sel.options[i].value;}"
          "if(!want) return 'no account option';"
          "sel.value=want;sel.dispatchEvent(new Event('change'));"
          "d.getElementById('BeginDate').value='" + begin + "';"
          "d.getElementById('EndDate').value='" + end + "';"
          "d.getElementById('queryButton').click();"
          "return 'ok '+want;})()";
    std::string sub = EvalStr(js);
    if (sub.rfind("ok", 0) != 0) {
        spdlog::error("[KLB] trans query submit failed: {}", sub);
        return "";
    }
    spdlog::info("[KLB] trans query submitted: {}", sub);

    // 等结果表
    if (!WaitBool("(" + IframeDoc() + ".querySelectorAll('table.j_tabV').length)>0",
                  30000)) {
        spdlog::error("[KLB] trans result table timeout");
       // return "";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 逐页采集（页码判断，尾页"下一页"是假链接）
    nlohmann::json allRecords = nlohmann::json::array();
    std::string pager, summary;
    std::string lastSig;
    int curPage = 0, totalPages = 1;
    for (int pageGuard = 0; pageGuard < 50; ++pageGuard) {
        std::string pageJson = EvalStr(
            "(function(){var d=" + IframeDoc() + ";"
            "var res={records:[],pager:'',summary:''};"
            "var ts=d.querySelectorAll('table.j_tabV');"
            "for(var ti=0;ti<ts.length;ti++){var rows=ts[ti].rows;"
            "for(var r=0;r<rows.length;r++){var cs=rows[r].cells;var cells=[];"
            "for(var k=0;k<cs.length;k++)cells.push(cs[k].innerText.trim());"
            "var line=cells.join('|');"
            "if(/^\\d{4}-\\d{2}-\\d{2}$/.test(cells[0])) res.records.push(cells);"
            "else if(line.indexOf('交易明细汇总')>=0) res.summary=line;"
            "else if(line.indexOf('页')>=0&&line.indexOf('记录')>=0) res.pager=line;}}"
            "return JSON.stringify(res);})()");
        try {
            auto pg = nlohmann::json::parse(pageJson);
            if (pg.contains("records"))
                for (auto& rec : pg["records"]) allRecords.push_back(rec);
            if (pg.value("pager", "") != "")   pager = pg["pager"];
            if (pg.value("summary", "") != "") summary = pg["summary"];
        } catch (const std::exception& e) {
            spdlog::error("[KLB] page parse failed: {}", e.what());
            return "";
        }
        std::smatch m;
        // 注意：pager 文本里是 \xa0（不换行空格），std::regex 的 \s 不认它
        if (std::regex_search(pager, m, std::regex("当前第(\\d+)页[^0-9]*共(\\d+)页"))) {
            curPage = std::stoi(m[1].str());
            totalPages = std::stoi(m[2].str());
        }
        spdlog::info("[KLB] trans page {}/{}, rows so far {}", curPage, totalPages,
                     allRecords.size());
        if (curPage >= totalPages) break;
        // 记录没变化（尾页假链接点了不动）则提前退出，防重复采集
        std::string firstSig;
        if (allRecords.is_array() && !allRecords.empty() &&
            allRecords[0].is_array() && !allRecords[0].empty() &&
            allRecords[0][0].is_string())
            firstSig = allRecords[0][0].get<std::string>() + "|" +
                       (allRecords[0].size() > 1 && allRecords[0][1].is_string()
                            ? allRecords[0][1].get<std::string>()
                            : "") + "|" +
                       std::to_string(allRecords.size());
        if (firstSig == lastSig) {
            spdlog::info("[KLB] trans page unchanged, stop");
            break;
        }
        lastSig = firstSig;
        if (!EvalBool("(function(){var d=" + IframeDoc() + ";"
                      "var as=d.querySelectorAll('table.j_tabV a');"
                      "for(var i=0;i<as.length;i++){"
                      "if((as[i].innerText||'').trim()==='下一页'){as[i].click();return true;}}"
                      "return false;})()"))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    }

    nlohmann::json out;
    out["queryTime"] = NowStamp();
    out["beginDate"] = begin;
    out["endDate"] = end;
    out["pager"] = pager;
    out["summary"] = summary;
    out["recordCount"] = allRecords.size();
    out["records"] = allRecords;
    ok = true;
    return out.dump(2);
}

std::vector<plugin::Transaction> KLBPlugin::GetHisTranImpl(
    const plugin::DateRange& range) {
    std::vector<plugin::Transaction> result;
    bool ok = false;
    std::string jsonText = QueryTransJson(range.begin, range.end, ok);
    if (!ok || jsonText.empty()) return result;

    // 落盘
    SaveTextFile("KLB_交易明细_" + range.begin + "_" + range.end + ".json", jsonText);

    try {
        auto out = nlohmann::json::parse(jsonText);
        static const std::regex numRe("[\\d.]+");
        for (auto& cells : out.value("records", nlohmann::json::array())) {
            if (!cells.is_array() || cells.size() < 8) continue;
            auto cell = [&](size_t i) -> std::string {
                return cells[i].is_string() ? cells[i].get<std::string>() : "";
            };
            std::string outAmt = cell(1), inAmt = cell(2);
            plugin::Transaction t;
            t.transDate     = cell(0);
            t.account       = cfg_.account;
            t.opAccount     = cell(4);
            t.opAccountName = cell(5);
            t.remark        = cell(6);
            t.sn            = cell(7);
            t.balanceAfter  = cell(3);
            t.currency      = "CNY";
            // 去掉 "元"/千分位
            auto clean = [](std::string s) {
                s.erase(std::remove_if(s.begin(), s.end(),
                        [](char c){ return !std::isdigit((unsigned char)c) && c!='.'; }),
                        s.end());
                return s;
            };
            if (!clean(outAmt).empty()) { t.amount = clean(outAmt); t.direction = "付"; }
            else                        { t.amount = clean(inAmt);  t.direction = "收"; }
            result.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        spdlog::error("[KLB] trans json parse failed: {}", e.what());
    }
    spdlog::info("[KLB] HisTran: {} rows", result.size());
    return result;
}

std::vector<plugin::Transaction> KLBPlugin::GetCurTranImpl() {
    // 查询版没有独立的"当日交易"页，用交易明细查当天
    return GetHisTranImpl({Today(), Today()});
}

// ----------------------------------------------------------------------------
// 回单下载（URMReceiptQryPre.do + URMPreviewReceipt.do 直出 PDF）
// ----------------------------------------------------------------------------

std::string KLBPlugin::XhrBase64(const std::string& url) {
    // 同步 XHR + x-user-defined 取二进制（Evaluate 不支持 awaitPromise）
    std::string urlEsc = util::ReplaceAll(url, "\\", "\\\\");
    urlEsc = util::ReplaceAll(urlEsc, "'", "\\'");
    std::string js =
        "(function(){var xhr=new XMLHttpRequest();"
        "try{xhr.open('GET','" + urlEsc + "',false);"
        "xhr.overrideMimeType('text/plain; charset=x-user-defined');"
        "xhr.send(null);}catch(e){return ''}"
        "if(xhr.status!=200) return '';"
        "var t=xhr.responseText;var bin='';"
        "for(var i=0;i<t.length;i++) bin+=String.fromCharCode(t.charCodeAt(i)&0xff);"
        "try{return btoa(bin);}catch(e){return ''}})()";
    return EvalStr(js, 60000);
}

std::string KLBPlugin::FetchReceiptPdf(const std::string& rowIdx) {
    // reqBatchNo 从当前回单查询页 hidden 域取
    std::string batch = EvalStr("var d=" + IframeDoc() + ";"
                                "var e=d.getElementById('reqBatchNo'); e?e.value:''");
    if (batch.empty()) return "";
    std::string url = "URMPreviewReceipt.do?reqBatchNo=" + batch +
                      "&trsFlag=1&checkNo=" + rowIdx + ",";
    std::string b64 = XhrBase64(url);
    if (b64.empty()) return "";
    std::string pdf = Base64Decode(b64);
    if (pdf.size() < 5 || pdf.compare(0, 5, "%PDF-") != 0) return "";
    return pdf;
}

std::string KLBPlugin::QueryReceiptList(const std::string& begin,
                                         const std::string& end, bool& ok) {
    ok = false;
    if (!GotoMenu("未下载/已下载回单查询", "URMReceiptQryPre.do")) return "";
    if (!WaitBool("!(" + IframeDoc() + ".getElementById('queryButton')==null)",
                  20000)) {
        spdlog::error("[KLB] receipt query form not ready");
        return "";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string acno = cfg_.account;
    std::string js = "(function(){var d=" + IframeDoc() + ";"
        "var sel=d.querySelector('select[name=AcNoAlias]')||d.getElementById('AcNoAlias');"
        "if(!sel) return 'no select';"
        "var want=null;";
    if (!acno.empty()) {
        js += "for(var i=0;i<sel.options.length;i++)"
              "if(sel.options[i].value.indexOf('" + acno + "')===0) want=sel.options[i].value;";
    }
    js += "if(!want){for(var i=0;i<sel.options.length;i++)"
          "if(sel.options[i].value.indexOf('/CNY/')>=0) want=sel.options[i].value;}"
          "if(!want) return 'no account option';"
          "sel.value=want;sel.dispatchEvent(new Event('change'));"
          "d.getElementById('BeginDate').value='" + begin + "';"
          "d.getElementById('EndDate').value='" + end + "';"
          "d.getElementById('queryButton').click();"
          "return 'ok';})()";
    std::string sub = EvalStr(js);
    if (sub != "ok") {
        spdlog::error("[KLB] receipt query submit failed: {}", sub);
        return "";
    }
    if (!WaitBool("(" + IframeDoc() + ".querySelectorAll('table.j_tabV').length)>0",
                  30000)) {
        spdlog::error("[KLB] receipt result timeout");
        return "";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 行结构: c0序号 c1交易日期 c2流水号 c3渠道 c4付款账号 c5金额 c6收款账号 c7户名 ... 末列 checkBill
    std::string rowsJson = EvalStr(
        "(function(){var d=" + IframeDoc() + ";"
        "var out=[];var cb=d.querySelectorAll('input[name=checkBill]');"
        "if(!cb.length) return JSON.stringify(out);"
        "var tbl=cb[0].closest('table');"
        "for(var r=0;r<tbl.rows.length;r++){var cs=tbl.rows[r].cells;"
        "if(cs.length<10) continue;"
        "var inp=cs[cs.length-1].querySelector('input[name=checkBill]');"
        "if(!inp) continue;"
        "var row={idx:inp.value};"
        "for(var k=0;k<cs.length;k++) row['c'+k]=cs[k].innerText.trim();"
        "out.push(row);}"
        "return JSON.stringify(out);})()");
    if (rowsJson.empty()) {
        spdlog::error("[KLB] receipt rows collect failed");
        return "";
    }
    ok = true;
    return rowsJson;
}

std::vector<std::string> KLBPlugin::DownBankSlipImpl(
    const plugin::DateRange& range) {
    std::vector<std::string> files;
    bool ok = false;
    std::string rowsJson = QueryReceiptList(range.begin, range.end, ok);
    if (!ok || rowsJson.empty()) return files;

    nlohmann::json manifest;
    manifest["account"] = cfg_.account;
    manifest["begin"] = range.begin;
    manifest["end"] = range.end;
    manifest["queryTime"] = NowStamp();
    manifest["receipts"] = nlohmann::json::array();

    fs::path outDir = fs::absolute("downloads") / "KLB回单";
    std::error_code ec;
    fs::create_directories(outDir, ec);

    try {
        auto rows = nlohmann::json::parse(rowsJson);
        for (auto& row : rows) {
            std::string idx    = row.value("idx", "");
            std::string date   = row.value("c1", "");
            std::string amtRaw = row.value("c5", "");
            std::string counter= row.value("c6", "");
            std::string payee  = row.value("c7", "");
            std::string amt;
            for (char c : amtRaw)
                if (std::isdigit((unsigned char)c) || c == '.') amt += c;

            std::string pdf = FetchReceiptPdf(idx);
            if (pdf.empty()) {
                spdlog::warn("[KLB] receipt idx={} fetch FAILED", idx);
                continue;
            }
            std::string label = payee.empty() ? counter : payee;
            label = util::ReplaceAll(label, "/", "_");
            if (label.size() > 40) label = label.substr(0, 40);
            std::string fn = "KLB回单_" + date + "_" + amt + "元_" + label + ".pdf";
            fs::path fp = outDir / fn;
            WriteBinaryFile(fp.string(), pdf);
            spdlog::info("[KLB] receipt saved: {} ({} bytes)", fp.string(), pdf.size());
            files.push_back(fp.string());
            manifest["receipts"].push_back({
                {"idx", idx}, {"date", date}, {"amount", amt},
                {"counterAcNo", counter}, {"payeeName", payee}, {"file", fn}
            });
        }
    } catch (const std::exception& e) {
        spdlog::error("[KLB] receipt loop failed: {}", e.what());
    }

    std::string mp = (outDir / ("回单清单_" + NowStamp() + ".json")).string();
    SaveTextFile(mp, manifest.dump(2));
    spdlog::info("[KLB] receipt manifest: {}", mp);
    return files;
}

// ----------------------------------------------------------------------------
// 不支持的功能
// ----------------------------------------------------------------------------

std::string KLBPlugin::PaymentImpl(const std::string&, const std::string&,
                                const std::string&, const std::string&,
                                const std::string&) {
    return "error: payment not supported on KLB query-only version";
}
std::string KLBPlugin::ApprovalImpl(const std::string&) {
    return "error: approval not supported on KLB query-only version";
}
std::string KLBPlugin::QueryImpl(const std::string&) {
    return "error: query not supported on KLB query-only version";
}

// ----------------------------------------------------------------------------
// 文件工具
// ----------------------------------------------------------------------------

bool KLBPlugin::WriteBinaryFile(const std::string& path,
                                 const std::string& content) {
    fs::path p = fs::path(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) return false;
    ofs.write(content.data(), (std::streamsize)content.size());
    return ofs.good();
}

bool KLBPlugin::SaveTextFile(const std::string& path,
                              const std::string& content) {
    return WriteBinaryFile(path, content);
}

std::string KLBPlugin::NowStamp() {
    char buf[32] = {0};
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string KLBPlugin::Today() {
    char buf[16] = {0};
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// base64 解码
std::string KLBPlugin::Base64Decode(const std::string& in) {
    static const std::string T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        size_t idx = T.find(static_cast<char>(c));
        if (idx == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(idx);
        valb += 6;
        if (valb >= 0) {
            out += static_cast<char>((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}

} // namespace klb
} // namespace newrpa
