#pragma once
// HUD 业务统计快照：由渲染线程（Renderer::tick）按 1Hz 写入，VulkanContext 只读。
// 为什么是 1Hz：chunkCount()/blockCount() 要抢 blocksMutex_（和业务线程同一个锁），
// 每帧查会把渲染帧率压力转嫁给业务 tick；相机/FPS 这类原子量才走每帧刷新。
namespace engine::render::hud {

struct HudInfo {
    long long chunkCount = 0;   // 已加载 chunk 数
    long long blockCount = 0;   // 非空气方块数
    long long tickCount = 0;    // 业务累计 tick
    double simTime = 0.0;       // 业务累计模拟秒
    double tps = 0.0;           // 业务实际 tps（tickCount 增量 / 墙钟增量）
    bool     valid = false;     // 是否已成功采集过一次（首秒内显示 "--"）
};

} // namespace engine::render::hud
