#include "demo/ConsoleRenderer.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <iostream>
#include <mutex>
#include <sstream>

namespace engine::demo {

void renderFrame(const std::vector<engine::shared::Block>& blocks,
                 double shipX, int state, int tick) {
    constexpr int viewR = 6;   // 视口 x 半径（格）
    const int cx = static_cast<int>(std::llround(shipX));
    const int x0 = cx - viewR, x1 = cx + viewR;
    const int y0 = -2, y1 = 6; // y 视口：含飞船 y(-1..1) 与放置演示 (0,5,0)
    // 整帧先拼成字符串再一次性输出：避免多次 cout 调用在管道缓冲下字节交错。
    std::ostringstream os;
    os << "  [帧 t=" << tick << " 打包状态=" << state
       << " 飞船x=" << shipX << " 视口x=[" << x0 << "," << x1 << "]]\n";
    for (int y = y1; y >= y0; --y) {
        os << "  y=" << (y >= 0 ? " " : "") << y << " |";
        for (int x = x0; x <= x1; ++x) {
            char c = ' ';
            // 飞船区域判定（投影）：格中心贴近飞船基准则画 S（3×3 机身）。
            // 改造扩建格 (2,0,0) 距基准 2 格 > 1.5 → 显示为 #（独立方块），
            // 恰好演示"刚体方块增删"在画面上的表现。
            if (std::abs(x - shipX) <= 1.5 && y >= -1 && y <= 1) {
                c = 'S';
            } else {
                for (const auto& b : blocks) {
                    if (b.pos.x == x && b.pos.y == y) { c = '#'; break; }
                }
            }
            os << c;
        }
        os << "|\n";
    }
    os << "         +";
    for (int x = x0; x <= x1; ++x) os << "-";
    os << "+\n";
    std::lock_guard<std::mutex> lock(engine::core::g_logMutex);
    std::cout << os.str();
}

} // namespace engine::demo
