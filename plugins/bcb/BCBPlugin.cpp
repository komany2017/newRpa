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
#include "newrpa/net/HttpDownloader.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

// ---- 查询版登录用的硬件盒子 / GDI+ PNG 解码 ----
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <newrpa/singleUsbbox/HidMouse.hpp>
#include "SkKeyBoard.h"
#include <objidl.h>
#include <gdiplus.h>

#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>
#include <iomanip>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <random>

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

std::string BCBPlugin::EvalStrQ(const std::string& js, int timeoutMs) {
    if (!rt_) return "";
    try {
        return rt_->EvalString(js, timeoutMs);
    } catch (const std::exception& e) {
        spdlog::warn("[BCB] EvalStr 超时/失败（跳过本轮）: {}", e.what());
        return "";
    } catch (...) {
        return "";
    }
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
    fs::path p = StateFilePath();
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
    std::multimap<std::string ,std::string> fileAccountMap;
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
            fileAccountMap.insert(std::make_pair(shortAcc, files.empty() ? "无文件" : files[0]));
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
        // 关键：必须传 fs::path（Unicode 版本），传 std::string 在 MinGW 下会按
        // ANSI 代码页解读 UTF-8 中文，导致生成的文件名乱码（如 余额→浣欓）。
        std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
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

    // 查询版与演示版是完全不同的流程（账号+密码+顶象滑块 vs 企业+证书+登录密码）
    if (url.find("version=query") != std::string::npos) {
        spdlog::info("[BCB] 查询版登录入口");
        return LoginQuery();
    }

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
    fs::path jp = fs::path(DownloadDir()) / "01_余额_data.json";
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
    fs::path jp = fs::path(DownloadDir()) / (tag + "_data.json");
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

// ----------------------------------------------------------------------------
// HttpDirect — 混合模式：CDP 登录建会话，之后纯 HTTP 直连下载数据
//
// 抓包结论（2026-11-21）：
//   演示版没有真正的会话型业务 API，数据文件全部是静态资源：
//     /cdn/demoYSB/AM/AM2/AM2DLZThatDay.dlz   当日交易 (GB2312 XML)
//     /cdn/demoYSB/AM/AM2/AM2DLZHistory.dlz   历史交易 (GB2312 XML)
//   这些文件免登录即可 GET（curl / WinHTTP 均验证 HTTP 200）。
//   因此"登录走 RPA、取数走 HTTP"的实现是：
//     1) 调 Login() 用 CDP 完成真实登录（保留浏览器会话、验证账号可用）
//     2) 从页面 URL 提取 origin，拼出静态资源地址
//     3) 用 HttpDownloader（WinHTTP/schannel）纯 HTTP 下载并落盘
//     4) 统计每条流水的笔数，返回摘要
// ----------------------------------------------------------------------------
std::string BCBPlugin::HttpDirect() {
    std::ostringstream summary;
    summary << "=== BCB HTTP 直连 (CDP 登录 + HTTP 取数) ===\n";

    // 1) CDP 登录建会话（用户要求：登录仍走 RPA）
    std::string login = Login();
    summary << "登录: " << login << "\n";
    if (login != "ok" && login != "already logged in") {
        summary << "登录失败，终止。\n";
        spdlog::error("[HTTP-DIRECT] Login failed: {}", login);
        return summary.str();
    }

    // 2) 从登录后的页面 URL 提取 origin（适配不同部署地址/端口）
    std::string href = EvalStr("location.href");
    std::string origin;
    size_t schemePos = href.find("://");
    if (schemePos != std::string::npos) {
        size_t hostStart = schemePos + 3;
        size_t slash = href.find('/', hostStart);
        origin = href.substr(0, slash == std::string::npos ? href.size() : slash);
    }
    if (origin.empty()) {
        origin = cfg_.loginUrl.empty()
            ? "https://cbank.bankofbeijing.com.cn:9081"
            : cfg_.loginUrl.substr(0, cfg_.loginUrl.find('/', 8));
    }
    summary << "Origin: " << origin << "\n";
    spdlog::info("[HTTP-DIRECT] origin={}", origin);

    // 3) 纯 HTTP 下载静态数据文件
    net::HttpDownloader dl;
    dl.SetIgnoreCertErrors(true);
    dl.SetTimeout(30000);

    struct Task { const char* name; const char* rel; const char* tag; };
    std::vector<Task> tasks = {
        {"当日交易", "/cdn/demoYSB/AM/AM2/AM2DLZThatDay.dlz",   "HTTP_当日交易"},
        {"历史交易", "/cdn/demoYSB/AM/AM2/AM2DLZHistory.dlz",   "HTTP_历史交易"}
    };

    std::string dir = DownloadDir();
    std::error_code ec;
    fs::create_directories(dir, ec);

    int success = 0;
    // 可读时间戳 YYYYMMDD_HHMMSS
    char tbuf[32] = {0};
    {
        std::time_t now = std::time(nullptr);
        std::tm tmNow{};
        localtime_s(&tmNow, &now);
        std::strftime(tbuf, sizeof(tbuf), "%Y%m%d_%H%M%S", &tmNow);
    }
    std::string t0 = tbuf;
    for (const auto& t : tasks) {
        std::string url = origin + t.rel;
        auto r = dl.Get(url);
        if (!r.ok()) {
            summary << t.name << ": 失败 — " << (r.error.empty() ? ("HTTP " + std::to_string(r.status)) : r.error) << "\n";
            spdlog::error("[HTTP-DIRECT] {} failed: {} status={}", t.name, r.error, r.status);
            continue;
        }
        // 落盘（GB2312 XML，原样保存字节）
        std::string fname = std::string(t.tag) + "_" + t0 + ".dlz";
        fs::path fp = fs::path(dir) / fname;
        std::ofstream ofs(fp, std::ios::binary);
        ofs.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
        ofs.close();

        // 统计 <Record> 笔数
        int records = 0;
        size_t p = 0;
        while ((p = r.body.find("<Record", p)) != std::string::npos) { ++records; p += 7; }
        summary << t.name << ": HTTP " << r.status << ", " << r.body.size()
                << " 字节, " << records << " 笔 → " << fp.filename().string() << "\n";
        spdlog::info("[HTTP-DIRECT] {} OK {} bytes {} records -> {}",
                     t.name, r.body.size(), records, fp.filename().string());
        ++success;
    }

    summary << "完成: " << success << "/" << tasks.size() << " 成功\n";
    summary << "说明: 演示版数据为静态资源，免登录可读；登录步骤用 CDP 完成以验证会话。\n";
    return summary.str();
}

} // namespace bcb
} // namespace newrpa

// ============================================================================
//          查 询 版 专 用 流 程（Python boj_login_complete.py 已验证）
//  与演示版完全不同：账号+密码+顶象滑块；密码走 pgeCtrl 安全控件（硬件键盘）；
//  滑块 = 顶象 dx_captcha（canvas 400x200，显示宽度可变 KX），盒子 HID 鼠标拖动。
// ============================================================================
namespace newrpa {
namespace bcb {
namespace {

struct GapCand { int x, y; double score; };

// 找最大的可见 Chrome 窗口（与 KLB 同款）
HWND FindBcbChromeWindow() {
    struct Ctx { HWND best; int len; } ctx{nullptr, 0};
    EnumWindows([](HWND h, LPARAM lp)->BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t cls[256] = {0};
        GetClassNameW(h, cls, 256);
        if (wcsstr(cls, L"Chrome_WidgetWin_1") == nullptr) return TRUE;
        RECT r{};
        if (!GetWindowRect(h, &r)) return TRUE;
        if (r.right - r.left < 600 || r.bottom - r.top < 400) return TRUE;
        int n = GetWindowTextLengthW(h);
        if (n > c->len) { c->len = n; c->best = h; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

bool FocusBcbChromeWindow() {
    HWND h = FindBcbChromeWindow();
    if (!h) return false;
    if (GetForegroundWindow() == h) return true;
    ShowWindow(h, SW_RESTORE);
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD myTid = GetCurrentThreadId();
    bool attached = false;
    if (fgTid && fgTid != myTid) attached = (AttachThreadInput(myTid, fgTid, TRUE) != 0);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    Sleep(200);
    return GetForegroundWindow() == h;
}

// GDI+ 解码 PNG -> 灰度（零第三方依赖，系统自带）
bool DecodePngGray(const std::vector<uint8_t>& png, int& W, int& H,
                   std::vector<uint8_t>& gray) {
    W = H = 0; gray.clear();
    if (png.size() < 8) return false;
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &si, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    do {
        HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (!hg) break;
        void* p = ::GlobalLock(hg);
        if (!p) { ::GlobalFree(hg); break; }
        memcpy(p, png.data(), png.size());
        ::GlobalUnlock(hg);
        IStream* st = nullptr;
        if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { ::GlobalFree(hg); break; }
        Gdiplus::GpBitmap* bmp = nullptr;
        UINT w = 0, h = 0;
        if (Gdiplus::DllExports::GdipCreateBitmapFromStream(st, &bmp) == Gdiplus::Ok && bmp &&
            Gdiplus::DllExports::GdipGetImageWidth(bmp, &w) == Gdiplus::Ok &&
            Gdiplus::DllExports::GdipGetImageHeight(bmp, &h) == Gdiplus::Ok && w > 50 && h > 50) {
            Gdiplus::BitmapData bd{};
            Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
            if (Gdiplus::DllExports::GdipBitmapLockBits(bmp, &rect, Gdiplus::ImageLockModeRead,
                                   PixelFormat32bppARGB, &bd) == Gdiplus::Ok && bd.Scan0) {
                gray.resize((size_t)w * h);
                const uint8_t* base = (const uint8_t*)bd.Scan0;
                INT stride = bd.Stride;
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* row = base + (size_t)y * stride;
                    for (UINT x = 0; x < w; ++x) {
                        const uint8_t* px = row + (size_t)x * 4;   // B G R A
                        gray[(size_t)y * w + x] =
                            (uint8_t)((px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8);
                    }
                }
                Gdiplus::DllExports::GdipBitmapUnlockBits(bmp, &bd);
                W = (int)w; H = (int)h; ok = true;
            }
            Gdiplus::DllExports::GdipDisposeImage(bmp);
        }
        st->Release();
    } while (false);
    Gdiplus::GdiplusShutdown(tok);
    return ok;
}

// 缺口候选：内环暗 vs 外环亮差值（Python candidates() 同款）
std::vector<GapCand> GapCandidates(const std::vector<uint8_t>& gray, int W, int H) {
    const int R = 26;
    const int y_lo = 55, y_hi = std::min(H - 45, 150);
    const int x_lo = 80, x_hi = W - 90;
    const int step = 4;
    std::vector<GapCand> best;
    if (y_hi <= y_lo || x_hi <= x_lo) return best;
    auto g = [&](int x, int y)->int {
        if (x < 0 || y < 0 || x >= W || y >= H) return 128;
        return gray[(size_t)y * W + x];
    };
    for (int cy = y_lo; cy < y_hi; cy += step) {
        for (int cx = x_lo; cx < x_hi; cx += step) {
            double isum = 0; int icnt = 0;
            for (int y = cy - R + 5; y <= cy + R - 4; y += 4)
                for (int x = cx - R + 5; x <= cx + R - 4; x += 4)
                    { isum += g(x, y); ++icnt; }
            double rsum = 0; int rcnt = 0;
            for (int y = cy - R - 3; y <= cy + R + 3; y += 4)
                for (int x = cx - R - 3; x <= cx + R + 3; x += 4) {
                    if (std::abs(x - cx) > R - 2 || std::abs(y - cy) > R - 2)
                        { rsum += g(x, y); ++rcnt; }
                }
            if (!icnt || !rcnt) continue;
            best.push_back({cx, cy, rsum / rcnt - isum / icnt});
        }
    }
    std::sort(best.begin(), best.end(),
              [](const GapCand& a, const GapCand& b) { return a.score > b.score; });
    std::vector<GapCand> picks;
    for (auto& b : best) {
        bool dup = false;
        for (auto& q : picks) if (std::abs(b.x - q.x) <= 38) { dup = true; break; }
        if (!dup) picks.push_back(b);
        if (picks.size() >= 6) break;
    }
    std::vector<GapCand> out;
    for (auto& p : picks)
        if (p.x >= x_lo && p.x <= x_hi) out.push_back(p);
    std::sort(out.begin(), out.end(),
              [](const GapCand& a, const GapCand& b) { return a.x < b.x; });
    if (out.size() > 4) out.resize(4);
    return out;
}

} // namespace

// ----------------------------------------------------------------------------
// 盒子键盘拟人输入（IME 隔离 + 字间随机停顿 + 长顿）
// ----------------------------------------------------------------------------
bool BCBPlugin::HardwareType(const std::string& text, int delayMs) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(wl > 1 ? wl - 1 : 0, L'\0');
    if (wl > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], wl);

    HWND h = FindBcbChromeWindow();
    if (!h) { spdlog::error("[BCB] 没找到 Chrome 窗口"); return false; }
    if (GetForegroundWindow() != h) {
        ShowWindow(h, SW_RESTORE);
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD myTid = GetCurrentThreadId();
        bool attached = false;
        if (fgTid && fgTid != myTid) attached = (AttachThreadInput(myTid, fgTid, TRUE) != 0);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        if (attached) AttachThreadInput(myTid, fgTid, FALSE);
        Sleep(200);
    }

    SkKeyBoard kb;
    kb.setImeIsolation(true);
    kb.setKeyDelay(delayMs);
    auto st = SkKeyBoard::detect();
    spdlog::info("[BCB] HW keyboard: usable={} detail={}", st.usable, st.detail);
    if (!kb.open()) {
        spdlog::error("[BCB] SkKeyBoard open failed: {}", kb.lastError());
        return false;
    }
    // 拟人：字间随机停顿，每 2~4 字一次"想一想"长顿
    // 注意：不能加 static —— lambda 捕获非自动存储期变量是未定义行为（C++20 起报错）
    std::mt19937 rng{
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
    auto rnd = [&rng](int lo, int hi) {
        return lo + (int)(rng() % (unsigned)(hi - lo + 1));
    };
    int sinceLong = 0, nextLong = rnd(2, 4);
    for (size_t i = 0; i < w.size(); ++i) {
        if (!kb.type(std::wstring(1, w[i]), delayMs)) {
            spdlog::error("[BCB] HW type failed: {}", kb.lastError());
            return false;
        }
        Sleep(rnd(40, 160));
        if (++sinceLong >= nextLong && i + 1 < w.size()) {
            Sleep(rnd(200, 600));
            sinceLong = 0; nextLong = rnd(2, 4);
        }
    }
    spdlog::info("[BCB] HW type ok, chars={}", w.size());
    return true;
}

// ----------------------------------------------------------------------------
// origin / 增益标定
// ----------------------------------------------------------------------------
bool BCBPlugin::CalibrateBoxOrigin(HidMouse& m) {
    EvalStr("window.__mp=[];document.addEventListener('mousemove',"
            "function(e){window.__mp=[e.clientX,e.clientY,e.buttons]},true)");
    double sx = 0, sy = 0; int cnt = 0;
    const double probes[][2] = {{900, 600}, {1100, 700}};
    for (auto& p : probes) {
        m.moveTo((int)p[0], (int)p[1]);
        Sleep(300);
        std::string s = EvalStr("JSON.stringify(window.__mp||[])");
        try {
            auto j = nlohmann::json::parse(s);
            if (j.is_array() && j.size() >= 2) {
                double cx = j[0].get<double>(), cy = j[1].get<double>();
                if (cx != 0 || cy != 0) {
                    sx += p[0] - cx * m.scale();
                    sy += p[1] - cy * m.scale();
                    ++cnt;
                }
            }
        } catch (...) {}
    }
    if (!cnt) { spdlog::error("[BCB] origin 标定失败"); return false; }
    m.setOrigin(sx / cnt, sy / cnt);
    spdlog::info("[BCB] box origin=({:.1f},{:.1f})", m.originX(), m.originY());
    return true;
}

double BCBPlugin::CalibrateGain(HidMouse& m) {
    m.moveRel(5, 0); Sleep(150);          // 预热丢弃首步非线性
    std::vector<double> gs;
    for (int i = 0; i < 3; ++i) {
        int x0, y0; HidMouse::currentPos(x0, y0); Sleep(60);
        m.moveRel(20, 0); Sleep(160);
        int x1, y1; HidMouse::currentPos(x1, y1);
        gs.push_back(std::abs(x1 - x0) / 20.0);
    }
    std::sort(gs.begin(), gs.end());
    double g = (gs.size() == 3) ? gs[1] : (gs.empty() ? 2.47 : gs.back());
    if (g <= 0.5) g = 2.47;
    spdlog::info("[BCB] rel gain={:.3f}", g);
    return g;
}

// ----------------------------------------------------------------------------
// 最小 jerk + OU 手抖 + 8ms 时间轴回放（每 80ms 实测位置校正）
// ----------------------------------------------------------------------------
bool BCBPlugin::TrackRel(HidMouse& m, double dxCss, double dyCss, double T,
                         double gain, const char* label) {
    const double dt = 0.008;
    const int N = std::max(10, (int)(T / dt));
    const double scale = m.scale();
    int maxn = std::max(6, std::min(45,
        (int)(std::abs(dxCss) * scale / T / dt / gain) + 2));
    int maxmy = std::max(2, std::min(30,
        (int)(std::abs(dyCss) * scale / T / dt / gain) + 2));
    int x0, y0; HidMouse::currentPos(x0, y0);
    const double tx = x0 + dxCss * scale, ty = y0 + dyCss * scale;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::normal_distribution<double> gauss(0.0, 1.0);
    double nx = 0, ny = 0, doneX = 0, doneY = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= N; ++i) {
        double tau = (double)i / N;
        double s = tau * tau * tau * (10 - 15 * tau + 6 * tau * tau);
        nx += -12 * nx * dt + 1.45 * std::sqrt(dt) * gauss(rng);
        ny += -12 * ny * dt + 2.10 * std::sqrt(dt) * gauss(rng);
        double wantX = x0 + (dxCss * s + nx) * scale;
        double wantY = y0 + (dyCss * s + ny) * scale;
        int n = (int)std::lround((wantX - (x0 + doneX)) / gain);
        int my = (int)std::lround((wantY - (y0 + doneY)) / gain);
        n = std::max(-maxn, std::min(maxn, n));
        my = std::max(-maxmy, std::min(maxmy, my));
        if (n || my) {
            m.moveRel(n, my);
            m.drain();   // 邮箱必须读走，否则固件重复应用上一帧
            doneX += n * gain; doneY += my * gain;
        }
        if (i % 10 == 0) {
            int px, py; HidMouse::currentPos(px, py);
            doneX = px - x0; doneY = py - y0;
        }
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count() < i * dt) {
            Sleep(0);
        }
    }
    int px, py; HidMouse::currentPos(px, py);
    double ex = (tx - px) / scale, ey = (ty - py) / scale;
    spdlog::info("[BCB] track[{}] D=({:.1f},{:.1f})css T={:.2f}s residual=({:.1f},{:.1f})",
                 label, dxCss, dyCss, T, ex, ey);
    return std::abs(ex) < 12 && std::abs(ey) < 12;
}

// ----------------------------------------------------------------------------
// 顶象滑块一轮自动尝试。返回空串 = 通过；否则失败原因
// ----------------------------------------------------------------------------
std::string BCBPlugin::SliderAttempt(HidMouse& m) {
    auto readSubMl = [&] {
        return EvalNum(
            "(function(){var s=document.querySelector('.dx_captcha_basic_sub-slider');"
            "return s?(parseFloat(getComputedStyle(s).marginLeft)||0):-1})()");
    };
    auto readSliderMl = [&] {
        return EvalNum(
            "(function(){var s=document.querySelector('.dx_captcha_basic_slider');"
            "return s?(parseFloat(getComputedStyle(s).marginLeft)||0):-1})()");
    };

    // 1 定位滑块按钮
    std::string s = EvalStr(
        "(function(){var r=document.querySelector('.dx_captcha_basic_slider');"
        "if(!r)return '{}';var b=r.getBoundingClientRect();"
        "return JSON.stringify({x:b.x,y:b.y,w:b.width,h:b.height});})()");
    if (s.empty() || s == "{}") return "slider-not-found";
    double sx = 0, sy = 0;
    try {
        auto j = nlohmann::json::parse(s);
        sx = j["x"].get<double>() + j["w"].get<double>() / 2;
        sy = j["y"].get<double>() + j["h"].get<double>() / 2;
    } catch (...) { return "slider-parse-failed"; }

    if (!CalibrateBoxOrigin(m)) return "origin-failed";
    double gain = CalibrateGain(m);

    // 2 自然移入（运动学轨迹），再闭环精调、按下
    int cx0, cy0; HidMouse::currentPos(cx0, cy0);
    double curX = (cx0 - m.originX()) / m.scale();
    double curY = (cy0 - m.originY()) / m.scale();
    double dx = sx - curX, dy = sy - curY;
    double Din = std::sqrt(dx * dx + dy * dy);
    TrackRel(m, dx, dy, std::max(0.35, std::min(0.75, 0.22 + 0.004 * Din)), gain, "approach");
    m.releaseAll(); Sleep(150);
    m.moveTo((int)m.cssToPhysX(sx), (int)m.cssToPhysY(sy)); Sleep(300);
    m.leftDown(); Sleep(200);

    // 3 起手 55px（触发 canvas 显示）
    TrackRel(m, 55.0, 0.0, 0.46, gain, "takeoff");
    Sleep(50);

    // 4 读几何（触发前父容器 display:none，rect 全 0，必须在起手后读）
    std::string geo = EvalStr(
        "(function(){"
        "var bg=document.querySelector('.dx_captcha_basic_bg');"
        "var cv=document.querySelector('.dx_captcha_basic_bg canvas');"
        "var sub=document.querySelector('.dx_captcha_basic_sub-slider');"
        "if(!bg||!cv||!sub)return '{}';"
        "var cr=cv.getBoundingClientRect(),sr=sub.getBoundingClientRect();"
        "return JSON.stringify({cvW:cv.width,cvH:cv.height,"
        "cvCssW:cr.width,cvCssH:cr.height,"
        "subLeft:parseFloat(getComputedStyle(sub).left)||0,"
        "subW:sr.width,"
        "subMl:parseFloat(getComputedStyle(sub).marginLeft)||0});})()");
    double cvW = 0, cvCssW = 0, subLeft = 0, subW = 0, subMl0 = 0;
    try {
        auto j = nlohmann::json::parse(geo);
        cvW = j["cvW"].get<double>();      cvCssW = j["cvCssW"].get<double>();
        subLeft = j["subLeft"].get<double>(); subW = j["subW"].get<double>();
        subMl0 = j["subMl"].get<double>();
    } catch (...) { m.leftUp(); return "geo-failed"; }
    if (cvW < 100 || cvCssW < 50) { m.leftUp(); return "puzzle-not-shown"; }
    double KX = cvCssW / cvW;
    double startCenter = subLeft + subW / 2;
    spdlog::info("[BCB] KX={:.4f} start_center={:.1f} subMl0={:.1f}", KX, startCenter, subMl0);

    // 5 抓 canvas -> 缺口候选
    std::string dataUrl = EvalStr(
        "(function(){var c=document.querySelector('.dx_captcha_basic_bg canvas');"
        "return (c && c.width>100) ? c.toDataURL('image/png') : '';})()");
    const std::string kPrefix = "data:image/png;base64,";
    if (dataUrl.size() <= kPrefix.size()) { m.leftUp(); return "canvas-capture-failed"; }
    auto png = util::Base64Decode(dataUrl.substr(kPrefix.size()));
    int W = 0, H = 0; std::vector<uint8_t> gray;
    if (!DecodePngGray(png, W, H, gray)) { m.leftUp(); return "png-decode-failed"; }
    auto cands = GapCandidates(gray, W, H);
    if (cands.empty()) { m.leftUp(); return "no-gap-candidate"; }
    GapCand best = cands[0];
    for (auto& c : cands) if (c.score > best.score) best = c;
    spdlog::info("[BCB] gap=({},{}) score={:.1f} cands={}", best.x, best.y, best.score, cands.size());

    // 6 speed 标定（顶象随机 speed：subMl = speed x sliderMl）
    double slMl = readSliderMl();
    double speed = 1.0;
    if (slMl > 6 && subMl0 > 2) speed = subMl0 / slMl;
    spdlog::info("[BCB] speed_est={:.3f}", speed);

    double targetSub = std::max(0.0, best.x * KX - startCenter);
    double targetCss = targetSub / speed + 35.0 - 30.0;
    spdlog::info("[BCB] target_sub={:.1f} target_css={:.1f}", targetSub, targetCss);

    // 7 主拖（按住期间不查询，中途不留死尸段）
    int cx1, cy1; HidMouse::currentPos(cx1, cy1);
    double curCss = (cx1 - m.originX()) / m.scale() - sx;
    double Dm = std::abs(targetCss - curCss);
    double Tm = std::max(0.32, std::min(1.15, 0.30 + 0.004 * Dm));
    TrackRel(m, targetCss - curCss, 0.0, Tm, gain, "main");
    Sleep(80);

    // 8 trim（补偿 speed 估计误差）
    double err = targetSub - readSubMl();
    if (std::abs(err) > 2.5) {
        spdlog::info("[BCB] trim err={:.1f}", err);
        TrackRel(m, err / speed, 0.0, 0.18, gain, "trim");
        Sleep(80);
    }
    spdlog::info("[BCB] final subMl={:.1f} target={:.1f} err={:.1f}",
                 readSubMl(), targetSub, targetSub - readSubMl());
    // 到位后按住停顿 2s 再松手（真人拖到位会稍作停顿，直接秒松易被判机器）
    spdlog::info("[BCB] 到位停顿 2000ms 后释放");
    Sleep(2000);
    m.leftUp(); Sleep(2500);

    // 9 判定：弹窗 = 失败；拼图收起且无弹窗 = 通过
    if (!PopupText().empty()) return "popup: " + PopupText();
    std::string st = EvalStr(
        "(function(){var c=document.querySelector('.dx_captcha_basic_content');"
        "var cap=document.getElementById('captcha');"
        "return JSON.stringify({disp:c?getComputedStyle(c).display:'gone',"
        "txt:(document.body.innerText||'').indexOf('验证成功')>=0,"
        "cap:cap?cap.className:''});})()");
    try {
        auto j = nlohmann::json::parse(st);
        std::string disp = j.value("disp", "gone");
        bool txt = j.value("txt", false);
        std::string cap = j.value("cap", "");
        if (disp == "none" || txt || cap.find("success") != std::string::npos) return "";
    } catch (...) {}
    return "puzzle-still-open";
}

// ----------------------------------------------------------------------------
// 人工等待秒数：默认 300s（5 分钟）
//   可用环境变量 BCB_MANUAL_WAIT_S 覆盖（>=30 才生效），例如在 launch.json
//   的 environment 里加 { "name": "BCB_MANUAL_WAIT_S", "value": "600" }
// ----------------------------------------------------------------------------
static int ManualWaitSeconds() {
    const char* env = std::getenv("BCB_MANUAL_WAIT_S");
    if (env && *env) {
        int v = std::atoi(env);
        if (v >= 30) return v;
    }
    return 15000;
}

// ----------------------------------------------------------------------------
// 鸣笛呼唤人工：不碰 HID，等真人拖滑块（最长 ManualWaitSeconds()），成功后自动点登录
// ----------------------------------------------------------------------------
std::string BCBPlugin::ManualTakeover() {
    const int waitS = ManualWaitSeconds();
    spdlog::warn("[BCB] >>>>> 需要人工处理：请在浏览器里手动把滑块拖到缺口（拖完不用点登录），最长等 {} 秒 <<<<<", waitS);
    for (int i = 0; i < 6; ++i) { Beep(i % 2 == 0 ? 980 : 620, 300); Sleep(50); }
    const std::string kStateJs =
        "(function(){"
        "var c=document.querySelector('.dx_captcha_basic_content');"
        "var cap=document.getElementById('captcha');"
        "var stb=document.querySelector('.dx_captcha_basic_state-box');"
        "var pop=[];document.querySelectorAll('.el-message-box,.el-dialog').forEach("
        "function(e){if(e.getBoundingClientRect().width>0)pop.push((e.innerText||'').trim().slice(0,60))});"
        "return JSON.stringify({disp:c?getComputedStyle(c).display:'none',"
        "capCls:cap?cap.className:'',stDisp:stb?getComputedStyle(stb).display:'none',"
        "txt:(document.body.innerText||'').indexOf('验证成功')>=0,"
        "url:location.href,pop:pop});})()";

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&t0]() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count();
    };

    int stage = 0;          // 0=还没看到按下  1=已按下拖动中
    int submits = 0;        // 已点「登录」次数
    int captchaLoops = 0;   // 弹窗已确认次数（含验证码类/凭证类）
    bool needRefill = false;// 点掉弹窗后密码框常被清空，再次提交前要重填
    bool submitted = false; // 本轮是否已点过「登录」
    double submitAt = 0.0;
    // 登录失败允许次数：提交 3 次都失败才叫人
    const int kMaxSubmits = 3;

    while (elapsed() < static_cast<double>(waitS)) {
        std::string s = EvalStrQ(kStateJs, 12000);
        if (s.empty()) { Sleep(400); continue; }
        nlohmann::json j;
        try { j = nlohmann::json::parse(s); } catch (...) { Sleep(400); continue; }

        // 1) 提示弹窗：先点掉「确定」，再判断还能不能再试
        //    规则：登录最多提交 3 次，3 次都失败才叫人。
        //    点确定后密码框会被清空，所以继续之前必须重填账号密码。
        if (j.contains("pop") && !j["pop"].empty()) {
            std::string popText;
            try {
                for (const auto& e : j["pop"]) popText += e.get<std::string>() + " | ";
            } catch (...) { popText = j["pop"].dump(); }

            bool credIssue = popText.find("密码") != std::string::npos ||
                             popText.find("账号") != std::string::npos ||
                             popText.find("标识号") != std::string::npos ||
                             popText.find("锁定") != std::string::npos ||
                             popText.find("冻结") != std::string::npos;

            ClickPopupOk();     // 先把框点掉，别挡住页面
            Sleep(800);
            ++captchaLoops;

            if (submits >= kMaxSubmits || captchaLoops > 6) {
                for (int i = 0; i < 10; ++i) { Beep(1500, 260); Sleep(80); }
                spdlog::error("[BCB] >>>>> 登录已失败 {} 次（最后一次提示「{}」）：需要人工介入！"
                              "自动流程已停止 <<<<<", submits, popText);
                return "need-manual: " + popText;
            }

            // 点确定后密码框已被清空、滑块状态也乱了 —— 原地重填会拿旧状态误判，
            // 直接重新加载页面最干净，再重填账号密码等人工拖。
            spdlog::warn("[BCB] 第 {} 次登录失败：{}{}（已点确定，重新加载并重填，还剩 {} 次机会）",
                         submits, popText, credIssue ? " [凭证类]" : " [验证码类]",
                         kMaxSubmits - submits);
            try { session_->Call("Page.reload", {{"ignoreCache", true}}, 10000); } catch (...) {}
            Sleep(6000);
            DismissCertPopup();   // reload 后证书提示可能再弹
            bool refilled = false;
            if (WaitForElement("input[placeholder*=\"账号\"]", 12000)) {
                refilled = FillQueryAccountPwd();
                if (!refilled) { Sleep(1500); refilled = FillQueryAccountPwd(); }
            }
            if (!refilled) {
                for (int i = 0; i < 10; ++i) { Beep(1500, 260); Sleep(80); }
                spdlog::error("[BCB] >>>>> 重载后重填账号密码失败：需要人工介入 <<<<<");
                return "need-manual: refill-failed";
            }
            stage = 0;           // 滑块已复位，等人工再拖
            submitted = false;
            Beep(880, 200);
            continue;
        }

        // 2) 离开登录页 = 成功
        std::string url = j.value("url", "");
        if (url.find("/login") == std::string::npos) {
            Beep(1200, 160); Beep(1200, 160);
            return "";
        }

        // 3) 滑块是否通过
        bool txt = j.value("txt", false);
        std::string cap = j.value("capCls", "");
        std::string disp = j.value("disp", "");
        bool sliderOk = txt || cap.find("success") != std::string::npos;
        if (!sliderOk && stage == 0 && disp == "block") {
            stage = 1; spdlog::info("[BCB] 检测到已按下滑块，继续拖...");
        } else if (!sliderOk && stage == 1 && disp == "none") {
            sliderOk = true;   // 拖完松手、拼图收起
        }
        if (sliderOk && !submitted) {
            if (submits >= kMaxSubmits) {
                for (int i = 0; i < 10; ++i) { Beep(1500, 260); Sleep(80); }
                spdlog::error("[BCB] >>>>> 已提交 {} 次仍未进入：需要人工介入 <<<<<", submits);
                return "need-manual: submit-limit";
            }
            if (needRefill) {
                spdlog::info("[BCB] 点确定后密码框可能为空，重新填账号密码再提交...");
                if (!FillQueryAccountPwd()) {
                    for (int i = 0; i < 8; ++i) { Beep(1300, 240); Sleep(80); }
                    spdlog::error("[BCB] >>>>> 重填账号密码失败，需要人工介入 <<<<<");
                    return "need-manual: refill-failed";
                }
                needRefill = false;
                Sleep(500);
            }
            // 提交前再核一次密码位数，防止空密码白提交一次
            std::string plen = EvalStrQ(
                "(function(){try{return ''+pgeCtrl.pwdLength()}catch(e){return 'E'}})()");
            if (plen != "6") {
                spdlog::warn("[BCB] 提交前密码位数={}，重填一次", plen);
                if (!FillQueryAccountPwd()) {
                    spdlog::error("[BCB] >>>>> 提交前重填密码失败：需要人工介入 <<<<<");
                    return "need-manual: refill-failed";
                }
            }
            Beep(1200, 160); Beep(1200, 160);
            // 松手后等 1.5~2.7s 再点登录：真人拖完会看一眼再点，
            // 太快提交时顶象常回「请使用图形验证码」（验证状态还没同步）
            int settle = 1500 + static_cast<int>(GetTickCount64() % 1200);
            spdlog::info("[BCB] 松手后停顿 {}ms 再提交", settle);
            Sleep(settle);
            spdlog::info("[BCB] 人工滑块通过，自动点登录（第 {} 次）...", submits + 1);
            try { ClickButtonByText("登录"); } catch (...) { 
                spdlog::warn("[BCB] 点登录超时，下一轮重试");
                Sleep(800); continue;
            }
            submitted = true;
            ++submits;
            submitAt = elapsed();
            Sleep(1500);
            continue;
        }
        if (submitted && elapsed() - submitAt > 30.0) return "submit-no-redirect";
        Sleep(600);
    }
    if (submitted) return "submit-no-redirect";
    return "manual-timeout";
}

// ----------------------------------------------------------------------------
// 查询版：填账号 + 盒子键盘输密码
// ----------------------------------------------------------------------------
bool BCBPlugin::FillQueryAccountPwd() {
    // 硬件键盘是真实 HID 设备，按键事件发给系统「前台窗口」——
    // Chrome 不在前台时字符会打到别人家（pwdLength=0）。每次填表前强制前置。
    if (!FocusBcbChromeWindow()) {
        spdlog::warn("[BCB] Chrome 窗口前置失败，硬件键盘可能落到其他窗口");
    }
    std::string acc = EvalStrQ(
        "(function(){var e=document.querySelector('input[placeholder*=\"账号\"]');"
        "if(!e)return 'no-input';"
        "e.value='';e.dispatchEvent(new Event('input',{bubbles:true}));"
        "e.focus();return 'ok';})()");
    if (acc != "ok") { spdlog::error("[BCB] 账号框没找到: {}", acc); return false; }
    Sleep(300);
    std::string accno = cfg_.account.empty() ? "20000037755514660017646" : cfg_.account;
    try {
        session_->Call("Input.insertText", {{"text", accno}}, 5000);
    } catch (...) { return false; }
    Sleep(500);
    // 等密码控件就绪：点掉弹窗/重新加载后 #inputId2 可能还没渲染出来
    if (!WaitForElement("#inputId2", 8000)) {
        spdlog::error("[BCB] 密码框 #inputId2 未出现（被替换成控件占位？）");
        return false;
    }
    std::string pwd = cfg_.password.empty() ? "123456" : cfg_.password;
    // 焦点可能被弹窗/上次点击抢走，打不进就再点一次密码框重来
    for (int tryNo = 1; tryNo <= 2; ++tryNo) {
        EvalStrQ(
            "(function(){var e=document.getElementById('inputId2');"
            "if(!e)return 'no';e.click();e.focus();return 'ok';})()");
        Sleep(900);
        if (!HardwareType(pwd, 130)) return false;
        Sleep(1000);
        std::string plen = EvalStrQ(
            "(function(){try{return ''+pgeCtrl.pwdLength()}catch(e){return 'E'}})()");
        spdlog::info("[BCB] pwdLength={}（第 {} 次输入）", plen, tryNo);
        if (plen == "6") return true;
        // 位数不对（残留/漏键）：清空重打
        EvalStrQ(
            "(function(){try{if(pgeCtrl.clearAll)pgeCtrl.clearAll();}catch(e){}"
            "var e=document.getElementById('inputId2');if(e){e.value='';}return '1';})()");
        Sleep(600);
    }
    return false;
}

// ----------------------------------------------------------------------------
// 页面可见弹窗文本
// ----------------------------------------------------------------------------
std::string BCBPlugin::PopupText() {
    std::string s = EvalStrQ(
        "(function(){var o=[];"
        "document.querySelectorAll('.el-message-box,.el-dialog').forEach("
        "function(e){if(e.getBoundingClientRect().width>0)"
        "o.push((e.innerText||'').trim().slice(0,60))});"
        "return JSON.stringify(o);})()");
    if (s.empty() || s == "[]") return "";
    return s;
}

// ----------------------------------------------------------------------------
// 点掉 Element UI 提示弹窗的「确定」
// 只认 .el-message-box / .el-dialog（顶象提示、业务提示都在这里）。
// 证书弹窗 #xtxAlert 不在此列 —— 它的「确定」会下载 client_Setup.exe，
// 仍由 DismissCertPopup() 直接移除 DOM，绝不点击。
// ----------------------------------------------------------------------------
bool BCBPlugin::ClickPopupOk() {
    std::string r = EvalStrQ(
        "(function(){"
        "var bs=[].slice.call(document.querySelectorAll('.el-message-box,.el-dialog'))"
        ".filter(function(e){return e.getBoundingClientRect().width>0;});"
        "for(var i=0;i<bs.length;i++){var b=bs[i];"
        "var btn=b.querySelector('.el-message-box__btns .el-button--primary,.el-dialog__footer .el-button--primary');"
        "if(!btn){var all=b.querySelectorAll('button');"
        "for(var k=0;k<all.length;k++){var t=(all[k].innerText||'').trim();"
        "if(/^(确定|确认|知道了|好的|OK)$/i.test(t)){btn=all[k];break;}}}"
        "if(btn){try{btn.click();}catch(e){return 'click-err';}"
        "return 'ok:'+(b.innerText||'').trim().replace(/\\s+/g,' ').slice(0,40);}"
        "}return 'none';})()");
    if (r.rfind("ok:", 0) == 0) {
        spdlog::info("[BCB] 已点弹窗「确定」: {}", r.substr(3));
        return true;
    }
    if (!r.empty() && r != "none") spdlog::warn("[BCB] 点弹窗确定失败: {}", r);
    return false;
}

// ----------------------------------------------------------------------------
// 查询版登录主流程：3 次自动 -> 失败/降级即鸣笛呼唤人工 -> 人拖完自动提交
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// 关掉「网银证书运行环境已升级」弹窗
// 页面源码（app~745a3043）逻辑：
//   window.sessionStorage.getItem("certDownload") || (document.body.appendChild(n) ...)
// 即只要 certDownload 有值就不再弹；已弹出时 #xtxAlert 可整块 remove。
// 弹窗点「确定」会跳转下载 client_Setup.exe —— 绝不能点，只移除。
// ----------------------------------------------------------------------------
int BCBPlugin::DismissCertPopup() {
    std::string n = EvalStrQ(
        "(function(){"
        "try{if(!window.sessionStorage.getItem('certDownload'))"
        "window.sessionStorage.setItem('certDownload','1');}catch(e){}"
        "var c=0;"
        "var a=document.getElementById('xtxAlert');"
        "if(a&&a.parentNode){a.parentNode.removeChild(a);c++;}"
        "return ''+c;})()");
    int cnt = 0;
    try { cnt = std::stoi(n); } catch (...) { cnt = 0; }
    if (cnt > 0) spdlog::info("[BCB] 已移除证书环境提示弹窗 x{}", cnt);
    return cnt;
}

std::string BCBPlugin::LoginQuery() {
    if (!session_ || !browser_) return "error: session not ready";
    if (loggedIn_) return "already logged in";

    const std::string kQueryUrl =
        "https://cbank.bankofbeijing.com.cn/login?redirect=%2F&version=query";
    std::string url = cfg_.loginUrl.empty() ? kQueryUrl : cfg_.loginUrl;
    if (url.find("version=query") == std::string::npos) url = kQueryUrl;

    HidMouse m;
    if (!m.open()) {
        spdlog::error("[BCB] 盒子鼠标打不开: {}", m.lastError());
        return "error: hid-mouse-open-failed";
    }

    // Chrome 156 的本地网络访问（LNA）权限：登录页会去连 wss://127.0.0.1:7802/27724/57658
    // 探测安全控件服务；未授权时浏览器右上角弹「访问此设备上的其他应用和服务」。
    // 这是 RPA 专用实例，直接预授权，避免每次都被弹窗打断。
    try {
        session_->Call("Browser.grantPermissions",
                       {{"permissions", {"localNetworkAccess"}}}, 5000);
        spdlog::info("[BCB] 已预授权 localNetworkAccess");
    } catch (...) {
        spdlog::warn("[BCB] 预授权 localNetworkAccess 失败（不影响后续流程）");
    }

    std::string curUrl = EvalStr("location.href");
    if (curUrl.find("bankofbeijing.com.cn") == std::string::npos ||
        curUrl.find("version=query") == std::string::npos) {
        spdlog::info("[BCB] Navigating to {}", url);
        page_->Navigate(url, 30000);
        Sleep(5000);
    }
    DismissCertPopup();   // 导航后先清一次，免得遮罩挡住后面的鼠标操作
    if (!WaitForElement("input[placeholder*=\"账号\"]", 10000)) {
        spdlog::error("[BCB] 查询版登录表单没找到. URL={}", EvalStr("location.href"));
        return "error: query login form not found";
    }

    std::string failReason;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        spdlog::info("[BCB] ===== 查询版登录自动尝试 {}/3 =====", attempt);
        if (attempt > 1) {
            try { session_->Call("Page.reload", {{"ignoreCache", true}}, 10000); }
            catch (...) {}
            Sleep(10000);
        }
        DismissCertPopup();   // reload 后可能再弹，每次尝试前都清
        if (!FillQueryAccountPwd()) { failReason = "fill-failed"; continue; }

        std::string sres = SliderAttempt(m);
        if (!sres.empty()) {
            failReason = sres;
            spdlog::warn("[BCB] 滑块尝试 {} 失败: {}", attempt, sres);
            // 顶象环境级降级（弹「请使用图形验证码」）：重试无意义，直接转人工
            if (sres.find("图形验证码") != std::string::npos) break;
            continue;
        }
        spdlog::info("[BCB] 滑块通过，提交登录...");
        ClickButtonByText("登录");
        Sleep(4000);
        std::string pop = PopupText();
        if (!pop.empty()) {
            spdlog::warn("[BCB] 提交被拒: {}", pop);
            if (pop.find("图形验证码") != std::string::npos) {
                // 顶象环境级降级：位置再准也会被拒 —— 直接呼唤人工
                failReason = "captcha-downgrade";
                break;
            }
            failReason = "rejected: " + pop;
            continue;
        }
        bool left = false;
        for (int i = 0; i < 30; ++i) {
            curUrl = EvalStr("location.href");
            if (curUrl.find("/login") == std::string::npos) { left = true; break; }
            Sleep(1000);
        }
        if (left) {
            loggedIn_ = true;
            spdlog::info("[BCB] 查询版登录成功: {}", EvalStr("location.href"));
            return "ok";
        }
        failReason = "no-redirect";
    }
    spdlog::warn("[BCB] 自动登录未成({})，转入人工接管", failReason);
    // 复位后重新填表，再呼唤人工
    try { session_->Call("Page.reload", {{"ignoreCache", true}}, 10000); } catch (...) {}
    Sleep(7000);
    DismissCertPopup();   // 人工接管前也清一次，别让人看见弹窗
    if (!WaitForElement("input[placeholder*=\"账号\"]", 10000))
        return "error: refill-form-not-found";
    if (!FillQueryAccountPwd()) return "error: refill-failed";
    std::string man = ManualTakeover();
    if (man.empty()) {
        loggedIn_ = true;
        return "ok";
    }
    if (man.rfind("need-manual", 0) == 0) {
        // 凭证类/反复失败：明确要求人工介入，不再自动重试
        for (int i = 0; i < 5; ++i) { Beep(1500, 500); Sleep(150); }
        spdlog::error("[BCB] >>>>> 需要人工介入：{}（自动流程已停止） <<<<<", man);
    }
    return "error: " + man;
}

} // namespace bcb
} // namespace newrpa
