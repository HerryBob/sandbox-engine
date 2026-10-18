#pragma once
#include <atomic>

namespace engine::shared {
	struct PlayerInput
	{
		std::atomic<double> camX{ 0.0 };
		std::atomic<double> camY{ 0.0 };
		std::atomic<double> camZ{ 0.0 };
		std::atomic<double> yaw{ 0.0 };   // 左右旋转（弧度）
		std::atomic<double> pitch{ 0.0 };  // 上下旋转（弧度）
        std::atomic<long long> clickSeq{ 0 };
		std::atomic<int>       clickButton{ -1 }; // 0=左键(破坏) 1=右键(放置)，-1=无
	};	
}