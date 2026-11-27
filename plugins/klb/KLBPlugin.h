#pragma once
// ============================================================================
// KLB 银行插件 — 昆仑银行企业网银（查询版）
//
// 入口: https://cbank-simp.klb.cn/eweb/prelogin.do?LoginType=Q&_locale=zh_CN&BankId=9999
// 科蓝 CSII eweb 老架构，密码走 iProtect 安全控件（驱动级钩子）。
//
// 登录核心策略（Python 原型 klb_login_final.py 已验证）：
//   1. CDP JS 填 CifNo/UserId（普通输入框）
//   2. CDP 合成鼠标点击 _safepass 控件聚焦（控件只挡合成键盘，不挡鼠标）
//   3. 硬件 HID 键盘盒子 kb_type.exe（HidKeyBoard，VID 0xC216/PID 0x0301）
//      发真实扫描码输密码 —— 合成键盘一律被控件拒绝（ERROR_CV_EMPTY）
//   4. JS 点 #Button，doLogin() 自动 execiProtectCommands(getvalue+getmfm) 提交
//
// 业务功能（页面内容全部在顶层 iframe#iframepage 内，同源可直取）：
//   - 注册账户余额  ActQryPre.do        （账户数据在 AcNoInfo radio value）
//   - 交易明细查询  ActTrsQryPre.do     （table.j_tabV + 分页）
//   - 回单下载      URMReceiptQryPre.do （URMPreviewReceipt.do 直出 PDF）
// ============================================================================

#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"
#include <newrpa/singleUsbbox/HidKeyBoard.h>  // 硬件 HID 键盘盒子

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace newrpa {
namespace klb {

class KLBPlugin : public plugin::IBankPlugin {
public:
    KLBPlugin();
    ~KLBPlugin() override;

    const char* GetId()           const override { return "KLB"; }
    const char* GetDisplayName()  const override { return "昆仑银行"; }
    uint32_t    Capabilities()     const override;

    void SetSession(std::shared_ptr<cdp::CDPSession> session,
                    std::shared_ptr<chrome::ChromeBrowser> browser) override;
    void SetConfig(const plugin::BankConfig& cfg) override;

    // P5: Result<T> 唯一接口；原实现保留为 XxxImpl，日志与行为不变
    arch::Result<arch::LoginOutcome> Login() override {
        arch::LoginOutcome out;
        out.raw = LoginImpl();
        out.loggedIn = (out.raw == "ok" || out.raw == "already logged in");
        return arch::Result<arch::LoginOutcome>::Ok(std::move(out));
    }
    arch::Result<std::vector<std::map<std::string, std::string>>>
    GetCurBal() override {
        return arch::Result<std::vector<std::map<std::string, std::string>>>::Ok(
            GetCurBalImpl());
    }
    arch::Result<std::vector<plugin::Transaction>> GetCurTran() override {
        return arch::Result<std::vector<plugin::Transaction>>::Ok(GetCurTranImpl());
    }
    arch::Result<std::vector<plugin::Transaction>>
    GetHisTran(const plugin::DateRange& range) override {
        return arch::Result<std::vector<plugin::Transaction>>::Ok(
            GetHisTranImpl(range));
    }
    arch::Result<std::vector<std::string>>
    DownBankSlip(const plugin::DateRange& range) override {
        return arch::Result<std::vector<std::string>>::Ok(
            DownBankSlipImpl(range));
    }
    arch::Result<std::string> Payment(const std::string& amount,
                                      const std::string& nums,
                                      const std::string& paccno,
                                      const std::string& filename,
                                      const std::string& sn) override {
        return arch::Result<std::string>::Ok(
            PaymentImpl(amount, nums, paccno, filename, sn));
    }
    arch::Result<std::string> Approval(const std::string& no) override {
        return arch::Result<std::string>::Ok(ApprovalImpl(no));
    }
    arch::Result<std::string> Query(const std::string& no) override {
        return arch::Result<std::string>::Ok(QueryImpl(no));
    }

    // --- 原实现（签名/日志逐字保留） ---
    std::string LoginImpl();
    std::vector<std::map<std::string, std::string>> GetCurBalImpl();
    std::vector<plugin::Transaction> GetCurTranImpl();
    std::vector<plugin::Transaction> GetHisTranImpl(
        const plugin::DateRange& range);
    std::vector<std::string> DownBankSlipImpl(
        const plugin::DateRange& range);
    std::string PaymentImpl(const std::string& amount,
                            const std::string& nums,
                            const std::string& paccno,
                            const std::string& filename,
                            const std::string& sn);
    std::string ApprovalImpl(const std::string& no);
    std::string QueryImpl(const std::string& no);
    void Close() override;

private:
    // --- 便捷 helper ---
    std::string EvalStr(const std::string& js, int timeoutMs = 10000);
    bool        EvalBool(const std::string& js, int timeoutMs = 5000);
    // 轮询某 JS 条件为真
    bool        WaitBool(const std::string& js, int timeoutMs = 15000,
                         int stepMs = 400);

    // iframe 文档前缀：顶层 iframe#iframepage（同源）
    static std::string IframeDoc();
    // 点顶层文档菜单（innerText 精确匹配），等 iframe.src 含 sub
    bool GotoMenu(const std::string& menuText, const std::string& iframeSub,
                  int timeoutMs = 20000);

    // 解析 Java bean toString 风格 "{k=v, k=v, ...}"
    static std::map<std::string, std::string> ParseBean(const std::string& s);

    // --- 登录专用 ---
    bool WaitLoginForm(int timeoutMs = 20000);
    // 找 Chrome 主窗口并强前台（ALT 松锁 + AttachThreadInput）
    bool FocusChromeWindow();

    // 硬件键盘按 Tab（从操作员框 Tab 进密码控件，走正常焦点导航）
    bool HardwareTab();

    // 取消 Chrome 置顶（输完密码恢复普通层级）
    void UnpinChromeTopmost();
    // 硬件键盘直接输文本（HidKeyBoard，先 backspace 清空）
    bool HardwareType(const std::string& text, int delayMs = 80);
    // 关掉可能弹着的 JS alert/confirm（密码错误提示），否则 CDP evaluate 被挂起
    void DismissJsDialog();

    // --- 业务 ---
    // 交易明细通用：打开 ActTrsQryPre.do，选账户填日期查询，返回记录 JSON
    std::string QueryTransJson(const std::string& begin, const std::string& end,
                               bool& ok);
    // 回单列表查询，返回 [{idx,date,amount,counterAcNo,payee}] JSON
    std::string QueryReceiptList(const std::string& begin,
                                 const std::string& end, bool& ok);
    // 取单笔回单 PDF（URMPreviewReceipt.do），失败返回空
    std::string FetchReceiptPdf(const std::string& rowIdx);
    // 页面 XHR 抓任意 URL，返回 base64（失败返回空）
    std::string XhrBase64(const std::string& url);
    static bool WriteBinaryFile(const std::string& path,
                                const std::string& content);
    static bool SaveTextFile(const std::string& path,
                             const std::string& content);
    static std::string NowStamp();          // YYYYMMDD_HHMMSS
    static std::string Today();             // YYYY-MM-DD

    // base64 解码（ receipt PDF 用）
    static std::string Base64Decode(const std::string& in);

private:
    plugin::BankConfig                     cfg_;
    std::shared_ptr<cdp::CDPSession>       session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;

    std::unique_ptr<cdp::PageDomain>       page_;
    std::unique_ptr<cdp::RuntimeDomain>    rt_;
    std::unique_ptr<cdp::InputDomain>      input_;

    bool loggedIn_ = false;

    // 登录失败计数：提交后没进业务页（密码错误等）就 +1，
    // 连续 2 次后 Login() 直接拒绝重试，防止反复输错把号锁了
    int loginFailCount_ = 0;

    // 最近一次 JS 弹窗内容（银行登录失败提示通常走 alert）
    std::string lastDialogMsg_;
    std::mutex  dialogMtx_;
    int         dialogEvHandle_ = 0;
};

} // namespace klb
} // namespace newrpa

// DLL 导出
extern "C" {
    NEWRPA_API newrpa::plugin::IBankPlugin* CreatePlugin();
    NEWRPA_API void DestroyPlugin(newrpa::plugin::IBankPlugin* p);
}
