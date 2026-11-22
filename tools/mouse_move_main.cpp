// mouse_move_main.cpp — 单头盒子硬件鼠标 CLI（HidMouse 自测 / 调试工具）
//
// 用法：
//   mouse_move.exe info                          显示 DPI / 缩放 / 屏幕尺寸 / 盒子状态
//   mouse_move.exe move <x> <y>                  物理坐标绝对移动
//   mouse_move.exe click <x> <y>                 物理坐标单击
//   mouse_move.exe down | up | release           按下左键 / 松开 / 释放所有键
//   mouse_move.exe calib <px> <py>               移到物理点(px,py)，输出该点供 CDP 反推 origin
//   mouse_move.exe css <ox> <oy> <cssX> <cssY>   设原点后按 CSS 坐标移动
//   mouse_move.exe drag <ox> <oy> <sx> <sy> <delta>  设原点后从 CSS(sx,sy) 拟人右拖 delta(CSS px)
//   mouse_move.exe pos                           读当前物理鼠标坐标
//
// 说明：
//   * calib 用法：先用本工具把鼠标移到已知物理点，再用 CDP 读 JS 的 clientX/clientY，
//     origin = 物理点 - client*scale。多个点求平均更准。得到 origin 后传给 css/drag。
//   * CSS->物理换算全程按 scale=DPI/96（本机 1.25）。
#include "newrpa/singleUsbbox/HidMouse.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void usage()
{
    std::printf("usage:\n"
                "  mouse_move.exe info\n"
                "  mouse_move.exe move <x> <y>\n"
                "  mouse_move.exe click <x> <y>\n"
                "  mouse_move.exe down | up | release\n"
                "  mouse_move.exe calib <px> <py>\n"
                "  mouse_move.exe css <ox> <oy> <cssX> <cssY>\n"
                "  mouse_move.exe drag <ox> <oy> <sx> <sy> <delta> [steps]\n"
                "  mouse_move.exe pos\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(); return 1; }
    const char* cmd = argv[1];

    HidMouse m;

    // pos / info 不需要盒子鼠标（info 仍要打开看状态）
    if (std::strcmp(cmd, "pos") == 0) {
        int x, y; HidMouse::currentPos(x, y);
        std::printf("pos=%d,%d\n", x, y);
        return 0;
    }

    if (!m.open()) {
        std::printf("OPEN FAIL: %s\n", m.lastError());
        return 2;
    }

    int w = 0, h = 0; m.screenSize(w, h);

    if (std::strcmp(cmd, "info") == 0) {
        int x, y; HidMouse::currentPos(x, y);
        std::printf("open=1 dpi=%d scale=%.3f screen=%dx%d origin=(%.1f,%.1f) pos=(%d,%d)\n",
                    m.dpi(), m.scale(), w, h, m.originX(), m.originY(), x, y);
        return 0;
    }
    if (std::strcmp(cmd, "move") == 0 && argc >= 4) {
        int x = std::atoi(argv[2]), y = std::atoi(argv[3]);
        bool ok = m.moveTo(x, y);
        std::printf("move(%d,%d)=%d\n", x, y, ok);
        return ok ? 0 : 3;
    }
    if (std::strcmp(cmd, "click") == 0 && argc >= 4) {
        int x = std::atoi(argv[2]), y = std::atoi(argv[3]);
        bool ok = m.click(x, y);
        std::printf("click(%d,%d)=%d\n", x, y, ok);
        return ok ? 0 : 3;
    }
    if (std::strcmp(cmd, "down") == 0) { bool ok = m.leftDown(); std::printf("down=%d\n", ok); return ok ? 0 : 3; }
    if (std::strcmp(cmd, "up") == 0)   { bool ok = m.leftUp();   std::printf("up=%d\n", ok);   return ok ? 0 : 3; }
    if (std::strcmp(cmd, "release") == 0) { bool ok = m.releaseAll(); std::printf("release=%d\n", ok); return ok ? 0 : 3; }
    if (std::strcmp(cmd, "calib") == 0 && argc >= 4) {
        int px = std::atoi(argv[2]), py = std::atoi(argv[3]);
        bool ok = m.moveTo(px, py);
        Sleep(300);  // MoveTo3 异步生效，等鼠标真正到位再读
        int ax, ay; HidMouse::currentPos(ax, ay);
        std::printf("calib target=(%d,%d) actual=(%d,%d) scale=%.3f ok=%d\n",
                    px, py, ax, ay, m.scale(), ok);
        return ok ? 0 : 3;
    }
    if (std::strcmp(cmd, "css") == 0 && argc >= 6) {
        double ox = std::atof(argv[2]), oy = std::atof(argv[3]);
        double cx = std::atof(argv[4]), cy = std::atof(argv[5]);
        m.setOrigin(ox, oy);
        bool ok = m.moveToCss(cx, cy);
        Sleep(300);
        int ax, ay; HidMouse::currentPos(ax, ay);
        std::printf("css(%.1f,%.1f) origin(%.1f,%.1f) -> phys(%.1f,%.1f)=%d actual=(%d,%d)\n",
                    cx, cy, ox, oy, m.cssToPhysX(cx), m.cssToPhysY(cy), ok, ax, ay);
        return ok ? 0 : 3;
    }
    if (std::strcmp(cmd, "drag") == 0 && argc >= 7) {
        double ox = std::atof(argv[2]), oy = std::atof(argv[3]);
        double sx = std::atof(argv[4]), sy = std::atof(argv[5]);
        double delta = std::atof(argv[6]);
        int steps = (argc >= 8) ? std::atoi(argv[7]) : 40;
        m.setOrigin(ox, oy);
        bool ok = m.dragRelativeCss(sx, sy, delta, steps);
        std::printf("drag start=(%.1f,%.1f) delta=%.1f css steps=%d => %d\n",
                    sx, sy, delta, steps, ok);
        return ok ? 0 : 3;
    }

    usage();
    return 1;
}
