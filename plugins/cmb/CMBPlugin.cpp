// ============================================================================
// CMB 银行插件实现 — 招商银行非证书用户（骨架 + 登录）
// 业务取数待登录后探测页面结构再补；当前返回明确的 "not implemented" 原因。
// 凭据只从 config 读取，不写日志、不进源码。
// ============================================================================

#include "CMBPlugin.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace newrpa {
namespace cmb {

extern "C" {
plugin::IBankPlugin* CreatePlugin()  { return new CMBPlugin(); }
void DestroyPlugin(plugin::IBankPlugin* p) { delete p; }
}

CMBPlugin::CMBPlugin() = default;
CMBPlugin::~CMBPlugin() { Close(); }

uint32_t CMBPlugin::Capabilities() const {
    return plugin::IBankPlugin::CAP_LOGIN;   // 取数能力待登录后补
}

void CMBPlugin::SetSession(std::shared_ptr<cdp::CDPSession> s,
                           std::shared_ptr<chrome::ChromeBrowser> b) {
    session_ = std::move(s);
    browser_ = std::move(b);
    if (session_) {
        page_ = std::make_unique<cdp::PageDomain>(session_.get());
        rt_   = std::make_unique<cdp::RuntimeDomain>(session_.get());
    }
}

void CMBPlugin::SetConfig(const plugin::BankConfig& cfg) { cfg_ = cfg; }
void CMBPlugin::Close() { loggedIn_.MarkLoggedOut(); }

std::string CMBPlugin::EvalStr(const std::string& js, int timeoutMs) {
    if (!rt_) return "";
    try { return rt_->EvalString(js, timeoutMs); }
    catch (...) { return ""; }
}

bool CMBPlugin::EvalBool(const std::string& js, int timeoutMs) {
    if (!rt_) return false;
    try { return rt_->EvalBool(js, timeoutMs); }
    catch (...) { return false; }
}

bool CMBPlugin::WaitForElement(const std::string& selector, int timeoutMs) {
    if (!rt_) return false;
    std::string js = "(function(){try{return !!document.querySelector(" +
                     nlohmann::json(selector).dump() + ")}catch(e){return false}})()";
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (EvalBool(js, 1000)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool CMBPlugin::SetInputValue(const std::string& selector,
                              const std::string& value) {
    std::string js =
        "(function(){var e=document.querySelector(" + nlohmann::json(selector).dump() +
        ");if(!e)return 'no-el';"
        "var s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
        "s.call(e," + nlohmann::json(value).dump() + ");"
        "e.dispatchEvent(new Event('input',{bubbles:true}));"
        "return ''+e.value.length})()";
    std::string r = EvalStr(js);
    return !r.empty() && r != "no-el";
}

std::string CMBPlugin::ReadExtraCode() {
    // SVG <text> 明文：取 120x42 的 data:image/svg+xml 图，拼 <text> 节点内容
    std::string js =
        "(function(){var im=[].slice.call(document.querySelectorAll('img')).find(function(e){"
        "return (e.src||'').indexOf('data:image/svg+xml')===0&&e.naturalWidth===120});"
        "if(!im)return '';"
        "var svg;try{svg=decodeURIComponent(escape(atob(im.src.split(',')[1])))}catch(e){return ''}"
        "var ms=svg.match(/<text[^>]*>([^<]*)<\\/text>/g)||[];"
        "return ms.map(function(x){return x.replace(/<[^>]*>/g,'')}).join('')})()";
    return EvalStr(js);
}

bool CMBPlugin::TypePassword(const std::string& password) {
    if (!rt_) return false;
    // 聚焦 CFCA 控件
    EvalStr("(function(){var e=document.querySelector('.cfca_passwaord_container');"
            "if(!e)return 'no';e.focus();e.click();return 'ok'})()");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const char* dotsJs =
        "(function(){var e=document.getElementById('password1');"
        "return e?e.innerText.length:0})()";

    auto sendChars = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            std::string ch(1, password[i]);
            nlohmann::json p = {{"type", "char"},
                                {"text", ch},
                                {"unmodifiedText", ch}};
            try { session_->Call("Input.dispatchKeyEvent", p, 5000); }
            catch (...) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        return true;
    };

    if (!sendChars(0, password.size())) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // 回读圆点数，控件偶发吞字时补齐（最多 5 轮）
    for (int round = 0; round < 5; ++round) {
        std::string d = EvalStr(dotsJs);
        size_t dots = 0;
        try { dots = (size_t)std::stoul(d); } catch (...) {}
        if (dots >= password.size()) break;
        if (!sendChars(dots, password.size())) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::string d = EvalStr(dotsJs);
    size_t dots = 0;
    try { dots = (size_t)std::stoul(d); } catch (...) {}
    spdlog::info("[CMB] 密码控件输入 {} 位", dots);
    return dots >= password.size();
}

std::string CMBPlugin::LoginImpl() {
    if (!session_ || !browser_) return "error: session not ready";
    if (loggedIn_.IsLoggedIn()) return "already logged in";
    if (cfg_.loginUrl.empty())
        return "error: CMB loginUrl not configured";

    spdlog::info("[CMB] 导航到 {}", cfg_.loginUrl);
    try { page_->Navigate(cfg_.loginUrl, 30000); }
    catch (const std::exception& e) {
        return std::string("error: navigate failed: ") + e.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    if (!WaitForElement("#loginName", 10000))
        return "error: login form not found";

    if (!SetInputValue("#loginName", cfg_.operatorId))
        return "error: set loginName failed";

    std::string code = ReadExtraCode();
    if (code.empty()) return "error: extra code not found";
    if (!SetInputValue("#extraCode", code))
        return "error: set extraCode failed";
    spdlog::info("[CMB] 附加码已自动解析并填入（{} 位）", code.size());

    if (!TypePassword(cfg_.password))
        return "error: password control input failed";

    // 点登录
    EvalStr("(function(){var b=[].slice.call(document.querySelectorAll('button')).find("
            "function(e){return (e.innerText||'').trim()==='登录'});"
            "if(b){b.click();return 'ok'}return 'no-btn'})()");

    // 等跳转或错误提示
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        std::string url = EvalStr("location.href", 3000);
        if (!url.empty() && url.find("/login") == std::string::npos) {
            loggedIn_.MarkLoggedIn();
            spdlog::info("[CMB] 登录成功: {}", url);
            return "ok";
        }
        std::string txt = EvalStr("(document.body.innerText||'').slice(-400)", 3000);
        if (txt.find("锁定") != std::string::npos)
            return "error: account locked - stop";
    }
    return "error: login timeout or rejected";
}

// ---- Result<T> 包装层 ----
arch::Result<arch::LoginOutcome> CMBPlugin::Login() {
    arch::LoginOutcome out;
    out.raw = LoginImpl();
    out.loggedIn = (out.raw == "ok" || out.raw == "already logged in");
    return arch::Result<arch::LoginOutcome>::Ok(std::move(out));
}

static arch::Result<std::vector<std::map<std::string, std::string>>>
NotImpl(const char* what) {
    return arch::Result<std::vector<std::map<std::string, std::string>>>::Fail(
        "cmb.not_implemented", std::string(what) + " 待登录后探测页面实现");
}

arch::Result<std::vector<std::map<std::string, std::string>>>
CMBPlugin::GetCurBal() { return NotImpl("余额"); }

arch::Result<std::vector<plugin::Transaction>>
CMBPlugin::GetCurTran() {
    return arch::Result<std::vector<plugin::Transaction>>::Fail(
        "cmb.not_implemented", "当日交易 待登录后探测页面实现");
}

arch::Result<std::vector<plugin::Transaction>>
CMBPlugin::GetHisTran(const plugin::DateRange&) {
    return arch::Result<std::vector<plugin::Transaction>>::Fail(
        "cmb.not_implemented", "历史交易 待登录后探测页面实现");
}

arch::Result<std::vector<std::string>>
CMBPlugin::DownBankSlip(const plugin::DateRange&) {
    return arch::Result<std::vector<std::string>>::Fail(
        "cmb.not_implemented", "电子回单 待登录后探测页面实现");
}

arch::Result<std::string>
CMBPlugin::Payment(const std::string&, const std::string&, const std::string&,
                   const std::string&, const std::string&) {
    return arch::Result<std::string>::Fail("cmb.not_implemented", "付款未实现");
}
arch::Result<std::string> CMBPlugin::Approval(const std::string&) {
    return arch::Result<std::string>::Fail("cmb.not_implemented", "审批未实现");
}
arch::Result<std::string> CMBPlugin::Query(const std::string&) {
    return arch::Result<std::string>::Fail("cmb.not_implemented", "查询未实现");
}

} // namespace cmb
} // namespace newrpa
