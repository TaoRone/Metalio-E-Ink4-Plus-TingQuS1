#pragma once

#include "lvgl.h"

// ---------------------------------------------------------------------------
// WereadScreen（墨水屏 · 微信读书书架，听趣S1 M1）
//
// - 双模式浏览：网格 3x3 大封面（默认，吃满屏高）/ 信息列表 7 行，顶栏一键切换，
//   选择持久化 NVS weread/view_mode；vk_prev/vk_next 翻页，无滚动
// - 封面链：Network 域逐本取数（SD 原始字节缓存 covers/<book_id>.img，miss 才下载，
//   302 手动跟随），回 LVGL 线程解码 L8（≤200x268）入内存 map 并补当前可见行，
//   当前页优先；epoch 失效制，离屏中止
// - 详情页：左侧大封面 + 右侧标题/元信息 + 下方进度/划线数（后台拉取）
// - 进页：无缓存且已配置 Key → Application::Schedule(Network) 拉取书架
// - 未配置 Key：空态提示 + 「输入 API Key」按钮 → 内置简版键盘输入
// - 页码行右侧「刷新」：节流 3s，联网同步后上屏
// ---------------------------------------------------------------------------

class WereadScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
};
