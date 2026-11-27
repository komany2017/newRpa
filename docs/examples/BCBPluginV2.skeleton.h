#pragma once
// ============================================================================
// BCBPluginV2 —— 北京银行插件按新架构重构的【设计骨架】
//
// 状态：设计示例，不参与 CMake 构建。
// 目的：展示如何把 BCBPlugin.cpp（3637 行）拆成
//       "瘦外壳 + 模板方法钩子 + 可组合策略 + 显式状态机"。
//
// 迁移时把本文件中的 TODO 占位替换为现有 BCBPlugin.cpp 中已验证的实现：
//   Prepare            ← SetQueryDateRange + GotoRoute + ClickDialogButton
//   CaptureRows        ← DumpVueRowsJson + 解析
//   DownloadReceipts   ← DoQueryExportPerRow（含 20s 等待与重试）
//   LoginFlow          ← LoginQuery + AutoClickWordCaptcha + WaitHumanClickCaptcha
//   BindAndSave        ← 默认实现已够用；需要 ZIP/CID 解码时接入 SlipBundle
//
// 本文件可通过 -fsyntax-only 编译，确保接口形状正确。
// ============================================================================

#include "newrpa/arch/all.h"
#include "newrpa/arch/PluginBase.h"

#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace bcb {
namespace v2 {

// ----------------------------------------------------------------------------
// ① 验证码策略：自动 → 人工 的责任链
// ----------------------------------------------------------------------------

class AutoCaptchaSolver : public arch::ICaptchaSolver {
public:
    std::string name() const override { return "auto"; }

    bool CanSolve(arch::CaptchaKind kind) const override {
        return kind == arch::CaptchaKind::Slider ||
               kind == arch::CaptchaKind::ClickWord ||
               kind == arch::CaptchaKind::ImageCode;
    }

    arch::ResultVoid Solve(const arch::CaptchaContext& c) override {
        // TODO: 包装 AutoClickWordCaptcha / SliderAttempt（现有实现已验证）
        (void)c;
        return arch::MakeFail("captcha.auto_failed",
                              "自动识别失败，转人工", true);
    }
};

class HumanCaptchaSolver : public arch::ICaptchaSolver {
public:
    std::string name() const override { return "human"; }

    bool CanSolve(arch::CaptchaKind) const override { return true; }

    arch::ResultVoid Solve(const arch::CaptchaContext& c) override {
        // TODO: MessageBoxW + 5 分钟 watchdog PostMessageW(WM_CLOSE)
        //       保留现有 WaitHumanClickCaptcha 的交互方式，不变。
        (void)c;
        return arch::MakeOk();
    }
};

inline arch::CaptchaChain MakeCaptchaChain() {
    arch::CaptchaChain chain;
    chain.Add(std::make_unique<AutoCaptchaSolver>());
    chain.Add(std::make_unique<HumanCaptchaSolver>()); // 兜底，永远在最后
    return chain;
}

// ----------------------------------------------------------------------------
// ② 登录流程：状态机驱动，替代 LoginQuery 里的散落布尔判断
// ----------------------------------------------------------------------------

class BcbLoginFlow {
public:
    explicit BcbLoginFlow(arch::CaptchaChain chain)
        : captcha_(std::move(chain)) {}

    arch::ResultVoid Run(arch::AppContext& ctx) {
        arch::ScopedTimer timer("bcb.login",
            [&ctx](const std::string&, long long ms) {
                ctx.Log("[login] elapsed=" + std::to_string(ms) + "ms");
            });

        // TODO: FillQueryAccountPwd（CDP 填账号 + 盒子键盘输密码）
        machine_.Fire(arch::LoginEvent::FillDone);

        // TODO: DetectClickWordCaptcha / DetectSlider
        machine_.Fire(arch::LoginEvent::CaptchaFound);

        arch::CaptchaContext cc;
        cc.session = ctx.session.get();
        cc.browser = ctx.browser.get();
        cc.kind = arch::CaptchaKind::ClickWord;
        cc.timeoutMs = 300000; // 5 分钟人工看门狗

        auto r = captcha_.Solve(cc);
        if (!r) {
            // 自动失败 → 转人工 → 人工也失败，完整降级轨迹会留在 Trace 里
            machine_.Fire(arch::LoginEvent::AutoFailed);  // Captcha → AutoSolving
            machine_.Fire(arch::LoginEvent::AutoFailed);  // AutoSolving → HumanRequired
            machine_.Fire(arch::LoginEvent::Error);       // → Failed
            return arch::MakeFail("login.captcha", r.error().message);
        }
        // 实际应按最终生效的 solver 名字区分 AutoSucceeded / HumanDone
        machine_.Fire(arch::LoginEvent::AutoSucceeded);

        // TODO: WaitUrlContains("/queryHome") 判定真正登录成功
        machine_.Fire(arch::LoginEvent::AutoSucceeded);
        ctx.Log("[login] trace:\n" + machine_.TraceJoined());
        return arch::MakeOk();
    }

private:
    arch::LoginStateMachine machine_ = arch::MakeLoginMachine();
    arch::CaptchaChain      captcha_;
};

// ----------------------------------------------------------------------------
// ③ 回单下载策略：实现 IReceiptDownloader
// ----------------------------------------------------------------------------

class BcbReceiptDownloader : public arch::IReceiptDownloader {
public:
    std::string name() const override { return "bcb.per-row"; }

    arch::Result<std::vector<arch::DownloadedFile>>
    Download(cdp::CDPSession& s,
             const std::vector<plugin::Transaction>& rows) override {
        // TODO: 逐行 SelectReceiptRow(i) → GenerateReceiptForSelection
        //       → WaitForNewFiles(..., 20000) → 失败可重试（error.retryable=true）
        (void)s;
        std::vector<arch::DownloadedFile> out;
        out.reserve(rows.size());
        for (const auto& row : rows)
            out.push_back(arch::DownloadedFile{"", row.sn, ""});
        return arch::Result<std::vector<arch::DownloadedFile>>::Ok(std::move(out));
    }
};

// ----------------------------------------------------------------------------
// ④ 瘦插件外壳：模板方法固定流程，差异只在三个钩子
// ----------------------------------------------------------------------------

class BcbPluginV2 : public arch::BankPluginBase {
public:
    const char* GetId() const override { return "BOB"; }
    const char* GetDisplayName() const override { return "北京银行企业银行查询版"; }

    uint32_t Capabilities() const override {
        return CAP_LOGIN | CAP_CUR_BALANCE | CAP_CUR_TRANS | CAP_YES_TRANS |
               CAP_SLIP | CAP_QUERY;
    }

    void SetSession(std::shared_ptr<cdp::CDPSession> session,
                    std::shared_ptr<chrome::ChromeBrowser> browser) override {
        session_ = std::move(session);
        browser_ = std::move(browser);
    }

    void SetConfig(const plugin::BankConfig& cfg) override { cfg_ = cfg; }

    arch::Result<arch::LoginOutcome> Login() override {
        arch::AppContext ctx;
        ctx.session = session_;
        ctx.browser = browser_;
        auto r = loginFlow_.Run(ctx);
        if (!r.ok())
            return arch::Result<arch::LoginOutcome>::Fail(
                r.error().code, r.error().message);
        arch::LoginOutcome out;
        out.raw = "ok";
        out.loggedIn = true;
        return arch::Result<arch::LoginOutcome>::Ok(std::move(out));
    }

    arch::Result<std::vector<std::map<std::string, std::string>>>
    GetCurBal() override {
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Ok({});
    }

    arch::Result<std::vector<plugin::Transaction>> GetCurTran() override {
        return arch::Result<std::vector<plugin::Transaction>>::Ok({});
    }

    arch::Result<std::vector<plugin::Transaction>>
    GetHisTran(const plugin::DateRange&) override {
        return arch::Result<std::vector<plugin::Transaction>>::Ok({});
    }

    arch::Result<std::string> Payment(const std::string&, const std::string&,
                                      const std::string&, const std::string&,
                                      const std::string&) override {
        return arch::Result<std::string>::Ok("not supported by BOB");
    }

    arch::Result<std::string> Approval(const std::string&) override {
        return arch::Result<std::string>::Ok("not supported by BOB");
    }

    arch::Result<std::string> Query(const std::string&) override {
        return arch::Result<std::string>::Ok("not supported by BOB");
    }

    void Close() override { loggedIn_ = false; }

protected:
    bool Prepare(arch::ExportContext& ec) override {
        // TODO: GotoRoute(BaseOrigin() + route) → SetQueryDateRange
        //       → ClickButtonByText("确定") → ClickPopupOk
        (void)ec;
        return session_ != nullptr;
    }

    std::vector<plugin::Transaction>
    CaptureRows(arch::ExportContext& ec) override {
        // TODO: DumpVueRowsJson() → 读 __vue__.store.states.data
        //       拿到页面未显示的流水号/金额/账号，逐行填 Transaction
        (void)ec;
        return {};
    }

    std::vector<arch::DownloadedFile>
    DownloadReceipts(arch::ExportContext&,
                     const std::vector<plugin::Transaction>& rows) override {
        if (!session_) return {};
        auto r = downloader_->Download(*session_, rows);
        return r.ok() ? r.value() : std::vector<arch::DownloadedFile>{};
    }

private:
    plugin::BankConfig                     cfg_;
    std::shared_ptr<cdp::CDPSession>       session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;
    bool                                   loggedIn_ = false;

    BcbLoginFlow                           loginFlow_{MakeCaptchaChain()};
    std::unique_ptr<arch::IReceiptDownloader> downloader_ =
        std::make_unique<BcbReceiptDownloader>();
};

// ----------------------------------------------------------------------------
// ⑤ 命令：SlipCommand 只描述业务，不再碰登录/能力检查
// ----------------------------------------------------------------------------

class SlipCommand : public arch::ICommand {
public:
    explicit SlipCommand(plugin::DateRange range)
        : range_(std::move(range)) {}

    std::string name() const override { return "slip"; }

    uint32_t requiredCaps() const override {
        return plugin::IBankPlugin::CAP_SLIP;
    }

    arch::ResultVoid execute(arch::AppContext& ctx) override {
        if (!ctx.bankPlugin)
            return arch::MakeFail("slip.no_plugin", "未加载银行插件");
        auto files = ctx.bankPlugin->DownBankSlip(range_);
        if (files.empty())
            return arch::MakeFail("slip.no_files", "未下载到回单", true);
        ctx.files = files;
        ctx.Log("[slip] downloaded " + std::to_string(files.size()) + " file(s)");
        return arch::MakeOk();
    }

private:
    plugin::DateRange range_;
};

} // namespace v2
} // namespace bcb
} // namespace newrpa
