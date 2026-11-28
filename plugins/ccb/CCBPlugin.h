#pragma once
// ============================================================================
// CCB 银行插件 — 中国建设银行企业网银（DemoV3 演示版）
//
// 登录入口：https://b2bstatic.ccb.com/V6/DemoV3/demo_login_account.html
// 结构（CDP 探测 2026-11-27）：
//   * 登录名    input#CUSTOMERID  普通 input（客户识别号，由页面预填）
//   * 操作员    input#USERID      普通 input（demo 预填 999999）
//   * 登录密码  input#LOG_PWD     普通 input，无安全控件、无验证码
//   * 登录按钮  input#LOGIN_BOX_BTN 初始 disabled，三字段非空后由 jQuery 解禁
//   * demo 版点击登录不做真实提交，按操作员代码直接跳转业务页：
//       999999 -> FN90001_manage.htm（主管版）
//       000002 -> FN90001_check.htm（复核版）
//       000001 -> FN90001_account.htm（账户版）
//
// 业务页是框架页，功能页通过 window.taskBar.createWin(code,title,url) 在
// iframe#win_<code> 内打开：
//   5111 -> FN00001.htm 活期账户（账户信息/实时余额/交易明细/历史余额）
//   0513 -> FN01001.htm 电子回单查询
//
// demo 限制：电子回单的「下载全部」是空桩（downAllN01A0A() 为 void(0)），
// 只能取回单列表，无法下载 PDF/OFD。
// ============================================================================

#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"
#include "newrpa/arch/StateStore.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace ccb {

class CCBPlugin : public plugin::IBankPlugin {
public:
    CCBPlugin();
    ~CCBPlugin() override;

    const char* GetId()          const override { return "CCB"; }
    const char* GetDisplayName() const override { return "中国建设银行（演示版）"; }
    uint32_t    Capabilities()    const override;

    void SetSession(std::shared_ptr<cdp::CDPSession> session,
                    std::shared_ptr<chrome::ChromeBrowser> browser) override;
    void SetConfig(const plugin::BankConfig& cfg) override;

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
    // ---- 基础 ----
    std::string LoginImpl();
    std::string EvalStr(const std::string& js, int timeoutMs = 15000);
    bool        EvalBool(const std::string& js, int timeoutMs = 5000);

    // ---- 框架页操作 ----
    // 调 taskBar.createWin 打开功能页；iframe id 为 win_<code>
    bool OpenFunc(const std::string& code, const std::string& title,
                  const std::string& url, int waitMs = 8000);
    // 等 iframe 就绪（contentDocument 可访问且 body 有文本）
    bool WaitFrame(const std::string& code, int timeoutMs = 15000);
    // 在功能页 iframe 内执行 JS，返回字符串结果
    std::string EvalInFrame(const std::string& code, const std::string& js,
                            int timeoutMs = 20000);

    // ---- 业务 ----
    // 打开活期账户页并勾选账户，返回是否成功
    bool PrepareAccount(const std::string& code);
    // 查询余额，返回 [{"account","name","currency","balance","available",...}]
    arch::Result<std::vector<std::map<std::string, std::string>>> QueryBalance();
    // 查询明细（日期格式 yyyyMMdd）
    arch::Result<std::vector<plugin::Transaction>>
    QueryDetail(const std::string& stDt, const std::string& edDt);

    static std::string NormalizeDate(const std::string& d);
    static std::string CleanAmt(const std::string& s);

private:
    plugin::BankConfig                     cfg_;
    std::shared_ptr<cdp::CDPSession>       session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;
    std::unique_ptr<cdp::PageDomain>       page_;
    std::unique_ptr<cdp::RuntimeDomain>    rt_;
    arch::LoginSessionState                loggedIn_;
    int                                    dialogEventHandle_ = -1;
};

} // namespace ccb
} // namespace newrpa

extern "C" {
    NEWRPA_API newrpa::plugin::IBankPlugin* CreatePlugin();
    NEWRPA_API void DestroyPlugin(newrpa::plugin::IBankPlugin* p);
}
