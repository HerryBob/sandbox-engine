#pragma once

#include <cstdint>
#include <string>

namespace engine::registry {

	//负责渲染管线分桶
	enum class RenderType :std::uint8_t {
		Solid = 0,
		Cutout = 1,
		Translucent = 2,
	};

	// 方块定义：注册表自己持字符串副本（std::string），不留模组 DLL 里的指针 ——
	// DLL 卸载后定义仍自洽可读，这是"注册内容跨 DLL 生命周期"的底线。
	struct BlockInfo{
		std::string name;
		// B59：原有一个 bool opaque，与 renderType 表达同一件事（是否遮挡剔除），
		// 两者矛盾时行为不可预期，已删除。**面剔除一律按 renderType 判定**：
		//   Solid       → 剔除与邻块共享的面
		//   Cutout/Translucent → 不剔除（要看到内部）
		// 模组 ABI 里的 ModBlockDef::opaque 仍保留（规则7：接口本轮不动），
		// 但引擎已不读它，登记时**不做**一致性校验（要报不一致需新增告警，属诊断增强）。
		// B60：hardness 是"破坏需要的时间尺度"，1.0 = 基准方块（stone）。
		//      目前只有 0 与大值两种用法（0 = 瞬间破坏），业务层尚未消费，仅存档与将来配方用。
		float hardness = 1.0f;
		RenderType renderType = RenderType::Solid;
		// B61：texture 与 model 的关系 ——
		//   · 两者都空   → 用默认立方体 + 缺省纹理（图集第一格）
		//   · 只有 model → 用模型；模型缺失面回退到纹理（若 texture 也空则用缺省）
		//   · 只有 texture → 默认立方体贴这张纹理
		//   · 两者都有   → 模型优先，纹理只作缺失回退
		std::string texture;   // 纹理名（无模型时 6 面兜底；模型化后仍用于缺失回退）
		std::string model;     // 模型名（assets/models/block/<name>.json）；空串 = 默认立方体

		// ---- 物理属性（#40）：供后续宏大结构物理使用，业务层目前只存不用 ----
		float mass = 1.0f;        // 质量（相对单位：1.0 = 一个基准方块，约 1 m³ 的土）
		bool solid = true;        // 是否参与碰撞（实体阻挡、可站立）
		float friction = 0.6f;    // 摩擦系数（0 = 冰面，1 = 完全不滑）
	};
}