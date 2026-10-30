#pragma once
// 模型加载器：扫描 assets/models/block/*.json → RapidJSON 解析 → 注册进 g_modelRegistry。
// 解析失败/结构缺漏 → 跳过该模型（stderr 警告），不崩引擎；方块会回退立方体兜底。
namespace engine::assets {
    // 启动时调用（须在 vkCtx.init 之前——图集 buildFromRegistry 要收集模型引用的纹理名）
    void loadBlockModels();
}
