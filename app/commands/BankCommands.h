#pragma once
// ============================================================================
// 银行业务命令集 —— 从 app/main.cpp 的 13 个 action 分支逐字迁移
//
// P1 原则：行为完全等价。日志文本、参数校验、Login 调用时机均保持原样。
// 命令只描述"做什么"，前置的插件加载 / Chrome 启动 / CDP 连接仍在 MainRun。
// ============================================================================

#include "newrpa/arch/Command.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace newrpa {
namespace app {

// 创建并注册全部命令的总线工厂
std::unique_ptr<arch::CommandBus> MakeBankCommandBus();

// 公共基类：提供与原 main.cpp 一致的 Login / 参数校验辅助
class BankCommandBase : public arch::ICommand {
protected:
    // P5：直接调用 Result<T> 唯一接口；返回原始字符串保证日志文本不变
    static std::string LoginViaPlugin(arch::AppContext& ctx) {
        if (!ctx.bankPlugin) return "";
        auto r = ctx.bankPlugin->Login();
        if (!r.ok()) return r.error().message;
        return r.value().raw;
    }

    // 对应原代码 "plugin->Login();"（不检查返回值）
    static void LoginQuiet(arch::AppContext& ctx) { LoginViaPlugin(ctx); }

    // 对应原代码严格检查 Login 的分支（all / all-accounts / sniff / http-session）
    static bool LoginStrict(arch::AppContext& ctx, const std::string& tag) {
        if (!ctx.bankPlugin) {
            spdlog::error("[{}] No plugin", tag);
            return false;
        }
        std::string r = LoginViaPlugin(ctx);
        spdlog::info("[{}] Login result: {}", tag, r);
        if (r != "ok" && r != "already logged in") {
            spdlog::error("[{}] Login failed, abort", tag);
            return false;
        }
        return true;
    }

    static bool RequireRange(arch::AppContext& ctx) {
        if (ctx.begin.empty() || ctx.end.empty()) {
            spdlog::error("--begin and --end required");
            return false;
        }
        return true;
    }
};

// ---- 简单业务命令 ----
class LoginCommand : public BankCommandBase {
public:
    std::string name() const override { return "login"; }
    // P1 保持原行为：能力检查与错误文本在 execute 内部完成，不交给总线
    uint32_t requiredCaps() const override { return 0; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class CurBalCommand : public BankCommandBase {
public:
    std::string name() const override { return "curbal"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class CurTranCommand : public BankCommandBase {
public:
    std::string name() const override { return "curtran"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class HisTranCommand : public BankCommandBase {
public:
    std::string name() const override { return "histran"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class SlipCommand : public BankCommandBase {
public:
    std::string name() const override { return "slip"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class PaymentCommand : public BankCommandBase {
public:
    std::string name() const override { return "payment"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class ApprovalCommand : public BankCommandBase {
public:
    std::string name() const override { return "approval"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class QueryCommand : public BankCommandBase {
public:
    std::string name() const override { return "query"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

// ---- 复合流程命令 ----
class AllCommand : public BankCommandBase {
public:
    std::string name() const override { return "all"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

// 支持 "all-accounts" 与别名 "all-account"
class AllAccountsCommand : public BankCommandBase {
public:
    explicit AllAccountsCommand(std::string alias = "all-accounts")
        : alias_(std::move(alias)) {}
    std::string name() const override { return alias_; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
private:
    std::string alias_;
};

// ---- 抓包 / HTTP 模式命令 ----
class SniffCommand : public BankCommandBase {
public:
    std::string name() const override { return "sniff"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class HttpDirectCommand : public BankCommandBase {
public:
    std::string name() const override { return "http-direct"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

class HttpSessionCommand : public BankCommandBase {
public:
    std::string name() const override { return "http-session"; }
    arch::ResultVoid execute(arch::AppContext& ctx) override;
};

} // namespace app
} // namespace newrpa
