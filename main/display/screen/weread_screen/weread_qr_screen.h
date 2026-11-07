#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// WereadQrScreen（微信读书墨水屏扫码登录，听趣S1 M2）
//
// - 显示微信扫码授权二维码（lv_qrcode），状态行提示 等待扫码/已扫码/成功/失败
// - 事件来自 weread::ink 服务层（Network 线程，本屏经 ScreenLvAsync 转 LVGL 执行）
// - 登录成功自动返回并 toast；kDone 回调供上层接续（在线阅读下载）
// ---------------------------------------------------------------------------

class WereadQrScreen {
public:
    // on_done：登录成功后的接续（在 LVGL 线程执行；可为空）
    static void Show(const char* back_screen_id, void (*on_done)(void*), void* on_done_user);
    static bool IsActive();
};
