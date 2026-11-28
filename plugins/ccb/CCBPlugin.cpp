// ============================================================================
// CCB 银行插件实现 — 中国建设银行企业网银（DemoV3 演示版）
//
// 探测结论（2026-11-27，CDP 无凭据/公开 demo 凭据）：
//   * 登录页三个字段都是普通 input，无安全控件、无验证码、无软键盘弹出
//   * 登录按钮 #LOGIN_BOX_BTN 初始 disabled，三字段非空后由 jQuery 解禁
//   * demo 版点击登录不提交表单，按操作员代码直接跳转业务页
//   * 业务页为框架页，功能页在 iframe#win_<code> 内由 taskBar.createWin 打开
//       5111 -> FN00001.htm 活期账户
//       0513 -> FN01001.htm 电子回单查询
//   * 回单「下载全部」是空桩（downAllN01A0A 为 void(0)），demo 只能取列表
//
// 凭据只从 BankConfig 读取，源码/日志不出现明文。
// ============================================================================

#include "CCBPlugin.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <thread>

namespace newrpa {
namespace ccb {

namespace {
using json = nlohmann::json;

// 功能页 iframe 前缀
constexpr const char* kFramePrefix = "win_";

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// 去掉千分位/空格，"--" 归一为 0
std::string CleanAmtImpl(const std::string& raw) {
    std::string s = Trim(raw);
    if (s.empty() || s == "--" || s == "-") return "0";
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t' || (unsigned char)c == 0xA0) continue;
        out.push_back(c);
    }
    if (out.empty() || out == "-" || out == "--") return "0";
    return out;
}

// YYYYMMDD -> 原样；YYYY-MM-DD -> YYYYMMDD；其他返回空
std::string ToCompactDate(const std::string& d) {
    std::string digits;
    for (char c : d) if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
    if (digits.size() == 8) return digits;
    return {};
}

// "20150915 21:13:16" -> "2015-09-15 21:13:16"
std::string FmtDateTime(const std::string& raw) {
    std::string s = Trim(raw);
    if (s.size() >= 8) {
        bool digits8 = true;
        for (int i = 0; i < 8; ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) { digits8 = false; break; }
        if (digits8) {
            std::string out = s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2);
            if (s.size() > 8) out += s.substr(8);
            return out;
        }
    }
    return s;
}

double ToDouble(const std::string& s) {
    try { return std::stod(s); } catch (...) { return 0.0; }
}
} // namespace

extern "C" {
plugin::IBankPlugin* CreatePlugin()  { return new CCBPlugin(); }
void DestroyPlugin(plugin::IBankPlugin* p) { delete p; }
}

CCBPlugin::CCBPlugin() = default;
CCBPlugin::~CCBPlugin() { Close(); }

uint32_t CCBPlugin::Capabilities() const {
    return plugin::IBankPlugin::CAP_LOGIN |
           plugin::IBankPlugin::CAP_CUR_BALANCE |
           plugin::IBankPlugin::CAP_CUR_TRANS |
           plugin::IBankPlugin::CAP_YES_TRANS |
           plugin::IBankPlugin::CAP_SLIP;
}

void CCBPlugin::SetSession(std::shared_ptr<cdp::CDPSession> s,
                           std::shared_ptr<chrome::ChromeBrowser> b) {
    session_ = std::move(s);
    browser_ = std::move(b);
    if (session_) {
        page_ = std::make_unique<cdp::PageDomain>(session_.get());
        rt_   = std::make_unique<cdp::RuntimeDomain>(session_.get());
        // 页面上的 alert 会阻塞 Runtime.evaluate；统一自动接受，避免卡死
        dialogEventHandle_ = session_->OnEvent(
            "Page.javascriptDialogOpening", [this](const json& p) {
                std::string msg = p.value("message", "");
                spdlog::warn("[CCB] 自动关闭页面提示: {}", msg.substr(0, 120));
                try {
                    session_->CallNoWait("Page.handleJavaScriptDialog",
                                         json{{"accept", true}});
                } catch (...) {}
            });
    }
}

void CCBPlugin::SetConfig(const plugin::BankConfig& cfg) { cfg_ = cfg; }

void CCBPlugin::Close() {
    if (session_ && dialogEventHandle_ >= 0) {
        try { session_->OffEvent("Page.javascriptDialogOpening", dialogEventHandle_); }
        catch (...) {}
    }
    dialogEventHandle_ = -1;
    loggedIn_.MarkLoggedOut();
}

// ---------------------------------------------------------------------------
// 基础 helper
// ---------------------------------------------------------------------------
std::string CCBPlugin::EvalStr(const std::string& js, int timeoutMs) {
    if (!rt_) return {};
    try { return rt_->EvalString(js, timeoutMs); }
    catch (const std::exception& e) {
        spdlog::warn("[CCB] eval 失败: {}", e.what());
        return {};
    }
}

bool CCBPlugin::EvalBool(const std::string& js, int timeoutMs) {
    if (!rt_) return false;
    try { return rt_->EvalBool(js, timeoutMs); }
    catch (...) { return false; }
}

std::string CCBPlugin::NormalizeDate(const std::string& d) {
    return ToCompactDate(d);
}

std::string CCBPlugin::CleanAmt(const std::string& s) {
    return CleanAmtImpl(s);
}

// ---------------------------------------------------------------------------
// 框架页：taskBar.createWin
// ---------------------------------------------------------------------------
bool CCBPlugin::OpenFunc(const std::string& code, const std::string& title,
                         const std::string& url, int waitMs) {
    json params = json::array({code, title, url, "", "15"});
    std::string js = "(function(){try{window.taskBar.createWin(" +
                     params[0].dump() + "," + params[1].dump() + "," +
                     params[2].dump() + ",\"\",\"15\");return 'ok'}catch(e){return 'ERR:'+e.message}})()";
    std::string r = EvalStr(js, 8000);
    if (r.rfind("ERR:", 0) == 0) {
        spdlog::error("[CCB] createWin 失败: {}", r);
        return false;
    }
    spdlog::info("[CCB] 打开功能页 {}({})", title, url);
    return WaitFrame(code, waitMs);
}

bool CCBPlugin::WaitFrame(const std::string& code, int timeoutMs) {
    std::string frameId = std::string(kFramePrefix) + code;
    std::string js =
        "(function(){try{var f=document.getElementById(" + json(frameId).dump() +
        ");if(!f)return '0';var d=f.contentDocument;if(!d||!d.body)return '0';"
        "return (d.body.innerText||'').length>20?'1':'0'}catch(e){return '0'}})()";
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (EvalStr(js, 3000) == "1") {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    spdlog::error("[CCB] 功能页 iframe {} 未就绪", frameId);
    return false;
}

std::string CCBPlugin::EvalInFrame(const std::string& code, const std::string& js,
                                   int timeoutMs) {
    std::string frameId = std::string(kFramePrefix) + code;
    std::string wrapped =
        "(function(){var f=document.getElementById(" + json(frameId).dump() +
        ");if(!f)return '{\"_err\":\"no_frame\"}';var d=f.contentDocument;"
        "if(!d||!d.body)return '{\"_err\":\"no_doc\"}';" + js + "})()";
    return EvalStr(wrapped, timeoutMs);
}

// ---------------------------------------------------------------------------
// 登录
// ---------------------------------------------------------------------------
std::string CCBPlugin::LoginImpl() {
    if (!session_ || !browser_) return "error: session not ready";
    if (loggedIn_.IsLoggedIn()) return "already logged in";
    if (cfg_.loginUrl.empty()) return "error: CCB loginUrl not configured";
    if (cfg_.account.empty() || cfg_.operatorId.empty() || cfg_.password.empty())
        return "error: CCB account/operatorId/password not configured";

    spdlog::info("[CCB] 导航到登录页");
    try { page_->Navigate(cfg_.loginUrl, 30000); }
    catch (const std::exception& e) {
        return std::string("error: navigate failed: ") + e.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // 等表单就绪
    bool ready = false;
    for (int i = 0; i < 40; ++i) {
        if (EvalBool("(function(){return !!document.getElementById('LOG_PWD')})()", 3000)) {
            ready = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!ready) return "error: login form not found";

    // 三字段填值（原生 setter + input/change，触发 jQuery 解禁按钮）
    auto fill = [this](const char* sel, const std::string& val) -> bool {
        std::string js =
            "(function(){var e=document.querySelector(" + json(sel).dump() +
            ");if(!e)return 'no-el';"
            "var s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
            "s.call(e," + json(val).dump() + ");"
            "e.dispatchEvent(new Event('input',{bubbles:true}));"
            "e.dispatchEvent(new Event('change',{bubbles:true}));"
            "return 'ok'})()";
        return EvalStr(js, 5000) == "ok";
    };

    if (!fill("#CUSTOMERID", cfg_.account))       return "error: fill CUSTOMERID failed";
    if (!fill("#USERID", cfg_.operatorId))        return "error: fill USERID failed";
    if (!fill("#LOG_PWD", cfg_.password))         return "error: fill LOG_PWD failed";
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // 按钮是否解禁
    std::string dis = EvalStr(
        "(function(){var b=document.getElementById('LOGIN_BOX_BTN');"
        "return b?String(b.disabled):'no-btn'})()");
    if (dis == "true") return "error: login button still disabled";
    spdlog::info("[CCB] 三字段已填，按钮已解禁");

    // 点击登录
    std::string clicked = EvalStr(
        "(function(){var b=document.getElementById('LOGIN_BOX_BTN');"
        "if(!b)return 'no-btn';b.click();return 'ok'})()");
    if (clicked != "ok") return "error: click login failed";

    // 等跳转：离开登录页即成功
    for (int i = 0; i < 25; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::string url = EvalStr("location.href", 3000);
        if (url.empty()) continue;
        bool leftLogin = url.find("demo_login") == std::string::npos &&
                         url.find("webpage_demo") != std::string::npos;
        if (leftLogin) {
            // 等框架与 taskBar 就绪
            bool taskBarOk = false;
            for (int k = 0; k < 20; ++k) {
                if (EvalBool("(function(){return !!(window.taskBar&&typeof window.taskBar.createWin==='function')})()", 3000)) {
                    taskBarOk = true; break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            loggedIn_.MarkLoggedIn(url);
            spdlog::info("[CCB] 登录成功: {} (taskBar={})", url,
                         taskBarOk ? "ok" : "missing");
            if (!taskBarOk) return "error: taskBar not ready after login";
            return "ok";
        }
        std::string body = EvalStr("(document.body.innerText||'').slice(0,300)", 3000);
        if (body.find("密码错误") != std::string::npos ||
            body.find("锁定") != std::string::npos) {
            return "error: login rejected: " + body.substr(0, 120);
        }
    }
    return "error: login timeout";
}

arch::Result<arch::LoginOutcome> CCBPlugin::Login() {
    arch::LoginOutcome out;
    out.raw = LoginImpl();
    out.loggedIn = (out.raw == "ok" || out.raw == "already logged in");
    if (!out.loggedIn)
        return arch::Result<arch::LoginOutcome>::Fail("bank.login_failed", out.raw);
    return arch::Result<arch::LoginOutcome>::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// 账户准备：打开活期账户页 + 勾选账号
// ---------------------------------------------------------------------------
bool CCBPlugin::PrepareAccount(const std::string& code) {
    if (!OpenFunc(code, "活期账户", "FN00001.htm")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // 勾选第一个 ACC_NO 复选框（未勾选时点「实时余额」会弹"请选择账户"）
    std::string r = EvalInFrame(code,
        "var c=d.querySelector('input[name=ACC_NO]');"
        "if(!c)return 'no-cb';"
        "if(!c.checked)c.click();"
        "return 'ok:'+c.checked;");
    if (r.rfind("ok:", 0) != 0) {
        spdlog::error("[CCB] 勾选账户失败: {}", r);
        return false;
    }
    spdlog::info("[CCB] 已勾选账户 ({})", r);
    return true;
}

// ---------------------------------------------------------------------------
// 余额
// ---------------------------------------------------------------------------
arch::Result<std::vector<std::map<std::string, std::string>>>
CCBPlugin::QueryBalance() {
    const std::string code = "5111";
    if (!PrepareAccount(code))
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
            "bank.query_failed", "打开活期账户页或勾选账户失败");

    // 触发实时余额查询
    std::string called = EvalInFrame(code,
        "var w=f.contentWindow;if(typeof w.MultiBalanceQuery!=='function')return 'no-fn';"
        "w.MultiBalanceQuery();return 'ok';");
    if (called != "ok")
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
            "bank.query_failed", "实时余额查询函数不可用: " + called);
    spdlog::info("[CCB] 已触发实时余额查询");

    // 等表头出现「账面余额」
    bool ok = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if (EvalInFrame(code, "return (d.body.innerText||'').indexOf('账面余额')>=0?'1':'0'") == "1") {
            ok = true; break;
        }
    }
    if (!ok)
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
            "bank.query_failed", "余额表未出现");

    // 解析：按表头名定位列
    std::string raw = EvalInFrame(code, R"JS(
        var tb=null;
        [].slice.call(d.querySelectorAll('table')).forEach(function(t){
            var s=t.innerText||'';
            if(s.indexOf('账面余额')>=0&&s.indexOf('可用余额')>=0)tb=t;
        });
        if(!tb)return '[]';
        var rows=[].slice.call(tb.querySelectorAll('tr'));
        if(!rows.length)return '[]';
        var head=[].slice.call(rows[0].children).map(function(c){
            return (c.innerText||'').replace(/\s+/g,'');
        });
        var idx=function(n){return head.indexOf(n)};
        var iAcc=idx('账号'),iName=idx('账户名称'),iCur=idx('币种'),
            iBal=idx('账面余额'),iAvail=idx('可用余额'),
            iFroz=idx('冻结金额'),iStat=idx('账户状态'),iBank=idx('开户机构');
        if(iAcc<0||iBal<0)return '[]';
        var out=[];
        rows.slice(1).forEach(function(tr){
            var cs=[].slice.call(tr.children);
            if(cs.length<head.length-2)return;
            var g=function(i){return i>=0&&cs[i]?(cs[i].innerText||'').replace(/\s+/g,' ').trim():''};
            if(!g(iAcc)||g(iAcc).indexOf('小计')>=0)return;
            out.push({account:g(iAcc),name:g(iName),currency:g(iCur),
                      balance:g(iBal),available:g(iAvail),frozen:g(iFroz),
                      status:g(iStat),bank:g(iBank)});
        });
        return JSON.stringify(out);
    )JS");

    std::vector<std::map<std::string, std::string>> result;
    try {
        auto arr = json::parse(raw);
        for (auto& it : arr) {
            std::map<std::string, std::string> m;
            m["account"]   = CleanAmt(it.value("account", ""));
            m["name"]      = it.value("name", "");
            m["currency"]  = it.value("currency", "");
            m["balance"]   = CleanAmt(it.value("balance", ""));
            m["available"] = CleanAmt(it.value("available", ""));
            m["frozen"]    = CleanAmt(it.value("frozen", ""));
            m["status"]    = it.value("status", "");
            m["bank"]      = it.value("bank", "");
            result.push_back(std::move(m));
        }
    } catch (const std::exception& e) {
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
            "bank.query_failed", std::string("余额解析失败: ") + e.what());
    }

    if (result.empty())
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
            "bank.query_failed", "余额表为空");
    spdlog::info("[CCB] 余额 {} 条，首条余额={}", result.size(), result[0]["balance"]);
    return arch::Result<std::vector<std::map<std::string, std::string>>>::Ok(
        std::move(result));
}

arch::Result<std::vector<std::map<std::string, std::string>>>
CCBPlugin::GetCurBal() {
    if (!loggedIn_.IsLoggedIn())
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
            "bank.login_failed", "未登录");
    return QueryBalance();
}

// ---------------------------------------------------------------------------
// 明细
// ---------------------------------------------------------------------------
arch::Result<std::vector<plugin::Transaction>>
CCBPlugin::QueryDetail(const std::string& stDt, const std::string& edDt) {
    using RetT = arch::Result<std::vector<plugin::Transaction>>;
    const std::string code = "5111";

    if (!PrepareAccount(code))
        return RetT::Fail("bank.query_failed", "打开活期账户页或勾选账户失败");

    // 切到交易明细表单
    std::string r = EvalInFrame(code,
        "var w=f.contentWindow;if(typeof w.detailQuery!=='function')return 'no-fn';"
        "w.detailQuery();return 'ok';");
    if (r != "ok") return RetT::Fail("bank.query_failed", "明细表单打开失败: " + r);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // 日期赋值（原生 setter，触发 change）
    std::string setDt = EvalInFrame(code,
        "var s=d.getElementById('StDt'),e=d.getElementById('EdDt');"
        "if(!s||!e)return 'no-el';"
        "var set=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
        "set.call(s,'" + stDt + "');"
        "s.dispatchEvent(new Event('change',{bubbles:true}));"
        "set.call(e,'" + edDt + "');"
        "e.dispatchEvent(new Event('change',{bubbles:true}));"
        "return s.value+'-'+e.value;");
    spdlog::info("[CCB] 明细日期 {}", setDt);
    if (setDt.rfind("no-el", 0) == 0)
        return RetT::Fail("bank.query_failed", "明细日期字段未找到");

    // 提交
    std::string sub = EvalInFrame(code,
        "var w=f.contentWindow;if(typeof w.submitToNext!=='function')return 'no-fn';"
        "w.submitToNext();return 'ok';");
    if (sub != "ok") return RetT::Fail("bank.query_failed", "明细提交失败: " + sub);
    spdlog::info("[CCB] 已提交明细查询，等待结果页");

    // 等结果页（FN00012.htm，iframe id 不变）
    bool ok = false;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if (EvalInFrame(code, "return (d.body.innerText||'').indexOf('交易时间')>=0?'1':'0'") == "1") {
            ok = true; break;
        }
    }
    if (!ok) return RetT::Fail("bank.query_failed", "明细结果页未出现");

    // 解析（列固定：0选择 1交易时间 2借方 3贷方 4余额 5对方户名 6对方账号 7对方机构 8记账日期 9摘要 10备注 11流水号）
    std::string raw = EvalInFrame(code, R"JS(
        var tb=null;
        [].slice.call(d.querySelectorAll('table')).forEach(function(t){
            var s=t.innerText||'';
            if(s.indexOf('交易时间')>=0&&s.indexOf('对方户名')>=0)tb=t;
        });
        if(!tb)return '[]';
        var acct='';
        var mm=(d.body.innerText||'').match(/账\s*号：\s*([0-9\*]+)/);
        if(mm)acct=mm[1];
        var out=[];
        [].slice.call(tb.querySelectorAll('tr')).forEach(function(tr){
            var cs=[].slice.call(tr.children);
            if(cs.length<12)return;
            var g=function(i){return cs[i]?(cs[i].innerText||'').replace(/\s+/g,' ').trim():''};
            var tm=g(1);
            if(!/^\d{8}\s+\d{2}:\d{2}/.test(tm))return;
            out.push({account:acct,time:tm,debit:g(2),credit:g(3),balance:g(4),
                      opName:g(5),opAccount:g(6),opBank:g(7),bookDate:g(8),
                      summary:g(9),remark:g(10),sn:g(11)});
        });
        return JSON.stringify(out);
    )JS");

    std::vector<plugin::Transaction> result;
    try {
        auto arr = json::parse(raw);
        for (auto& it : arr) {
            plugin::Transaction t;
            t.account        = CleanAmt(it.value("account", ""));
            t.transDate      = FmtDateTime(it.value("time", ""));
            t.amount         = CleanAmt(it.value("debit", "")) != "0"
                                   ? CleanAmt(it.value("debit", ""))
                                   : CleanAmt(it.value("credit", ""));
            t.direction      = ToDouble(CleanAmt(it.value("debit", ""))) > 0 ? "付" : "收";
            t.balanceAfter   = CleanAmt(it.value("balance", ""));
            t.opAccountName  = it.value("opName", "");
            t.opAccount      = CleanAmt(it.value("opAccount", ""));
            t.currency       = "CNY";
            t.remark         = it.value("summary", "");
            std::string extra = it.value("remark", "");
            if (!extra.empty()) t.remark += " " + extra;
            t.sn             = it.value("sn", "");
            result.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        return RetT::Fail("bank.query_failed", std::string("明细解析失败: ") + e.what());
    }

    spdlog::info("[CCB] 明细 {} 条", result.size());
    return RetT::Ok(std::move(result));
}

arch::Result<std::vector<plugin::Transaction>> CCBPlugin::GetCurTran() {
    if (!loggedIn_.IsLoggedIn())
        return arch::Result<std::vector<plugin::Transaction>>::Fail(
            "bank.login_failed", "未登录");
    // 当日：起止同一天
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return QueryDetail(buf, buf);
}

arch::Result<std::vector<plugin::Transaction>>
CCBPlugin::GetHisTran(const plugin::DateRange& range) {
    if (!loggedIn_.IsLoggedIn())
        return arch::Result<std::vector<plugin::Transaction>>::Fail(
            "bank.login_failed", "未登录");
    std::string st = NormalizeDate(range.begin);
    std::string ed = NormalizeDate(range.end);
    if (st.empty() || ed.empty())
        return arch::Result<std::vector<plugin::Transaction>>::Fail(
            "bank.range_missing", "起止日期格式应为 YYYY-MM-DD 或 YYYYMMDD");
    return QueryDetail(st, ed);
}

// ---------------------------------------------------------------------------
// 电子回单（demo 只能取列表，无法下载文件）
// ---------------------------------------------------------------------------
arch::Result<std::vector<std::string>>
CCBPlugin::DownBankSlip(const plugin::DateRange& range) {
    using RetT = arch::Result<std::vector<std::string>>;
    if (!loggedIn_.IsLoggedIn()) return RetT::Fail("bank.login_failed", "未登录");

    std::string st = NormalizeDate(range.begin);
    std::string ed = NormalizeDate(range.end);
    if (st.empty() || ed.empty())
        return RetT::Fail("bank.range_missing", "起止日期格式应为 YYYY-MM-DD 或 YYYYMMDD");

    const std::string code = "0513";
    if (!OpenFunc(code, "电子回单查询", "FN01001.htm"))
        return RetT::Fail("bank.slip_failed", "打开电子回单页失败");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // 设日期
    std::string setDt = EvalInFrame(code,
        "var s=d.getElementById('BEGIN_DATE'),e=d.getElementById('END_DATE');"
        "if(!s||!e)return 'no-el';"
        "var set=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
        "set.call(s,'" + st + "');s.dispatchEvent(new Event('change',{bubbles:true}));"
        "set.call(e,'" + ed + "');e.dispatchEvent(new Event('change',{bubbles:true}));"
        "return s.value+'-'+e.value;");
    spdlog::info("[CCB] 回单日期 {}", setDt);
    if (setDt.rfind("no-el", 0) == 0)
        return RetT::Fail("bank.slip_failed", "回单日期字段未找到");

    // 查询
    std::string q = EvalInFrame(code,
        "var w=f.contentWindow;if(typeof w.doRefresh!=='function')return 'no-fn';"
        "w.doRefresh();return 'ok';");
    if (q != "ok") return RetT::Fail("bank.slip_failed", "回单查询失败: " + q);

    // 等结果
    bool ok = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        if (EvalInFrame(code, "return (d.body.innerText||'').indexOf('回单编号')>=0?'1':'0'") == "1") {
            ok = true; break;
        }
    }
    if (!ok) return RetT::Fail("bank.slip_failed", "回单结果表未出现");

    std::string raw = EvalInFrame(code, R"JS(
        var tb=null;
        [].slice.call(d.querySelectorAll('table')).forEach(function(t){
            if((t.innerText||'').indexOf('回单编号')>=0)tb=t;
        });
        if(!tb)return '[]';
        var out=[];
        [].slice.call(tb.querySelectorAll('tr')).forEach(function(tr){
            var cs=[].slice.call(tr.children);
            if(cs.length<11)return;
            var g=function(i){return cs[i]?(cs[i].innerText||'').replace(/\s+/g,' ').trim():''};
            var d8=g(2);
            if(!/^\d{8}/.test(d8))return;
            out.push({no:g(1),date:d8,sn:g(3),currency:g(4),debit:g(5),
                      credit:g(6),opAccount:g(7),opName:g(8),
                      voucherType:g(9),voucherNo:g(10)});
        });
        return JSON.stringify(out);
    )JS");

    std::vector<std::string> result;
    try {
        auto arr = json::parse(raw);
        for (auto& it : arr) {
            std::string line = it.value("no", "") + "|" +
                               FmtDateTime(it.value("date", "")) + "|" +
                               CleanAmt(it.value("debit", "")) + "|" +
                               CleanAmt(it.value("credit", "")) + "|" +
                               CleanAmt(it.value("opAccount", "")) + "|" +
                               it.value("opName", "") + "|" +
                               it.value("voucherType", "") + "|" +
                               it.value("voucherNo", "");
            result.push_back(line);
        }
    } catch (const std::exception& e) {
        return RetT::Fail("bank.slip_failed", std::string("回单解析失败: ") + e.what());
    }

    spdlog::warn("[CCB] 回单 {} 条。demo 版「下载全部」为空桩（downAllN01A0A 为 void(0)），"
                 "仅返回回单清单，不产生文件", result.size());
    return RetT::Ok(std::move(result));
}

// ---------------------------------------------------------------------------
// 未实现的业务
// ---------------------------------------------------------------------------
arch::Result<std::string>
CCBPlugin::Payment(const std::string&, const std::string&, const std::string&,
                   const std::string&, const std::string&) {
    return arch::Result<std::string>::Fail("ccb.not_implemented", "付款未实现");
}
arch::Result<std::string> CCBPlugin::Approval(const std::string&) {
    return arch::Result<std::string>::Fail("ccb.not_implemented", "审批未实现");
}
arch::Result<std::string> CCBPlugin::Query(const std::string&) {
    return arch::Result<std::string>::Fail("ccb.not_implemented", "查询未实现");
}

} // namespace ccb
} // namespace newrpa
