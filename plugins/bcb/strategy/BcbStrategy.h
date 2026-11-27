#pragma once
// ============================================================================
// 北京银行查询版：滑块算法 / 验证码策略的抽象边界（P3）
//
// 设计目标：
//   * BCBPlugin 只保留"编排"，不再内联 700 行滑块/验证码算法；
//   * 算法通过 IBcbAutomationHost 使用 CDP / 页面 / HID 能力，不反向依赖插件类；
//   * 验证码按策略注册到责任链，当前默认仍是「点选验证码直接转人工」——
//     超级鹰自动点字策略已实现但默认关闭，打开开关即可切换，不改业务代码。
// ============================================================================

#include "newrpa/arch/Result.h"
#include "newrpa/arch/Strategy.h"

#include <memory>
#include <string>
#include <vector>

class HidMouse;   // 全局命名空间：core/include/newrpa/singleUsbbox/HidMouse.hpp

namespace newrpa {
namespace cdp { class CDPSession; }

namespace bcb {
namespace strategy {

// ----------------------------------------------------------------------------
// 策略所需的宿主能力。BCBPlugin 实现此接口（P3 桥接区），策略不 include
// BCBPlugin.h，从而打破"上帝类被所有算法反向依赖"的耦合。
// ----------------------------------------------------------------------------
class IBcbAutomationHost {
public:
    virtual ~IBcbAutomationHost() = default;

    virtual std::string HostEvalStr(const std::string& js) = 0;
    virtual std::string HostEvalStrQ(const std::string& js, int timeoutMs = 10000) = 0;
    virtual std::string HostEvalStrAsync(const std::string& js, int timeoutMs = 10000) = 0;
    virtual double      HostEvalNum(const std::string& js) = 0;
    virtual bool        HostEvalBool(const std::string& js) = 0;

    virtual std::string HostPopupText() = 0;
    virtual bool        HostCalibrateBoxOrigin(::HidMouse& m) = 0;
    virtual bool        HostClickElementJs(const std::string& selector) = 0;
    virtual bool        HostClickButtonByText(const std::string& text) = 0;
    virtual bool        HostFillQueryAccountPwd() = 0;

    virtual cdp::CDPSession* HostSession() = 0;
    virtual std::string      HostDownloadDir() = 0;
    virtual bool             HostSaveCapture(const std::string& path,
                                             const std::string& bytes) = 0;
    virtual void             HostMarkLoggedIn() = 0;

    // P4：版本变体使用的导出能力（路由/tag 由变体决定，宿主负责执行）
    virtual std::vector<std::string>
    HostDoQueryExport(const std::string& route, const std::string& begin,
                      const std::string& end, const std::string& tag,
                      bool receipt) = 0;
    virtual std::vector<std::string>
    HostDoExport(const std::string& route, const std::string& tag,
                 bool receipt) = 0;
    virtual std::vector<std::string>
    HostDoQueryExportPerRow(const std::string& route, const std::string& begin,
                            const std::string& end, const std::string& tag) = 0;
};

// ----------------------------------------------------------------------------
// 顶象滑块策略
// ----------------------------------------------------------------------------
class ISliderSolver {
public:
    virtual ~ISliderSolver() = default;
    virtual std::string name() const = 0;
    // 返回空串 = 通过；否则为失败原因（与原 SliderAttempt 完全一致）
    virtual std::string Attempt(IBcbAutomationHost& host, ::HidMouse& m) = 0;
};

class DingXiangSliderSolver : public ISliderSolver {
public:
    std::string name() const override { return "dingxiang-slider"; }
    std::string Attempt(IBcbAutomationHost& host, ::HidMouse& m) override;

private:
    // rel 增益标定（预热丢弃首步非线性，取中位数）
    double CalibrateGain(::HidMouse& m);
    // 最小 jerk + OU 噪声 + 11ms 时间轴回放（每帧实测位置闭环）
    bool TrackRel(::HidMouse& m, double dxCss, double dyCss, double T,
                  double gain, const char* label);
};

// ----------------------------------------------------------------------------
// 文字点选验证码策略
// ----------------------------------------------------------------------------
class IBcbCaptchaSolver {
public:
    virtual ~IBcbCaptchaSolver() = default;
    virtual std::string name() const = 0;
    virtual bool CanSolve(arch::CaptchaKind kind) const = 0;

    // 页面当前是否为本策略能处理的验证码
    virtual bool Detect(IBcbAutomationHost& host) = 0;
    // 返回空串 = 已处理成功；否则为失败原因
    virtual std::string SolveBcb(IBcbAutomationHost& host, ::HidMouse* m) = 0;
};

// 超级鹰 9800 自动点字。enableAuto=false 时仅保留实现，不参与责任链匹配，
// 用于保持当前"点选验证码直接转人工"的既定行为。
class ChaojiyingClickWordSolver : public IBcbCaptchaSolver {
public:
    explicit ChaojiyingClickWordSolver(bool enableAuto = false)
        : enableAuto_(enableAuto) {}

    std::string name() const override { return "chaojiying-clickword"; }
    bool CanSolve(arch::CaptchaKind kind) const override;
    bool Detect(IBcbAutomationHost& host) override;
    std::string SolveBcb(IBcbAutomationHost& host, ::HidMouse* m) override;
    void SetEnabled(bool enabled) { enableAuto_ = enabled; }

private:
    bool enableAuto_;
};

// 弹窗 + 蜂鸣呼叫人工，5 分钟看门狗自动停止（沿用原实现）
class HumanClickWordSolver : public IBcbCaptchaSolver {
public:
    std::string name() const override { return "human-clickword"; }
    bool CanSolve(arch::CaptchaKind kind) const override;
    bool Detect(IBcbAutomationHost& host) override;
    std::string SolveBcb(IBcbAutomationHost& host, ::HidMouse* m) override;
};

// 责任链：按注册顺序尝试。Detect 任一命中即为验证码；SolveBcb 依次尝试。
class BcbCaptchaChain {
public:
    void Add(std::unique_ptr<IBcbCaptchaSolver> solver);
    bool DetectClickWord(IBcbAutomationHost& host);
    std::string SolveBcb(IBcbAutomationHost& host, ::HidMouse* m = nullptr);

    // 运行期开关：默认关闭自动点字，保持现有行为；true 时自动策略优先。
    void SetAutoClickWordEnabled(bool enabled);

private:
    std::vector<std::unique_ptr<IBcbCaptchaSolver>> solvers_;
};

// 把 BCB 责任链桥接到 P0 的 arch::ICaptchaSolver，便于命令层统一按
// Result<T> 方式调用；字符串错误码原样保留在 Result 的 message 中。
class BcbCaptchaArchAdapter : public arch::ICaptchaSolver {
public:
    BcbCaptchaArchAdapter(IBcbAutomationHost& host, BcbCaptchaChain& chain)
        : host_(host), chain_(chain) {}

    std::string name() const override { return "bcb-captcha-adapter"; }
    bool CanSolve(arch::CaptchaKind kind) const override;
    arch::ResultVoid Solve(const arch::CaptchaContext& c) override;

private:
    IBcbAutomationHost& host_;
    BcbCaptchaChain&    chain_;
};

} // namespace strategy
} // namespace bcb
} // namespace newrpa
