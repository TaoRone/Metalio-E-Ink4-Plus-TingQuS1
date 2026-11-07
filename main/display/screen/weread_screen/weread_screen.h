#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// WereadScreen（墨水屏 · 微信读书书架，听趣S1 M1）
//
// - 书架列表：标题 + 作者 + 读完标记，vk_prev/vk_next 翻页，无滚动
// - 进页：无缓存且已配置 Key → Application::Schedule(Network) 拉取书架
// - 未配置 Key：空态提示 + 「输入 API Key」按钮 → 内置简版键盘输入
// - 页码行右侧「刷新」：节流 3s，联网同步后上屏
// ---------------------------------------------------------------------------

class WereadScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
};
