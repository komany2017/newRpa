// P3：顶象滑块自动尝试策略。算法与轨迹逻辑逐字搬自 BCBPlugin.cpp，
// 仅把插件私有能力调用改为 IBcbAutomationHost 接口调用。
#include "BcbStrategy.h"
#include "SliderVision.h"
#include "newrpa/util/StringUtils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <newrpa/singleUsbbox/HidMouse.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>

namespace newrpa {
namespace bcb {
namespace strategy {
using namespace slider;

double DingXiangSliderSolver::CalibrateGain(HidMouse& m) {
    m.moveRel(5, 0); Sleep(150);          // 预热丢弃首步非线性
    std::vector<double> gs;
    for (int i = 0; i < 3; ++i) {
        int x0, y0; HidMouse::currentPos(x0, y0); Sleep(60);
        m.moveRel(20, 0); Sleep(160);
        int x1, y1; HidMouse::currentPos(x1, y1);
        gs.push_back(std::abs(x1 - x0) / 20.0);
    }
    std::sort(gs.begin(), gs.end());
    double g = (gs.size() == 3) ? gs[1] : (gs.empty() ? 2.47 : gs.back());
    if (g <= 0.5) g = 2.47;
    spdlog::info("[BCB] rel gain={:.3f}", g);
    return g;
}

// ----------------------------------------------------------------------------
// 拟人平滑拖动轨迹
//
// 顶象拒绝的真正原因不是缺口选错，而是轨迹被判机器：
//   * 旧实现每帧 m.drain() 内含 Sleep(15)+Sleep(12)，8ms 时间轴被拉成 27ms+
//     一顿一顿的，页面看到的 mousemove 是稀疏跳变；
//   * gain≈2.0，每帧位移取整后经常为 0，进一步减少事件数；
//   * 到位后 trim 产生反向回拉，是行为模型最敏感的机器特征。
//
// 新实现：
//   * 11ms/帧（≈90Hz，真人鼠标常见采样率），drainFast 无 Sleep；
//   * 每帧 GetCursorPos 闭环，把固件偶发重复帧当手抖吸收；
//   * 速度曲线 tau + 0.32*tau*(1-tau)：起步略快、末端减速；
//   * OU 噪声在最后 25% 衰减到 0，终点精确、无需反向回拉；
//   * 记录实际帧数/耗时/事件数，便于下次定位。
// ----------------------------------------------------------------------------
bool DingXiangSliderSolver::TrackRel(HidMouse& m, double dxCss, double dyCss, double T,
                         double gain, const char* label) {
    const double dt = 0.011;
    int N = std::max(14, (int)(T / dt));
    N = std::min(N, 110);
    const double scale = m.scale();
    int x0, y0; HidMouse::currentPos(x0, y0);
    const double tx = x0 + dxCss * scale, ty = y0 + dyCss * scale;
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::normal_distribution<double> gauss(0.0, 1.0);
    double nx = 0, ny = 0;
    int events = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= N; ++i) {
        double tau = (double)i / N;
        // 起步略快、末端减速（s(0)=0, s(1)=1）
        double s = tau + 0.32 * tau * (1.0 - tau);
        // OU 手抖，末端 25% 线性衰减到 0，保证终点不飘
        nx += -9.0 * nx * dt + 3.0 * std::sqrt(dt) * gauss(rng);
        ny += -9.0 * ny * dt + 4.2 * std::sqrt(dt) * gauss(rng);
        double nw = (tau < 0.75) ? 1.0 : (1.0 - (tau - 0.75) / 0.25);
        double wantX = x0 + (dxCss * s + nx * nw) * scale;
        double wantY = y0 + (dyCss * s + ny * nw) * scale;

        // 每帧基于真实光标位置闭环（不是累积期望值）
        int px, py; HidMouse::currentPos(px, py);
        int sx = (int)std::lround((wantX - px) / gain);
        int sy = (int)std::lround((wantY - py) / gain);
        sx = std::max(-24, std::min(24, sx));
        sy = std::max(-18, std::min(18, sy));
        if (sx || sy) {
            m.moveRel(sx, sy);
            m.drainFast();
            ++events;
        }
        while (std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count() < i * dt) {
            Sleep(0);
        }
    }
    int px, py; HidMouse::currentPos(px, py);
    double ex = (tx - px) / scale, ey = (ty - py) / scale;
    double actual = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    spdlog::info("[BCB] track[{}] D=({:.1f},{:.1f})css T={:.2f}s frames={} events={} "
                 "actual={:.2f}s residual=({:.1f},{:.1f})",
                 label, dxCss, dyCss, T, N, events, actual, ex, ey);
    return std::abs(ex) < 6 && std::abs(ey) < 6;
}

// ----------------------------------------------------------------------------
// 顶象滑块一轮自动尝试。返回空串 = 通过；否则失败原因
// ----------------------------------------------------------------------------
std::string DingXiangSliderSolver::Attempt(IBcbAutomationHost& host, ::HidMouse& m) {
    auto readSubMl = [&] {
        return host.HostEvalNum(
            "(function(){var s=document.querySelector('.dx_captcha_basic_sub-slider');"
            "return s?(parseFloat(getComputedStyle(s).marginLeft)||0):-1})()");
    };
    auto readSliderMl = [&] {
        return host.HostEvalNum(
            "(function(){var s=document.querySelector('.dx_captcha_basic_slider');"
            "return s?(parseFloat(getComputedStyle(s).marginLeft)||0):-1})()");
    };

    // 1 定位滑块按钮
    std::string s = host.HostEvalStr(
        "(function(){var r=document.querySelector('.dx_captcha_basic_slider');"
        "if(!r)return '{}';var b=r.getBoundingClientRect();"
        "return JSON.stringify({x:b.x,y:b.y,w:b.width,h:b.height});})()");
    if (s.empty() || s == "{}") return "slider-not-found";
    double sx = 0, sy = 0;
    try {
        auto j = nlohmann::json::parse(s);
        sx = j["x"].get<double>() + j["w"].get<double>() / 2;
        sy = j["y"].get<double>() + j["h"].get<double>() / 2;
    } catch (...) { return "slider-parse-failed"; }

    if (!host.HostCalibrateBoxOrigin(m)) return "origin-failed";
    double gain = CalibrateGain(m);

    ShapeMask pieceMask;   // 起手后拼图块完全显示再抓（见 5b）

    // 2 自然移入（运动学轨迹），再闭环精调、按下
    int cx0, cy0; HidMouse::currentPos(cx0, cy0);
    double curX = (cx0 - m.originX()) / m.scale();
    double curY = (cy0 - m.originY()) / m.scale();
    double dx = sx - curX, dy = sy - curY;
    double Din = std::sqrt(dx * dx + dy * dy);
    TrackRel(m, dx, dy, std::max(0.35, std::min(0.75, 0.22 + 0.004 * Din)), gain, "approach");
    m.releaseAll(); Sleep(150);
    m.moveTo((int)m.cssToPhysX(sx), (int)m.cssToPhysY(sy)); Sleep(300);
    m.leftDown(); Sleep(200);

    // 3 起手 55px（触发 canvas 显示）
    TrackRel(m, 55.0, 0.0, 0.46, gain, "takeoff");
    Sleep(50);

    // 4 读几何（触发前父容器 display:none，rect 全 0，必须在起手后读）
    std::string geo = host.HostEvalStr(
        "(function(){"
        "var bg=document.querySelector('.dx_captcha_basic_bg');"
        "var cv=document.querySelector('.dx_captcha_basic_bg canvas');"
        "var sub=document.querySelector('.dx_captcha_basic_sub-slider');"
        "if(!bg||!cv||!sub)return '{}';"
        "var cr=cv.getBoundingClientRect(),sr=sub.getBoundingClientRect();"
        "return JSON.stringify({cvW:cv.width,cvH:cv.height,"
        "cvCssW:cr.width,cvCssH:cr.height,"
        "subLeft:parseFloat(getComputedStyle(sub).left)||0,"
        "subW:sr.width,"
        "subMl:parseFloat(getComputedStyle(sub).marginLeft)||0});})()");
    double cvW = 0, cvCssW = 0, subLeft = 0, subW = 0, subMl0 = 0;
    try {
        auto j = nlohmann::json::parse(geo);
        cvW = j["cvW"].get<double>();      cvCssW = j["cvCssW"].get<double>();
        subLeft = j["subLeft"].get<double>(); subW = j["subW"].get<double>();
        subMl0 = j["subMl"].get<double>();
    } catch (...) { m.leftUp(); return "geo-failed"; }
    if (cvW < 100 || cvCssW < 50) { m.leftUp(); return "puzzle-not-shown"; }
    double KX = cvCssW / cvW;
    double startCenter = subLeft + subW / 2;
    spdlog::info("[BCB] KX={:.4f} start_center={:.1f} subMl0={:.1f}", KX, startCenter, subMl0);

    // 5 抓 canvas -> 缺口候选
    std::string dataUrl = host.HostEvalStr(
        "(function(){var c=document.querySelector('.dx_captcha_basic_bg canvas');"
        "return (c && c.width>100) ? c.toDataURL('image/png') : '';})()");
    const std::string kPrefix = "data:image/png;base64,";
    if (dataUrl.size() <= kPrefix.size()) { m.leftUp(); return "canvas-capture-failed"; }
    auto png = util::Base64Decode(dataUrl.substr(kPrefix.size()));
    int W = 0, H = 0; std::vector<uint8_t> gray;
    if (!DecodePngGray(png, W, H, gray)) { m.leftUp(); return "png-decode-failed"; }
    auto cands = GapCandidates(gray, W, H);
    if (cands.empty()) { m.leftUp(); return "no-gap-candidate"; }

    // 5b 抓拼图块形状：必须在起手后抓——提前抓时拼图块还在入场动画/半透明，
    //     轮廓只有 1425 点（完整应 ~2911），命中率会掉到 0.25 而选错缺口。
    //     实测起手后 CORS Image 走缓存抓图仅 ~78ms，不构成明显静止。
    {
        std::string pieceDiag = host.HostEvalStrQ(
            "(function(){var c=document.querySelector('.dx_captcha_basic_sub-slider img');"
            "if(!c)return 'no-img';return JSON.stringify({complete:c.complete,"
            "nw:c.naturalWidth,nh:c.naturalHeight,co:c.crossOrigin,"
            "srcLen:(c.src||'').length})})()", 4000);
        spdlog::info("[BCB] 拼图块 img 诊断: {}", pieceDiag);
        std::string pieceUrl = host.HostEvalStrAsync(
            "(function(){var c=document.querySelector('.dx_captcha_basic_sub-slider img');"
            "if(!c)return Promise.resolve('');"
            "return new Promise(function(res){"
            "var im=new Image();im.crossOrigin='anonymous';"
            "im.onload=function(){try{var cv=document.createElement('canvas');"
            "cv.width=im.naturalWidth;cv.height=im.naturalHeight;"
            "cv.getContext('2d').drawImage(im,0,0);res(cv.toDataURL('image/png'))}"
            "catch(e){res('')}};"
            "im.onerror=function(){res('')};"
            "im.src=c.src})})()",
            8000);
        const std::string kPngPrefix = "data:image/png;base64,";
        if (pieceUrl.size() > kPngPrefix.size()) {
            auto piecePng = util::Base64Decode(pieceUrl.substr(kPngPrefix.size()));
            int PW = 0, PH = 0; std::vector<uint32_t> argb;
            if (DecodePngARGB(piecePng, PW, PH, argb)) {
                pieceMask = BuildShapeMask(argb, PW, PH);
                spdlog::info("[BCB] 拼图块 {}x{} 形状点={} 半径={}", PW, PH,
                             pieceMask.pts.size(), pieceMask.radius);
            }
        }
    }

    // 5c 选缺口：有拼图块形状就按「外形一致」命中率挑，否则退回环形差分最高分
    GapCand best = cands[0];
    for (auto& c : cands) if (c.score > best.score) best = c;
    if (!pieceMask.pts.empty()) {
        double shapeScore = 0.0;
        int idx = PickShapeMatchedGap(cands, gray, W, H, pieceMask, shapeScore);
        if (idx >= 0 && shapeScore >= 0.10) {
            best = cands[idx];
            spdlog::info("[BCB] 边缘检测选缺口: ({},{}) 融合命中率={:.3f}",
                         best.x, best.y, shapeScore);
        } else {
            spdlog::warn("[BCB] 外形匹配命中率过低({:.3f})，退回亮度差分", shapeScore);
        }
    } else {
        spdlog::warn("[BCB] 未取到拼图块形状，使用亮度差分选缺口");
    }
    spdlog::info("[BCB] gap=({},{}) score={:.1f} cands={}", best.x, best.y, best.score, cands.size());

    // 6 speed 标定（顶象随机 speed：subMl = speed x sliderMl）
    double slMl = readSliderMl();
    double speed = 1.0;
    if (slMl > 6 && subMl0 > 2) speed = subMl0 / slMl;
    spdlog::info("[BCB] speed_est={:.3f}", speed);

    double targetSub = std::max(0.0, best.x * KX - startCenter);
    // 旧的 "+35-30"（+5px）是历史遗留补偿，实测各轮过冲 -8.2/-3.0/+0.4 不一致，
    // 固定值治标不治本。改成主拖留 6px 余量、宁欠勿过，到位后用 5b 闭环同向补齐。
    double targetCss = std::max(0.0, targetSub / speed - 6.0);
    spdlog::info("[BCB] target_sub={:.1f} target_css={:.1f} (留6px余量同向逼近)",
                 targetSub, targetCss);

    // 7 主拖（按住期间不查询，中途不留死尸段）
    int cx1, cy1; HidMouse::currentPos(cx1, cy1);
    double curCss = (cx1 - m.originX()) / m.scale() - sx;
    double Dm = std::abs(targetCss - curCss);
    double Tm = std::max(0.32, std::min(1.15, 0.30 + 0.004 * Dm));
    TrackRel(m, targetCss - curCss, 0.0, Tm, gain, "main");
    Sleep(80);

    // 8 闭环同向逼近：主拖留了 6px 余量，这里最多两次只往前补，绝不倒回。
    //   顶象对反向回拉极敏感；宁可留 -1~0px 微过冲，也不能往回拖。
    for (int k = 0; k < 2; ++k) {
        double err = targetSub - readSubMl();
        if (err > 1.5) {
            spdlog::info("[BCB] 同向逼近 #{} err={:.1f}", k + 1, err);
            TrackRel(m, err / speed, 0.0, 0.14, gain, "nudge");
            Sleep(70);
        } else {
            break;
        }
    }
    double err = targetSub - readSubMl();
    if (err < -1.0) spdlog::warn("[BCB] 微过冲 {:.1f}px，不回拉", -err);
    spdlog::info("[BCB] final subMl={:.1f} target={:.1f} err={:.1f}",
                 readSubMl(), targetSub, targetSub - readSubMl());
    // 到位后按住停顿 2s 再松手（真人拖到位会稍作停顿，直接秒松易被判机器）
    spdlog::info("[BCB] 到位停顿 2000ms 后释放");
    Sleep(2000);
    m.leftUp(); Sleep(2500);

    // 轨迹诊断：页面真实收到的 mousemove 摘要（自动/人工对比用）
    std::string seqDiag = host.HostEvalStrQ(
        "(function(){var a=window.__mseq||[];if(!a.length)return '{}';"
        "var ds=[],dt=[],n=0;"
        "for(var i=1;i<a.length;i++){if(a[i][3]!==1)continue;"
        "ds.push(Math.abs(a[i][1]-a[i-1][1])+Math.abs(a[i][2]-a[i-1][2]));"
        "dt.push(a[i][0]-a[i-1][0]);n++}"
        "ds.sort(function(x,y){return x-y});dt.sort(function(x,y){return x-y});"
        "return JSON.stringify({total:a.length,down:n,"
        "dtMed:dt.length?dt[dt.length>>1]:0,dtMax:dt.length?dt[dt.length-1]:0,"
        "stepMed:ds.length?ds[ds.length>>1]:0,stepMax:ds.length?ds[ds.length-1]:0,"
        "x0:a[0][1],x1:a[a.length-1][1]})})()", 3000);
    spdlog::info("[BCB] 轨迹诊断(页面mousemove): {}", seqDiag);

    // 9 判定：弹窗 = 失败；拼图收起且无弹窗 = 通过
    if (!host.HostPopupText().empty()) return "popup: " + host.HostPopupText();
    std::string st = host.HostEvalStr(
        "(function(){var c=document.querySelector('.dx_captcha_basic_content');"
        "var cap=document.getElementById('captcha');"
        "return JSON.stringify({disp:c?getComputedStyle(c).display:'gone',"
        "txt:(document.body.innerText||'').indexOf('验证成功')>=0,"
        "cap:cap?cap.className:''});})()");
    try {
        auto j = nlohmann::json::parse(st);
        std::string disp = j.value("disp", "gone");
        bool txt = j.value("txt", false);
        std::string cap = j.value("cap", "");
        if (disp == "none" || txt || cap.find("success") != std::string::npos) return "";
    } catch (...) {}
    return "puzzle-still-open";
}


} // namespace strategy
} // namespace bcb
} // namespace newrpa
