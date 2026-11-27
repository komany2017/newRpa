#pragma once
// ============================================================================
// IWorkflowStep / RetryStep / WorkflowPipeline
//
// 把散落在 DoQueryExportPerRow 里的 "等待 20s + 重试 + 轮末兜底"
// 收敛成可组合的步骤与装饰器；参数从 AppContext 读取，不再硬编码。
// ============================================================================

#include "newrpa/arch/Result.h"
#include "newrpa/arch/Context.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace newrpa {
namespace arch {

struct IWorkflowStep {
    virtual ~IWorkflowStep() = default;
    virtual std::string name() const = 0;
    virtual ResultVoid run(AppContext& ctx) = 0;
};

// 便利：用 lambda 快速造一个步骤
class LambdaStep : public IWorkflowStep {
public:
    using Fn = std::function<ResultVoid(AppContext&)>;

    LambdaStep(std::string name, Fn fn)
        : name_(std::move(name)), fn_(std::move(fn)) {}

    std::string name() const override { return name_; }
    ResultVoid run(AppContext& ctx) override {
        return fn_ ? fn_(ctx) : MakeFail("step.no_fn", "步骤未设置执行函数");
    }

private:
    std::string name_;
    Fn          fn_;
};

// 装饰器：只对 retryable 错误重试
class RetryStep : public IWorkflowStep {
public:
    RetryStep(std::unique_ptr<IWorkflowStep> inner, int attempts = 3,
              int delayMs = 2000)
        : inner_(std::move(inner)), attempts_(attempts), delayMs_(delayMs) {}

    std::string name() const override {
        return "retry(" + (inner_ ? inner_->name() : std::string("null")) + ")";
    }

    ResultVoid run(AppContext& ctx) override {
        if (!inner_)
            return MakeFail("step.no_inner", "重试装饰器没有内部步骤");

        ResultVoid last = MakeFail("step.not_run", "步骤尚未执行");
        int total = attempts_ > 0 ? attempts_ : 1;
        for (int i = 0; i < total; ++i) {
            last = inner_->run(ctx);
            if (last.ok()) return last;
            if (!last.error().retryable) return last; // 不可重试，立即失败
            if (i + 1 < total && delayMs_ > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        }
        return last;
    }

private:
    std::unique_ptr<IWorkflowStep> inner_;
    int attempts_ = 3;
    int delayMs_  = 2000;
};

class WorkflowPipeline {
public:
    void Add(std::unique_ptr<IWorkflowStep> step) {
        if (step) steps_.push_back(std::move(step));
    }

    void AddFn(std::string name, LambdaStep::Fn fn) {
        Add(std::make_unique<LambdaStep>(std::move(name), std::move(fn)));
    }

    ResultVoid Run(AppContext& ctx) {
        for (const auto& step : steps_) {
            auto start = std::chrono::steady_clock::now();
            auto r = step->run(ctx);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            std::string line = "[step] " + step->name() +
                               " elapsed=" + std::to_string(ms) + "ms result=" +
                               (r.ok() ? "ok" : "err:" + r.error().code);
            ctx.Log(line);
            if (!r) return r; // 失败即短路
        }
        return MakeOk();
    }

    std::vector<std::string> Names() const {
        std::vector<std::string> out;
        out.reserve(steps_.size());
        for (const auto& s : steps_) out.push_back(s->name());
        return out;
    }

private:
    std::vector<std::unique_ptr<IWorkflowStep>> steps_;
};

} // namespace arch
} // namespace newrpa
