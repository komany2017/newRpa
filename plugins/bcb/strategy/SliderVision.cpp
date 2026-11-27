// P3：从 BCBPlugin.cpp 原样抽出的顶象滑块视觉算法，算法本体未改动。
#include "SliderVision.h"
#include <spdlog/spdlog.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace newrpa {
namespace bcb {
namespace slider {

// GDI+ 解码 PNG -> 灰度（零第三方依赖，系统自带）
bool DecodePngGray(const std::vector<uint8_t>& png, int& W, int& H,
                   std::vector<uint8_t>& gray) {
    W = H = 0; gray.clear();
    if (png.size() < 8) return false;
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &si, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    do {
        HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (!hg) break;
        void* p = ::GlobalLock(hg);
        if (!p) { ::GlobalFree(hg); break; }
        memcpy(p, png.data(), png.size());
        ::GlobalUnlock(hg);
        IStream* st = nullptr;
        if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { ::GlobalFree(hg); break; }
        Gdiplus::GpBitmap* bmp = nullptr;
        UINT w = 0, h = 0;
        if (Gdiplus::DllExports::GdipCreateBitmapFromStream(st, &bmp) == Gdiplus::Ok && bmp &&
            Gdiplus::DllExports::GdipGetImageWidth(bmp, &w) == Gdiplus::Ok &&
            Gdiplus::DllExports::GdipGetImageHeight(bmp, &h) == Gdiplus::Ok && w > 50 && h > 50) {
            Gdiplus::BitmapData bd{};
            Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
            if (Gdiplus::DllExports::GdipBitmapLockBits(bmp, &rect, Gdiplus::ImageLockModeRead,
                                   PixelFormat32bppARGB, &bd) == Gdiplus::Ok && bd.Scan0) {
                gray.resize((size_t)w * h);
                const uint8_t* base = (const uint8_t*)bd.Scan0;
                INT stride = bd.Stride;
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* row = base + (size_t)y * stride;
                    for (UINT x = 0; x < w; ++x) {
                        const uint8_t* px = row + (size_t)x * 4;   // B G R A
                        gray[(size_t)y * w + x] =
                            (uint8_t)((px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8);
                    }
                }
                Gdiplus::DllExports::GdipBitmapUnlockBits(bmp, &bd);
                W = (int)w; H = (int)h; ok = true;
            }
            Gdiplus::DllExports::GdipDisposeImage(bmp);
        }
        st->Release();
    } while (false);
    Gdiplus::GdiplusShutdown(tok);
    return ok;
}

// 缺口候选：内环暗 vs 外环亮差值（Python candidates() 同款）
std::vector<GapCand> GapCandidates(const std::vector<uint8_t>& gray, int W, int H) {
    const int R = 26;
    const int y_lo = 55, y_hi = std::min(H - 45, 150);
    const int x_lo = 80, x_hi = W - 90;
    const int step = 4;
    std::vector<GapCand> best;
    if (y_hi <= y_lo || x_hi <= x_lo) return best;
    auto g = [&](int x, int y)->int {
        if (x < 0 || y < 0 || x >= W || y >= H) return 128;
        return gray[(size_t)y * W + x];
    };
    for (int cy = y_lo; cy < y_hi; cy += step) {
        for (int cx = x_lo; cx < x_hi; cx += step) {
            double isum = 0; int icnt = 0;
            for (int y = cy - R + 5; y <= cy + R - 4; y += 4)
                for (int x = cx - R + 5; x <= cx + R - 4; x += 4)
                    { isum += g(x, y); ++icnt; }
            double rsum = 0; int rcnt = 0;
            for (int y = cy - R - 3; y <= cy + R + 3; y += 4)
                for (int x = cx - R - 3; x <= cx + R + 3; x += 4) {
                    if (std::abs(x - cx) > R - 2 || std::abs(y - cy) > R - 2)
                        { rsum += g(x, y); ++rcnt; }
                }
            if (!icnt || !rcnt) continue;
            best.push_back({cx, cy, rsum / rcnt - isum / icnt});
        }
    }
    std::sort(best.begin(), best.end(),
              [](const GapCand& a, const GapCand& b) { return a.score > b.score; });
    std::vector<GapCand> picks;
    for (auto& b : best) {
        bool dup = false;
        for (auto& q : picks) if (std::abs(b.x - q.x) <= 38) { dup = true; break; }
        if (!dup) picks.push_back(b);
        if (picks.size() >= 6) break;
    }
    std::vector<GapCand> out;
    for (auto& p : picks)
        if (p.x >= x_lo && p.x <= x_hi) out.push_back(p);
    std::sort(out.begin(), out.end(),
              [](const GapCand& a, const GapCand& b) { return a.x < b.x; });
    if (out.size() > 4) out.resize(4);
    return out;
}

// ---------------------------------------------------------------------------
// 外形一致匹配（顶象滑块「两个缺口选一个」的正确解法）
//   拼图块是一张独立远程 PNG（.dx_captcha_basic_sub-slider img，带 alpha），
//   背景 canvas 上画着 2 个半透明缺口，只有一个与拼图块外形一致。
//   做法：提取拼图块 alpha 形状掩膜（相对中心的点集），盖到每个候选缺口中心，
//   统计落在「局部暗区」的点比例 —— 命中率最高的即外形一致的真缺口。
//   实测：真缺口命中率 ~0.52，干扰缺口 ~0.06，区分度极高。
// ---------------------------------------------------------------------------

// GDI+ 解码 PNG -> ARGB（保留 alpha，用于拼图块形状掩膜）
bool DecodePngARGB(const std::vector<uint8_t>& png, int& W, int& H,
                   std::vector<uint32_t>& argb) {
    W = H = 0; argb.clear();
    if (png.size() < 8) return false;
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &si, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    do {
        HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (!hg) break;
        void* p = ::GlobalLock(hg);
        if (!p) { ::GlobalFree(hg); break; }
        memcpy(p, png.data(), png.size());
        ::GlobalUnlock(hg);
        IStream* st = nullptr;
        if (CreateStreamOnHGlobal(hg, TRUE, &st) != S_OK) { ::GlobalFree(hg); break; }
        Gdiplus::GpBitmap* bmp = nullptr;
        UINT w = 0, h = 0;
        if (Gdiplus::DllExports::GdipCreateBitmapFromStream(st, &bmp) == Gdiplus::Ok && bmp &&
            Gdiplus::DllExports::GdipGetImageWidth(bmp, &w) == Gdiplus::Ok &&
            Gdiplus::DllExports::GdipGetImageHeight(bmp, &h) == Gdiplus::Ok && w > 8 && h > 8) {
            Gdiplus::BitmapData bd{};
            Gdiplus::Rect rect(0, 0, (INT)w, (INT)h);
            if (Gdiplus::DllExports::GdipBitmapLockBits(bmp, &rect, Gdiplus::ImageLockModeRead,
                                   PixelFormat32bppARGB, &bd) == Gdiplus::Ok && bd.Scan0) {
                argb.resize((size_t)w * h);
                const uint8_t* base = (const uint8_t*)bd.Scan0;
                INT stride = bd.Stride;
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* row = base + (size_t)y * stride;
                    for (UINT x = 0; x < w; ++x) {
                        const uint8_t* px = row + (size_t)x * 4;   // B G R A
                        uint32_t a = px[3];
                        uint32_t gv = (px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8;
                        argb[(size_t)y * w + x] = (a << 24) | (gv << 16) | (gv << 8) | gv;
                    }
                }
                Gdiplus::DllExports::GdipBitmapUnlockBits(bmp, &bd);
                W = (int)w; H = (int)h; ok = true;
            }
            Gdiplus::DllExports::GdipDisposeImage(bmp);
        }
        st->Release();
    } while (false);
    Gdiplus::GdiplusShutdown(tok);
    return ok;
}

ShapeMask BuildShapeMask(const std::vector<uint32_t>& argb, int W, int H) {
    ShapeMask sm;
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint32_t a = (argb[(size_t)y * W + x] >> 24) & 0xFF;
            if (a >= 128) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
        }
    if (x1 < x0) return sm;
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    auto alpha = [&](int x, int y)->int {
        if (x < 0 || y < 0 || x >= W || y >= H) return 0;
        return (int)((argb[(size_t)y * W + x] >> 24) & 0xFF);
    };
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (alpha(x, y) < 128) continue;
            sm.pts.push_back({x - cx, y - cy});
            // 轮廓点：4 邻域任一透明，即为形状边缘
            if (alpha(x - 1, y) < 128 || alpha(x + 1, y) < 128 ||
                alpha(x, y - 1) < 128 || alpha(x, y + 1) < 128)
                sm.outline.push_back({x - cx, y - cy});
        }
    sm.radius = std::max(x1 - x0, y1 - y0) / 2;
    return sm;
}

// Sobel 边缘强度图（背景 canvas 灰度 -> 每点梯度幅值 0~255）
std::vector<uint8_t> SobelEdges(const std::vector<uint8_t>& gray, int W, int H) {
    std::vector<uint8_t> e((size_t)W * H, 0);
    auto g = [&](int x, int y)->int {
        if (x < 0 || y < 0 || x >= W || y >= H) return 0;
        return gray[(size_t)y * W + x];
    };
    for (int y = 1; y < H - 1; ++y)
        for (int x = 1; x < W - 1; ++x) {
            int gx = -g(x-1,y-1) - 2*g(x-1,y) - g(x-1,y+1)
                     + g(x+1,y-1) + 2*g(x+1,y) + g(x+1,y+1);
            int gy = -g(x-1,y-1) - 2*g(x,y-1) - g(x+1,y-1)
                     + g(x-1,y+1) + 2*g(x,y+1) + g(x+1,y+1);
            int m = (std::abs(gx) + std::abs(gy)) / 2;
            e[(size_t)y * W + x] = (uint8_t)std::min(255, m);
        }
    return e;
}

// 边缘匹配：把拼图块轮廓点平移到候选中心，统计落在背景强边缘上的比例。
// 真缺口的边界形状与拼图块轮廓一致，命中率显著高于干扰缺口。
double ShapeEdgeScore(const std::vector<uint8_t>& edge, int W, int H,
                      const ShapeMask& sm, int cx, int cy) {
    if (sm.outline.empty()) return 0.0;
    int hit = 0, total = 0;
    for (const auto& pt : sm.outline) {
        int x = cx + pt.first, y = cy + pt.second;
        if (x < 3 || y < 3 || x >= W - 3 || y >= H - 3) continue;
        int mx = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                mx = std::max(mx, (int)edge[(size_t)(y + dy) * W + (x + dx)]);
        ++total;
        if (mx >= 30) ++hit;   // 梯度阈值：明显边缘
    }
    return total ? (double)hit / total : 0.0;
}

// 计算某个候选中心处，拼图块形状掩膜覆盖区域的「暗区命中率」
double ShapeDarknessScore(const std::vector<uint8_t>& gray, int W, int H,
                          const ShapeMask& sm, int cx, int cy) {
    if (sm.pts.empty()) return 0.0;
    auto g = [&](int x, int y)->int {
        if (x < 0 || y < 0 || x >= W || y >= H) return -1;
        return gray[(size_t)y * W + x];
    };
    int hit = 0, total = 0;
    const int LR = 15;   // 局部均值窗口
    for (const auto& pt : sm.pts) {
        int x = cx + pt.first, y = cy + pt.second;
        int v = g(x, y);
        if (v < 0) continue;
        long long sum = 0; int n = 0;
        for (int yy = y - LR; yy <= y + LR; yy += 3)
            for (int xx = x - LR; xx <= x + LR; xx += 3) {
                int lv = g(xx, yy);
                if (lv >= 0) { sum += lv; ++n; }
            }
        if (!n) continue;
        int localMean = (int)(sum / n);
        ++total;
        if (localMean - v > 18) ++hit;   // 明显暗于局部均值 = 落在缺口阴影内
    }
    return total ? (double)hit / total : 0.0;
}

// 从候选中选出目标缺口（返回下标；-1 表示无拼图块信息，退化用亮度差分最高分）。
// 主判据=边缘匹配（拼图块轮廓落在背景强边缘上的比例，不受背景明暗干扰）；
// 暗区命中率仅作辅助加分，二者融合：score = 0.75*edge + 0.25*dark。
int PickShapeMatchedGap(const std::vector<GapCand>& cands,
                        const std::vector<uint8_t>& gray, int W, int H,
                        const ShapeMask& sm, double& outScore) {
    outScore = 0.0;
    if (cands.empty() || sm.pts.empty()) return -1;
    std::vector<uint8_t> edge = sm.outline.empty()
        ? std::vector<uint8_t>() : SobelEdges(gray, W, H);
    int best = -1; double bestScore = -1.0;
    for (int i = 0; i < (int)cands.size(); ++i) {
        double dark = ShapeDarknessScore(gray, W, H, sm, cands[i].x, cands[i].y);
        double es = edge.empty() ? 0.0
            : ShapeEdgeScore(edge, W, H, sm, cands[i].x, cands[i].y);
        double sc = 0.75 * es + 0.25 * dark;
        spdlog::info("[BCB] 候选缺口 x={} 边缘命中={:.3f} 暗区命中={:.3f} 融合={:.3f}",
                     cands[i].x, es, dark, sc);
        if (sc > bestScore) { bestScore = sc; best = i; }
    }
    outScore = bestScore;
    return best;
}


} // namespace slider
} // namespace bcb
} // namespace newrpa
