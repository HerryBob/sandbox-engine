#pragma once

#include <cstdint>
namespace engine::registry {

	//负责渲染管线分桶
	enum class RenderType :std::uint8_t {
		Solid = 0,
		Cutout = 1,
		Translucent = 2,
	};

	struct BlockInfo{
		const char* name;
		bool opaque = true;
		float hardness = 1.0f;
		RenderType renderType = RenderType::Solid;
		const char* texture = nullptr;  // 纹理名（无模型时 6 面兜底；模型化后仍用于缺失回退）
		const char* model = nullptr;    // 模型名（assets/models/block/<name>.json）；nullptr = 默认立方体
	};
}