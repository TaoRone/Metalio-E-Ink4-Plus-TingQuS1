#pragma once

// ---------------------------------------------------------------------------
// WereadDigestScreen（微信读书 AI 大纲阅读页，听趣S1 M2）
//
// - 数据源：GET weread.qq.com/web/book/briefOutline?bookId=（官方 AI 精读，
//   网页 Cookie 鉴权，Cookie 由 QR 屏「粘贴网页Cookie」存入 NVS weread/web_cookie）
// - 结构：全书概要 + 主题问答列表；本页按段渲染，音量键/滑动翻页
// ---------------------------------------------------------------------------

class WereadDigestScreen {
public:
    // 拉取并展示（内部转 Network 域拉取，完成后上屏；任意线程可调）
    static void Show(const char* book_id, const char* book_title);
};
