#pragma once
// ============================================================================
// 策略接口：验证码 / 流水抓取 / 回单下载
//
// 只定义抽象，不含实现。现有 BCBPlugin 里的具体函数可逐步包成策略类，
// 主流程不再依赖"顶象滑块"或"超级鹰"这类具体实现。
// ============================================================================

#include "newrpa/arch/Result.h"
#include "newrpa/plugin/IBankPlugin.h"

#include <memory>
#include <string>
#include <vector>

// HidMouse 定义在全局命名空间（newrpa/singleUsbbox/HidMouse.hpp）
class HidMouse;

namespace newrpa {
namespace cdp { class CDPSession; }
namespace chrome { class ChromeBrowser; }

namespace arch {

enum class CaptchaKind {
    None,
    Slider,      // 顶象滑块
    ClickWord,   // 文字点选
    ImageCode    // 普通图片验证码
};

struct CaptchaContext {
    cdp::CDPSession*    session = nullptr;
    chrome::ChromeBrowser* browser = nullptr;
    HidMouse*           mouse = nullptr;   // 无硬件时为空
    CaptchaKind         kind = CaptchaKind::None;
    std::string         prompt;             // 页面提示文本，如 "请依次点击 XX"
    std::string         imageDataUrl;       // 验证码图片 dataURL（可选）
    int                 timeoutMs = 300000; // 人工介入默认 5 分钟
};

struct ICaptchaSolver {
    virtual ~ICaptchaSolver() = default;
    virtual std::string name() const = 0;
    virtual bool CanSolve(CaptchaKind kind) const = 0;
    virtual ResultVoid Solve(const CaptchaContext& c) = 0;
};

// 责任链：按注册顺序尝试，失败且可重试时降级到下一个策略
class CaptchaChain : public ICaptchaSolver {
public:
    void Add(std::unique_ptr<ICaptchaSolver> solver) {
        if (solver) solvers_.push_back(std::move(solver));
    }

    std::string name() const override { return "captcha-chain"; }

    bool CanSolve(CaptchaKind kind) const override {
        for (const auto& s : solvers_)
            if (s->CanSolve(kind)) return true;
        return false;
    }

    ResultVoid Solve(const CaptchaContext& c) override {
        ResultVoid last = MakeFail("captcha.no_solver",
                                   "没有能处理该验证码的策略");
        for (const auto& s : solvers_) {
            if (!s->CanSolve(c.kind)) continue;
            auto r = s->Solve(c);
            if (r.ok()) return r;
            last = r;
        }
        return last;
    }

private:
    std::vector<std::unique_ptr<ICaptchaSolver>> solvers_;
};

// 流水抓取策略：DOM / Vue 数据模型 / HTTP 直连
struct ITransactionExtractor {
    virtual ~ITransactionExtractor() = default;
    virtual std::string name() const = 0;
    virtual Result<std::vector<plugin::Transaction>>
    Extract(cdp::CDPSession& session, const plugin::DateRange& range) = 0;
};

// 回单下载策略：逐条勾选 / 批量打包
struct DownloadedFile {
    std::string path;     // 本地文件路径
    std::string sn;       // 对应流水号（空 = 按下载顺序匹配）
    std::string checksum; // 可选校验值
};

struct IReceiptDownloader {
    virtual ~IReceiptDownloader() = default;
    virtual std::string name() const = 0;
    virtual Result<std::vector<DownloadedFile>>
    Download(cdp::CDPSession& session,
             const std::vector<plugin::Transaction>& rows) = 0;
};

} // namespace arch
} // namespace newrpa
