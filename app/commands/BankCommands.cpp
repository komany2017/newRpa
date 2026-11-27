// ============================================================================
// 银行业务命令实现 —— 从 main.cpp::MainRun 的 action 分支逐字迁移
// P1 约束：日志文本、参数校验、Login 调用时机与改造前完全一致。
// ============================================================================

#include "BankCommands.h"

#include "newrpa/net/CookieJar.h"
#include "newrpa/net/HttpSession.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace newrpa {
namespace app {

// 约定：error.message 为空表示命令已经自行输出过日志，主控不再重复打印
static arch::ResultVoid SilentFail(const std::string& code) {
    return arch::MakeFail(code, "");
}

// ---------------------------------------------------------------------------
// 简单业务命令
// ---------------------------------------------------------------------------

arch::ResultVoid LoginCommand::execute(arch::AppContext& ctx) {
    if (!ctx.bankPlugin ||
        (ctx.bankPlugin->Capabilities() & plugin::IBankPlugin::CAP_LOGIN) == 0) {
        spdlog::error("Plugin doesn't support login");
        return SilentFail("login.no_cap");
    }
    spdlog::info("Login result: {}", LoginViaPlugin(ctx));
    return arch::MakeOk();
}

arch::ResultVoid CurBalCommand::execute(arch::AppContext& ctx) {
    LoginQuiet(ctx);
    if (!ctx.bankPlugin) return SilentFail("curbal.no_plugin");
    auto rr = ctx.bankPlugin->GetCurBal();
    if (!rr) return SilentFail("curbal.failed");
    auto rows = rr.value();
    spdlog::info("CurBal: {} rows", rows.size());
    for (auto& row : rows)
        for (auto& kv : row)
            spdlog::info("  {}={}", kv.first, kv.second);
    return arch::MakeOk();
}

arch::ResultVoid CurTranCommand::execute(arch::AppContext& ctx) {
    LoginQuiet(ctx);
    if (!ctx.bankPlugin) return SilentFail("curtran.no_plugin");
    auto rr = ctx.bankPlugin->GetCurTran();
    if (!rr) return SilentFail("curtran.failed");
    auto rows = rr.value();
    spdlog::info("CurTran: {} rows", rows.size());
    for (auto& r : rows)
        spdlog::info("  {} | {} | {} | {} | {}", r.transDate, r.direction,
                     r.amount, r.opAccount, r.opAccountName);
    return arch::MakeOk();
}

arch::ResultVoid HisTranCommand::execute(arch::AppContext& ctx) {
    if (!RequireRange(ctx)) return SilentFail("histran.bad_range");
    if (!ctx.bankPlugin) return SilentFail("histran.no_plugin");
    LoginQuiet(ctx);
    plugin::DateRange range{ctx.begin, ctx.end};
    auto rr = ctx.bankPlugin->GetHisTran(range);
    if (!rr) return SilentFail("histran.failed");
    auto rows = rr.value();
    spdlog::info("HisTran: {} rows", rows.size());
    return arch::MakeOk();
}

arch::ResultVoid SlipCommand::execute(arch::AppContext& ctx) {
    if (!RequireRange(ctx)) return SilentFail("slip.bad_range");
    if (!ctx.bankPlugin) return SilentFail("slip.no_plugin");
    LoginQuiet(ctx);
    plugin::DateRange range{ctx.begin, ctx.end};
    auto rr = ctx.bankPlugin->DownBankSlip(range);
    if (!rr) return SilentFail("slip.failed");
    auto files = rr.value();
    for (auto& f : files) spdlog::info("Slip: {}", f);
    return arch::MakeOk();
}

arch::ResultVoid PaymentCommand::execute(arch::AppContext& ctx) {
    if (!ctx.bankPlugin) return SilentFail("payment.no_plugin");
    auto r = ctx.bankPlugin->Payment(ctx.amount, ctx.nums, ctx.paccno);
    if (!r) return SilentFail("payment.failed");
    spdlog::info("Payment: {}", r.value());
    return arch::MakeOk();
}

arch::ResultVoid ApprovalCommand::execute(arch::AppContext& ctx) {
    if (!ctx.bankPlugin) return SilentFail("approval.no_plugin");
    auto r = ctx.bankPlugin->Approval(ctx.no);
    if (!r) return SilentFail("approval.failed");
    spdlog::info("Approval: {}", r.value());
    return arch::MakeOk();
}

arch::ResultVoid QueryCommand::execute(arch::AppContext& ctx) {
    if (!ctx.bankPlugin) return SilentFail("query.no_plugin");
    auto r = ctx.bankPlugin->Query(ctx.no);
    if (!r) return SilentFail("query.failed");
    spdlog::info("Query: {}", r.value());
    return arch::MakeOk();
}

// ---------------------------------------------------------------------------
// 复合流程命令
// ---------------------------------------------------------------------------

arch::ResultVoid AllCommand::execute(arch::AppContext& ctx) {
    if (!LoginStrict(ctx, "ALL")) return SilentFail("all.login_failed");

    auto balR = ctx.bankPlugin->GetCurBal();
    if (!balR) return SilentFail("all.curbal_failed");
    auto bal = balR.value();
    spdlog::info("[ALL] CurBal: {} rows", bal.size());
    auto curR = ctx.bankPlugin->GetCurTran();
    if (!curR) return SilentFail("all.curtran_failed");
    auto cur = curR.value();
    spdlog::info("[ALL] CurTran: {} rows", cur.size());

    std::string begin = ctx.begin.empty() ? "2026-01-01" : ctx.begin;
    std::string end = ctx.end.empty() ? "2026-11-20" : ctx.end;
    plugin::DateRange range{begin, end};
    auto hisR = ctx.bankPlugin->GetHisTran(range);
    if (!hisR) return SilentFail("all.histran_failed");
    auto his = hisR.value();
    spdlog::info("[ALL] HisTran: {} rows", his.size());
    auto slipR = ctx.bankPlugin->DownBankSlip(range);
    if (!slipR) return SilentFail("all.slip_failed");
    auto files = slipR.value();
    for (auto& f : files) spdlog::info("[ALL] Slip: {}", f);
    return arch::MakeOk();
}

arch::ResultVoid AllAccountsCommand::execute(arch::AppContext& ctx) {
    if (!LoginStrict(ctx, "ALL-ACC"))
        return SilentFail("all-accounts.login_failed");
    auto summaryR = ctx.bankPlugin->RunAllAccounts();
    if (!summaryR) return SilentFail("all-accounts.failed");
    spdlog::info("[ALL-ACC] SUMMARY:\n{}", summaryR.value());
    return arch::MakeOk();
}

arch::ResultVoid HttpDirectCommand::execute(arch::AppContext& ctx) {
    if (!ctx.bankPlugin) return SilentFail("http-direct.no_plugin");
    auto summaryR = ctx.bankPlugin->HttpDirect();
    if (!summaryR) return SilentFail("http-direct.failed");
    spdlog::info("[HTTP-DIRECT] SUMMARY:\n{}", summaryR.value());
    return arch::MakeOk();
}

// ---------------------------------------------------------------------------
// sniff —— CDP 抓包（原样迁移，含事件注册/反注册与 Cookie 快照）
// ---------------------------------------------------------------------------

arch::ResultVoid SniffCommand::execute(arch::AppContext& ctx) {
    if (!ctx.session || !ctx.bankPlugin)
        return arch::MakeFail("sniff.no_session", "sniff 需要 CDP 会话");

    json records = json::array();
    std::mutex sniffMtx;

    int hReq = ctx.session->OnEvent("Network.requestWillBeSent", [&](const json& p) {
        auto req = p.value("request", json::object());
        std::string url = req.value("url", "");
        std::string type = p.value("type", "");
        bool isStatic =
            (type == "Image" || type == "Stylesheet" || type == "Font" ||
             type == "Script" || type == "Media");
        if (isStatic) return;
        json rec = {{"event", "request"},
                    {"requestId", p.value("requestId", "")},
                    {"type", type},
                    {"method", req.value("method", "")},
                    {"url", url}};
        if (req.contains("headers")) rec["headers"] = req["headers"];
        if (req.contains("postData"))
            rec["postData"] = req["postData"].get<std::string>();
        std::lock_guard<std::mutex> lk(sniffMtx);
        records.push_back(std::move(rec));
    });

    int hResp = ctx.session->OnEvent("Network.responseReceived", [&](const json& p) {
        auto resp = p.value("response", json::object());
        std::string type = p.value("type", "");
        if (type == "Image" || type == "Stylesheet" || type == "Font" ||
            type == "Media")
            return;
        json rec = {{"event", "response"},
                    {"requestId", p.value("requestId", "")},
                    {"status", resp.value("status", 0)},
                    {"mimeType", resp.value("mimeType", "")},
                    {"url", resp.value("url", "")}};
        if (resp.contains("headers")) rec["headers"] = resp["headers"];
        std::lock_guard<std::mutex> lk(sniffMtx);
        records.push_back(std::move(rec));
    });

    spdlog::info("[SNIFF] Capturing network, running login + business actions...");
    auto lr = ctx.bankPlugin->Login();
    std::string r = lr.ok() ? lr.value().raw : lr.error().message;
    spdlog::info("[SNIFF] Login: {}", r);
    if (r == "ok" || r == "already logged in") {
        (void)ctx.bankPlugin->GetCurBal();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        (void)ctx.bankPlugin->GetCurTran();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        plugin::DateRange dr{"2026-11-01", "2026-11-20"};
        (void)ctx.bankPlugin->GetHisTran(dr);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    try {
        auto ck = ctx.session->Call("Network.getCookies", json::object(), 5000);
        json out = json::object();
        if (ck.contains("cookies")) {
            json arr = json::array();
            for (auto& c : ck["cookies"]) {
                arr.push_back({{"name", c.value("name", "")},
                               {"value", c.value("value", "")},
                               {"domain", c.value("domain", "")},
                               {"path", c.value("path", "")}});
            }
            out["cookies"] = arr;
        }
        std::lock_guard<std::mutex> lk(sniffMtx);
        records.push_back({{"event", "cookies_snapshot"}, {"data", out}});
    } catch (const std::exception& e) {
        spdlog::warn("[SNIFF] getCookies failed: {}", e.what());
    }

    ctx.session->OffEvent("Network.requestWillBeSent", hReq);
    ctx.session->OffEvent("Network.responseReceived", hResp);

    json dump;
    dump["loginUrl"] = (ctx.globalConfig && !ctx.globalConfig->banks.empty())
                           ? ctx.globalConfig->banks[0].loginUrl
                           : "";
    dump["records"] = records;
    fs::path outPath = fs::absolute("sniff_result.json");
    std::ofstream ofs(outPath, std::ios::binary);
    ofs << dump.dump(2);
    ofs.close();
    spdlog::info("[SNIFF] Wrote {} records to {}", records.size(), outPath.string());
    return arch::MakeOk();
}

// ---------------------------------------------------------------------------
// http-session —— Cookie 持久化 + HTTP 任务（原样迁移）
// ---------------------------------------------------------------------------

arch::ResultVoid HttpSessionCommand::execute(arch::AppContext& ctx) {
    using net::CookieJar;
    using net::HttpSession;
    using net::HttpTask;

    if (!ctx.bankPlugin || !ctx.session)
        return arch::MakeFail("http-session.no_context",
                              "http-session 需要插件与 CDP 会话");

    const fs::path stateDir = ctx.state.Root();
    std::error_code mkEc;
    fs::create_directories(stateDir, mkEc);
    fs::path cookiePath = ctx.state.CookieFile(ctx.bank.bankName);

    CookieJar jar;
    bool reused = false;
    if (ctx.reuseCookie && fs::exists(cookiePath)) {
        reused = jar.Load(cookiePath.string());
        if (reused)
            spdlog::info("[HTTP-SESSION] Reused {} cookies from {}", jar.size(),
                         cookiePath.string());
        else
            spdlog::warn("[HTTP-SESSION] Cookie file invalid, will login again");
    }

    if (!reused) {
        auto lr = ctx.bankPlugin->Login();
        std::string r = lr.ok() ? lr.value().raw : lr.error().message;
        spdlog::info("[HTTP-SESSION] Login: {}", r);
        if (r != "ok" && r != "already logged in") {
            spdlog::error("[HTTP-SESSION] Login failed, abort");
            return SilentFail("http-session.login_failed");
        }
        try {
            auto cks = ctx.session->Call("Network.getCookies", json::object(), 5000);
            jar = CookieJar::FromCdp(cks);
            if (!jar.Save(cookiePath.string()))
                spdlog::warn("[HTTP-SESSION] Cannot save cookies to {}",
                             cookiePath.string());
            else
                spdlog::info("[HTTP-SESSION] Saved {} cookies -> {}", jar.size(),
                             cookiePath.string());
        } catch (const std::exception& e) {
            spdlog::error("[HTTP-SESSION] Network.getCookies failed: {}", e.what());
            return SilentFail("http-session.getcookies_failed");
        }
    }

    std::string href;
    try {
        auto ev = ctx.session->Call(
            "Runtime.evaluate",
            json{{"expression", "location.href"}, {"returnByValue", true}}, 5000);
        if (ev.contains("result") && ev["result"].contains("result"))
            href = ev["result"]["result"].value("value", "");
    } catch (const std::exception& e) {
        spdlog::warn("[HTTP-SESSION] get location.href failed: {}", e.what());
    }
    std::string origin;
    size_t sp = href.find("://");
    if (sp != std::string::npos) {
        size_t slash = href.find('/', sp + 3);
        origin = href.substr(0, slash == std::string::npos ? href.size() : slash);
    }
    if (origin.empty() && !ctx.bank.loginUrl.empty()) {
        size_t sc = ctx.bank.loginUrl.find("://");
        if (sc != std::string::npos) {
            size_t slash = ctx.bank.loginUrl.find('/', sc + 3);
            origin = ctx.bank.loginUrl.substr(
                0, slash == std::string::npos ? ctx.bank.loginUrl.size() : slash);
        }
    }
    if (origin.empty())
        origin = "https://cbank.bankofbeijing.com.cn:9081";
    spdlog::info("[HTTP-SESSION] origin={}, cookies={}", origin, jar.size());

    std::vector<HttpTask> tasks;
    try {
        std::ifstream ifs(fs::path(ctx.configPath), std::ios::binary);
        if (ifs) {
            json root;
            ifs >> root;
            if (root.contains("httpTasks"))
                tasks = HttpSession::TasksFromJson(root["httpTasks"]);
        }
    } catch (const std::exception& e) {
        spdlog::warn("[HTTP-SESSION] parse httpTasks failed: {}", e.what());
    }
    if (tasks.empty()) {
        spdlog::warn("[HTTP-SESSION] No httpTasks in config, using BCB defaults");
        tasks = HttpSession::DefaultTasks(origin);
    }

    HttpSession hs;
    hs.SetCookieJar(jar);
    hs.SetOrigin(origin);
    hs.SetBank(ctx.bank.bankName);
    hs.SetReferer(origin + "/");
    hs.SetIgnoreCertErrors(true);
    hs.SetTimeout(30000);
    auto summary = hs.RunTasks(tasks, fs::absolute("downloads").string());
    spdlog::info("[HTTP-SESSION] SUMMARY:\n{}", summary);
    return arch::MakeOk();
}

// ---------------------------------------------------------------------------
// 总线工厂
// ---------------------------------------------------------------------------

std::unique_ptr<arch::CommandBus> MakeBankCommandBus() {
    auto bus = std::make_unique<arch::CommandBus>();
    bus->Register(std::make_unique<LoginCommand>());
    bus->Register(std::make_unique<CurBalCommand>());
    bus->Register(std::make_unique<CurTranCommand>());
    bus->Register(std::make_unique<HisTranCommand>());
    bus->Register(std::make_unique<SlipCommand>());
    bus->Register(std::make_unique<PaymentCommand>());
    bus->Register(std::make_unique<ApprovalCommand>());
    bus->Register(std::make_unique<QueryCommand>());
    bus->Register(std::make_unique<AllCommand>());
    bus->Register(std::make_unique<AllAccountsCommand>("all-accounts"));
    bus->Register(std::make_unique<AllAccountsCommand>("all-account"));
    bus->Register(std::make_unique<SniffCommand>());
    bus->Register(std::make_unique<HttpDirectCommand>());
    bus->Register(std::make_unique<HttpSessionCommand>());
    return bus;
}

} // namespace app
} // namespace newrpa
