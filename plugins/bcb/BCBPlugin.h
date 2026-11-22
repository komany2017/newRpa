#pragma once
// ============================================================================
// BCB 银行插件 — 北京银行企业网银（标准版演示版）
//
// CDP + Chrome 驱动，功能：登录 / 当日余额 / 当日交易 / 历史交易 / 电子回单
// 演示版入口：https://cbank.bankofbeijing.com.cn:9081/login
//
// 核心策略：点银行"导出 Excel/CSV"按钮 → CDP Network 域拦截 response body → 存本地
//           如果页面没导出按钮，fallback 到 DOM 抓表格
// ============================================================================

#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"

class HidMouse;   // core/include/newrpa/singleUsbbox/HidMouse.hpp（单头盒子硬件鼠标）

#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace bcb {

class BCBPlugin : public plugin::IBankPlugin {
public:
    BCBPlugin();
    ~BCBPlugin() override;

    const char* GetId()           const override { return "newrpa.bcb"; }
    const char* GetDisplayName()  const override { return "北京银行演示版"; }
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
    bool WaitForElement(const std::string& selector,
                        int timeoutMs = 10000);
    std::string EvalStr(const std::string& js);
    bool        EvalBool(const std::string& js);
    double      EvalNum(const std::string& js);
    bool        FillInput(const std::string& selector,
                          const std::string& value);
    bool        ClickElementJs(const std::string& selector);
    bool        WaitUrlContains(const std::string& sub,
                                int timeoutMs = 15000);

    // 打开侧边菜单项（北京银行用 frame + 菜单树）
    bool OpenMenu(const std::vector<std::string>& menuPath);

    // 点页面上的 "导出/下载 Excel/CSV" 按钮 —— 返回 true 表示找到并点击
    bool ClickExportButton();

    // 用 NetworkDomain 拦截下载，存到指定路径
    // urlKeyword: 响应 URL 包含的关键词（空=不限）
    // contentTypeKeyword: Content-Type 包含的关键词
    // 返回捕获到的文件本地路径列表
    std::vector<std::string> CaptureAndSaveDownload(
        const std::string& urlKeyword = "",
        const std::string& contentTypeKeyword = "spreadsheet",
        int timeoutMs = 15000);

    // 从 bytes 数组写文件
    static bool WriteBinaryFile(const std::string& path,
                                const std::string& content);

    // 解析交易流水表格（DOM 方式 fallback）
    std::string ScrapeTableJson();

    // 页面所有文本（诊断用）
    std::string PageDiag();

    // ---- 演示版专用流程（Python CDP 脚本已验证）----
    // 绝对下载目录（进程工作目录下的 downloads）
    static std::string DownloadDir();
    // Browser.setDownloadBehavior 授权原生下载到 DownloadDir
    bool EnableNativeDownloads();
    // Page.navigate 到 BASE/route 并等待渲染
    bool GotoRoute(const std::string& route);
    // 按按钮文字精确点击（演示版按钮无 id/type）
    bool ClickButtonByText(const std::string& text);
    // 点击弹窗(.el-message-box/.el-dialog)里含关键词的按钮
    bool ClickDialogButton(const std::string& text);
    // 抓取页面所有表格为 JSON（含表头行）
    std::string DumpTablesJson();
    // 写文本到文件（自动建目录）
    static bool SaveTextFile(const std::string& path, const std::string& content);
    // 点击"下载"并处理二次确认弹窗
    void ClickDownloadWithConfirm();
    // 轮询下载目录，把 before 之后出现的新文件重命名为 tag_原名
    std::vector<std::string> WaitForNewFiles(
        const std::vector<std::string>& before,
        const std::string& tag, int timeoutMs = 12000);
    // 通用：导航->确定->抓表->下载->落盘；receipt=true 时先勾选/生成/选方式
    std::vector<std::string> DoExport(const std::string& route,
                                      const std::string& tag,
                                      bool receipt = false);

    // ---- 账号遍历 + 增量时间（all-accounts 流程）----
    // 读某个 el-select 的 Vue 选项 [{label, value}]
    std::vector<std::pair<std::string,std::string>> SelOptions(int index);
    // 用 Vue handleOptionSelect 切换某个 el-select 到指定 value
    bool SetSelByValue(int index, const std::string& value);
    // 当前 el-select 的 value
    std::string SelValue(int index);
    // 历史交易增量时间状态文件路径
    static std::string StateFilePath();
    // 读上次成功获取历史交易的 end（无则返回空）
    static std::string LoadLastHisEnd();
    // 保存本次历史交易 end（作为下次 begin）
    static bool SaveLastHisEnd(const std::string& endYmd);
    // 计算默认半年前的日期 YYYYMMDD
    static std::string HalfYearAgoYmd();
    // 把 YYYYMMDD 转成 YYYY-MM-DD
    static std::string YmdToDash(const std::string& ymd);
    // all-accounts：遍历所有账号下载四类数据；返回统计信息
    std::string RunAllAccounts();

    // 混合模式：CDP 登录建会话后，用纯 HTTP 直连下载静态数据文件
    std::string HttpDirect() override;

    // ---- 查询版专用流程（Python boj_login_complete.py / boj_box.py 已验证）----
    // 注意：查询版与演示版完全不同——账号+密码+顶象滑块，无企业选择、无证书密码，
    // 密码走 pgeCtrl 安全控件（必须硬件键盘输入），入口不带 :9081 端口。
    // 登录成功标志：URL 离开 /login 进入 /queryHome。
    std::string LoginQuery();
    // 盒子键盘拟人输入（IME 隔离 + 字间随机停顿 + 长顿）
    bool HardwareType(const std::string& text, int delayMs);
    // CDP mousemove 反推 origin（css->物理），并 setOrigin 到鼠标对象
    bool CalibrateBoxOrigin(HidMouse& m);
    // rel 增益标定（预热丢弃首步非线性，取中位数）
    double CalibrateGain(HidMouse& m);
    // 最小 jerk + OU 噪声 + 8ms 时间轴回放（每 80ms 实测位置校正）
    bool TrackRel(HidMouse& m, double dxCss, double dyCss, double T,
                  double gain, const char* label);
    // 顶象滑块一轮自动尝试（按下->起手->缺口->主拖->trim->松开->判定）
    // 返回空串 = 通过；否则返回失败原因
    std::string SliderAttempt(HidMouse& m);
    // 鸣笛呼唤人工：不碰 HID，等真人拖滑块（最长 150s），成功后自动点登录
    // 返回空串 = 登录成功；否则失败原因
    std::string ManualTakeover();
    // 查询版：填账号（CDP insertText）+ 盒子键盘输密码（pgeCtrl 校验 6 位）
    bool FillQueryAccountPwd();
    // 页面上可见的 .el-message-box/.el-dialog 文本（空串 = 无弹窗）
    std::string PopupText();
    // 点掉 Element UI 提示弹窗的「确定」按钮（返回 true = 已点）
    // 只处理 .el-message-box/.el-dialog；证书弹窗 #xtxAlert 的「确定」会下载 exe，不在此处理
    bool ClickPopupOk();
    // 容错版 EvalStr：页面忙/加载中导致的 CDP 超时只返回空串，不抛异常中断整个 run
    std::string EvalStrQ(const std::string& js, int timeoutMs = 10000);
    // 关掉北京银行「网银证书运行环境已升级」弹窗（#xtxAlert）。
    // 页面逻辑：sessionStorage.certDownload 为真即不再弹；已弹出则直接移除 DOM。
    // 返回移除的弹窗个数（0 = 本来就没弹）。
    int DismissCertPopup();

private:
    plugin::BankConfig                    cfg_;
    std::shared_ptr<cdp::CDPSession>      session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;

    std::unique_ptr<cdp::PageDomain>      page_;
    std::unique_ptr<cdp::RuntimeDomain>   rt_;
    std::unique_ptr<cdp::DomDomain>       dom_;
    std::unique_ptr<cdp::InputDomain>     input_;
    std::unique_ptr<cdp::NetworkDomain>   net_;

    bool loggedIn_ = false;
};

} // namespace bcb
} // namespace newrpa

// DLL 导出
extern "C" {
    NEWRPA_API newrpa::plugin::IBankPlugin* CreatePlugin();
    NEWRPA_API void DestroyPlugin(newrpa::plugin::IBankPlugin* p);
}
