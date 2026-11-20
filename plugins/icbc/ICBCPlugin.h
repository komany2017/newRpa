#pragma once
// ============================================================================
// ICBC 银行插件 — 中国工商银行
//
// 基于 CDP + Chrome，替代旧项目里的 ICBCBANK + IE DOM 操控
// ============================================================================

#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"

#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace icbc {

class ICBCPlugin : public plugin::IBankPlugin {
public:
    ICBCPlugin();
    ~ICBCPlugin() override;

    const char* GetId()           const override { return "newrpa.icbc"; }
    const char* GetDisplayName()  const override { return "中国工商银行"; }
    uint32_t    Capabilities()     const override;

    void SetSession(std::shared_ptr<cdp::CDPSession> session,
                    std::shared_ptr<chrome::ChromeBrowser> browser) override;
    void SetConfig(const plugin::BankConfig& cfg) override;

    std::string Login() override;
    std::vector<std::map<std::string, std::string>> GetCurBal() override;
    std::vector<plugin::Transaction> GetCurTran() override;
    std::vector<plugin::Transaction> GetHisTran(
        const plugin::DateRange& range) override;
    std::vector<std::string> DownBankSlip(
        const plugin::DateRange& range) override;
    std::string Payment(const std::string& amount,
                        const std::string& nums,
                        const std::string& paccno,
                        const std::string& filename,
                        const std::string& sn) override;
    std::string Approval(const std::string& no) override;
    std::string Query(const std::string& no) override;
    void Close() override;

private:
    // --- 便捷 helper ---

    // 等元素出现（最多 timeoutMs 毫秒），返回 runtime objectId
    // selector: CSS selector，或带 frameId 前缀的形式
    bool WaitForElement(const std::string& selector,
                        int timeoutMs = 10000);

    // 用 JS 表达式执行，返回字符串值
    std::string EvalStr(const std::string& js);
    bool        EvalBool(const std::string& js);

    // 填 input 的值：selector = "#userName" 这种
    bool FillInput(const std::string& selector,
                   const std::string& value);

    // 点按钮/链接：两种方式都试（CDP Input.MouseClick + JS click）
    bool ClickElement(const std::string& selector);

    // JS 版 click，处理 iframe 里的元素
    bool ClickElementJs(const std::string& selector);

    // 等 URL 包含某子串（页面跳转）
    bool WaitUrlContains(const std::string& sub,
                         int timeoutMs = 15000);

    // 处理登录过程中的 UKey / 软键盘 / 验证码
    std::string HandleLoginPrompts();

    // 解析余额页面
    std::vector<std::map<std::string, std::string>> ParseCurBal();
    // 解析流水页面表格
    std::vector<plugin::Transaction>                ParseTransTable();

private:
    plugin::BankConfig                    cfg_;
    std::shared_ptr<cdp::CDPSession>      session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;

    // Domain 引用（从 session 构造）
    std::unique_ptr<cdp::PageDomain>      page_;
    std::unique_ptr<cdp::RuntimeDomain>   rt_;
    std::unique_ptr<cdp::DomDomain>       dom_;
    std::unique_ptr<cdp::InputDomain>     input_;

    bool loggedIn_ = false;
};

} // namespace icbc
} // namespace newrpa

// DLL 导出
extern "C" {
    NEWRPA_API newrpa::plugin::IBankPlugin* CreatePlugin();
    NEWRPA_API void DestroyPlugin(newrpa::plugin::IBankPlugin* p);
}
