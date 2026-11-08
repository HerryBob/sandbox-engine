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
		bool opaque = true;
		float hardness = 1.0f;
		RenderType renderType = RenderType::Solid;
		std::string texture;   // 纹理名（无模型时 6 面兜底；模型化后仍用于缺失回退）
		std::string model;     // 模型名（assets/models/block/<name>.json）；空串 = 默认立方体

		// ---- 物理属性（#40）：供后续宏大结构物理使用，业务层目前只存不用 ----
		float mass = 1.0f;        // 质量（相对单位：1.0 = 一个基准方块，约 1 m³ 的土）
		bool solid = true;        // 是否参与碰撞（实体阻挡、可站立）
		float friction = 0.6f;    // 摩擦系数（0 = 冰面，1 = 完全不滑）
	};
}