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

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
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
constexpr int kRowsPerPage = 6;
constexpr int kListFrameBorder = 2;
constexpr int kListFramePad = 8;
constexpr int kKeyThrottleMs = 3000;

struct Ui {
    lv_obj_t* screen = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* list_frame = nullptr;
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* key_btn = nullptr;   // 未配置 Key 时的输入按钮
};

Ui s_ui;
bool s_alive = false;
int s_page = 0;
std::vector<weread::ShelfBook> s_books;
std::atomic<bool> s_busy{false};
std::atomic<int64_t> s_last_sync_us{0};

// 前向声明（定义在文件后部）
void OnRowClicked(lv_event_t* e);
void OpenDetail(int idx);
void BackToShelf();

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

int PageCount() {
    return (static_cast<int>(s_books.size()) + kRowsPerPage - 1) / kRowsPerPage;
}

void FillRow(lv_obj_t* row, const weread::ShelfBook& b) {
    lv_obj_t* title = lv_label_create(row);
    std::string text = b.title;
    if (b.finish_reading) text += "  ✓";
    lv_label_set_text(title, text.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_HOR_RES - kPad * 2 - kListFramePad * 2 - kListFrameBorder * 2 - 8);
    lv_obj_set_height(title, kRowTitleH);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* author = lv_label_create(row);
    std::string atext = b.author.empty() ? " " : b.author;
    if (!b.category.empty()) {
        atext += " · ";
        atext += b.category;
    }
    lv_label_set_text(author, atext.c_str());
    lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
    lv_obj_set_width(author, LV_HOR_RES - kPad * 2 - kListFramePad * 2 - kListFrameBorder * 2 - 8);
    lv_obj_set_style_text_font(author, UiFont(), 0);
    lv_obj_set_style_text_color(author, lv_color_black(), 0);
    lv_obj_set_style_text_opa(author, LV_OPA_60, 0);
    lv_obj_align(author, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    lv_obj_clear_flag(author, LV_OBJ_FLAG_CLICKABLE);
}

void RenderPage() {
    if (s_ui.list_frame == nullptr) return;
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
        FillRow(row, s_books[start + i]);
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

void SetStatus(const char* text) {
    if (s_ui.status_lbl != nullptr) lv_label_set_text(s_ui.status_lbl, text ? text : "");
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
    s_books = std::move(msg->books);
    s_page = 0;
    SetStatus("");
    RenderPage();
    if (!msg->raw_body.empty()) {
        weread::SaveShelfCacheBody(msg->raw_body);  // 失败静默：缓存是优化不是必须
    }
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

// ---------------------------------------------------------------------------
// 图书详情页：基础信息 + 后台拉取阅读进度 / 划线数
// ---------------------------------------------------------------------------

struct DetailResultMsg {
    int idx = -1;
    int progress = -1;   // -1 = 未知
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
    char buf[192];
    std::snprintf(buf, sizeof(buf), "阅读进度: %s\n划线/笔记: %s",
                  msg->progress >= 0 ? (std::to_string(msg->progress) + "%").c_str() : "未知",
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

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, b.title.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_HOR_RES - kPad * 4);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad * 2, 56);

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
    lv_obj_align(meta, LV_ALIGN_TOP_LEFT, kPad * 2, 200);

    lv_obj_t* info = lv_label_create(scr);
    lv_label_set_text(info, "阅读进度: 加载中…");
    lv_obj_set_style_text_font(info, UiFont(), 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, kPad * 2, 420);
    s_d_info = info;

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
}

void OnScreenUnloaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_ui.screen) return;
    // 不清数据：键盘页/详情页临时顶上来时保留；真正离开应用由 Create() 全量重置
    s_alive = false;
    if (s_detail == nullptr) {
        PowerPolicy::GetInstance().Release(PowerNeed::ForegroundNetwork);
        ScreenVolumeKeysPaging(false);
    }
}

}  // namespace

lv_obj_t* WereadScreen::Create() {
    s_ui = Ui{};
    s_alive = true;   // Create 即视为激活（Loaded 之前可能先收到异步结果）
    s_page = 0;
    s_books.clear();
    s_busy.store(false);
    s_detail_idx = -1;
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

    lv_obj_t* refresh = lv_btn_create(header);
    lv_obj_set_size(refresh, 110, 40);
    lv_obj_set_style_bg_color(refresh, lv_color_black(), 0);
    lv_obj_t* rl = lv_label_create(refresh);
    lv_label_set_text(rl, Lang::Strings::WEREAD_REFRESH);
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(refresh, OnRefreshClicked, LV_EVENT_CLICKED, nullptr);

    // 列表外框
    const int list_y = status.height + kPad + kHeaderH + kRowGap;
    const int list_h = kRowsPerPage * (kRowH + kRowGap) + kListFramePad * 2 + kListFrameBorder * 2;
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
