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
		const char* texture = nullptr;  // 纹理名（对应 textures/<name>.png；nullptr = 未注册 → 缺省棋盘格）
	};
}