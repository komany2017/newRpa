// P3：文字点选验证码策略。三段实现逐字搬自 BCBPlugin.cpp，
// 仅把插件私有能力调用改为 IBcbAutomationHost 接口调用。
#include "BcbStrategy.h"
#include "newrpa/util/StringUtils.h"
#include "newrpa/ocr/ChaojiyingOcr.h"
#include "newrpa/singleUsbbox/HidMouse.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
namespace newrpa {
namespace bcb {
namespace strategy {

// ----------------------------------------------------------------------------
// 顶象降级后的「文字点选验证码」检测与人工呼叫
// 图中样式：一张背景图 + 「请依次点击 XX 的汉字」，人工要按顺序点汉字。
// 这不是滑块，RPA 无法自动识别 —— 一旦出现必须鸣笛叫人，且不能 reload
// （刷新会换一张验证码，人工白点）。
// ----------------------------------------------------------------------------
bool ChaojiyingClickWordSolver::Detect(IBcbAutomationHost& host) {
    // 用可见文本判断，兼容顶象各种 DOM 结构版本：
    // 典型提示：「请依次点击 美 的汉字」「请点击图中的XX」「依次点击…汉字」
    std::string js =
        "(function(){var t=(document.body.innerText||'').replace(/\\s+/g,'');"
        // 明确命中：依次点击 + 汉字/文字/点击
        "if(/依次点击/.test(t))return true;"
        "if(/请点击.{0,10}(汉字|文字|字)/.test(t))return true;"
        "if(/点击.{0,6}(图中|下面|下列).{0,6}(汉字|文字|字)/.test(t))return true;"
        // 顶象文字点选容器特征（不同版本 class 名）
        "var sel=['.dx_captcha_word','.dx_captcha_click','.dx_captcha_basic_click',"
        "'.dx_captcha_text','.yidun_text_click','[class*=word-click]','[class*=text-click]'];"
        "for(var s of sel){var e=document.querySelector(s);"
        "if(e&&e.getBoundingClientRect().width>0)return true;}"
        "return false;})()";
    bool hit = host.HostEvalBool(js);
    if (hit) spdlog::warn("[BCB] 检测到「文字点选验证码」（需人工按顺序点汉字）");
    return hit;
}

// 把一个 UTF-8 字符串按字符切开（中文点选提示里可能有多个目标字）
static std::vector<std::string> Utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// 自动点选汉字验证码（超级鹰 9800 返回「字,x,y|字,x,y」，坐标相对上传的图）：
//   1) 找到验证码背景 img/canvas 元素，取它在 viewport 的位置和尺寸
//   2) 用 CDP clip 只截这个元素 -> 上传超级鹰 9800 -> 得到每个字的图内坐标
//   3) 从「请依次点击 XX 的汉字」解析目标字序列
//   4) 按目标字顺序匹配 OCR 坐标，换算成物理坐标，用 HID 逐个点击
//   5) 点「确认/验证」，等验证码通过
// 返回空串=通过；否则失败原因（调用方转人工）。
std::string ChaojiyingClickWordSolver::SolveBcb(IBcbAutomationHost& host, ::HidMouse* m)
{
    if (!m) return "clickword-no-mouse";
    if (!newrpa::ocr::GlobalOcrReady()) {
        spdlog::warn("[BCB] 超级鹰打码未配置，无法自动点选");
        return "ocr-not-ready";
    }
    // 点字用物理坐标，必须先标定 CSS->物理 的 origin（滑块链路也会标定，
    // 这里独立兜底，保证从任意调用点进来都能正确换算）。
    if (!host.HostCalibrateBoxOrigin(*m)) {
        spdlog::warn("[BCB] 点选前 origin 标定失败");
        return "clickword-origin-failed";
    }

    // 1) 定位验证码图片元素：顶象点选一般在 .dx_captcha_* 容器里的 img/canvas。
    //    取面积最大且可见的那张作为背景图。
    std::string geoJs =
        "(function(){"
        "var cands=[];"
        "document.querySelectorAll('canvas,img').forEach(function(e){"
        "var r=e.getBoundingClientRect();"
        "if(r.width<120||r.height<60)return;"
        "if(getComputedStyle(e).display==='none')return;"
        "var cls=(e.className||'')+' '+(e.parentElement?e.parentElement.className:'');"
        "if(cls.indexOf('captcha')<0 && r.width>360)return;"  // 无 captcha 线索时仅收接近整宽的
        "cands.push({x:r.left,y:r.top,w:r.width,h:r.height,a:r.width*r.height,cls:cls.slice(0,60)});"
        "});"
        "cands.sort(function(a,b){return b.a-a.a});"
        // 关键发现：目标字（「请依次点击 XX 的汉字」里的 XX）是一张独立 PNG 图片，
        // 位于 .dx_captcha_clickword_hint-cnt img（112x25），不在任何 DOM 文本里。
        // 必须把这张小图送 OCR 才能读出要点哪个字。
        "var hintImg='';"
        "var hi=document.querySelector('.dx_captcha_clickword_hint-cnt img');"
        "if(hi&&hi.src&&hi.src.indexOf('data:image')===0)hintImg=hi.src;"
        "var t=(document.body.innerText||'').replace(/\\s+/g,'');"
        "return JSON.stringify({img:cands[0]||null,hintImg:hintImg,body:t.slice(-150)});"
        "})()";
    std::string geo = host.HostEvalStrQ(geoJs, 8000);
    // 日志不打印巨大的 hintImg base64，只摘要
    {
        std::string brief = geo;
        auto hp = brief.find("\"hintImg\":\"data:");
        if (hp != std::string::npos) {
            auto he = brief.find("\",\"", hp);
            if (he != std::string::npos) brief.replace(hp + 11, he - hp - 11, "...(base64)");
        }
        spdlog::info("[BCB] 点选验证码 geo: {}", brief.substr(0, 900));
    }
    if (geo.empty()) return "clickword-geo-failed";

    double imgX = 0, imgY = 0, imgW = 0, imgH = 0;
    std::string hintImg;
    try {
        auto j = nlohmann::json::parse(geo);
        hintImg = j.value("hintImg", "");
        if (!j.contains("img") || j["img"].is_null()) return "clickword-img-not-found";
        imgX = j["img"].value("x", 0.0);
        imgY = j["img"].value("y", 0.0);
        imgW = j["img"].value("w", 0.0);
        imgH = j["img"].value("h", 0.0);
    } catch (...) { return "clickword-geo-parse-failed"; }
    if (imgW < 120 || imgH < 60) return "clickword-img-too-small";

    // 2) 用 CDP clip 只截验证码元素。
    //    captureScreenshot 的 clip 是 CSS 像素（deviceScaleFactor=1），
    //    返回的 PNG 尺寸 ≈ imgW x imgH，OCR 坐标即图内坐标。
    nlohmann::json clip = {
        {"x", imgX}, {"y", imgY}, {"width", imgW}, {"height", imgH}, {"scale", 1}
    };
    std::string b64;
    try {
        auto r = host.HostSession()->Call("Page.captureScreenshot",
                                {{"format", "png"}, {"clip", clip}}, 15000);
        b64 = r.value("data", "");
    } catch (const std::exception& e) {
        spdlog::warn("[BCB] 点选截图失败: {}", e.what());
        return "clickword-capture-failed";
    }
    const std::string kPre = "data:image/png;base64,";
    std::string dataUrl = kPre + b64;
    // 落盘一份便于离线复核
    try {
        auto png = util::Base64Decode(b64);
        host.HostSaveCapture((fs::path(host.HostDownloadDir()) / "_clickword_capture.png").string(),
                        std::string(reinterpret_cast<const char*>(png.data()), png.size()));
    } catch (...) {}

    // 3) 超级鹰 9800：返回「字,x,y|字,x,y」
    std::string code, ocrErr;
    newrpa::ocr::ChaojiyingOcr ocr(newrpa::ocr::GlobalOcrCred());
    if (!ocr.OcrFromDataUrl(dataUrl, "9800", code, ocrErr)) {
        spdlog::warn("[BCB] 点选 OCR 失败: {}", ocrErr);
        return "clickword-ocr-failed: " + ocrErr;
    }
    spdlog::info("[BCB] 点选 OCR 结果: {}", code);

    // 解析成 {字 -> {x,y}}（同名取第一个）
    std::map<std::string, std::pair<int,int>> pos;
    {
        std::stringstream ss(code);
        std::string seg;
        while (std::getline(ss, seg, '|')) {
            // 从右往左找两个逗号：字,x,y（字本身可能含 UTF-8 多字节但不含逗号）
            auto p2 = seg.rfind(',');
            if (p2 == std::string::npos) continue;
            auto p1 = seg.rfind(',', p2 - 1);
            if (p1 == std::string::npos) continue;
            std::string ch = seg.substr(0, p1);
            int x = std::atoi(seg.substr(p1 + 1, p2 - p1 - 1).c_str());
            int y = std::atoi(seg.substr(p2 + 1).c_str());
            if (!ch.empty() && !pos.count(ch)) pos[ch] = {x, y};
        }
    }
    if (pos.empty()) return "clickword-ocr-empty";

    // 4) 目标字识别：目标字是独立 PNG（.dx_captcha_clickword_hint-cnt img，112x25）
    //    上的扭曲艺术字，普通文字题(1004/2004)识别不准；实测点选定位题 9800 能返回中文
    //    （形如「字,x,y|字,x,y」），取其中的汉字与候选图 OCR 结果求交集 = 要点的目标字。
    std::string hintText;
    if (hintImg.empty()) {
        spdlog::warn("[BCB] 未找到目标字提示图片(hint-cnt img)，转人工");
        return "clickword-hintimg-not-found";
    }
    {
        std::string hintErr;
        if (!ocr.OcrFromDataUrl(hintImg, "9800", hintText, hintErr)) {
            spdlog::warn("[BCB] 目标字提示图 OCR(9800) 失败: {}", hintErr);
            return "clickword-hint-ocr-failed: " + hintErr;
        }
        spdlog::info("[BCB] 目标字提示图 OCR(9800) 原始结果: {}", hintText);
    }
    // 9800 返回「字,x,y|...」。关键：超级鹰返回的中文可能是 UTF-8 中文，也可能是
    // 字面 \uXXXX 转义串（取决于 OcrFromDataUrl 是否解码）。候选图 pos 的 key 用的是
    // 同样的原始串，因此这里必须用完全相同的切法（逗号前一段）才能求交集——不能用
    // 中文字节判断去过滤，否则字面 \uXXXX 会被当成 ASCII 丢掉。
    std::vector<std::string> hintChars;
    {
        std::stringstream ss(hintText);
        std::string seg;
        while (std::getline(ss, seg, '|')) {
            auto p = seg.find(',');
            std::string key = (p == std::string::npos) ? seg : seg.substr(0, p);
            if (!key.empty()) hintChars.push_back(key);
        }
    }
    if (hintChars.empty()) {
        spdlog::warn("[BCB] 目标字提示图未解析出字 (raw={})，转人工", hintText);
        return "clickword-hint-empty";
    }
    // 与候选字 OCR 字集求交集，命中即目标字（同一字只点一次）
    std::vector<std::pair<int,int>> targets;
    std::string picked;
    std::set<std::string> used;
    for (const auto& ch : hintChars) {
        if (used.count(ch)) continue;
        auto it = pos.find(ch);
        if (it != pos.end()) {
            targets.push_back(it->second);
            picked += ch;
            used.insert(ch);
        }
    }
    spdlog::info("[BCB] 点选目标字 提示图字集={} 命中={} 个数={}",
                 hintText, picked, targets.size());
    if (targets.empty()) return "clickword-no-target-match";

    // 5) 按序点击：captureScreenshot 在真实 DPI 下返回的图被放大了 scale 倍
    //    （本机 DPI 120/96=1.25），所以 OCR 图内像素要先 /scale 还原成 CSS 偏移，
    //    再叠加元素在 viewport 的 CSS 位置，最后经 origin/scale 转物理。
    //    每个字之间加拟人小停顿，点击前先移过去停一下再按下。
    double dpr = m->scale();
    spdlog::info("[BCB] 点选坐标换算: 截图dpr={:.3f} 元素viewport=({:.0f},{:.0f},{:.0f}x{:.0f})",
                 dpr, imgX, imgY, imgW, imgH);
    for (size_t i = 0; i < targets.size(); ++i) {
        double cssX = imgX + targets[i].first  / dpr;
        double cssY = imgY + targets[i].second / dpr;
        int physX = (int)m->cssToPhysX(cssX);
        int physY = (int)m->cssToPhysY(cssY);
        spdlog::info("[BCB] 点字[{}] 图内=({},{}) css=({:.0f},{:.0f}) 物理=({},{})",
                     i + 1, targets[i].first, targets[i].second, cssX, cssY, physX, physY);
        m->moveTo(physX, physY);
        Sleep(180 + (i % 3) * 70);
        m->leftDown();
        Sleep(90);
        m->leftUp();
        Sleep(320);
    }

    // 6) 点「确认/验证/确定」按钮（若有）
    host.HostClickElementJs(".dx_captcha_basic_btn, .dx_captcha_basic_submit");
    Sleep(600);
    host.HostClickButtonByText("确认");
    host.HostClickButtonByText("确定");
    Sleep(1800);

    // 7) 等验证码通过：容器 display:none 或含 success
    for (int i = 0; i < 12; ++i) {
        std::string st = host.HostEvalStrQ(
            "(function(){var c=document.querySelector('.dx_captcha_basic_content');"
            "var cap=document.getElementById('captcha');"
            "return JSON.stringify({disp:c?getComputedStyle(c).display:'gone',"
            "txt:(document.body.innerText||'').indexOf('验证成功')>=0,"
            "cap:cap?cap.className:''});})()", 5000);
        try {
            auto j = nlohmann::json::parse(st);
            if (j.value("disp", "") == "none" || j.value("txt", false) ||
                j.value("cap", "").find("success") != std::string::npos) {
                spdlog::info("[BCB] 点选验证码已通过");
                return "";
            }
        } catch (...) {}
        Sleep(600);
    }

    // 8) 验证码通过但还没登录：确保账号密码还在，点「登录」提交，等跳转离开 /login
    if (!host.HostPopupText().empty()) return "clickword-popup: " + host.HostPopupText();
    std::string plen = host.HostEvalStrQ(
        "(function(){try{return ''+pgeCtrl.pwdLength()}catch(e){return 'E'}})()");
    if (plen != "6") {
        spdlog::info("[BCB] 点选通过后密码位数={}，补填一次", plen);
        if (!host.HostFillQueryAccountPwd()) return "clickword-refill-failed";
    }
    host.HostClickButtonByText("登录");
    for (int i = 0; i < 25; ++i) {
        Sleep(1000);
        std::string url = host.HostEvalStrQ("location.href", 5000);
        if (!url.empty() && url.find("/login") == std::string::npos) {
            spdlog::info("[BCB] 自动点选验证码后登录成功: {}", url);
            return "";
        }
    }
    return "clickword-submit-no-redirect";
}

std::string HumanClickWordSolver::SolveBcb(IBcbAutomationHost& host, ::HidMouse*) {
    const int waitS = 300;   // 固定 5 分钟；未点击确定就自动结束登录
    spdlog::warn("[BCB] >>>>> 文字点选验证码：弹出对话框请人工处理，点确定后继续（5分钟未点自动停止）<<<<<");
    Beep(1000, 200);

    const wchar_t* title = L"北京银行查询版 - 需要人工处理验证码";
    const wchar_t* text =
        L"检测到顶象「文字点选验证码」，无法自动识别。\n\n"
        L"请在浏览器窗口中：\n"
        L"  1) 按提示依次点击正确的汉字完成验证；\n"
        L"  2) 如需登录，请点「登录」按钮完成登录。\n\n"
        L"完成后回到本对话框点「确定」，程序将自动继续获取余额、流水等信息。\n"
        L"注意：5 分钟内未点确定，程序将自动停止登录。";

    // 对话框是模态阻塞的，用 watchdog 线程在 waitS 秒后找到该窗口并关闭它，
    // 实现「5 分钟未点确定自动停止」。timedOut 标记区分是用户点的还是超时关的。
    std::atomic<bool> timedOut{false};
    std::atomic<bool> dismissed{false};
    std::thread watchdog([&]() {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(waitS);
        while (!dismissed.load() &&
               std::chrono::steady_clock::now() < deadline) {
            Sleep(300);
        }
        if (dismissed.load()) return;          // 用户已点确定
        HWND h = FindWindowW(nullptr, title);
        if (h) {
            timedOut.store(true);
            spdlog::warn("[BCB] 人工 5 分钟未点确定，自动关闭对话框并停止登录");
            PostMessageW(h, WM_CLOSE, 0, 0);   // 让 MessageBox 返回
        }
    });

    MessageBoxW(nullptr, text, title, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    dismissed.store(true);
    watchdog.join();

    if (timedOut.load()) {
        return "need-manual: click-word-captcha-timeout";
    }
    spdlog::info("[BCB] 人工已点对话框确定，检查登录状态并继续");

    // 点确定后给一段缓冲等 URL 离开 /login（用户可能确定后才点登录）。
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(
               std::chrono::steady_clock::now() - t0).count() < 30.0) {
        std::string url = host.HostEvalStrQ("location.href", 5000);
        if (url.find("/login") == std::string::npos && !url.empty()) {
            host.HostMarkLoggedIn();
            spdlog::info("[BCB] 人工完成点选验证码并登录成功: {}", url);
            return "";
        }
        Sleep(700);
    }
    return "need-manual: login-not-complete-after-confirm";
}


// ---------------------------------------------------------------------------
// 策略注册 / 责任链 / 与 P0 arch 接口的桥接
// ---------------------------------------------------------------------------
bool ChaojiyingClickWordSolver::CanSolve(arch::CaptchaKind k) const {
    return enableAuto_ && k == arch::CaptchaKind::ClickWord;
}

bool HumanClickWordSolver::Detect(IBcbAutomationHost&) {
    // 人工策略不自行判定页面；检测统一由超级鹰点选检测器负责。
    return false;
}

bool HumanClickWordSolver::CanSolve(arch::CaptchaKind k) const {
    return k == arch::CaptchaKind::ClickWord;
}

void BcbCaptchaChain::Add(std::unique_ptr<IBcbCaptchaSolver> solver) {
    if (solver) solvers_.push_back(std::move(solver));
}

bool BcbCaptchaChain::DetectClickWord(IBcbAutomationHost& host) {
    for (const auto& s : solvers_) {
        if (s->Detect(host)) return true;
    }
    return false;
}

std::string BcbCaptchaChain::SolveBcb(IBcbAutomationHost& host, ::HidMouse* m) {
    std::string last;
    for (const auto& s : solvers_) {
        if (!s->CanSolve(arch::CaptchaKind::ClickWord)) continue;
        last = s->SolveBcb(host, m);
        if (last.empty()) return last;
    }
    return last.empty() ? "captcha.no_solver" : last;
}

void BcbCaptchaChain::SetAutoClickWordEnabled(bool enabled) {
    for (auto& s : solvers_) {
        auto* p = dynamic_cast<ChaojiyingClickWordSolver*>(s.get());
        if (p) p->SetEnabled(enabled);
    }
}

bool BcbCaptchaArchAdapter::CanSolve(arch::CaptchaKind k) const {
    return k == arch::CaptchaKind::ClickWord;
}

arch::ResultVoid BcbCaptchaArchAdapter::Solve(const arch::CaptchaContext& c) {
    std::string r = chain_.SolveBcb(host_, c.mouse);
    if (r.empty()) return arch::MakeOk();
    return arch::MakeFail("bcb.captcha", r);
}

} // namespace strategy
} // namespace bcb
} // namespace newrpa
