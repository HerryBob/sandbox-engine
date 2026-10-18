#pragma once
#include "business/BusinessWorld.hpp"

namespace engine::demo {

// 演示驱动：tick 事件源（替代原 BusinessWorld.advanceTick 内的演示状态机）。
// 职责：按业务 tick 号发起世界操作（全量同步/放置/破坏/打包/改造/解包），
//       观察三轨联动与 time_scale 效果。业务世界本身不含任何演示逻辑。
// 将来玩家输入/脚本系统就是另一个"事件源"，走同样的命令式接口。
class DemoDriver {
public:
    explicit DemoDriver(engine::business::BusinessWorld& bw);

    // 每业务 tick 调用一次（业务线程 lambda 内，advanceTick 之后）。
    void step();

private:
    engine::business::BusinessWorld& bw_;
};

} // namespace engine::demo
