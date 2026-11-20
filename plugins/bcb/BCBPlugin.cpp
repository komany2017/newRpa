// ============================================================================
// BCB 银行插件实现 — 北京银行企业网银（演示版）
//
// 入口: https://cbank.bankofbeijing.com.cn:9081/login
// 演示账号: 企业名称=测试企业 / 证书密码=111111 / 用户名称=提交人 / 登录密码=111111
//
// 核心策略：点银行"导出 Excel/CSV"按钮 → CDP Network 域拦截 response body → base64 解码存本地
// 没有导出按钮时 fallback DOM 抓表格
// ============================================================================

#include "BCBPlugin.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/io/ExcelWriter.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>
#include <iomanip>
#include <ctime>

using namespace newrpa;
namespace fs = std::filesystem;

// 简单 CSV 行解析（处理双引号包裹的字段）
static std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> result;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') {
            if (inQuotes && i + 1 < line.size() && line[i+1] == '"') {
                cur += '"'; i++;  // 双引号转义
            } else {
                inQuotes = !inQuotes;
            }
        } else if (c == ',' && !inQuotes) {
            result.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    result.push_back(cur);
    return result;
}

// 把 DumpTablesJson 产出的 [{rowCount,data:[[...],[...]]}] 转成 "表头->值" 行
static std::vector<std::map<std::string, std::string>> TablesJsonToRows(
    const std::string& jsonText) {
    std::vector<std::map<std::string, std::string>> rows;
    try {
        auto arr = nlohmann::json::parse(jsonText);
        for (auto& tbl : arr) {
            if (!tbl.contains("data")) continue;
            auto data = tbl["data"];
            if (!data.is_array() || data.empty()) continue;
            auto header = data[0];
            if (!header.is_array() || header.empty()) continue;
            for (size_t i = 1; i < data.size(); ++i) {
                auto cells = data[i];
                if (!cells.is_array()) continue;
                std::map<std::string, std::string> row;
                for (size_t j = 0; j < cells.size() && j < header.size(); ++j) {
                    std::string h = header[j].is_string() ? header[j].get<std::string>() : "";
                    std::string v = cells[j].is_string() ? cells[j].get<std::string>() : cells[j].dump();
                    if (!h.empty()) row[h] = v;
                }
                if (!row.empty()) rows.push_back(std::move(row));
            }
        }
    } catch (...) {}
    return rows;
}

namespace newrpa {
namespace bcb {

// ----------------------------------------------------------------------------
// DLL 导出
// ----------------------------------------------------------------------------
extern "C" {
plugin::IBankPlugin* CreatePlugin()  { return new BCBPlugin(); }
void DestroyPlugin(plugin::IBankPlugin* p) { delete p; }
}

// ----------------------------------------------------------------------------
// 构造/析构
// ----------------------------------------------------------------------------

BCBPlugin::BCBPlugin()  = default;
BCBPlugin::~BCBPlugin() { Close(); }

uint32_t BCBPlugin::Capabilities() const {
    return plugin::IBankPlugin::CAP_LOGIN
         | plugin::IBankPlugin::CAP_CUR_BALANCE
         | plugin::IBankPlugin::CAP_CUR_TRANS
         | plugin::IBankPlugin::CAP_YES_TRANS
         | plugin::IBankPlugin::CAP_SLIP;
}

void BCBPlugin::SetSession(std::shared_ptr<cdp::CDPSession> s,
                            std::shared_ptr<chrome::ChromeBrowser> b) {
    session_ = std::move(s);
    browser_ = std::move(b);
    if (session_) {
        page_   = std::make_unique<cdp::PageDomain>(session_.get());
        rt_     = std::make_unique<cdp::RuntimeDomain>(session_.get());
        dom_    = std::make_unique<cdp::DomDomain>(session_.get());
        input_  = std::make_unique<cdp::InputDomain>(session_.get());
        net_    = std::make_unique<cdp::NetworkDomain>(session_.get());
        // 显式 enable Network（Chrome 156+ 部分 domain 需要）
        try { net_->Enable(); } catch (...) {}
    }
}

void BCBPlugin::SetConfig(const plugin::BankConfig& cfg) {
    cfg_ = cfg;
}

void BCBPlugin::Close() {
    loggedIn_ = false;
}

// ----------------------------------------------------------------------------
// 便捷 helper
// ----------------------------------------------------------------------------

std::string BCBPlugin::EvalStr(const std::string& js) {
    if (!rt_) return "";
    return rt_->EvalString(js);
}

bool BCBPlugin::EvalBool(const std::string& js) {
    if (!rt_) return false;
    return rt_->EvalBool(js);
}

double BCBPlugin::EvalNum(const std::string& js) {
    if (!rt_) return 0.0;
    return rt_->EvalNumber(js);
}

bool BCBPlugin::WaitForElement(const std::string& selector, int timeoutMs) {
    if (!rt_) return false;
    std::string escapedSel = util::ReplaceAll(selector, "\\", "\\\\");
    escapedSel = util::ReplaceAll(escapedSel, "\"", "\\\"");

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string js = "(function() {"
            "var frames = [document];"
            "try { for (var i = 0; i < document.frames.length; i++) {"
            "  try { frames.push(document.frames[i].document); } catch(e){}"
            "}} catch(e){}"
            "var sel = \"" + escapedSel + "\";"
            "for (var d of frames) {"
            "  try { var s = d.querySelector(sel); if (s) return true; } catch(e){}"
            "}"
            "return false;"
            "})()";
        if (EvalBool(js)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool BCBPlugin::WaitUrlContains(const std::string& sub, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string url = EvalStr("location.href");
        if (url.find(sub) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool BCBPlugin::FillInput(const std::string& selector, const std::string& value) {
    if (!rt_) return false;
    std::string escapedVal = util::ReplaceAll(value, "\\", "\\\\");
    escapedVal = util::ReplaceAll(escapedVal, "\"", "\\\"");
    std::string escapedSel = util::ReplaceAll(selector, "\\", "\\\\");
    escapedSel = util::ReplaceAll(escapedSel, "\"", "\\\"");

    std::string js = "(function() {"
        "var frames = [document];"
        "try { for (var i = 0; i < document.frames.length; i++) {"
        "  try { frames.push(document.frames[i].document); } catch(e){}"
        "}} catch(e){}"
        "var sel = \"" + escapedSel + "\";"
        "for (var d of frames) {"
        "  try {"
        "    var el = d.querySelector(sel);"
        "    if (!el) continue;"
        "    var proto = Object.getPrototypeOf(el);"
        "    var setter = null;"
        "    if (proto) {"
        "      var desc = Object.getOwnPropertyDescriptor(proto, 'value');"
        "      if (desc && desc.set) setter = desc.set;"
        "    }"
        "    if (setter) { setter.call(el, \"" + escapedVal + "\"); }"
        "    else { el.value = \"" + escapedVal + "\"; }"
        "    ['input','change'].forEach(function(ev){"
        "      el.dispatchEvent(new Event(ev, { bubbles: true }));"
        "    });"
        "    return true;"
        "  } catch(e){}"
        "}"
        "return false;"
        "})()";
    return EvalBool(js);
}

bool BCBPlugin::ClickElementJs(const std::string& selector) {
    if (!rt_) return false;
    std::string escapedSel = util::ReplaceAll(selector, "\\", "\\\\");
    escapedSel = util::ReplaceAll(escapedSel, "\"", "\\\"");

    std::string js = "(function() {"
        "var frames = [document];"
        "try { for (var i = 0; i < document.frames.length; i++) {"
        "  try { frames.push(document.frames[i].document); } catch(e){}"
        "}} catch(e){}"
        "var sel = \"" + escapedSel + "\";"
        "for (var d of frames) {"
        "  try {"
        "    var s = d.querySelector(sel);"
        "    if (s) { s.click(); return true; }"
        "  } catch(e){}"
        "}"
        "return false;"
        "})()";
    return EvalBool(js);
}

std::string BCBPlugin::PageDiag() {
    return EvalStr(R"(
        (function() {
            var frames = [document];
            try { for (var i=0;i<document.frames.length;i++) {
                try { frames.push(document.frames[i].document); } catch(e){}
            } } catch(e){}
            var out = [];
            for (var d of frames) {
                try {
                    var text = (d.body && d.body.innerText) || '';
                    if (text) out.push('FRAME:' + text.substring(0, 200));
                } catch(e){}
            }
            return out.join(' | ');
        })()
    )");
}

// ============================================================================
//              演示版专用流程（Python CDP 脚本已验证）
//   路由直连 → 点确定 → 授权原生下载 → 点下载+二次确认 → 落盘重命名
// ============================================================================

std::string BCBPlugin::DownloadDir() {
    // 用原生绝对路径（正斜杠会导致写入失败，参考 rpa_bob_excel.cpp）
    fs::path p = fs::absolute("downloads");
    try { fs::create_directories(p); } catch (...) {}
    return p.string();
}

bool BCBPlugin::SaveTextFile(const std::string& path, const std::string& content) {
    return WriteBinaryFile(path, content);
}

bool BCBPlugin::EnableNativeDownloads() {
    if (!session_) return false;
    try {
        session_->Call("Browser.setDownloadBehavior", {
            {"behavior", "allow"},
            {"downloadPath", DownloadDir()},
            {"eventsEnabled", true}
        }, 10000);
        spdlog::info("[BCB] Native download enabled -> {}", DownloadDir());
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("[BCB] Browser.setDownloadBehavior failed: {}", e.what());
        return false;
    }
}

bool BCBPlugin::GotoRoute(const std::string& route) {
    std::string url = "https://cbank.bankofbeijing.com.cn:9081/" + route;
    spdlog::info("[BCB] Goto {}", url);
    try {
        session_->Call("Page.navigate", {{"url", url}}, 25000);
    } catch (const std::exception& e) {
        spdlog::warn("[BCB] Page.navigate exception: {}", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    return true;
}

bool BCBPlugin::ClickButtonByText(const std::string& text) {
    std::string lit = nlohmann::json(text).dump();  // 安全转义（中文转 \uXXXX）
    // 只点击"可见"按钮，避开隐藏弹窗/筛选区里的同名按钮（如"确认"）
    std::string js =
        "(function(){var t=" + lit + ";var b=document.querySelectorAll('button');"
        "function vis(e){if(!e||e.disabled)return false;if(e.offsetParent===null)return false;"
        "var cs=getComputedStyle(e);if(cs.display==='none'||cs.visibility==='hidden')return false;"
        "var r=e.getBoundingClientRect();return r.width>0&&r.height>0;}"
        "var cand=[];for(var e of b){if((e.innerText||e.textContent||'').trim()===t&&vis(e))cand.push(e);}"
        "if(cand.length){cand[cand.length-1].click();return true;}return false;})()";
    bool ok = EvalBool(js);
    spdlog::info("[BCB] ClickButtonByText('{}') = {}", text, ok);
    return ok;
}

bool BCBPlugin::ClickDialogButton(const std::string& text) {
    std::string lit = nlohmann::json(text).dump();
    std::string js =
        "(function(){var t=" + lit + ";var b=document.querySelectorAll('.el-message-box button,.el-dialog button');"
        "for(var e of b){var x=(e.innerText||'').replace(/\\s+/g,'');"
        "if(x.indexOf(t)>=0){e.click();return true;}}return false;})()";
    return EvalBool(js);
}

std::string BCBPlugin::DumpTablesJson() {
    return EvalStr(R"(
        (function(){var out=[];var ts=document.querySelectorAll('table');
        for(var tb of ts){var rows=tb.querySelectorAll('tr');if(rows.length<1)continue;
        var data=[];for(var i=0;i<rows.length&&i<200;i++){var cs=rows[i].querySelectorAll('th,td');
        var arr=[];for(var c of cs)arr.push((c.innerText||'').trim());data.push(arr);}
        out.push({rowCount:rows.length,data:data});}return JSON.stringify(out);})()
    )");
}

void BCBPlugin::ClickDownloadWithConfirm() {
    bool clicked = ClickButtonByText("下载");
    if (!clicked) ClickButtonByText("下 载");
    // 二次确认弹窗（参考 rpa_bob_excel.cpp）
    for (int k = 0; k < 14; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (ClickDialogButton("下载")) { spdlog::info("[BCB] 二次确认: 已点下载"); break; }
        if (ClickDialogButton("确定")) { spdlog::info("[BCB] 二次确认: 已点确定"); break; }
    }
}

std::vector<std::string> BCBPlugin::WaitForNewFiles(
    const std::vector<std::string>& before, const std::string& tag, int timeoutMs) {
    std::vector<std::string> saved;
    std::set<std::string> known(before.begin(), before.end());
    std::string dir = DownloadDir();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string fn = it->path().filename().string();
            if (known.count(fn)) continue;
            if (fn.size() >= 13 && fn.substr(fn.size() - 13) == ".crdownload") continue;
            if (fn.size() > 5 && fn.substr(fn.size() - 5) == ".json") continue;
            if (it->file_size(ec) == 0) continue;
            std::string newname = tag + "_" + fn;
            fs::path dst = fs::path(dir) / newname;
            std::error_code e2;
            if (fs::exists(dst, e2)) fs::remove(dst, e2);
            fs::rename(it->path(), dst, e2);
            if (!e2) {
                spdlog::info("[BCB] ★ 下载文件: {} ({} bytes)", newname,
                             fs::file_size(dst, e2));
                saved.push_back(dst.string());
                known.insert(fn);
                known.insert(newname);
            }
        }
        if (!saved.empty()) {
            // 再观察 1.5 秒，收集同批第二个文件（如下载两次）
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }
    if (saved.empty()) spdlog::warn("[BCB] 未捕获到原生下载文件");
    return saved;
}

std::vector<std::string> BCBPlugin::DoExport(const std::string& route,
                                             const std::string& tag, bool receipt) {
    spdlog::info("[BCB] --- {} ---", tag);
    EnableNativeDownloads();
    GotoRoute(route);
    ClickButtonByText("确定");
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));

    // 抓表格 → data.json
    std::string tables = DumpTablesJson();
    if (!tables.empty() && tables != "[]") {
        std::string jp = (fs::path(DownloadDir()) / (tag + "_data.json")).string();
        SaveTextFile(jp, tables);
        spdlog::info("[BCB] {} 表格数据已存 ({} bytes)", tag, tables.size());
    }

    // 快照下载目录（在写 data.json 之后）
    std::vector<std::string> before;
    {
        std::string dir = DownloadDir();
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (!ec && it->is_regular_file(ec)) before.push_back(it->path().filename().string());
        }
    }

    if (receipt) {
        // 电子回单：勾选 → 生成回单 → 选方式一 → 确认 → 刷新（内联 JS，取首个匹配）
        // 用 el-table 的 toggleRowSelection 直接操作 Vue 选择模型（最可靠）
        EvalStr("(function(){var t=document.querySelector('.el-table');"
                "if(!t||!t.__vue__)return 'no-vue';var vm=t.__vue__;"
                "var d=vm.store&&vm.store.states&&vm.store.states.data;if(!d)return 'no-data';"
                "for(var x of d)vm.toggleRowSelection(x,true);return String(d.length);})()");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){if((e.innerText||'').trim()==='生成回单'&&!e.disabled){e.click();return true;}}return false;})()");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        EvalBool("(function(){var e=document.querySelectorAll('.el-dialog label,.el-dialog .el-radio,.el-message-box label');"
                 "for(var x of e){if((x.innerText||'').indexOf('方式一')>=0){x.click();return true;}}return false;})()");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        bool conf = EvalBool("(function(){var b=document.querySelectorAll('.el-dialog button');for(var e of b){if((e.innerText||'').replace(/\\s+/g,'')==='确认'){e.click();return true;}}return false;})()");
        if (!conf) EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){if((e.innerText||'').trim()==='确认'&&!e.disabled){e.click();return true;}}return false;})()");
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));
        EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){if((e.innerText||'').trim()==='刷新'&&!e.disabled){e.click();return true;}}return false;})()");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    ClickDownloadWithConfirm();
    return WaitForNewFiles(before, tag);
}

// ============================================================================
//        账号遍历 + 增量时间（all-accounts）
// ============================================================================

std::vector<std::pair<std::string, std::string>> BCBPlugin::SelOptions(int index) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string js =
        "(function(){var s=document.querySelectorAll('.el-select')[" + std::to_string(index) + "];"
        "var vm=s&&s.__vue__;if(!vm||!vm.options)return '[]';var o=[];"
        "for(var k=0;k<vm.options.length;k++)o.push({l:String(vm.options[k].label||''),v:vm.options[k].value});"
        "return JSON.stringify(o);})()";
    std::string raw = EvalStr(js);
    try {
        auto arr = nlohmann::json::parse(raw);
        for (auto& o : arr) {
            std::string label = o.value("l", "");
            std::string value;
            auto v = o.value("v", nlohmann::json(""));
            if (v.is_string()) value = v.get<std::string>();
            else value = v.dump();
            out.push_back({label, value});
        }
    } catch (...) {}
    return out;
}

bool BCBPlugin::SetSelByValue(int index, const std::string& value) {
    std::string valLit = nlohmann::json(value).dump();
    std::string js =
        "(function(){var s=document.querySelectorAll('.el-select')[" + std::to_string(index) + "];"
        "var vm=s&&s.__vue__;if(!vm)return 'no-vue';var opt=null;"
        "for(var k=0;k<vm.options.length;k++){var ov=vm.options[k].value;"
        "if(String(ov)===" + valLit + "){opt=vm.options[k];break;}}"
        "if(!opt)return 'no-opt';try{vm.handleOptionSelect(opt,true);return 'ok';}"
        "catch(e){vm.$emit('input'," + valLit + ");return 'emit';}})()";
    std::string r = EvalStr(js);
    spdlog::info("[BCB] SetSel[{}]={} -> {}", index, value, r);
    return r == "ok" || r == "emit";
}

std::string BCBPlugin::SelValue(int index) {
    std::string js =
        "(function(){var s=document.querySelectorAll('.el-select')[" + std::to_string(index) + "];"
        "var vm=s&&s.__vue__;if(!vm)return '';var v=vm.value;"
        "return v===null||v===undefined?'':(typeof v==='string'?v:JSON.stringify(v));})()";
    return EvalStr(js);
}

std::string BCBPlugin::StateFilePath() {
    return (fs::absolute("state") / "bcb_histran_state.json").string();
}

std::string BCBPlugin::LoadLastHisEnd() {
    std::string p = StateFilePath();
    std::ifstream f(p);
    if (!f) return "";
    try {
        nlohmann::json j; f >> j;
        return j.value("last_success_end", "");
    } catch (...) { return ""; }
}

bool BCBPlugin::SaveLastHisEnd(const std::string& endYmd) {
    try {
        fs::path p = StateFilePath();
        fs::create_directories(p.parent_path());
        nlohmann::json j;
        std::ifstream f(p);
        if (f) { try { f >> j; } catch (...) { j = nlohmann::json::object(); } }
        else j = nlohmann::json::object();
        j["last_success_end"] = endYmd;
        j["updated_at"] = util::FormatTime(std::time(nullptr));
        std::ofstream o(p, std::ios::trunc);
        o << j.dump(2);
        spdlog::info("[BCB] Saved histran state: last_success_end={}", endYmd);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("[BCB] Save state failed: {}", e.what());
        return false;
    }
}

std::string BCBPlugin::HalfYearAgoYmd() {
    auto t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    tm.tm_mon -= 6;                       // 往前半年
    std::mktime(&tm);                     // 规格化
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

std::string BCBPlugin::YmdToDash(const std::string& ymd) {
    if (ymd.size() == 8) return ymd.substr(0,4) + "-" + ymd.substr(4,2) + "-" + ymd.substr(6,2);
    return ymd;
}

// 快照下载目录当前文件集合
static std::vector<std::string> SnapshotDownloadDir(const std::string& dir) {
    std::vector<std::string> r;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec))
        if (!ec && it->is_regular_file(ec)) r.push_back(it->path().filename().string());
    return r;
}

std::string BCBPlugin::RunAllAccounts() {
    if (!loggedIn_) Login();
    if (!loggedIn_) return "login-failed";

    EnableNativeDownloads();
    int okFiles = 0;
    std::string detail;

    // ---------- 1) 历史交易（优先；增量时间） ----------
    std::string lastEnd = LoadLastHisEnd();
    std::string histBegin = lastEnd.empty() ? HalfYearAgoYmd() : lastEnd;
    std::string todayYmd;
    {
        auto t = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &t);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        todayYmd = buf;
    }
    spdlog::info("[ALL-ACC] 历史交易区间: {} ~ {} (来源: {})",
                 histBegin, todayYmd, lastEnd.empty() ? "默认半年" : "上次成功时间");

    bool histranOK = true;
    GotoRoute("AM/AM2/TAM2A01");
    auto orgs = SelOptions(0);           // 机构
    const int accIdxBase = 1;            // 账号 select 索引
    for (size_t oi = 0; oi < orgs.size(); ++oi) {
        SetSelByValue(0, orgs[oi].second);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        auto accs = SelOptions(accIdxBase);
        for (size_t ai = 0; ai < accs.size(); ++ai) {
            SetSelByValue(accIdxBase, accs[ai].second);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            ClickButtonByText("确定");
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            auto before = SnapshotDownloadDir(DownloadDir());
            ClickDownloadWithConfirm();
            std::string acctTag = accs[ai].second;
            std::string shortAcc = acctTag.size() >= 4 ? acctTag.substr(acctTag.size()-4) : acctTag;
            auto files = WaitForNewFiles(before, "历史交易_" + shortAcc, 9000);
            okFiles += (int)files.size();
            detail += "  历史交易 " + shortAcc + ": " + std::to_string(files.size()) + " 文件\n";
        }
    }
    // 历史交易全部账号完成 → 保存本次 end 作为下次 begin
    if (histranOK && okFiles > 0) SaveLastHisEnd(todayYmd);

    // ---------- 2) 当日交易 ----------
    GotoRoute("AM/AM2/TAM2A01");
    orgs = SelOptions(0);
    for (size_t oi = 0; oi < orgs.size(); ++oi) {
        SetSelByValue(0, orgs[oi].second);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        auto accs = SelOptions(accIdxBase);
        for (size_t ai = 0; ai < accs.size(); ++ai) {
            SetSelByValue(accIdxBase, accs[ai].second);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            ClickButtonByText("确定");
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            auto before = SnapshotDownloadDir(DownloadDir());
            ClickDownloadWithConfirm();
            std::string acctTag = accs[ai].second;
            std::string shortAcc = acctTag.size() >= 4 ? acctTag.substr(acctTag.size()-4) : acctTag;
            auto files = WaitForNewFiles(before, "当日交易_" + shortAcc, 9000);
            okFiles += (int)files.size();
            detail += "  当日交易 " + shortAcc + ": " + std::to_string(files.size()) + " 文件\n";
        }
    }

    // ---------- 3) 余额（全部账号一次） ----------
    {
        EnableNativeDownloads();
        GotoRoute("AM/AM1/TAM1A01");
        ClickButtonByText("确定");
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        std::string tables = DumpTablesJson();
        if (!tables.empty() && tables != "[]")
            SaveTextFile((fs::path(DownloadDir()) / "余额_全部账号_data.json").string(), tables);
        auto before = SnapshotDownloadDir(DownloadDir());
        ClickDownloadWithConfirm();
        auto files = WaitForNewFiles(before, "余额_全部账号", 9000);
        okFiles += (int)files.size();
        detail += "  余额(全部): " + std::to_string(files.size()) + " 文件\n";
    }

    // ---------- 4) 电子回单（遍历机构 × 账号） ----------
    GotoRoute("AM/AM3/NAM3001");
    orgs = SelOptions(0);
    for (size_t oi = 0; oi < orgs.size(); ++oi) {
        SetSelByValue(0, orgs[oi].second);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        auto accs = SelOptions(accIdxBase);
        for (size_t ai = 0; ai < accs.size(); ++ai) {
            SetSelByValue(accIdxBase, accs[ai].second);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            // 回单页"确定"用首个可见匹配（页面有多个同名按钮，避免点错）
            EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){var t=(e.innerText||'').trim();if(t==='确定'&&e.offsetParent!==null&&!e.disabled){e.click();return true;}}return false;})()");
            std::this_thread::sleep_for(std::chrono::milliseconds(3500));
            auto before = SnapshotDownloadDir(DownloadDir());
            // 回单：勾选 → 生成 → 方式一 → 确认 → 刷新 → 下载
            // 用探测脚本验证过的内联 JS，避开可见性/多按钮匹配歧义
            // 用 el-table 的 toggleRowSelection 直接操作 Vue 选择模型（最可靠）
            // 遍历所有 el-table，对每个有数据的表全选；返回 JSON 诊断
            std::string picked = EvalStr(
                "(function(){var ts=document.querySelectorAll('.el-table');var out=[];"
                "for(var i=0;i<ts.length;i++){var vm=ts[i].__vue__;"
                "if(!vm||!vm.store||!vm.store.states||!vm.store.states.data)continue;"
                "var d=vm.store.states.data;var n=0;"
                "for(var k=0;k<d.length;k++){try{vm.toggleRowSelection(d[k],true);n++;}catch(e){}}"
                "out.push(i+':'+d.length+'/'+n);}"
                "return JSON.stringify({tables:ts.length,done:out,"
                "checked:document.querySelectorAll('.el-checkbox.is-checked').length});})()");
            spdlog::info("[BCB] 回单勾选(toggleRowSelection)={}", picked);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            bool gen = EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){if((e.innerText||'').trim()==='生成回单'&&!e.disabled){e.click();return true;}}return false;})()");
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            std::string dlgDump = EvalStr("(function(){var ds=document.querySelectorAll('.el-dialog,.el-message-box');var o=[];for(var d of ds){if(getComputedStyle(d).display==='none')continue;o.push((d.innerText||'').substring(0,120));}return JSON.stringify(o);})()");
            spdlog::info("[BCB] 回单gen={} 弹窗={}", gen, dlgDump);
            bool mode = EvalBool("(function(){var e=document.querySelectorAll('.el-dialog label,.el-dialog .el-radio,.el-message-box label');"
                     "for(var x of e){if((x.innerText||'').indexOf('方式一')>=0){x.click();return true;}}return false;})()");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            // "确认"按钮：优先弹窗内，其次全局第一个匹配
            bool conf = EvalBool("(function(){var b=document.querySelectorAll('.el-dialog button,.el-message-box button');for(var e of b){if((e.innerText||'').replace(/\\s+/g,'')==='确认'){e.click();return true;}}return false;})()");
            if (!conf) conf = EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){if((e.innerText||'').replace(/\\s+/g,'')==='确认'&&!e.disabled){e.click();return true;}}return false;})()");
            spdlog::info("[BCB] 回单流程 picked={} gen={} mode={} conf={}", picked, gen, mode, conf);
            std::this_thread::sleep_for(std::chrono::milliseconds(4500));
            EvalBool("(function(){var b=document.querySelectorAll('button');for(var e of b){var t=(e.innerText||'').trim();if(t==='刷新'&&!e.disabled){e.click();return true;}}return false;})()");
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            ClickDownloadWithConfirm();
            std::string acctTag = accs[ai].second;
            std::string shortAcc = acctTag.size() >= 4 ? acctTag.substr(acctTag.size()-4) : acctTag;
            auto files = WaitForNewFiles(before, "电子回单_" + shortAcc, 9000);
            okFiles += (int)files.size();
            detail += "  电子回单 " + shortAcc + ": " + std::to_string(files.size()) + " 文件\n";
        }
    }

    return "all-accounts done, files=" + std::to_string(okFiles) + "\n" + detail;
}

// ----------------------------------------------------------------------------
// OpenMenu — 在页面菜单树里逐级点开
//
// 北京银行典型菜单路径:
//   { "账户查询", "余额查询" }
//   { "账户查询", "当日明细" }
//   { "账户查询", "历史明细" }
//   { "电子回单", "电子回单查询" }
// ----------------------------------------------------------------------------
bool BCBPlugin::OpenMenu(const std::vector<std::string>& menuPath) {
    if (menuPath.empty()) return true;

    for (auto& item : menuPath) {
        spdlog::info("[BCB] OpenMenu step: {}", item);
        std::string itemEsc = util::ReplaceAll(item, "\"", "\\\"");
        std::string js = R"(
            (function() {
                var frames = [document];
                try {
                    for (var i = 0; i < document.frames.length; i++) {
                        try { frames.push(document.frames[i].document); } catch(e){}
                    }
                } catch(e){}
                var target = ")" + itemEsc + R"(";
                for (var d of frames) {
                    try {
                        var all = d.querySelectorAll('a, span, li, div, td, button, h3, h4');
                        for (var el of all) {
                            var t = (el.innerText || el.textContent || '').trim();
                            if (t === target || t.indexOf(target) !== -1) {
                                try { el.scrollIntoView({block:'center'}); } catch(e){}
                                el.click();
                                return true;
                            }
                        }
                    } catch(e){}
                }
                return false;
            })()
        )";
        bool found = false;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (EvalBool(js)) { found = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (!found) {
            spdlog::warn("[BCB] Menu item '{}' not found", item);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
    return true;
}

// ----------------------------------------------------------------------------
// ClickExportButton — 找页面上的"导出/下载"按钮并点击
//
// 银行页面常见导出按钮关键词：
//   - 文字: "导出" "下载" "导出Excel" "导出CSV" "Excel"
//   - id/class: export, download, btnExport, btnDownload
// ----------------------------------------------------------------------------
bool BCBPlugin::ClickExportButton() {
    spdlog::info("[BCB] Looking for export button...");
    std::string js = R"(
        (function() {
            var frames = [document];
            try {
                for (var i = 0; i < document.frames.length; i++) {
                    try { frames.push(document.frames[i].document); } catch(e){}
                }
            } catch(e){}
            // 关键词列表（不区分大小写）
            var keywords = ['导出', '下载', 'export', 'download', 'Excel', 'CSV', 'csv', 'xlsx'];
            // id/class 关键词
            var attrKw = ['export', 'download', 'Export', 'Download', 'btnExport', 'btnDownload'];

            for (var d of frames) {
                try {
                    // 先查带关键词的按钮/链接
                    var all = d.querySelectorAll('button, a, input, span, div, td');
                    for (var el of all) {
                        // 1. 文字匹配
                        var t = (el.innerText || el.textContent || '').trim();
                        for (var kw of keywords) {
                            if (t && t.indexOf(kw) !== -1 && t.length < 20) {
                                // 排除导航菜单里的"导出"
                                try { el.scrollIntoView({block:'center'}); } catch(e){}
                                el.click();
                                return true;
                            }
                        }
                        // 2. id/class 匹配
                        var eid = el.id || '';
                        var ecls = el.className || '';
                        var attrHit = false;
                        for (var ak of attrKw) {
                            if ((eid && eid.indexOf(ak) !== -1) ||
                                (typeof ecls === 'string' && ecls.indexOf(ak) !== -1)) {
                                attrHit = true; break;
                            }
                        }
                        if (attrHit) {
                            try { el.scrollIntoView({block:'center'}); } catch(e){}
                            el.click();
                            return true;
                        }
                    }
                } catch(e){}
            }
            return false;
        })()
    )";

    // 等最多 3 秒让按钮出现
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (EvalBool(js)) {
            spdlog::info("[BCB] Export button clicked");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    spdlog::warn("[BCB] Export button not found");
    return false;
}

// ----------------------------------------------------------------------------
// CaptureAndSaveDownload — NetworkDomain 拦截下载 → 存本地
// ----------------------------------------------------------------------------
bool BCBPlugin::WriteBinaryFile(const std::string& path,
                                const std::string& content) {
    try {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), content.size());
        return f.good();
    } catch (...) { return false; }
}

std::vector<std::string> BCBPlugin::CaptureAndSaveDownload(
    const std::string& urlKeyword,
    const std::string& contentTypeKeyword,
    int timeoutMs) {

    std::vector<std::string> saved;
    if (!net_) {
        spdlog::error("[BCB] NetworkDomain not initialized");
        return saved;
    }

    // 1. 开始监听
    spdlog::info("[BCB] Start capture: urlKw='{}' ctKw='{}'",
                 urlKeyword, contentTypeKeyword);
    net_->StartCaptureDownload(urlKeyword, contentTypeKeyword);

    // 2. 等一小段时间让下载触发
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 3. 等下载到达（最多 timeoutMs）
    auto captures = net_->StopCaptureDownload(timeoutMs);
    spdlog::info("[BCB] Captured {} download(s)", captures.size());

    // 4. 每个都存本地
    static const char* kDownloadDir = "downloads";
    try { fs::create_directories(kDownloadDir); } catch (...) {}

    int idx = 0;
    for (auto& rb : captures) {
        std::string fname = rb.filename;
        if (fname.empty() || fname == "download.bin") {
            // 根据 content-type 猜扩展名
            if (rb.contentType.find("spreadsheet") != std::string::npos ||
                rb.contentType.find("ms-excel") != std::string::npos)
                fname = "export_" + std::to_string(++idx) + ".xlsx";
            else if (rb.contentType.find("csv") != std::string::npos)
                fname = "export_" + std::to_string(++idx) + ".csv";
            else if (rb.contentType.find("pdf") != std::string::npos)
                fname = "slip_" + std::to_string(++idx) + ".pdf";
            else if (rb.contentType.find("octet-stream") != std::string::npos)
                fname = "export_" + std::to_string(++idx) + ".bin";
            else
                fname = "export_" + std::to_string(++idx);
        }

        std::string localPath = (fs::path(kDownloadDir) / fname).string();

        if (WriteBinaryFile(localPath, rb.content)) {
            spdlog::info("[BCB] Saved: {} ({} bytes, ct={})",
                         localPath, rb.content.size(), rb.contentType);
            saved.push_back(localPath);
        } else {
            spdlog::error("[BCB] Write failed: {}", localPath);
        }
    }

    return saved;
}

// ----------------------------------------------------------------------------
// ScrapeTableJson — DOM 方式抓页面上的第一个表格（fallback 用）
// ----------------------------------------------------------------------------
std::string BCBPlugin::ScrapeTableJson() {
    std::string js = R"(
        (function() {
            var frames = [document];
            try {
                for (var i = 0; i < document.frames.length; i++) {
                    try { frames.push(document.frames[i].document); } catch(e){}
                }
            } catch(e){}
            for (var d of frames) {
                try {
                    var table = d.querySelector(
                        'table.result, table.data, table.list, ' +
                        'table[class*="result"], table[class*="list"], ' +
                        'table'
                    );
                    if (!table) continue;
                    var rows = table.querySelectorAll('tr');
                    if (rows.length < 2) continue;

                    var header = [];
                    var headCells = rows[0].querySelectorAll('th, td');
                    for (var c of headCells) header.push((c.innerText || '').trim());
                    if (header.length === 0) continue;

                    var out = [];
                    for (var i = 1; i < rows.length; i++) {
                        var cells = rows[i].querySelectorAll('td');
                        if (cells.length < 2) continue;
                        var obj = {};
                        for (var j = 0; j < cells.length && j < header.length; j++)
                            obj[header[j]] = (cells[j].innerText || '').trim();
                        out.push(obj);
                    }
                    if (out.length > 0) return JSON.stringify(out);
                } catch(e){}
            }
            return '[]';
        })()
    )";
    return EvalStr(js);
}

// ============================================================================
//                              业 务 方 法
// ============================================================================

// ----------------------------------------------------------------------------
// Login — 核心登录流程
// ----------------------------------------------------------------------------
std::string BCBPlugin::Login() {
    if (!session_ || !browser_) return "error: session not ready";
    if (loggedIn_) return "already logged in";

    std::string url = cfg_.loginUrl.empty()
        ? "https://cbank.bankofbeijing.com.cn:9081/login?redirect=%2F"
        : cfg_.loginUrl;

    // 如果当前已经是目标页面，跳过 Navigate
    std::string curUrl = EvalStr("location.href");
    if (curUrl.find("bankofbeijing.com.cn") != std::string::npos) {
        spdlog::info("[BCB] Already on target page, skip navigate");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
        spdlog::info("[BCB] Navigating to {}", url);
        page_->Navigate(url, 30000);
    }

    // 给 Vue SPA 渲染时间
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // 等密码框
    bool formOk = WaitForElement("input[type='password']", 5000);
    if (!formOk) {
        formOk = WaitForElement(
            "input[name*='pass'], input[id*='pass'], input[placeholder*='密码']", 5000);
    }
    if (!formOk) {
        spdlog::error("[BCB] Login form not found. URL={}",
                      EvalStr("location.href"));
        spdlog::info("[BCB] Page: {}", PageDiag());
        return "error: login form not found";
    }
    spdlog::info("[BCB] Login form visible");

    // --- 1. 企业名称 select ---
    {
        std::string company = cfg_.company.empty() ? "测试企业" : cfg_.company;
        std::string companyEsc = util::ReplaceAll(company, "\"", "\\\"");
        std::string jsSetCorp = R"(
            (function() {
                var frames = [document];
                try { for (var i=0;i<document.frames.length;i++) {
                    try { frames.push(document.frames[i].document); } catch(e){}
                } } catch(e){}
                var target = ")" + companyEsc + R"(";
                for (var d of frames) {
                    try {
                        var selects = d.querySelectorAll('select');
                        for (var sel of selects) {
                            var opts = sel.querySelectorAll('option');
                            for (var opt of opts) {
                                var t = (opt.innerText || opt.textContent || '').trim();
                                if ((opt.value && opt.value.indexOf(target) !== -1) ||
                                    t === target) {
                                    sel.value = opt.value;
                                    sel.dispatchEvent(new Event('change', {bubbles:true}));
                                    return true;
                                }
                            }
                        }
                    } catch(e){}
                }
                return true;
            })()
        )";
        EvalBool(jsSetCorp);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // --- 2. 证书密码 ---
    {
        std::string pwd = cfg_.certPassword.empty() ? "111111" : cfg_.certPassword;
        std::vector<std::string> selList = {
            "input[name*='cert']",
            "input[id*='cert']",
            "input[placeholder*='证书']",
            "input[type='password']"   // 兜底
        };
        for (auto& s : selList) {
            if (WaitForElement(s, 500)) {
                if (FillInput(s, pwd)) {
                    spdlog::info("[BCB] Filled cert password via {}", s);
                    break;
                }
            }
        }
    }

    // --- 3. 用户名称 select ---
    {
        std::string jsSetUser = R"(
            (function() {
                var frames = [document];
                try { for (var i=0;i<document.frames.length;i++) {
                    try { frames.push(document.frames[i].document); } catch(e){}
                } } catch(e){}
                var selects = [];
                for (var d of frames) {
                    try {
                        var ss = d.querySelectorAll('select');
                        for (var s of ss) selects.push(s);
                    } catch(e){}
                }
                var target = '提交人';
                if (selects.length >= 2) {
                    var sel = selects[1];
                    var opts = sel.querySelectorAll('option');
                    for (var opt of opts) {
                        var t = (opt.innerText || opt.textContent || '').trim();
                        if (t.indexOf(target) !== -1 || t.indexOf('提交') !== -1) {
                            sel.value = opt.value;
                            sel.dispatchEvent(new Event('change', {bubbles:true}));
                            return true;
                        }
                    }
                }
                return true;
            })()
        )";
        EvalBool(jsSetUser);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // --- 4. 登录密码 ---
    // 北京银行演示版有两个 type=password 框：[0]=证书密码，[1]=登录密码，
    // 均无 name/id，只有 class=el-input__inner。用索引精确定位第二个，
    // 避免 input[type='password'] 命中第一个而覆盖证书密码。
    {
        std::string pwd = cfg_.password.empty() ? "111111" : cfg_.password;
        std::string esc = util::ReplaceAll(pwd, "\\", "\\\\");
        esc = util::ReplaceAll(esc, "\"", "\\\"");
        std::string js = "(function(){"
            "var ins=document.querySelectorAll('input[type=password]');"
            "if(ins.length<2) return false;"
            "var i=ins[1];"
            "var s=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;"
            "s.call(i,\"" + esc + "\");"
            "i.dispatchEvent(new Event('input',{bubbles:true}));"
            "i.dispatchEvent(new Event('change',{bubbles:true}));"
            "return i.value===\"" + esc + "\";"
            "})()";
        if (!EvalBool(js)) {
            spdlog::warn("[BCB] Indexed login password fill failed, fallback to selectors");
            std::vector<std::string> selList = {
                "input[name*='loginPwd']", "input[name='password']", "input[id*='Pwd']"
            };
            bool filled = false;
            for (auto& s : selList) {
                if (FillInput(s, pwd)) { filled = true;
                    spdlog::info("[BCB] Filled login password via {}", s); break; }
            }
            if (!filled) {
                spdlog::error("[BCB] Login password field not found");
                return "error: login password field not found";
            }
        } else {
            spdlog::info("[BCB] Filled login password via index[1]");
        }
    }

    // --- 5. 点登录按钮 ---
    {
        // 北京银行演示版登录按钮是 Element-UI 的
        // <button class="promaryBtn">登录</button>：无 type、无 id。
        // 优先用按钮文字精确匹配点击，避免依赖 id/type/value。
        std::string jsClickByText = R"(
            (function() {
                var bs = document.querySelectorAll('button');
                for (var b of bs) {
                    var t = (b.innerText || b.textContent || '').trim();
                    if (t === '登录') { b.click(); return true; }
                }
                return false;
            })()
        )";
        bool clicked = EvalBool(jsClickByText);
        if (clicked) {
            spdlog::info("[BCB] Clicked login button via text '登录'");
        } else {
            std::vector<std::string> btnSel = {
                "button.promaryBtn",
                "input[type='submit']",
                "button[type='submit']",
                "input[value*='登录']",
                "a.login",
                "#btnLogin", "#Submit", "#loginBtn"
            };
            for (auto& s : btnSel) {
                if (ClickElementJs(s)) {
                    clicked = true;
                    spdlog::info("[BCB] Clicked login button via {}", s);
                    break;
                }
            }
        }
        if (!clicked) {
            spdlog::warn("[BCB] Login button not found, trying Enter");
            input_->KeyPress("Enter", "Enter", "13");
        }
    }

    // --- 6. 等跳转 ---
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::string afterUrl = EvalStr("location.href");
    spdlog::info("[BCB] After login URL: {}", afterUrl);

    if (afterUrl.find("/login") == std::string::npos) {
        loggedIn_ = true;
        spdlog::info("[BCB] Login OK");
        return "ok";
    }

    // 检查错误提示
    std::string err = EvalStr(R"(
        (function() {
            var frames = [document];
            try { for (var i=0;i<document.frames.length;i++) {
                try { frames.push(document.frames[i].document); } catch(e){}
            } } catch(e){}
            for (var d of frames) {
                try {
                    var els = d.querySelectorAll(
                        '.error, .tip, .msg, .alert, ' +
                        '[id*=Error], [class*=error], [class*=warn]'
                    );
                    for (var el of els) {
                        var t = (el.innerText || '').trim();
                        if (t) return t;
                    }
                } catch(e){}
            }
            return '';
        })()
    )");
    if (!err.empty()) {
        spdlog::error("[BCB] Login error: {}", err);
        return "error: " + err;
    }

    // 证书/UKey 未插入 — 演示版预期行为
    return "need_cert_or_ukey";
}

// ----------------------------------------------------------------------------
// GetCurBal — 当日余额查询（下载方式优先）
// ----------------------------------------------------------------------------
std::vector<std::map<std::string, std::string>> BCBPlugin::GetCurBal() {
    if (!loggedIn_) { Login(); }
    if (!loggedIn_) return {};

    // 演示版：路由直连 + 原生下载（Python CDP 脚本已验证）
    auto files = DoExport("AM/AM1/TAM1A01", "01_余额");

    std::vector<std::map<std::string, std::string>> result;
    // 从表格数据还原行
    std::string jp = (fs::path(DownloadDir()) / "01_余额_data.json").string();
    std::ifstream jf(jp);
    if (jf) {
        std::stringstream ss; ss << jf.rdbuf();
        for (auto& r : TablesJsonToRows(ss.str())) {
            r["_source"] = "dom";
            result.push_back(std::move(r));
        }
    }
    // 附加下载文件记录
    for (auto& f : files) {
        std::map<std::string, std::string> row;
        row["_source"] = "download";
        row["_file"] = f;
        result.push_back(std::move(row));
    }

    spdlog::info("[BCB] GetCurBal: {} rows, {} files", result.size(), files.size());
    io::WriteCurBalCsv("bcb_curbal_" + util::TodayStr() + ".csv", result);
    return result;
}

// ----------------------------------------------------------------------------
// GetCurTran — 当日交易（等同 GetHisTran(today, today)）
// ----------------------------------------------------------------------------
std::vector<plugin::Transaction> BCBPlugin::GetCurTran() {
    if (!loggedIn_) { Login(); }
    return GetHisTran({util::TodayStr(), util::TodayStr()});
}

// ----------------------------------------------------------------------------
// GetHisTran — 历史交易（下载优先，DOM fallback）
// ----------------------------------------------------------------------------
std::vector<plugin::Transaction> BCBPlugin::GetHisTran(
    const plugin::DateRange& range) {

    if (!loggedIn_) { Login(); }
    if (!loggedIn_) return {};

    // 当日与历史走同一明细页，用日期区间区分 tag
    bool isToday = (range.begin == range.end);
    std::string tag = isToday ? "02_当日交易" : "03_历史交易";
    auto files = DoExport("AM/AM2/TAM2A01", tag);

    std::vector<plugin::Transaction> result;
    std::string jp = (fs::path(DownloadDir()) / (tag + "_data.json")).string();
    std::ifstream jf(jp);
    if (jf) {
        std::stringstream ss; ss << jf.rdbuf();
        for (auto& r : TablesJsonToRows(ss.str())) {
            plugin::Transaction t;
            auto get = [&](std::initializer_list<const char*> keys) -> std::string {
                for (auto* k : keys) {
                    auto it = r.find(k);
                    if (it != r.end()) return it->second;
                }
                return "";
            };
            t.sn            = get({"流水号", "业务流水号", "凭证号"});
            t.transDate     = get({"交易日期", "日期", "发生日期", "交易时间", "时间"});
            t.amount        = get({"发生额", "金额", "交易金额"});
            t.direction     = get({"借贷标志", "方向", "收/付", "收付", "借贷"});
            t.balanceAfter  = get({"余额", "交易后余额"});
            t.opAccount     = get({"对方账号", "对方账户", "对手账号"});
            t.opAccountName = get({"对方户名", "对方名称", "对手户名"});
            t.remark        = get({"摘要", "用途", "备注", "附言"});
            t.currency      = get({"币种"});
            if (t.currency.empty()) t.currency = "CNY";
            t.account       = cfg_.account;
            result.push_back(std::move(t));
        }
    }

    spdlog::info("[BCB] GetHisTran: {} rows ({} - {}), {} files",
                 result.size(), range.begin, range.end, files.size());
    auto csvPath = io::WriteTransactionsCsv(
        "bcb_trans_" + range.begin + "_" + range.end + ".csv", result);
    spdlog::info("[BCB] Saved CSV: {}", csvPath);
    return result;
}

// ----------------------------------------------------------------------------
// DownBankSlip — 电子回单下载
//
// 典型流程:
//   1. 菜单: 电子回单 → 电子回单查询 / 电子回单打印
//   2. 填查询条件（日期 / 账号）
//   3. 查询 → 点"下载/打印/回单"按钮 → Network 拦截 PDF
// ----------------------------------------------------------------------------
std::vector<std::string> BCBPlugin::DownBankSlip(
    const plugin::DateRange& range) {

    std::vector<std::string> saved;
    if (!loggedIn_) { Login(); }
    if (!loggedIn_) return saved;

    // 演示版电子回单：勾选 → 生成回单 → 选方式一 → 确认 → 下载
    saved = DoExport("AM/AM3/NAM3001", "04_电子回单", /*receipt=*/true);

    spdlog::info("[BCB] DownBankSlip: {} file(s) saved", saved.size());
    return saved;
}

// ----------------------------------------------------------------------------
// 桩函数
// ----------------------------------------------------------------------------

std::string BCBPlugin::Payment(
    const std::string& amount, const std::string& nums,
    const std::string& paccno, const std::string& filename,
    const std::string& sn) {
    spdlog::warn("[BCB] Payment stub");
    return "stub";
}

std::string BCBPlugin::Approval(const std::string& no) {
    spdlog::warn("[BCB] Approval stub");
    return "stub";
}

std::string BCBPlugin::Query(const std::string& no) {
    spdlog::warn("[BCB] Query stub");
    return "stub";
}

} // namespace bcb
} // namespace newrpa
