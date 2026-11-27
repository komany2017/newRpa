#pragma once
// ============================================================================
// CMB 银行插件 — 招商银行非证书用户网银（U-Bank）
//
// 登录入口：https://u.ebank.cmbchina.com/ubcoreweb/ubcore/login
// 登录表单（CDP 探测 2026-11-27）：
//   * 登录名 input#loginName                 —— 普通 input，CDP 可注入
//   * 登录密码 div.cfca_passwaord_container   —— CFCA 安全控件，CDP char 事件可输入
//   * 附加码 input#extraCode + SVG <text>     —— 字符明文藏在 SVG text 节点，可直接解析
//
// 状态：骨架 + 登录方法已实现；业务取数（余额/流水/回单）待登录后探测页面再补。
// 凭据只从 BankConfig 读取，源码/日志不出现明文。
// ============================================================================

#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"
#include "newrpa/arch/StateStore.h"

#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace cmb {

class CMBPlugin : public plugin::IBankPlugin {
public:
    CMBPlugin();
    ~CMBPlugin() override;

    const char* GetId()          const override { return "CMB"; }
    const char* GetDisplayName() const override { return "招商银行非证书用户"; }
    uint32_t    Capabilities()    const override;

    void SetSession(std::shared_ptr<cdp::CDPSession> session,
                    std::shared_ptr<chrome::ChromeBrowser> browser) override;
    void SetConfig(const plugin::BankConfig& cfg) override;

    // P5: Result<T> 统一接口
    arch::Result<arch::LoginOutcome> Login() override;
    arch::Result<std::vector<std::map<std::string, std::string>>>
    GetCurBal() override;
    arch::Result<std::vector<plugin::Transaction>> GetCurTran() override;
    arch::Result<std::vector<plugin::Transaction>>
    GetHisTran(const plugin::DateRange& range) override;
    arch::Result<std::vector<std::string>>
    DownBankSlip(const plugin::DateRange& range) override;
    arch::Result<std::string>
    Payment(const std::string& amount, const std::string& nums,
            const std::string& paccno, const std::string& filename = "",
            const std::string& sn = "") override;
    arch::Result<std::string> Approval(const std::string& no) override;
    arch::Result<std::string> Query(const std::string& no) override;
    void Close() override;

private:
    // 登录实现（原字符串语义，由 Result 包装层转发）
    std::string LoginImpl();

    // 通用 helper
    std::string EvalStr(const std::string& js, int timeoutMs = 10000);
    bool        EvalBool(const std::string& js, int timeoutMs = 5000);
    bool        WaitForElement(const std::string& selector, int timeoutMs = 5000);

    // 附加码：从 SVG data URL 中解析 <text> 明文
    std::string ReadExtraCode();

    // 登录名/附加码注入（保留 React state 通过原生 setter + input 事件）
    bool SetInputValue(const std::string& selector, const std::string& value);

    // CFCA 密码控件：聚焦后逐字符 char 事件输入，回读圆点数补足
    bool TypePassword(const std::string& password);

private:
    plugin::BankConfig                     cfg_;
    std::shared_ptr<cdp::CDPSession>       session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;
    std::unique_ptr<cdp::PageDomain>       page_;
    std::unique_ptr<cdp::RuntimeDomain>    rt_;
    arch::LoginSessionState                loggedIn_;
};

} // namespace cmb
} // namespace newrpa

extern "C" {
    NEWRPA_API newrpa::plugin::IBankPlugin* CreatePlugin();
    NEWRPA_API void DestroyPlugin(newrpa::plugin::IBankPlugin* p);
}
