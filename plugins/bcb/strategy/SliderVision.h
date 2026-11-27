#pragma once
// 顶象滑块视觉算法（P3 从 BCBPlugin.cpp 原样抽出）：纯函数，不依赖插件类。
#include <cstdint>
#include <utility>
#include <vector>

namespace newrpa {
namespace bcb {
namespace slider {

struct GapCand { int x, y; double score; };

bool DecodePngGray(const std::vector<uint8_t>& png, int& W, int& H,
                   std::vector<uint8_t>& gray);
std::vector<GapCand> GapCandidates(const std::vector<uint8_t>& gray, int W, int H);

bool DecodePngARGB(const std::vector<uint8_t>& png, int& W, int& H,
                   std::vector<uint32_t>& argb);

struct ShapeMask {
    std::vector<std::pair<int,int>> pts;      // 实心点，相对中心偏移
    std::vector<std::pair<int,int>> outline;  // 轮廓点
    int radius = 0;                           // 覆盖半径（像素）
};
ShapeMask BuildShapeMask(const std::vector<uint32_t>& argb, int W, int H);
std::vector<uint8_t> SobelEdges(const std::vector<uint8_t>& gray, int W, int H);
double ShapeEdgeScore(const std::vector<uint8_t>& edge, int W, int H,
                      const ShapeMask& sm, int cx, int cy);
double ShapeDarknessScore(const std::vector<uint8_t>& gray, int W, int H,
                          const ShapeMask& sm, int cx, int cy);
int PickShapeMatchedGap(const std::vector<GapCand>& cands,
                        const std::vector<uint8_t>& gray, int W, int H,
                        const ShapeMask& sm, double& outScore);

} // namespace slider
} // namespace bcb
} // namespace newrpa
