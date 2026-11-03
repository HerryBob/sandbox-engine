#pragma once
#include <atomic>

namespace engine::shared {
	// 玩家点击输入（输入线程鼠标回调写，业务线程消费做 DDA 放置/破坏）。
	// 相机状态已拆到 CameraState（输入线程写，渲染/业务只读）。
	struct PlayerInput
	{
        std::atomic<long long> clickSeq{ 0 };
		std::atomic<int>       clickButton{ -1 }; // 0=左键(破坏) 1=右键(放置)，-1=无
        // F3 调试浮层开关请求：键盘回调在"按下"瞬间置 true，渲染线程 exchange(false) 消费。
        // 用请求位而不是共享 bool：输入线程只管发事件，可见性状态归渲染线程独占。
        std::atomic<bool>      toggleHud{ false };
	};	
}
