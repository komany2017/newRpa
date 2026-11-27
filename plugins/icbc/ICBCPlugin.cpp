// ============================================================================
// ICBC 银行插件实现 — 原型级别的 CDP 操作
//
// 设计思路：
//   1. 所有复杂操作优先走 Runtime.evaluate（在页面 JS 上下文里跑代码），
//      因为网银 DOM 变化频繁，JS 比 CDP DOM API 灵活。
//   2. CDP Domain 做底层原语：导航、截图、网络拦截。
//   3. iframe 场景单独处理（工行有些菜单在 iframe 里）。
// ============================================================================

#include "ICBCPlugin.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/io/ExcelWriter.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>

using namespace newrpa;

namespace newrpa {
namespace icbc {

// ----------------------------------------------------------------------------
// DLL 导出
// ----------------------------------------------------------------------------
extern "C" {
plugin::IBankPlugin* CreatePlugin()  { return new ICBCPlugin(); }
void DestroyPlugin(plugin::IBankPlugin* p) { delete p; }
}

// ----------------------------------------------------------------------------
// 构造/析构
// ----------------------------------------------------------------------------

ICBCPlugin::ICBCPlugin()  = default;
ICBCPlugin::~ICBCPlugin() { Close(); }

uint32_t ICBCPlugin::Capabilities() const {
    return plugin::IBankPlugin::CAP_LOGIN
         | plugin::IBankPlugin::CAP_CUR_BALANCE
         | plugin::IBankPlugin::CAP_CUR_TRANS
         | plugin::IBankPlugin::CAP_YES_TRANS
         | plugin::IBankPlugin::CAP_SLIP
         | plugin::IBankPlugin::CAP_PAYMENT
         | plugin::IBankPlugin::CAP_APPROVAL
         | plugin::IBankPlugin::CAP_QUERY;
}

void ICBCPlugin::SetSession(std::shared_ptr<cdp::CDPSession> s,
                             std::shared_ptr<chrome::ChromeBrowser> b) {
    session_ = std::move(s);
    browser_ = std::move(b);
    if (session_) {
        page_   = std::make_unique<cdp::PageDomain>(session_.get());
        rt_     = std::make_unique<cdp::RuntimeDomain>(session_.get());
        dom_    = std::make_unique<cdp::DomDomain>(session_.get());
        input_  = std::make_unique<cdp::InputDomain>(session_.get());
    }
}

void ICBCPlugin::SetConfig(const plugin::BankConfig& cfg) {
    cfg_ = cfg;
}

void ICBCPlugin::Close() {
    loggedIn_ = false;
    // session_ 是 shared_ptr，由外部 ChromeBrowser 持有，不用自己关
}

// ----------------------------------------------------------------------------
// 便捷 helper
// ----------------------------------------------------------------------------

std::string ICBCPlugin::EvalStr(const std::string& js) {
    if (!rt_) return "";
    return rt_->EvalString(js);
}

bool ICBCPlugin::EvalBool(const std::string& js) {
    if (!rt_) return false;
    return rt_->EvalBool(js);
}

bool ICBCPlugin::WaitForElement(const std::string& selector,
                                 int timeoutMs) {
    if (!rt_) return false;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string js =
            "(function(){ var s = document.querySelector('" + selector + "');"
            " return !!s && s.offsetWidth > 0 && s.offsetHeight > 0; })()";
        if (EvalBool(js)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool ICBCPlugin::WaitUrlContains(const std::string& sub, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string url = EvalStr("location.href");
        if (url.find(sub) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool ICBCPlugin::FillInput(const std::string& selector,
                            const std::string& value) {
    if (!rt_) return false;
    // 原生 set value 不会触发 React/Vue 的 change 事件，所以用
    // 一个 dispatchEvent 方案：先 set，再 dispatch input/change
    std::string escaped = util::ReplaceAll(value, "'", "\\'");
    std::string js = R"(
        (function() {
            var el = document.querySelector(')" + selector + R"(');
            if (!el) return false;
            var proto = Object.getPrototypeOf(el);
            var setter = null;
            // 尝试原生 value setter
            if (proto) {
                var desc = Object.getOwnPropertyDescriptor(proto, 'value');
                if (desc && desc.set) setter = desc.set;
            }
            if (setter) { setter.call(el, ')" + escaped + R"('); }
            else { el.value = ')" + escaped + R"('; }
            // 触发事件
            ['input', 'change'].forEach(function(ev) {
                el.dispatchEvent(new Event(ev, { bubbles: true }));
            });
            el.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' }));
            return true;
        })()
    )";
    return EvalBool(js);
}

bool ICBCPlugin::ClickElementJs(const std::string& selector) {
    if (!rt_) return false;
    std::string js =
        "(function(){ var s = document.querySelector('" + selector + "');"
        " if (!s) return false; s.click(); return true; })()";
    return EvalBool(js);
}

bool ICBCPlugin::ClickElement(const std::string& selector) {
    // 先用 CDP DOM + Input 精准点击，失败了再退到 JS click
    if (!session_) return false;
    auto ids = dom_->QuerySelector(selector);
    if (!ids.empty()) {
        auto box = dom_->GetBoxModel(ids[0]);
        if (!box.empty()) {
            auto& content = box["content"]; // [x,y,x,y,x,y,x,y] 左上、右上、右下、左下
            double x = (content[0].get<double>() + content[2].get<double>()) / 2;
            double y = (content[1].get<double>() + content[5].get<double>()) / 2;
            input_->MouseClick(x, y);
            return true;
        }
    }
    return ClickElementJs(selector);
}

std::string ICBCPlugin::HandleLoginPrompts() {
    // UKey / 证书选择框 — Chrome 可能弹原生对话框
    // 先等 2 秒看有没有新页面
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 如果 URL 包含 main.html / index_main 之类，说明登录成功
    if (WaitUrlContains("main", 5000) || WaitUrlContains("index_", 5000)) {
        loggedIn_ = true;
        return "ok";
    }

    // 如果出现错误提示，读出来
    std::string err = EvalStr(
        "(() => { var e = document.querySelector('.error, .tip, .msgError, "
        "[id*=Error], [id*=err], [class*=error]');"
        " return e ? e.innerText.trim() : ''; })()");
    if (!err.empty()) return "error: " + err;

    return "unknown";
}

// ----------------------------------------------------------------------------
// Login — 核心登录流程
// ----------------------------------------------------------------------------

std::string ICBCPlugin::LoginImpl() {
    if (!session_ || !browser_) return "error: session not ready";
    if (loggedIn_) return "already logged in";

    spdlog::info("[ICBC] Navigating to {}", cfg_.loginUrl);

    // 1. 导航到登录页
    page_->Navigate(cfg_.loginUrl, 15000);

    // 2. 等登录表单出现
    // 工行登录页常见元素 id: userName / loginName / accountNo
    std::vector<std::string> userNameSelectors = {
        "#userName", "#loginName", "#accountNo",
        "input[name='userName']", "input[name='loginName']",
        "input[type='text']"
    };
    std::vector<std::string> pwdSelectors = {
        "#password", "#loginPwd",
        "input[name='password']", "input[name='pwd']",
        "input[type='password']"
    };
    std::vector<std::string> btnSelectors = {
        "#loginBtn", "#LoginBtn", "#btnLogin",
        "input[type='submit']", "button[type='submit']",
        "input[value='登录']", "a.login", "#Submit"
    };

    std::string userSel, pwdSel, btnSel;
    for (auto& s : userNameSelectors)
        if (WaitForElement(s, 3000)) { userSel = s; break; }
    for (auto& s : pwdSelectors)
        if (WaitForElement(s, 1000)) { pwdSel = s; break; }
    for (auto& s : btnSelectors)
        if (WaitForElement(s, 1000)) { btnSel = s; break; }

    if (userSel.empty() || pwdSel.empty()) {
        spdlog::error("[ICBC] Login form not found");
        spdlog::info("[ICBC] Current URL: {}", EvalStr("location.href"));
        // 截图方便调试
        auto b64 = page_->CaptureScreenshotBase64();
        spdlog::info("[ICBC] Screenshot length: {}", b64.size());
        return "error: login form not found";
    }

    spdlog::info("[ICBC] Found login form: user={}, pwd={}, btn={}",
                 userSel, pwdSel, btnSel);

    // 3. 填账号
    if (!FillInput(userSel, cfg_.account))
        return "error: fill account failed";

    // 4. 填密码 —— 工行可能有软键盘，直接设 value 依然有效（后端校验的是 input.value）
    if (!FillInput(pwdSel, cfg_.password))
        return "error: fill password failed";

    // 5. 点登录
    if (!ClickElement(btnSel)) {
        spdlog::warn("[ICBC] Click login button failed, trying Enter key");
        input_->KeyPress("Enter", "Enter", "13");
    }

    // 6. 处理后续（UKey / 验证码 / 跳转）
    return HandleLoginPrompts();
}

// ----------------------------------------------------------------------------
// GetCurBal — 当日余额查询
// ----------------------------------------------------------------------------

std::vector<std::map<std::string, std::string>> ICBCPlugin::GetCurBalImpl() {
    if (!loggedIn_) { LoginImpl(); }
    if (!loggedIn_) return {};

    // 工行余额查询页面 URL 模式（根据旧项目 IBANK 里的 curbalUrl）
    // 实际 URL 要从页面菜单里找，或用菜单导航
    // 简单版：直接在页面里执行 JS 找账户余额元素
    std::vector<std::map<std::string, std::string>> result;

    // 工行主页面账户列表通常在 table / li 里，带 accountNo / balance 文本
    // 尝试多个常见选择器
    std::string js = R"(
        (function() {
            var rows = document.querySelectorAll(
                'table tr, .accRow, tr.accList, li.accList, ' +
                '[class*="balance"], [class*="Balance"]'
            );
            var data = [];
            for (var r of rows) {
                var text = r.innerText.trim();
                if (text && text.length < 200) data.push(text);
            }
            return data.join('\n---\n');
        })()
    )";
    std::string raw = EvalStr(js);
    if (!raw.empty()) {
        auto parts = util::Split(raw, '\n');
        std::map<std::string, std::string> row;
        row["raw"] = raw;
        row["account"] = cfg_.account;
        row["time"]    = util::FormatTime(std::time(nullptr));
        result.push_back(std::move(row));
    }

    if (result.empty()) {
        // 兜底：从整页文本里搜余额模式（如 "余额 1,234.56"）
        std::string allText = EvalStr("document.body.innerText");
        auto row = ParseCurBal();
        if (!row.empty()) {
            result = row;
        }
    }

    // 写 CSV 备份（和旧项目 ExcelOperation 保持一致）
    io::WriteCurBalCsv(
        "curbal_" + util::TodayStr() + ".csv", result);
    return result;
}

std::vector<std::map<std::string, std::string>> ICBCPlugin::ParseCurBal() {
    // 余额可能在多个 iframe 的 DOM 里
    std::vector<std::map<std::string, std::string>> result;
    std::map<std::string, std::string> row;

    // 用正则匹配 "¥ 1,234,567.89" 或 "CNY 1234.56" 模式
    std::string all = EvalStr("document.body.innerText");
    // 先用简单粗暴的方式
    size_t pos = all.find("¥");
    if (pos != std::string::npos) {
        row["balance"] = all.substr(pos, 30);
    } else {
        // 找 "余额" / "可用余额"
        for (auto& kw : {"余额", "可用余额", "可用"}) {
            size_t i = all.find(kw);
            if (i != std::string::npos) {
                row["balance"] = all.substr(i, 50);
                break;
            }
        }
    }
    row["account"] = cfg_.account;
    row["time"]    = util::FormatTime(std::time(nullptr));
    if (!row.empty()) result.push_back(std::move(row));
    return result;
}

// ----------------------------------------------------------------------------
// GetCurTran / GetHisTran — 流水查询
// ----------------------------------------------------------------------------

std::vector<plugin::Transaction> ICBCPlugin::GetCurTranImpl() {
    if (!loggedIn_) { LoginImpl(); }
    // 工行当日交易页面，从菜单导航
    // 实际 URL 取决于工行版本号，先导航到 menu tree
    return GetHisTranImpl({util::TodayStr(), util::TodayStr()});
}

std::vector<plugin::Transaction> ICBCPlugin::GetHisTranImpl(
    const plugin::DateRange& range) {

    if (!loggedIn_) { LoginImpl(); }
    if (!loggedIn_) return {};

    // 核心：在工行页面里找"交易明细查询"入口 → 填时间 → 查 → 抓表格
    // 每个版本的菜单名/元素 id 都不同，所以用 Runtime.evaluate 做表格提取
    //
    // 步骤：
    // 1. 导航到交易查询入口（可能是菜单项点击，也可能直接 URL）
    // 2. 填起止日期（selector 可能是 input[type=date] 或 <a>/<span> 点击日期控件）
    // 3. 点查询
    // 4. 抓结果表格

    // --- 临时方案：直接在当前页面（主页面）找流水表格 ---
    std::string js = R"(
        (function() {
            var table = document.querySelector(
                'table.result, table.data, table.list, table, ' +
                '[class*="result"] table, [class*="trans"] table'
            );
            if (!table) return '';
            var rows = table.querySelectorAll('tr');
            var out = [];
            for (var i = 1; i < rows.length; i++) {  // 跳过 header
                var cells = rows[i].querySelectorAll('td');
                if (cells.length < 4) continue;
                var cols = [];
                for (var c of cells) cols.push(c.innerText.trim());
                out.push(cols.join(' | '));
            }
            return out.join('\n');
        })()
    )";
    std::string raw = EvalStr(js);
    auto parsed = ParseTransTable();
    auto csvPath = io::WriteTransactionsCsv(
        "trans_" + range.begin + "_" + range.end + ".csv", parsed);
    spdlog::info("[ICBC] Saved {} rows to {}", parsed.size(), csvPath);
    return parsed;
}

std::vector<plugin::Transaction> ICBCPlugin::ParseTransTable() {
    std::vector<plugin::Transaction> result;
    std::string js = R"(
        (function() {
            var table = document.querySelector('table');
            if (!table) return '[]';
            var rows = table.querySelectorAll('tr');
            var cols = rows[0].querySelectorAll('th, td');
            var header = [];
            for (var c of cols) header.push(c.innerText.trim());

            var out = [];
            for (var i = 1; i < rows.length; i++) {
                var cells = rows[i].querySelectorAll('td');
                if (cells.length < 3) continue;
                var obj = {};
                for (var j = 0; j < cells.length && j < header.length; j++)
                    obj[header[j]] = cells[j].innerText.trim();
                out.push(obj);
            }
            return JSON.stringify(out);
        })()
    )";
    try {
        auto arr = nlohmann::json::parse(EvalStr(js));
        for (auto& row : arr) {
            plugin::Transaction t;
            // 工行表头字段：日期、摘要、金额、余额、对方户名...
            // 尽量匹配常见中文名
            t.transDate    = row.value("交易日期",   row.value("日期", ""));
            t.amount       = row.value("金额",       row.value("发生额", ""));
            t.remark       = row.value("摘要",       row.value("用途", ""));
            t.opAccount    = row.value("对方账号",   row.value("对方账户", ""));
            t.opAccountName= row.value("对方户名",   row.value("对方名称", ""));
            t.balanceAfter = row.value("余额",       "");
            t.direction    = row.value("借贷标志",   row.value("方向", ""));
            result.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[ICBC] Parse trans table failed: {}", e.what());
    }
    return result;
}

// ----------------------------------------------------------------------------
// DownBankSlip — 回单下载
// ----------------------------------------------------------------------------

std::vector<std::string> ICBCPlugin::DownBankSlipImpl(
    const plugin::DateRange& range) {
    if (!loggedIn_) { LoginImpl(); }
    // 回单下载需要工行的"电子回单"菜单，每个回单对应一个 PDF
    // 实际 URL 结构：/ebs/servlet/... 下载 PDF
    // 这里返回空实现，等接真实工行环境
    spdlog::info("[ICBC] DownBankSlip stub (need real ICBC env)");
    return {};
}

// ----------------------------------------------------------------------------
// Payment / Approval / Query — 付款/审批/查询
// ----------------------------------------------------------------------------

std::string ICBCPlugin::PaymentImpl(
    const std::string& amount, const std::string& nums,
    const std::string& paccno, const std::string& filename,
    const std::string& sn) {
    spdlog::warn("[ICBC] Payment stub");
    return "stub";
}

std::string ICBCPlugin::ApprovalImpl(const std::string& no) {
    spdlog::warn("[ICBC] Approval stub");
    return "stub";
}

std::string ICBCPlugin::QueryImpl(const std::string& no) {
    spdlog::warn("[ICBC] Query stub");
    return "stub";
}

} // namespace icbc
} // namespace newrpa
