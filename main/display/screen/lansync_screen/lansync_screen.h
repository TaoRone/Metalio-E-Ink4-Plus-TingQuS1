#pragma once

#include "lvgl.h"

#include <string>

// ---------------------------------------------------------------------------
// LansyncScreen（墨水屏 · 局域网剪贴板，听趣S1）
//
// - 设备连上 WiFi 后进本页：启动 :8080 小服务，屏幕显示访问地址
// - 电脑浏览器打开后：3 个粘贴位 + 「设为微信读书 Key」按钮
// - 内容存 NVS namespace "clipboard" / slot1..3；微信读书键盘页 PASTE1..3 插入
// - 离开页面自动关闭服务
// ---------------------------------------------------------------------------

class LansyncScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();

    // 微信读书键盘等页面读取剪贴板槽位（NVS "clipboard"/slotN）
    static std::string GetSlot(int n);  // n = 1..3
};
