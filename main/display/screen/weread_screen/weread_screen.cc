#include "weread_screen/weread_screen.h"

#include <application.h>
#include <board.h>
#include <assets/lang_config.h>
#include <power_policy.h>
#include <weread/weread_service.h>

#include "screen_common.h"
#include "vk_key_handler.h"
#include "fontpack_lvgl.h"
#include "lansync_screen/lansync_screen.h"
#include "epd_content_policy.h"
#include "reader/image_util.h"
#include "reader/reader_types.h"
#include "weread_qr_screen.h"

#include <weread/weread_ink.h>
#include <weread/weread_online.h>
#include "book_screen/book_screen.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 布局常量（参考 task_screen 的尺寸习惯，行高固定、无滚动）
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "WereadScreen";
constexpr const char* kScreenId = "weread";

constexpr int kPad = 12;
constexpr int kRowGap = 8;
constexpr int kHeaderH = 44;
constexpr int kFooterH = 36;
constexpr int kRowH = 132;       // 两行文本（标题+作者）
constexpr int kRowTitleH = 40;
constexpr int kRowsPerPage = 7;
constexpr int kListFrameBorder = 2;
constexpr int kListFramePad = 8;
constexpr int kKeyThrottleMs = 3000;

// 封面缩略图（有 cover URL 的行/格预留，后台补齐后贴图）
constexpr int kThumbW = 80;    // 列表行槽（解码尺寸的等比缩小显示）
constexpr int kThumbH = 112;
constexpr int kThumbGap = 12;
constexpr int kGridCols = 3;   // 网格 3x3=9 本/页
constexpr int kGridRows = 3;
constexpr int kGridGap = 16;
constexpr int kGridTitleH = 36;
constexpr int kCoverBoxW = 200;   // 封面盒上限（网格按格动态计算，详情页用满）
constexpr int kCoverBoxH = 267;
constexpr int kDecodeW = 200;     // 统一解码尺寸：覆盖网格/详情最大槽位
constexpr int kDecodeH = 268;

struct Ui {
    lv_obj_t* screen = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* list_frame = nullptr;
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* key_btn = nullptr;    // 未配置 Key 时的输入按钮
    lv_obj_t* mode_btn = nullptr;   // 网格/列表切换
};

Ui s_ui;
bool s_alive = false;
bool s_grid = false;   // 书架浏览模式（NVS 持久化，网格为默认）
int s_page = 0;
std::vector<weread::ShelfBook> s_books;
std::atomic<bool> s_busy{false};
std::atomic<int64_t> s_last_sync_us{0};

// 前向声明（定义在文件后部）
void OnRowClicked(lv_event_t* e);
void OpenDetail(int idx);
void BackToShelf();
void OnOnlineReadClicked(lv_event_t* e);
void RenderPage();
void AttachThumb(lv_obj_t* box, const reader::RasterImage& img, int box_w, int box_h);
void PatchRowThumb(int index);
void CoverFillApplyOne(void* user_data);
void CoverFillScheduleNext();
void StartCoverFill();

// ---------------------------------------------------------------------------
// 封面缩略图（M1 封面：cover URL → SD 原始字节缓存 → 解码 L8 → 80x112 槽位）
//
// 链式后台填充：每本书一个 Network 域任务（先查 SD 缓存，miss 才保网下载），
// 完成后经 ScreenLvAsync 回 LVGL 线程解码入 s_covers 并补当前可见行，然后
// 续下一本。epoch 在 Create / 屏幕卸载 / 新书架同步时 +1 使旧链失效；
// 全部状态（s_fill / s_covers / s_cover_bad）只在 LVGL 线程读写。
// ---------------------------------------------------------------------------
struct CoverWorkItem {
    int index = -1;          // s_books 下标
    std::string book_id;
    std::string url;
};
struct CoverFillState {
    uint32_t epoch = 0;
    size_t cursor = 0;
    std::vector<CoverWorkItem> items;
};
struct CoverFillMsg {
    uint32_t epoch = 0;
    int index = -1;
    std::string book_id;
    std::vector<uint8_t> bytes;  // 封面原始字节（下载或缓存读出）；失败为空
};

std::unique_ptr<CoverFillState> s_fill;
std::unordered_map<std::string, std::unique_ptr<reader::RasterImage>> s_covers;
std::unordered_set<std::string> s_cover_bad;  // 本次会话解码/下载失败，不再重试
std::atomic<uint32_t> s_cover_epoch{0};
std::atomic<bool> s_cover_abort{false};
std::atomic<bool> s_cover_net_dead{false};  // 本链 EnsureNetwork 失败过，跳过后续下载

// 键盘状态
lv_obj_t* s_kb_screen = nullptr;
lv_obj_t* s_kb_input_lbl = nullptr;  // 键盘页输入显示行（键盘页为单例，直接静态持有）
std::string s_kb_input;
bool s_kb_shift = false;

// 图书详情页状态
lv_obj_t* s_detail = nullptr;
lv_obj_t* s_d_info = nullptr;
int s_detail_idx = -1;

struct KbKeyRef {
    lv_obj_t* label;
    char base;
};
std::vector<KbKeyRef> s_kb_char_keys;  // 字符键标签，SHIFT 切换时刷新显示

void KbRefreshCharKeys() {
    for (const KbKeyRef& k : s_kb_char_keys) {
        char c = k.base;
        if (s_kb_shift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        char text[2] = {c, '\0'};
        lv_label_set_text(k.label, text);
    }
}

void DisableScroll(lv_obj_t* obj) {
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
const lv_font_t* UiTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : UiFont();
}

// ---------------------------------------------------------------------------
// 书架渲染
// ---------------------------------------------------------------------------

int ItemsPerPage() { return s_grid ? kGridCols * kGridRows : kRowsPerPage; }

int PageCount() {
    return (static_cast<int>(s_books.size()) + ItemsPerPage() - 1) / ItemsPerPage();
}

void FillRow(lv_obj_t* row, const weread::ShelfBook& b, int index) {
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    // 有 cover URL 且本会话未标记失败的行：左侧预留 80x112 缩略图槽
    const bool want_thumb = !b.cover.empty() && s_cover_bad.count(b.book_id) == 0;
    const int text_x = want_thumb ? (kThumbW + kThumbGap) : 4;
    const int text_w = LV_HOR_RES - kPad * 2 - kListFramePad * 2 - kListFrameBorder * 2 - 8 -
                       (want_thumb ? (kThumbW + kThumbGap) : 0);

    if (want_thumb) {
        lv_obj_t* box = lv_obj_create(row);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, kThumbW, kThumbH);
        lv_obj_set_style_bg_color(box, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(box);
        auto it = s_covers.find(b.book_id);
        if (it != s_covers.end() && it->second != nullptr && !it->second->empty()) {
            AttachThumb(box, *it->second, kThumbW, kThumbH);
        }
    }

    lv_obj_t* title = lv_label_create(row);
    std::string text = b.title;
    if (b.finish_reading) text += "  ✓";
    lv_label_set_text(title, text.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, text_w);
    lv_obj_set_height(title, kRowTitleH);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, text_x, 2);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* author = lv_label_create(row);
    std::string atext = b.author.empty() ? " " : b.author;
    if (!b.category.empty()) {
        atext += " · ";
        atext += b.category;
    }
    lv_label_set_text(author, atext.c_str());
    lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
    lv_obj_set_width(author, text_w);
    lv_obj_set_style_text_font(author, UiFont(), 0);
    lv_obj_set_style_text_color(author, lv_color_black(), 0);
    lv_obj_set_style_text_opa(author, LV_OPA_60, 0);
    lv_obj_align(author, LV_ALIGN_BOTTOM_LEFT, text_x, -2);
    lv_obj_clear_flag(author, LV_OBJ_FLAG_CLICKABLE);
}

void RenderListPage() {
    if (s_ui.list_frame == nullptr) return;
    lv_obj_set_style_layout(s_ui.list_frame, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(s_ui.list_frame, LV_FLEX_FLOW_COLUMN);
    // 行距均分撑满框高（7 行/页；尾页不满时各行也会均匀铺开而非堆顶）
    lv_obj_set_flex_align(s_ui.list_frame, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clean(s_ui.list_frame);

    const int total = PageCount();
    if (s_page >= total) s_page = total > 0 ? total - 1 : 0;

    char page_text[32];
    if (total == 0) {
        std::snprintf(page_text, sizeof(page_text), "0 / 0");
    } else {
        std::snprintf(page_text, sizeof(page_text), "%d / %d", s_page + 1, total);
    }
    lv_label_set_text(s_ui.page_lbl, page_text);

    const int start = s_page * kRowsPerPage;
    for (int i = 0; i < kRowsPerPage && start + i < static_cast<int>(s_books.size()); ++i) {
        lv_obj_t* row = lv_obj_create(s_ui.list_frame);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_HOR_RES - kPad * 2 - kListFramePad * 2 - kListFrameBorder * 2);
        lv_obj_set_height(row, kRowH);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(start + i)));
        DisableScroll(row);
        FillRow(row, s_books[start + i], start + i);
    }

    if (s_books.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.list_frame);
        lv_label_set_text(hint, weread::HasApiKey() ? Lang::Strings::WEREAD_EMPTY
                                                    : Lang::Strings::WEREAD_NO_KEY);
        lv_obj_set_style_text_font(hint, UiFont(), 0);
        lv_obj_set_style_text_color(hint, lv_color_black(), 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_ui.key_btn != nullptr) {
        if (weread::HasApiKey()) lv_obj_add_flag(s_ui.key_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_ui.key_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void RenderEmptyState() {
    lv_obj_t* hint = lv_label_create(s_ui.list_frame);
    lv_label_set_text(hint, weread::HasApiKey() ? Lang::Strings::WEREAD_EMPTY
                                                : Lang::Strings::WEREAD_NO_KEY);
    lv_obj_set_style_text_font(hint, UiFont(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    if (s_ui.key_btn != nullptr) {
        if (weread::HasApiKey()) lv_obj_add_flag(s_ui.key_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_ui.key_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// 网格模式：3x3 封面格，封面 170x227（3:4 书型）+ 下方居中书名
void RenderGridPage() {
    if (s_ui.list_frame == nullptr) return;
    // 格子用绝对坐标排布，需脱离列表模式的纵向 flex（LVGL9 关 flex = 回 LV_LAYOUT_NONE）
    lv_obj_set_style_layout(s_ui.list_frame, LV_LAYOUT_NONE, 0);
    lv_obj_clean(s_ui.list_frame);

    const int total = PageCount();
    if (s_page >= total) s_page = total > 0 ? total - 1 : 0;

    char page_text[32];
    if (total == 0) {
        std::snprintf(page_text, sizeof(page_text), "0 / 0");
    } else {
        std::snprintf(page_text, sizeof(page_text), "%d / %d", s_page + 1, total);
    }
    lv_label_set_text(s_ui.page_lbl, page_text);

    if (s_books.empty()) {
        RenderEmptyState();
        return;
    }

    const int content_w = LV_HOR_RES - kPad * 2 - kListFramePad * 2 - kListFrameBorder * 2;
    const int frame_content_h = static_cast<int>(lv_obj_get_content_height(s_ui.list_frame));
    const int cell_w = (content_w - kGridGap * (kGridCols - 1)) / kGridCols;
    const int cell_h = (frame_content_h - kGridGap * (kGridRows - 1)) / kGridRows;
    // 封面取 3:4 书型上限：先限宽（格宽-边距），再限高（格高-书名行）
    int cover_w = std::min(cell_w - 16, kCoverBoxW);
    int cover_h = cover_w * 4 / 3;
    const int cover_h_max = std::min(cell_h - 8 - kGridTitleH, kCoverBoxH);
    if (cover_h > cover_h_max) {
        cover_h = cover_h_max;
        cover_w = cover_h * 3 / 4;
    }

    const int start = s_page * (kGridCols * kGridRows);
    const int count = std::min(kGridCols * kGridRows,
                               static_cast<int>(s_books.size()) - start);
    for (int i = 0; i < count; ++i) {
        const int idx = start + i;
        const int col = i % kGridCols;
        const int row = i / kGridCols;
        const weread::ShelfBook& b = s_books[static_cast<size_t>(idx)];

        lv_obj_t* cell = lv_obj_create(s_ui.list_frame);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_pos(cell, col * (cell_w + kGridGap), row * (cell_h + kGridGap));
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
        lv_obj_add_event_cb(cell, OnRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
        DisableScroll(cell);

        // 封面盒：无 URL / 本会话已失败的书不预留（书名仍在格底，视觉整齐）
        if (!b.cover.empty() && s_cover_bad.count(b.book_id) == 0) {
            lv_obj_t* box = lv_obj_create(cell);
            lv_obj_remove_style_all(box);
            lv_obj_set_size(box, cover_w, cover_h);
            lv_obj_set_style_bg_color(box, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(box, lv_color_black(), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            // 在书名行之上的区域垂直居中
            const int box_y = std::max(0, (cell_h - 8 - kGridTitleH - cover_h) / 2);
            lv_obj_align(box, LV_ALIGN_TOP_MID, 0, box_y);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(box);
            auto it = s_covers.find(b.book_id);
            if (it != s_covers.end() && it->second != nullptr && !it->second->empty()) {
                AttachThumb(box, *it->second, cover_w, cover_h);
            }
        }

        lv_obj_t* title = lv_label_create(cell);
        std::string text = b.title;
        if (b.finish_reading) text += " ✓";
        lv_label_set_text(title, text.c_str());
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, cell_w);
        lv_obj_set_style_text_font(title, UiFont(), 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(title, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    }
}

void RenderPage() {
    if (s_grid) {
        RenderGridPage();
    } else {
        RenderListPage();
    }
}

void SetStatus(const char* text) {
    if (s_ui.status_lbl != nullptr) lv_label_set_text(s_ui.status_lbl, text ? text : "");
}

// ---------------------------------------------------------------------------
// 封面缩略图：渲染助手 + 链式后台填充
// ---------------------------------------------------------------------------

// 把封面图等比缩放进槽位并贴上（解码统一为网格尺寸 170x227，列表槽缩小显示）
void AttachThumb(lv_obj_t* box, const reader::RasterImage& img, int box_w, int box_h) {
    lv_obj_clean(box);
    lv_obj_t* thumb = lv_image_create(box);
    lv_image_set_src(thumb, &img.dsc);
    // 与本地书架一致：封面走保灰阶路径（Shelf 不压暗），不用云列表的纯黑白
    EpdSetContentRole(thumb, EpdContentRole::Shelf);
    int w = img.width;
    int h = img.height;
    if (w > box_w) {
        h = static_cast<int>((static_cast<int64_t>(h) * box_w) / w);
        w = box_w;
    }
    if (h > box_h) {
        w = static_cast<int>((static_cast<int64_t>(w) * box_h) / h);
        h = box_h;
    }
    lv_obj_set_size(thumb, w, h);
    lv_obj_center(thumb);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* FindRowByIndex(int index) {
    if (s_ui.list_frame == nullptr || !lv_obj_is_valid(s_ui.list_frame)) return nullptr;
    const uint32_t n = lv_obj_get_child_count(s_ui.list_frame);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(s_ui.list_frame, i);
        if (row != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row))) == index) {
            return row;
        }
    }
    return nullptr;
}

// 行/格内的封面盒：按控件类型找（网格盒尺寸动态，不能按固定尺寸匹配）
lv_obj_t* FindThumbBox(lv_obj_t* row) {
    if (row == nullptr) return nullptr;
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(row, i);
        if (c != nullptr && lv_obj_check_type(c, &lv_obj_class)) return c;
    }
    return nullptr;
}

void PatchRowThumb(int index) {
    if (index < 0 || index >= static_cast<int>(s_books.size())) return;
    auto it = s_covers.find(s_books[static_cast<size_t>(index)].book_id);
    if (it == s_covers.end() || it->second == nullptr || it->second->empty()) return;
    lv_obj_t* row = FindRowByIndex(index);
    if (row == nullptr) return;  // 非本页：数据已在 s_covers，翻页 RenderPage 时带上
    lv_obj_t* box = FindThumbBox(row);
    if (box == nullptr) return;  // 无槽位（无 URL/已失败）：无需补图
    AttachThumb(box, *it->second, lv_obj_get_width(box), lv_obj_get_height(box));
}

// Network 域任务：单本封面取数（先 SD 缓存，miss 才保网下载并回写缓存）
void CoverFillJob(const CoverWorkItem& item, uint32_t epoch) {
    auto* msg = new (std::nothrow) CoverFillMsg{};
    if (msg == nullptr) return;  // 链断：下次 StartCoverFill 重建
    msg->epoch = epoch;
    msg->index = item.index;
    msg->book_id = item.book_id;
    bool ok = weread::LoadCoverCache(item.book_id, msg->bytes);
    if (!ok && !item.url.empty() && !s_cover_net_dead.load()) {
        // 封面是 nice-to-have：短保网，失败一次则本链余下全部跳过
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
        do {
            if (!Board::GetInstance().EnsureNetworkReady(8000)) {
                s_cover_net_dead.store(true);
                break;
            }
            std::string err;
            ok = weread::FetchCoverBytes(item.url, msg->bytes, err);
        } while (false);
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        if (ok) {
            // 缓存写失败不判失败：本次已拿到字节，仅少一层加速
            weread::SaveCoverCache(item.book_id, msg->bytes.data(), msg->bytes.size());
        }
    }
    if (!ok) msg->bytes.clear();
    if (!ScreenLvAsync(CoverFillApplyOne, msg)) {
        delete msg;  // 屏已销毁：链随卸载终止，回页后 Loaded 重建
    }
}

// LVGL 线程：续链（下一本）
void CoverFillScheduleNext() {
    if (s_fill == nullptr) return;
    if (!s_alive || s_cover_abort.load() || s_fill->epoch != s_cover_epoch.load()) {
        s_fill.reset();
        return;
    }
    if (s_fill->cursor >= s_fill->items.size()) {
        s_fill.reset();
        return;
    }
    const CoverWorkItem item = s_fill->items[s_fill->cursor++];
    const uint32_t epoch = s_fill->epoch;
    Application::GetInstance().Schedule([item, epoch]() { CoverFillJob(item, epoch); },
                                        Application::TaskDomain::Network);
}

// LVGL 线程：单本封面回屏（解码入 map + 补当前可见行 + 续链）
void CoverFillApplyOne(void* user_data) {
    std::unique_ptr<CoverFillMsg> msg(static_cast<CoverFillMsg*>(user_data));
    if (msg == nullptr) return;
    if (!s_alive || s_cover_abort.load() || msg->epoch != s_cover_epoch.load()) {
        if (s_fill != nullptr && s_fill->epoch == msg->epoch) s_fill.reset();
        return;  // 旧链结果：丢弃且不续链
    }
    bool ok = false;
    if (!msg->bytes.empty() && msg->index >= 0 && msg->index < static_cast<int>(s_books.size()) &&
        s_books[static_cast<size_t>(msg->index)].book_id == msg->book_id) {
        std::unique_ptr<reader::RasterImage> img(new (std::nothrow) reader::RasterImage());
        if (img != nullptr &&
            reader::DecodeImageToL8(msg->bytes.data(), msg->bytes.size(), kDecodeW, kDecodeH,
                                    *img) &&
            !img->empty()) {
            img->BindDsc();
            s_covers[msg->book_id] = std::move(img);
            PatchRowThumb(msg->index);
            ok = true;
        }
    }
    if (!ok) s_cover_bad.insert(msg->book_id);
    CoverFillScheduleNext();
}

// LVGL 线程：启动/续跑封面填充链（幂等：同 epoch 链存活则直接返回）
void StartCoverFill() {
    if (!s_alive) return;
    if (s_fill != nullptr && !s_cover_abort.load() && s_fill->epoch == s_cover_epoch.load()) {
        return;  // 链存活
    }
    s_fill.reset();
    if (s_books.empty()) return;

    std::unique_ptr<CoverFillState> state(new (std::nothrow) CoverFillState());
    if (state == nullptr) return;
    state->epoch = ++s_cover_epoch;
    state->cursor = 0;

    // 当前页优先（翻页立即有图），其余按书架顺序补齐
    const int per = ItemsPerPage();
    const int start = s_page * per;
    std::vector<char> queued(s_books.size(), 0);
    for (int i = start; i < start + per && i < static_cast<int>(s_books.size()); ++i) {
        queued[static_cast<size_t>(i)] = 1;
    }
    auto append = [&](int i) {
        const weread::ShelfBook& b = s_books[static_cast<size_t>(i)];
        if (b.cover.empty() || b.book_id.empty()) return;
        if (s_covers.count(b.book_id) || s_cover_bad.count(b.book_id)) return;
        CoverWorkItem it;
        it.index = i;
        it.book_id = b.book_id;
        it.url = b.cover;
        state->items.push_back(std::move(it));
    };
    for (int i = 0; i < static_cast<int>(s_books.size()); ++i) {
        if (queued[static_cast<size_t>(i)]) append(i);
    }
    for (int i = 0; i < static_cast<int>(s_books.size()); ++i) {
        if (!queued[static_cast<size_t>(i)]) append(i);
    }
    if (state->items.empty()) return;

    s_cover_abort.store(false);
    s_cover_net_dead.store(false);
    s_fill = std::move(state);
    CoverFillScheduleNext();
}

// ---------------------------------------------------------------------------
// 网络同步：Schedule(Network 域) 内 Ensure 网络 + 拉取，结果经 ScreenLvAsync 上屏
// ---------------------------------------------------------------------------

struct SyncResultMsg {
    std::vector<weread::ShelfBook> books;
    std::string raw_body;   // 同步成功后写入 SD 缓存
    std::string err;
    bool has_key = false;
};

void AsyncApplySync(void* user_data) {
    std::unique_ptr<SyncResultMsg> msg(static_cast<SyncResultMsg*>(user_data));
    if (!s_alive) {
        ESP_LOGW(kTag, "sync result dropped: screen not active");
        s_busy.store(false);
        return;
    }
    s_busy.store(false);
    if (!msg->err.empty()) {
        SetStatus(msg->err.c_str());
        return;
    }
    // 新书架数据：旧封面链失效；失败标记清零（URL/网络可能已恢复）
    ++s_cover_epoch;
    s_fill.reset();
    s_cover_bad.clear();
    s_books = std::move(msg->books);
    s_page = 0;
    SetStatus("");
    RenderPage();
    if (!msg->raw_body.empty()) {
        weread::SaveShelfCacheBody(msg->raw_body);  // 失败静默：缓存是优化不是必须
    }
    StartCoverFill();
}

void StartSync() {
    const int64_t now = esp_timer_get_time();
    if (s_busy.load()) return;
    if (now - s_last_sync_us.load() < kKeyThrottleMs * 1000LL) return;
    if (!weread::HasApiKey()) {
        SetStatus(Lang::Strings::WEREAD_NO_KEY);
        RenderPage();
        return;
    }
    s_last_sync_us.store(now);
    s_busy.store(true);
    SetStatus(Lang::Strings::WEREAD_LOADING);

    Application::GetInstance().Schedule([now]() {
        auto* msg = new SyncResultMsg{};
        msg->has_key = weread::HasApiKey();
        // 云同步：硬占网，持有期间不计无操作进待机，防同步中途断网
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
        do {
            const int64_t net_begin = esp_timer_get_time();
            if (!Board::GetInstance().EnsureNetworkReady(30000)) {
                msg->err = Lang::Strings::WEREAD_NET_NOT_READY;
                ESP_LOGW(kTag, "ensure network timeout after %lld ms",
                         (long long)((esp_timer_get_time() - net_begin) / 1000));
                break;
            }
            ESP_LOGI(kTag, "network ready in %lld ms",
                     (long long)((esp_timer_get_time() - net_begin) / 1000));
            std::string err;
            if (!weread::FetchShelf(msg->books, msg->raw_body, err)) {
                // 连接类错误重试一次
                vTaskDelay(pdMS_TO_TICKS(500));
                Board::GetInstance().EnsureNetworkReady(15000);
                if (!weread::FetchShelf(msg->books, msg->raw_body, err)) {
                    msg->err = Lang::Strings::WEREAD_SYNC_FAIL;
                    if (!err.empty()) {
                        msg->err += ": ";
                        msg->err += err;
                    }
                }
            }
        } while (false);
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        if (now != s_last_sync_us.load()) { /* noop：仅消告警 */ }
        if (!ScreenLvAsync(AsyncApplySync, msg)) {
            delete msg;
        }
    }, Application::TaskDomain::Network);
}

// ---------------------------------------------------------------------------
// 简版 Key 键盘（字符集 [A-Za-z0-9_-]，符合 wrk- Key 格式）
// ---------------------------------------------------------------------------

const char* kKeyRows[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm_",
};

void KbUpdateInput() { /* 输入行刷新 */ }

void KbRefreshInputLabel(lv_obj_t* label) {
    lv_label_set_text(label, s_kb_input.empty() ? " " : s_kb_input.c_str());
}

void KbClose(bool save) {
    if (save) {
        if (!weread::SetApiKey(s_kb_input)) {
            ESP_LOGW(kTag, "key rejected by format check");
        }
    }
    lv_obj_t* kb = s_kb_screen;
    s_kb_screen = nullptr;
    s_kb_input_lbl = nullptr;
    s_kb_char_keys.clear();
    if (kb != nullptr) {
        // 先切回书架页，再异步删除键盘页（禁止在自身事件里直接删当前 screen）
        if (s_ui.screen != nullptr) {
            lv_screen_load(s_ui.screen);
        }
        lv_obj_delete_async(kb);
    }
    if (s_alive) {
        RenderPage();
        if (save && weread::HasApiKey()) StartSync();
    }
}

void OnKbKey(lv_event_t* e) {
    const char* ch = static_cast<const char*>(lv_event_get_user_data(e));
    if (ch == nullptr) return;
    if (std::strcmp(ch, "BS") == 0) {
        if (!s_kb_input.empty()) s_kb_input.pop_back();
    } else if (std::strcmp(ch, "OK") == 0) {
        KbClose(true);
        return;
    } else if (std::strcmp(ch, "CANCEL") == 0) {
        KbClose(false);
        return;
    } else if (std::strcmp(ch, "USEKEY") == 0) {
        // 一键采用剪贴板里的微信读书 Key：扫描粘贴位 1..3，首个合法值直接生效
        for (int n = 1; n <= 3; ++n) {
            const std::string slot = LansyncScreen::GetSlot(n);
            if (weread::SetApiKey(slot)) {
                s_kb_input = slot;
                KbClose(true);
                return;
            }
        }
        ESP_LOGW(kTag, "no valid weread key in clipboard slots");
        return;
    } else if (std::strncmp(ch, "PASTE", 5) == 0) {
        const int n = ch[5] >= '1' && ch[5] <= '3' ? ch[5] - '0' : 0;
        if (n > 0) {
            std::string slot = LansyncScreen::GetSlot(n);
            if (slot.empty()) return;
            for (char c : slot) {
                if (s_kb_input.size() >= 64) break;
                s_kb_input.push_back(c);
            }
        }
    } else if (std::strcmp(ch, "SHIFT") == 0) {
        s_kb_shift = !s_kb_shift;
        KbRefreshCharKeys();
    } else if (s_kb_input.size() < 64) {
        char c = ch[0];
        if (s_kb_shift && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        s_kb_input.push_back(c);
    }
    if (s_kb_input_lbl != nullptr) KbRefreshInputLabel(s_kb_input_lbl);
}

void OpenKeyInput() {
    if (s_kb_screen != nullptr) return;
    s_kb_input.clear();
    s_kb_shift = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_kb_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    // 输入显示行
    lv_obj_t* input = lv_label_create(scr);
    s_kb_input_lbl = input;
    lv_label_set_text(input, " ");
    lv_obj_set_width(input, LV_HOR_RES - kPad * 4);
    lv_obj_set_style_text_font(input, UiTitleFont(), 0);
    lv_obj_align(input, LV_ALIGN_TOP_MID, 0, 60);

    // 4 行按键
    const int key_w = 62;
    const int key_h = 60;
    const int gap = 6;
    int y = 140;
    for (int r = 0; r < 4; ++r) {
        const int n = static_cast<int>(std::strlen(kKeyRows[r]));
        const int row_w = n * key_w + (n - 1) * gap;
        int x = (LV_HOR_RES - row_w) / 2;
        for (int i = 0; i < n; ++i) {
            const char base = kKeyRows[r][i];
            char* ch = new char[2]{base, '\0'};
            lv_obj_t* btn = lv_btn_create(scr);
            lv_obj_set_size(btn, key_w, key_h);
            lv_obj_set_pos(btn, x, y);
            lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
            lv_obj_set_style_border_color(btn, lv_color_black(), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_t* lbl = lv_label_create(btn);
            char text[2] = {base, '\0'};
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            lv_obj_center(lbl);
            s_kb_char_keys.push_back({lbl, base});
            lv_obj_add_event_cb(btn, OnKbKey, LV_EVENT_CLICKED, ch);
            x += key_w + gap;
        }
        y += key_h + gap;
    }

    // 功能行：切换大小写 / 退格 / 完成 / 取消
    const char* fkeys[] = {"SHIFT", "BS", "OK", "CANCEL"};
    const int fkey_w = 140;
    const int fgap = 16;
    const int frow_w = 4 * fkey_w + 3 * fgap;
    int fx = (LV_HOR_RES - frow_w) / 2;
    for (const char* fk : fkeys) {
        lv_obj_t* btn = lv_btn_create(scr);
        lv_obj_set_size(btn, fkey_w, key_h);
        lv_obj_set_pos(btn, fx, y + 12);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, fk);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, OnKbKey, LV_EVENT_CLICKED, const_cast<char*>(fk));
        fx += fkey_w + fgap;
    }

    // 剪贴板行：USEKEY（一键采用剪贴板 Key）/ PASTE1 / PASTE2 / PASTE3
    const char* pkeys[] = {"USEKEY", "PASTE1", "PASTE2", "PASTE3"};
    const int pkey_w = 150;
    const int prow_w = 4 * pkey_w + 3 * 12;
    int px = (LV_HOR_RES - prow_w) / 2;
    y += key_h + 16;
    for (const char* pk : pkeys) {
        lv_obj_t* btn = lv_btn_create(scr);
        lv_obj_set_size(btn, pkey_w, key_h);
        lv_obj_set_pos(btn, px, y + 12);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, pk);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, OnKbKey, LV_EVENT_CLICKED, const_cast<char*>(pk));
        px += pkey_w + 12;
    }

    lv_screen_load(scr);
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

void OnRefreshClicked(lv_event_t* /*e*/) { StartSync(); }

void OnKeyBtnClicked(lv_event_t* /*e*/) { OpenKeyInput(); }

void UpdateModeBtnLabel() {
    if (s_ui.mode_btn == nullptr) return;
    lv_obj_t* l = lv_obj_get_child(s_ui.mode_btn, 0);
    if (l != nullptr) lv_label_set_text(l, s_grid ? "列表" : "网格");  // 按钮显示目标模式
}

void OnModeClicked(lv_event_t* /*e*/) {
    s_grid = !s_grid;
    weread::SetPreferGridView(s_grid);  // NVS 持久化，重启保持
    s_page = 0;
    UpdateModeBtnLabel();
    RenderPage();
    StartCoverFill();
}

// ---------------------------------------------------------------------------
// 图书详情页：基础信息 + 后台拉取阅读进度 / 划线数
// ---------------------------------------------------------------------------

struct DetailResultMsg {
    int idx = -1;
    int progress = -1;   // -1 = 未知
    int64_t reading_sec = -1;
    int bookmarks = -1;
    std::string err;
};

std::string FormatReadTime(int64_t epoch_sec) {
    if (epoch_sec <= 0) return "-";
    time_t t = static_cast<time_t>(epoch_sec);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}

void DetailApplyInfo(void* user_data) {
    std::unique_ptr<DetailResultMsg> msg(static_cast<DetailResultMsg*>(user_data));
    if (s_detail == nullptr || msg->idx != s_detail_idx || s_d_info == nullptr) return;
    if (!msg->err.empty()) {
        lv_label_set_text(s_d_info, "进度获取失败（网络或接口限制）");
        return;
    }
    std::string time_text = "暂无记录";
    if (msg->reading_sec > 0) {
        const int64_t h = msg->reading_sec / 3600;
        const int m = static_cast<int>((msg->reading_sec % 3600) / 60);
        time_text = h > 0 ? std::to_string(h) + "小时" + std::to_string(m) + "分"
                          : std::to_string(m) + "分钟";
    }
    char buf[224];
    std::snprintf(buf, sizeof(buf), "阅读进度: %s\n阅读时长: %s\n划线/笔记: %s",
                  msg->progress > 0 ? (std::to_string(msg->progress) + "%").c_str() : "暂无记录",
                  time_text.c_str(),
                  msg->bookmarks >= 0 ? (std::to_string(msg->bookmarks) + " 条").c_str() : "未知");
    lv_label_set_text(s_d_info, buf);
}

void BackToShelf() {
    if (s_detail != nullptr) {
        lv_obj_t* d = s_detail;
        s_detail = nullptr;
        s_detail_idx = -1;
        if (s_ui.screen != nullptr) lv_screen_load(s_ui.screen);
        lv_obj_delete_async(d);
    }
}

bool DetailOnVkKey(const char* key) {
    if (std::strcmp(key, "vk_prev") == 0) {
        ScreenLvAsync([](void*) { BackToShelf(); });
        return true;
    }
    return false;
}

void OpenDetail(int idx) {
    if (s_detail != nullptr || idx < 0 || idx >= static_cast<int>(s_books.size())) return;
    const weread::ShelfBook& b = s_books[idx];

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    // 左侧大封面（复用 s_covers 内存图；无 URL/已失败不预留，文本保持全宽）
    const bool show_cover = !b.cover.empty() && s_cover_bad.count(b.book_id) == 0;
    const int text_x = show_cover ? (kPad * 2 + kCoverBoxW + 20) : kPad * 2;
    const int text_w = LV_HOR_RES - text_x - kPad * 2;

    if (show_cover) {
        lv_obj_t* box = lv_obj_create(scr);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, kCoverBoxW, kCoverBoxH);
        lv_obj_set_style_bg_color(box, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_align(box, LV_ALIGN_TOP_LEFT, kPad * 2, 56);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(box);
        auto it = s_covers.find(b.book_id);
        if (it != s_covers.end() && it->second != nullptr && !it->second->empty()) {
            AttachThumb(box, *it->second, kCoverBoxW, kCoverBoxH);
        }
    }

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, b.title.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, text_w);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, text_x, 56);

    std::string base;
    base += b.author.empty() ? "-" : b.author;
    base += " · ";
    base += b.category.empty() ? "未分类" : b.category;
    base += "\n状态: ";
    base += b.finish_reading ? "已读完" : "在读/未读";
    base += "\n最近阅读: ";
    base += FormatReadTime(b.read_update_time);

    lv_obj_t* meta = lv_label_create(scr);
    lv_label_set_text(meta, base.c_str());
    lv_obj_set_style_text_font(meta, UiFont(), 0);
    lv_obj_align(meta, LV_ALIGN_TOP_LEFT, text_x, 236);

    lv_obj_t* info = lv_label_create(scr);
    lv_label_set_text(info, "阅读进度: 加载中…");
    lv_obj_set_style_text_font(info, UiFont(), 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, kPad * 2, 56 + kCoverBoxH + 24);
    s_d_info = info;

    lv_obj_t* online = lv_btn_create(scr);
    lv_obj_set_size(online, LV_HOR_RES - kPad * 4, 52);
    lv_obj_set_style_bg_color(online, lv_color_black(), 0);
    lv_obj_set_style_text_font(online, UiFont(), 0);
    lv_obj_align(online, LV_ALIGN_BOTTOM_LEFT, kPad * 2, -(kPad * 2 + 52 + 12));
    lv_obj_t* ol = lv_label_create(online);
    lv_label_set_text(ol, "在线阅读");
    lv_obj_set_style_text_font(ol, UiFont(), 0);
    lv_obj_set_style_text_color(ol, lv_color_white(), 0);
    lv_obj_center(ol);
    lv_obj_add_event_cb(online, OnOnlineReadClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "提示: 在线阅读(M2)即将提供；可用传书导入本地 epub/txt 阅读");
    lv_obj_set_style_text_font(hint, UiFont(), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, kPad * 2, -kPad * 2);

    VkKey_AttachScreen(scr, "weread_detail",
                       VkKeyScreenDesc{+[]() -> lv_obj_t* { return s_ui.screen; },
                                       DetailOnVkKey, nullptr, nullptr, nullptr,
                                       nullptr, nullptr, nullptr});

    s_detail = scr;
    s_detail_idx = idx;
    lv_screen_load(scr);

    // 后台拉取进度与划线数（OtaDownload 硬占网）
    Application::GetInstance().Schedule([idx, book_id = b.book_id]() {
        auto* msg = new DetailResultMsg{};
        msg->idx = idx;
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
        do {
            if (!Board::GetInstance().EnsureNetworkReady(30000)) {
                msg->err = "net";
                break;
            }
            std::string err;
            msg->progress = weread::FetchProgressPercent(book_id, err);
            err.clear();
            msg->reading_sec = weread::FetchReadingTimeSec(book_id, err);
            err.clear();
            weread::FetchBookmarkCount(book_id, msg->bookmarks, err);
        } while (false);
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        if (!ScreenLvAsync(DetailApplyInfo, msg)) delete msg;
    }, Application::TaskDomain::Network);
}

void OnRowClicked(lv_event_t* e) {
    const intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    OpenDetail(static_cast<int>(idx));
}

// M2 在线阅读（实验）：未登录先走墨水屏扫码登录，已登录直接拉正文合成 .ebook 并打开
void OnOnlineReadClicked(lv_event_t* /*e*/);

struct OnlineUiMsg {
    weread::online::UiEvent ev;
};

void OnlineUiApply(void* user_data) {
    std::unique_ptr<OnlineUiMsg> msg(static_cast<OnlineUiMsg*>(user_data));
    if (msg == nullptr) return;
    if (s_d_info != nullptr && lv_obj_is_valid(s_d_info)) {
        lv_label_set_text(s_d_info, msg->ev.message.c_str());
    }
    if (msg->ev.type == weread::online::UiEvent::Type::kDone) {
        ScreenShowToast("合成完成，正在打开…");
        BookScreen::OpenBookByPath(msg->ev.message.c_str());
    } else if (msg->ev.type == weread::online::UiEvent::Type::kError) {
        ScreenShowToast(msg->ev.message.c_str());
    }
}

void StartOnlineForCurrentBook() {
    if (s_detail_idx < 0 || s_detail_idx >= static_cast<int>(s_books.size())) return;
    const weread::ShelfBook& b = s_books[static_cast<size_t>(s_detail_idx)];
    weread::online::SetUiHandler([](const weread::online::UiEvent& ev) {
        auto* msg = new (std::nothrow) OnlineUiMsg{ev};
        if (msg == nullptr) return;
        if (!ScreenLvAsync(OnlineUiApply, msg)) delete msg;
    });
    if (!weread::online::StartOnlineRead(b.book_id, b.title, b.author)) {
        ScreenShowToast("已有下载在进行中");
    } else if (s_d_info != nullptr && lv_obj_is_valid(s_d_info)) {
        lv_label_set_text(s_d_info, "开始在线阅读…");
    }
}

void OnOnlineReadClicked(lv_event_t* /*e*/) {
    if (s_detail_idx < 0 || s_detail_idx >= static_cast<int>(s_books.size())) return;
    if (!weread::ink::HasCredentials()) {
        WereadQrScreen::Show(kScreenId, [](void*) { StartOnlineForCurrentBook(); }, nullptr);
        return;
    }
    StartOnlineForCurrentBook();
}

bool OnVkKey(const char* key) {
    if (s_kb_screen != nullptr) return false;  // 键盘页自行处理点击
    if (std::strcmp(key, "vk_prev") == 0) {
        if (s_page > 0) {
            --s_page;
            ScreenLvAsync([](void*) { RenderPage(); });
        }
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        if (s_page + 1 < PageCount()) {
            ++s_page;
            ScreenLvAsync([](void*) { RenderPage(); });
        }
        return true;
    }
    return false;
}

void OnScreenLoaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_ui.screen) return;
    s_alive = true;  // 从键盘页/详情页返回时重新激活
    // 前台联网页面：软保网，避免 30s 网络宽限到期后 WiFi 被电源策略降掉
    PowerPolicy::GetInstance().Acquire(PowerNeed::ForegroundNetwork);
    // 音量键映射为翻页（返回/离开本页时归还）
    ScreenVolumeKeysPaging(true);

    // 先进缓存（离线也能看书架），再后台刷新
    if (s_books.empty()) {
        std::string cached;
        if (weread::LoadShelfCacheBody(cached)) {
            std::vector<weread::ShelfBook> books;
            std::string perr;
            if (weread::ParseShelfBody(cached, books, perr)) {
                s_books = std::move(books);
                RenderPage();
                SetStatus("缓存");
            }
        }
    }
    // 自动刷新：仅当缓存缺失或距上次同步超过 60s（从详情返回不触发同步风暴）
    const bool stale = (esp_timer_get_time() - s_last_sync_us.load()) > 60LL * 1000000LL;
    if (weread::HasApiKey() && (s_books.empty() || stale)) StartSync();
    // 封面填充（幂等）：缓存书架离线可显，同步完成后由 AsyncApplySync 再启
    StartCoverFill();
}

void OnScreenUnloaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_ui.screen) return;
    // 不清数据：键盘页/详情页临时顶上来时保留；真正离开应用由 Create() 全量重置
    s_alive = false;
    // 终止封面链（离屏期间不再下载/补图）；回页后 Loaded 用新 epoch 重建
    s_cover_abort.store(true);
    ++s_cover_epoch;
    if (s_detail == nullptr) {
        PowerPolicy::GetInstance().Release(PowerNeed::ForegroundNetwork);
        ScreenVolumeKeysPaging(false);
    }
}

}  // namespace

lv_obj_t* WereadScreen::Create() {
    s_ui = Ui{};
    s_alive = true;   // Create 即视为激活（Loaded 之前可能先收到异步结果）
    s_grid = weread::PreferGridView();  // 上次选择的浏览模式（默认网格）
    s_page = 0;
    s_books.clear();
    s_busy.store(false);
    s_detail_idx = -1;
    // 封面状态全量重置（内存图 / 失败标记 / 填充链）；SD 缓存仍有效可秒回
    s_fill.reset();
    s_covers.clear();
    s_cover_bad.clear();
    ++s_cover_epoch;
    s_cover_abort.store(true);  // Loaded 之前不启链
    // 注意：不从这边删除旧详情页——屏幕生命周期归系统导航栈管理，
    // 它返回首页时可能已经删除；二次释放会造成 LVGL 堆损坏（表现为再进应用卡死）。
    // 极端情况下泄漏一个页面对象，可接受。
    s_detail = nullptr;

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);

    // 标题行：标题 + 状态 + 刷新
    lv_obj_t* header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_HOR_RES - kPad * 2, kHeaderH);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, status.height + kPad);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, kRowGap, 0);
    DisableScroll(header);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, Lang::Strings::WEREAD_TITLE);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);

    s_ui.status_lbl = lv_label_create(header);
    lv_label_set_text(s_ui.status_lbl, "");
    lv_obj_set_style_text_font(s_ui.status_lbl, UiFont(), 0);
    lv_obj_set_flex_grow(s_ui.status_lbl, 1);

    // 网格/列表切换（白底黑边；标签为目标模式名，选择持久化到 NVS）
    s_ui.mode_btn = lv_btn_create(header);
    lv_obj_set_size(s_ui.mode_btn, 110, 40);
    lv_obj_set_style_bg_color(s_ui.mode_btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_ui.mode_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ui.mode_btn, 2, 0);
    lv_obj_t* ml = lv_label_create(s_ui.mode_btn);
    lv_obj_set_style_text_color(ml, lv_color_black(), 0);
    lv_obj_center(ml);
    lv_obj_add_event_cb(s_ui.mode_btn, OnModeClicked, LV_EVENT_CLICKED, nullptr);
    UpdateModeBtnLabel();

    lv_obj_t* refresh = lv_btn_create(header);
    lv_obj_set_size(refresh, 110, 40);
    lv_obj_set_style_bg_color(refresh, lv_color_black(), 0);
    lv_obj_t* rl = lv_label_create(refresh);
    lv_label_set_text(rl, Lang::Strings::WEREAD_REFRESH);
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(refresh, OnRefreshClicked, LV_EVENT_CLICKED, nullptr);

    // 列表外框：高度动态吃满标题栏与底栏之间的全部可用高度
    const int list_y = status.height + kPad + kHeaderH + kRowGap;
    const int list_h = LV_VER_RES - list_y - kRowGap - kFooterH - kPad;
    s_ui.list_frame = lv_obj_create(scr);
    lv_obj_set_size(s_ui.list_frame, LV_HOR_RES - kPad * 2, list_h);
    lv_obj_align(s_ui.list_frame, LV_ALIGN_TOP_MID, 0, list_y);
    lv_obj_set_style_bg_color(s_ui.list_frame, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_ui.list_frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ui.list_frame, kListFrameBorder, 0);
    lv_obj_set_style_pad_all(s_ui.list_frame, kListFramePad, 0);
    lv_obj_set_flex_flow(s_ui.list_frame, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list_frame, kRowGap, 0);
    lv_obj_set_flex_align(s_ui.list_frame, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    DisableScroll(s_ui.list_frame);

    // 页码 + Key 输入按钮（未配置 Key 时显示）
    lv_obj_t* footer = lv_obj_create(scr);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_HOR_RES - kPad * 2, kFooterH);
    lv_obj_align(footer, LV_ALIGN_TOP_MID, 0, list_y + list_h + kRowGap);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(footer);

    s_ui.page_lbl = lv_label_create(footer);
    lv_label_set_text(s_ui.page_lbl, "0 / 0");
    lv_obj_set_style_text_font(s_ui.page_lbl, UiFont(), 0);

    s_ui.key_btn = lv_btn_create(footer);
    lv_obj_set_size(s_ui.key_btn, 200, kFooterH);
    lv_obj_set_style_bg_color(s_ui.key_btn, lv_color_black(), 0);
    lv_obj_t* kl = lv_label_create(s_ui.key_btn);
    lv_label_set_text(kl, "API Key");
    lv_obj_set_style_text_color(kl, lv_color_white(), 0);
    lv_obj_center(kl);
    lv_obj_add_event_cb(s_ui.key_btn, OnKeyBtnClicked, LV_EVENT_CLICKED, nullptr);

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{WereadScreen::Create, OnVkKey,
                                                       nullptr, nullptr, nullptr, nullptr,
                                                       nullptr, nullptr});
    return scr;
}

bool WereadScreen::IsActive() {
    return s_alive && s_ui.screen != nullptr;
}
