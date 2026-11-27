// 架构基础设施自检：验证 core/include/newrpa/arch 下头文件可独立编译。
//
// 编译方法（在工程根目录，Git Bash）：
//   /e/Workdaddy/mingw64/bin/g++.exe -std=c++17 -fsyntax-only \
//     -Icore/include -Ithird_party/nlohmann_json/include tools/arch_selftest.cpp
//
// 本文件不参与 CMake 构建，仅作为架构层的可编译示例与回归检查。

#include "newrpa/arch/all.h"

int main() {
    using namespace newrpa::arch;

    auto ok = Result<int>::Ok(42);
    auto err = Result<int>::Fail("test", "demo", true);

    auto machine = MakeLoginMachine();
    machine.Fire(LoginEvent::FillDone);
    machine.Fire(LoginEvent::CaptchaFound);

    CommandBus bus;
    AppContext ctx;
    ctx.Log("selftest");

    WorkflowPipeline pipe;
    pipe.AddFn("noop", [](AppContext&) { return MakeOk(); });
    pipe.Run(ctx);

    CaptchaChain chain;
    (void)chain.CanSolve(CaptchaKind::None);

    // P5：直接验证 Result 接口的失败语义（无需并存适配器）
    Result<LoginOutcome> nullLogin =
        Result<LoginOutcome>::Fail(BankErr::NoPlugin, "未加载银行插件");
    if (nullLogin.ok()) return 2;
    if (Result<std::vector<newrpa::plugin::Transaction>>().ok()) return 3;
    if (Result<std::vector<std::string>>().ok()) return 4;
    if (nullLogin.error().code != BankErr::NoPlugin) return 5;

    // 验证 LoginOutcome 保留原始字符串
    LoginOutcome lo{"ok", true};
    if (!lo.loggedIn || lo.raw != "ok") return 6;

    return (ok.ok() && !err.ok() && ok.value() == 42) ? 0 : 1;
}
