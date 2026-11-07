#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// KosyncScreen（墨水屏 · KOReader 同步设置，听趣S1 Phase 3）
//
// - 启用开关 / 服务器地址 / 用户名 / 密码（MD5 存储）/ 拉取后自动跳转
// - 「验证登录」走 GET /users/auth；「注册账号」走 POST /users/create
// - 配置存 NVS namespace "kosync"；与 KOReader 填同一服务器和账号即互通进度
// ---------------------------------------------------------------------------

class KosyncScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
};
