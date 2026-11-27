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

#include "newrpa/arch/StateStore.h"
#include "newrpa/plugin/IBankPlugin.h"
#include "newrpa/cdp/CDPSession.h"
#include "newrpa/cdp/CDPDomains.h"
#include "newrpa/chrome/ChromeBrowser.h"
#include "strategy/BcbStrategy.h"
#include "variant/BcbVariant.h"

class HidMouse;   // core/include/newrpa/singleUsbbox/HidMouse.hpp（单头盒子硬件鼠标）

#include <memory>
#include <string>
#include <vector>

namespace newrpa {
namespace bcb {

class BCBPlugin : public plugin::IBankPlugin,
                  public strategy::IBcbAutomationHost {
public:
    BCBPlugin();
    ~BCBPlugin() override;

    const char* GetId()           const override { return "BOB"; }
    const char* GetDisplayName()  const override { return "北京银行企业银行查询版"; }
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


public:
    // --- P3: IBcbAutomationHost 桥接区（薄转发，行为与原私有 helper 完全一致） ---
    std::string HostEvalStr(const std::string& js) override { return EvalStr(js); }
    std::string HostEvalStrQ(const std::string& js, int timeoutMs = 10000) override {
        return EvalStrQ(js, timeoutMs);
    }
    std::string HostEvalStrAsync(const std::string& js, int timeoutMs = 10000) override {
        return EvalStrAsync(js, timeoutMs);
    }
    double HostEvalNum(const std::string& js) override { return EvalNum(js); }
    bool   HostEvalBool(const std::string& js) override { return EvalBool(js); }
    std::string HostPopupText() override { return PopupText(); }
    bool HostCalibrateBoxOrigin(::HidMouse& m) override { return CalibrateBoxOrigin(m); }
    bool HostClickElementJs(const std::string& s) override { return ClickElementJs(s); }
    bool HostClickButtonByText(const std::string& t) override { return ClickButtonByText(t); }
    bool HostFillQueryAccountPwd() override { return FillQueryAccountPwd(); }
    cdp::CDPSession* HostSession() override { return session_.get(); }
    std::string HostDownloadDir() override { return DownloadDir(); }
    bool HostSaveCapture(const std::string& p, const std::string& b) override {
        return WriteBinaryFile(p, b);
    }
    void HostMarkLoggedIn() override { loggedIn_.MarkLoggedIn(); }
    std::vector<std::string>
    HostDoQueryExport(const std::string& route, const std::string& begin,
                      const std::string& end, const std::string& tag,
                      bool receipt) override {
        return DoQueryExport(route, begin, end, tag, receipt);
    }
    std::vector<std::string>
    HostDoExport(const std::string& route, const std::string& tag,
                 bool receipt) override {
        return DoExport(route, tag, receipt);
    }
    std::vector<std::string>
    HostDoQueryExportPerRow(const std::string& route, const std::string& begin,
                            const std::string& end, const std::string& tag) override {
        return DoQueryExportPerRow(route, begin, end, tag);
    }

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
    // 查询版专用：从 el-table 提取「表头+数据」组成规范 JSON
    // （el-table 的 header table 与 body table 分离，通用 DumpTablesJson 会错位）
    std::string DumpQueryTableJson();

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
    void LogPageMessages();
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
    arch::Result<std::string> RunAllAccounts() override {
        return arch::Result<std::string>::Ok(RunAllAccountsImpl());
    }
    std::string RunAllAccountsImpl();

    // 混合模式：CDP 登录建会话后，用纯 HTTP 直连下载静态数据文件
    arch::Result<std::string> HttpDirect() override {
        return arch::Result<std::string>::Ok(HttpDirectImpl());
    }
    std::string HttpDirectImpl();

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
    // 顶象滑块一轮自动尝试（按下->起手->缺口->主拖->trim->松开->判定）
    // 返回空串 = 通过；否则返回失败原因
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
    // 检测顶象降级后的「文字点选验证码」（请依次点击 XX 的汉字）。
    // 这种不是滑块，RPA 无法自动识别，必须鸣笛转人工按顺序点字。
    // 返回 true = 页面当前是点选验证码。
    // 鸣笛呼叫人工处理点选验证码：不碰 HID、不 reload（刷掉验证码人工就没法点），
    // 只轮询页面直到离开 /login 或超时。返回空串=人工完成登录；否则失败原因。
    // 自动点选汉字验证码：抓验证码背景图 → 超级鹰 9800 拿到每个汉字坐标 →
    // 从页面提示解析目标字序列 → 按序用 HID 鼠标点击，再点「确定/验证」并等通过。
    // 返回空串=通过；否则失败原因（调用方再转人工）。
    // 容错版 EvalStr：页面忙/加载中导致的 CDP 超时只返回空串，不抛异常中断整个 run
    std::string EvalStrQ(const std::string& js, int timeoutMs = 10000);
    // 异步版 EvalStr：JS 返回 Promise（用于 crossOrigin 重新加载图片并转 dataURL）
    std::string EvalStrAsync(const std::string& js, int timeoutMs = 10000);
    // 关掉北京银行「网银证书运行环境已升级」弹窗（#xtxAlert）。
    // 页面逻辑：sessionStorage.certDownload 为真即不再弹；已弹出则直接移除 DOM。
    // 返回移除的弹窗个数（0 = 本来就没弹）。
    int DismissCertPopup();

    // ---- 查询版业务取数（复用演示版页面，区别仅在 origin 与日期设置）----
    // 当前配置是否为查询版（loginUrl 带 version=query）
    // 从 cfg_.loginUrl 提取 origin（scheme://host[:port]），供 GotoRoute 用
    std::string BaseOrigin();
    // 设置 Element UI 日期 range 组件（开始/结束日期），begin/end 为 YYYYMMDD
    // 查询版真实数据必须设置为 YYYYMMDD 数组，否则接口报「日期错误」
    bool SetQueryDateRange(const std::string& begin, const std::string& end);
    // 查询版四类数据统一入口：导航->设日期->确定->抓表->下载->落盘
    // 返回下载文件路径列表；dataTag 用于 data.json 文件名
    std::vector<std::string> DoQueryExport(const std::string& route,
                                           const std::string& begin,
                                           const std::string& end,
                                           const std::string& dataTag,
                                           bool receipt = false);
    // 从 el-table 的 Vue 数据模型里导出「全部字段」（含页面未显示的流水号/账号等）
    // 返回 JSON: [{tableIndex,fields:[...],rowCount,rows:[{k:v}]}]
    // 页面没有 el-table / 没有 __vue__ 时返回空串
    std::string DumpVueRowsJson();
    // 当前回单表格的数据行数（读不到返回 0）
    int CountReceiptRows();
    // 清空勾选后只勾第 rowIdx 行；返回空串=成功，否则失败原因
    std::string SelectReceiptRow(int rowIdx);
    // 点「生成回单」→ 选「方式一」→「确认」→「刷新」
    bool GenerateReceiptForSelection();
    // 逐条下载电子回单：一次只勾一行 → 生成 → 下载 → 落盘为 dataTag_NN_*
    // 返回下载文件路径列表（页面上没数据时回退批量）
    std::vector<std::string> DoQueryExportPerRow(const std::string& route,
                                                 const std::string& begin,
                                                 const std::string& end,
                                                 const std::string& dataTag);

private:
    std::unique_ptr<variant::BcbVariant>       variant_;
    std::unique_ptr<strategy::ISliderSolver>   sliderSolver_;
    std::unique_ptr<strategy::BcbCaptchaChain> captchaChain_;

    plugin::BankConfig                    cfg_;
    std::shared_ptr<cdp::CDPSession>      session_;
    std::shared_ptr<chrome::ChromeBrowser> browser_;

    std::unique_ptr<cdp::PageDomain>      page_;
    std::unique_ptr<cdp::RuntimeDomain>   rt_;
    std::unique_ptr<cdp::DomDomain>       dom_;
    std::unique_ptr<cdp::InputDomain>     input_;
    std::unique_ptr<cdp::NetworkDomain>   net_;

    arch::LoginSessionState loggedIn_;

    // Page.javascriptDialogOpening 事件句柄（自动接管原生 alert/confirm）
    int  jsDialogEvHandle_ = 0;
    // 控制线程兜底：accept 掉仍挂着的 JS 弹窗。
    // 弹窗挂起期间 Runtime.evaluate 会全部阻塞 -> 3s 超时 -> 异常直冲 MainRun
    // -> 栈展开时 ChromeLauncher::Shutdown() 杀掉 Chrome，F5 调试随之中断。
    void DismissBcbJsDialog();
    // 带容错的 EvalBool：单次超时只返回 false，不把异常抛到 MainRun
    bool EvalBoolQ(const std::string& js, int timeoutMs = 3000);
    // 取消 Chrome 置顶（登录前置顶后必须恢复，否则一直压住 VSCode 等窗口）
    void UnpinBcbChromeTopmost();
};

} // namespace bcb
} // namespace newrpa

// DLL 导出
extern "C" {
    NEWRPA_API newrpa::plugin::IBankPlugin* CreatePlugin();
    NEWRPA_API void DestroyPlugin(newrpa::plugin::IBankPlugin* p);
}
