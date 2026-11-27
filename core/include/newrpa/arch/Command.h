#pragma once
// ============================================================================
// ICommand / CommandBus — 取代 app/main.cpp 的长 if-else 分发
//
// 现状：每加一个 action 都要改 main.cpp，登录/能力检查在每个分支重复。
// 目标：命令自描述（名字/能力/是否需要登录），CommandBus 统一前置检查。
//
// 用法：
//   bus.Register(std::make_unique<SlipCommand>());
//   auto r = bus.Execute("slip", ctx);
// ============================================================================

#include "newrpa/arch/Result.h"
#include "newrpa/arch/Context.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace newrpa {
namespace arch {

struct ICommand {
    virtual ~ICommand() = default;

    // 命令行 action 名，如 "slip" / "curbal" / "histran"
    virtual std::string name() const = 0;

    // 需要的插件能力位图（IBankPlugin::CAP_*），0 = 不检查
    virtual uint32_t requiredCaps() const { return 0; }

    // 是否需要预登录（默认 true；login 命令自己重写为 false）
    virtual bool needLogin() const { return true; }

    // 业务执行
    virtual ResultVoid execute(AppContext& ctx) = 0;
};

class CommandBus {
public:
    // P1 迁移阶段保持 false：每个命令沿用原 main.cpp 自己的 Login() 调用时序。
    // 后续阶段确认行为等价后再开启，由总线统一做登录前置检查。
    void SetAutoLogin(bool enabled) noexcept { autoLogin_ = enabled; }
    bool AutoLogin() const noexcept { return autoLogin_; }

    void Register(std::unique_ptr<ICommand> cmd) {
        if (!cmd) return;
        auto key = cmd->name();
        commands_[key] = std::move(cmd);
    }

    // 只在命令不存在时注册，便于默认命令被配置覆盖
    void RegisterIfAbsent(std::unique_ptr<ICommand> cmd) {
        if (!cmd) return;
        auto key = cmd->name();
        if (commands_.find(key) == commands_.end())
            commands_[key] = std::move(cmd);
    }

    ResultVoid Execute(const std::string& name, AppContext& ctx) {
        auto it = commands_.find(name);
        if (it == commands_.end())
            return MakeFail("command.unknown",
                            "未知 action: " + name + "（可用: " + ListJoined() + "）");

        ICommand& cmd = *it->second;
        if (cmd.requiredCaps() != 0 && ctx.bankPlugin == nullptr)
            return MakeFail("command.no_plugin", "插件未注入，无法检查能力");

        if (cmd.requiredCaps() != 0) {
            uint32_t have = ctx.bankPlugin->Capabilities();
            uint32_t need = cmd.requiredCaps();
            if ((have & need) != need)
                return MakeFail("command.cap_missing",
                                "插件 " + std::string(ctx.bankPlugin->GetId()) +
                                    " 不支持该操作");
        }

        // 登录是横切关注点；P1 阶段关闭，命令保留原有 Login() 调用时序
        if (autoLogin_ && cmd.needLogin() && ctx.bankPlugin != nullptr) {
            Result<LoginOutcome> lr;
            try {
                lr = ctx.bankPlugin->Login();
            } catch (const std::exception& e) {
                return MakeFail("command.login_exception", e.what());
            }
            if (!lr.ok())
                return MakeFail("command.login_failed",
                                "登录失败: " + lr.error().message);
            const std::string& raw = lr.value().raw;
            if (raw != "ok" && raw != "already logged in")
                return MakeFail("command.login_failed", "登录失败: " + raw);
            ctx.Log("[command] login ok");
        }

        ctx.Log("[command] " + name);
        auto r = cmd.execute(ctx);
        if (!r)
            ctx.Log("[command] " + name + " FAILED: " + r.error().code +
                    " - " + r.error().message);
        else
            ctx.Log("[command] " + name + " ok");
        return r;
    }

    std::vector<std::string> List() const {
        std::vector<std::string> out;
        out.reserve(commands_.size());
        for (const auto& kv : commands_) out.push_back(kv.first);
        return out;
    }

    std::string ListJoined() const {
        std::string s;
        for (const auto& kv : commands_) {
            if (!s.empty()) s += ", ";
            s += kv.first;
        }
        return s;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ICommand>> commands_;
    bool autoLogin_ = false;
};

} // namespace arch
} // namespace newrpa
