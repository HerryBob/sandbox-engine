#include "render/Renderer.hpp"
#include "business/BusinessWorld.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <iostream>
#include <mutex>
#include <sstream>

namespace engine::render {

void Renderer::tick() {

    if (!first_) {
        first_ = true;
        vk_.rebuildMeshFromBlocks(world_.renderSnapshot());
    }
    engine::shared::BlockChangeEvent ev;
    while (changeQueue_.pop(ev)) {
        changeEventsReceived_.fetch_add(1, std::memory_order_relaxed);
        for (const auto& cp : ev.dirtyChunks) dirtyChunks_.insert(cp);
    }

    // ② 按业务模拟时刻插值刚体位置（渲染插值基座消费端）。
    //    原型直接查"业务当前 simTime"→ 钳到最新快照；真实渲染用渲染累计
    //    模拟时刻插值（帧率无关，机制已由 sampleShipPos 提供）。
    double px = 0.0, py = 0.0, pz = 0.0;
    world_.sampleShipPos(world_.simTime(), px, py, pz); // 结果原型阶段仅用于帧头显示

    // ③ 伪渲染帧（降频 1Hz）：数据通路验证。真实渲染落地后此步替换为
    //    脏 chunk 重建 + 顶点烘焙 + swapchain 上屏。
    ++frames_;
    if (frames_ % 60 == 0) renderFrame();
}

void Renderer::renderFrame() {
    const double shipX = world_.shipX();
    const int    state = static_cast<int>(world_.contraptionState());
    const int    tick  = static_cast<int>(world_.tickCount());
    constexpr int viewR = 6;   // 视口 x 半径（格）
    const int cx = static_cast<int>(std::llround(shipX));
    const int x0 = cx - viewR, x1 = cx + viewR;
    const int y0 = -2, y1 = 6; // y 视口：含飞船 y(-1..1) 与放置演示 (0,5,0)
    // 帧数据在渲染线程内拉取（锁内拷贝 28 块，1Hz 频率锁竞争可忽略）。
    const auto blocks = world_.renderSnapshot();
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

} // namespace engine::render
