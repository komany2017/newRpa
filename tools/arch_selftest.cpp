// 架构基础设施自检：验证 core/include/newrpa/arch 下头文件可独立编译。
//
// 编译方法（在工程根目录，Git Bash）：
//   /e/Workdaddy/mingw64/bin/g++.exe -std=c++20 -fsyntax-only \
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

    // P6：LoginState 与 StateStore 的轻量自检（不落盘）
    LoginSessionState ls;
    if (ls.IsLoggedIn()) return 7;
    ls.MarkLoggedIn("ok");
    if (!ls.IsLoggedIn() || ls.LastRaw() != "ok") return 8;
    ls.MarkLoggedOut();
    if (ls.IsLoggedIn()) return 9;
    StateStore st(fs::path("state"));
    if (st.CookieFile("BOB").filename().string() != "cookies_BOB.json") return 10;
    if (st.HisTranFile("bcb").filename().string() != "bcb_histran_state.json") return 11;

    // P7：RetryPolicy 只对 retryable 错误重试，WaitPolicy 默认参数来自实测
    int calls = 0;
    RetryPolicy rp{3, 0};
    auto rr = rp.Run([&](int) -> Result<std::string> {
        ++calls;
        return calls < 3
            ? Result<std::string>::Fail(Error::Retryable("retry", "again"))
            : Result<std::string>::Ok("ok");
    });
    if (!rr.ok() || calls != 3 || rr.value() != "ok") return 12;
    WaitPolicy wp;
    if (wp.intervalMs != 500 || wp.timeoutMs != 20000 || wp.settleMs != 1500) return 13;

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
