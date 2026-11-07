#include "kosync_screen/kosync_screen.h"

#include <application.h>
#include <assets/lang_config.h>
#include <board.h>
#include <power_policy.h>
#include <reader/kosync.h>

#include "screen_common.h"
#include "vk_key_handler.h"
#include "fontpack_lvgl.h"
#include "lansync_screen/lansync_screen.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// KOReader 同步设置屏（听趣S1 Phase 3）
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "KosyncScreen";
constexpr const char* kScreenId = "kosync";

constexpr int kPad = 12;
constexpr int kRowH = 96;
constexpr int kRowGap = 10;
constexpr int kHeaderH = 44;
constexpr int kRowLabelH = 34;

struct Ui {
    lv_obj_t* screen = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* list = nullptr;        // 设置行容器
    lv_obj_t* auth_lbl = nullptr;    // 登录/注册结果行
};

Ui s_ui;
bool s_alive = false;

// 设置行标识
enum class RowId : intptr_t {
    kEnabled = 0,
    kServer = 1,
    kUsername = 2,
    kPassword = 3,
    kAutoJump = 4,
    kMatchMode = 5,
    kAutoSync = 6,
};

// ---------------------------------------------------------------------------
// 前向声明（定义在文件后部）
void RenderRows();
void OpenKeyboard(int field);
void ShowServerChooser();

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
const lv_font_t* UiTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : UiFont();
}

void DisableScroll(lv_obj_t* obj) {
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------------------------------------------------------------------------
// 键盘（全字符：ABC 页 + SYM 符号页；单例覆盖屏）
// ---------------------------------------------------------------------------

const char* kAbcRows[4] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm_",
};
const char* kSymRows[4] = {
    "!@#$%^&*()",
    "-_=+[]{};:",
    "'\",.<>/?`~",
    "|()<>",  // 与上行部分重复，保证末行有键（括号在密码里高频）
};

lv_obj_t* s_kb_screen = nullptr;
lv_obj_t* s_kb_input_lbl = nullptr;
lv_obj_t* s_kb_grid = nullptr;   // 字符键区容器（SYM 切换时重建）
std::string s_kb_input;
std::string s_kb_title;
bool s_kb_shift = false;
bool s_kb_sym = false;

struct KbCharKey {
    lv_obj_t* label;
    char base;
    int row;  // 0=abc 1=sym
};
std::vector<KbCharKey> s_kb_char_keys;

// 当前编辑目标：0=服务器 1=用户名 2=密码
int s_kb_field = -1;

void KbRefreshCharKeys() {
    for (const KbCharKey& k : s_kb_char_keys) {
        if ((k.row == 1) != s_kb_sym) continue;  // 只刷新当前页
        char c = k.base;
        if (!s_kb_sym && s_kb_shift && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        char text[2] = {c, '\0'};
        lv_label_set_text(k.label, text);
    }
}

void KbRebuildKeyGrid(lv_obj_t* scr, int key_w, int key_h, int gap, int y);

void KbClose(bool save) {
    const int field = s_kb_field;
    const std::string input = s_kb_input;
    s_kb_field = -1;
    if (save) {
        switch (field) {
            case 0:
                if (!reader::kosync::SetServer(input)) {
                    if (s_ui.auth_lbl != nullptr) {
                        lv_label_set_text(s_ui.auth_lbl, "服务器须以 https:// 开头");
                    }
                }
                break;
            case 1:
                if (!reader::kosync::SetUsername(input)) {
                    if (s_ui.auth_lbl != nullptr) {
                        lv_label_set_text(s_ui.auth_lbl, "用户名含非法字符（引号/反斜杠/控制符）");
                    }
                }
                break;
            case 2:
                if (!reader::kosync::SetPassword(input)) {
                    if (s_ui.auth_lbl != nullptr) {
                        lv_label_set_text(s_ui.auth_lbl, "密码含非法字符（引号/反斜杠/控制符）");
                    }
                }
                break;
            default:
                break;
        }
    }
    lv_obj_t* kb = s_kb_screen;
    s_kb_screen = nullptr;
    s_kb_input_lbl = nullptr;
    s_kb_grid = nullptr;
    s_kb_char_keys.clear();
    if (kb != nullptr) {
        if (s_ui.screen != nullptr) lv_screen_load(s_ui.screen);
        lv_obj_delete_async(kb);
    }
    if (s_alive) RenderRows();
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
    } else if (std::strcmp(ch, "SHIFT") == 0) {
        s_kb_shift = !s_kb_shift;
        KbRefreshCharKeys();
    } else if (std::strcmp(ch, "SYM") == 0) {
        s_kb_sym = !s_kb_sym;
        // 键是 s_kb_grid 容器的子对象，y 用容器相对坐标（与初始构建一致）
        KbRebuildKeyGrid(s_kb_screen, 62, 60, 6, 0);
        if (s_kb_screen != nullptr) lv_obj_invalidate(s_kb_screen);
    } else if (std::strncmp(ch, "PASTE", 5) == 0) {
        // 局域网剪贴板槽位 1..3 追加填入（网页端先粘贴到槽位）
        const int n = ch[5] >= '1' && ch[5] <= '3' ? ch[5] - '0' : 0;
        if (n > 0) {
            const std::string slot = LansyncScreen::GetSlot(n);
            for (char c : slot) {
                if (s_kb_input.size() >= 64) break;
                s_kb_input.push_back(c);
            }
        }
    } else if (s_kb_input.size() < 64) {
        char c = ch[0];
        // SHIFT 生效于 ABC 页字母（SYM 页无大小写概念）
        if (!s_kb_sym && s_kb_shift && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        s_kb_input.push_back(c);
    }
    if (s_kb_input_lbl != nullptr) {
        lv_label_set_text(s_kb_input_lbl, s_kb_input.empty() ? " " : s_kb_input.c_str());
    }
}

void KbRebuildKeyGrid(lv_obj_t* /*scr*/, int key_w, int key_h, int gap, int y) {
    if (s_kb_grid == nullptr) return;
    lv_obj_clean(s_kb_grid);
    s_kb_char_keys.clear();  // 旧按钮已随 clean 释放，标签指针全部作废，必须先清再重建
    const char* const* rows = s_kb_sym ? kSymRows : kAbcRows;
    for (int r = 0; r < 4; ++r) {
        const int n = static_cast<int>(std::strlen(rows[r]));
        const int row_w = n * key_w + (n - 1) * gap;
        int x = (LV_HOR_RES - row_w) / 2;
        for (int i = 0; i < n; ++i) {
            const char base = rows[r][i];
            char* ch = new char[2]{base, '\0'};
            lv_obj_t* btn = lv_btn_create(s_kb_grid);
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
            s_kb_char_keys.push_back({lbl, base, s_kb_sym ? 1 : 0});
            lv_obj_add_event_cb(btn, OnKbKey, LV_EVENT_CLICKED, ch);
            x += key_w + gap;
        }
        y += key_h + gap;
    }
    KbRefreshCharKeys();
}

void OpenKeyboard(int field) {
    if (s_kb_screen != nullptr) return;
    s_kb_field = field;
    s_kb_input.clear();
    s_kb_shift = false;
    s_kb_sym = false;
    s_kb_title = field == 0 ? "服务器地址" : (field == 1 ? "用户名" : "密码");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_kb_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, s_kb_title.c_str());
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad * 2, 56);

    lv_obj_t* input = lv_label_create(scr);
    s_kb_input_lbl = input;
    lv_label_set_text(input, " ");
    lv_obj_set_width(input, LV_HOR_RES - kPad * 4);
    lv_obj_set_style_text_font(input, UiTitleFont(), 0);
    lv_obj_align(input, LV_ALIGN_TOP_LEFT, kPad * 2, 108);

    // 字符键区容器（绝对定位子键；SYM 切换时重建内容）
    s_kb_grid = lv_obj_create(scr);
    lv_obj_remove_style_all(s_kb_grid);
    lv_obj_set_size(s_kb_grid, LV_HOR_RES, 4 * (60 + 6));
    lv_obj_align(s_kb_grid, LV_ALIGN_TOP_MID, 0, 168);
    DisableScroll(s_kb_grid);
    KbRebuildKeyGrid(scr, 62, 60, 6, 0);

    // 功能行：SHIFT / SYM / BS / OK / CANCEL
    const char* fkeys[] = {"SHIFT", "SYM", "BS", "OK", "CANCEL"};
    const int fkey_w = 118;
    const int fgap = 10;
    const int frow_w = 5 * fkey_w + 4 * fgap;
    int fx = (LV_HOR_RES - frow_w) / 2;
    const int fy = 168 + 4 * (60 + 6) + 14;
    for (const char* fk : fkeys) {
        lv_obj_t* btn = lv_btn_create(scr);
        lv_obj_set_size(btn, fkey_w, 60);
        lv_obj_set_pos(btn, fx, fy);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, fk);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, OnKbKey, LV_EVENT_CLICKED, const_cast<char*>(fk));
        fx += fkey_w + fgap;
    }

    // 剪贴板行：PASTE1/2/3（局域网剪贴板网页端粘贴后一键填入）
    const char* pkeys[] = {"PASTE1", "PASTE2", "PASTE3"};
    const int pkey_w = (LV_HOR_RES - kPad * 2 - 2 * 12) / 3;
    int px = kPad;
    const int py = fy + 60 + 12;
    for (const char* pk : pkeys) {
        lv_obj_t* btn = lv_btn_create(scr);
        lv_obj_set_size(btn, pkey_w, 60);
        lv_obj_set_pos(btn, px, py);
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
// 设置行
// ---------------------------------------------------------------------------

struct RowMsg {
    RowId id;
};

void RowValueText(RowId id, char* buf, size_t buf_sz) {
    switch (id) {
        case RowId::kEnabled:
            std::snprintf(buf, buf_sz, "%s", reader::kosync::IsEnabled() ? "开" : "关");
            break;
        case RowId::kServer: {
            const std::string s = reader::kosync::Server();
            std::snprintf(buf, buf_sz, "%s", s.c_str());
            break;
        }
        case RowId::kUsername: {
            const std::string s = reader::kosync::Username();
            std::snprintf(buf, buf_sz, "%s", s.empty() ? "未设置" : s.c_str());
            break;
        }
        case RowId::kPassword:
            std::snprintf(buf, buf_sz, "%s",
                          reader::kosync::HasCredentials() ? "••••••" : "未设置");
            break;
        case RowId::kAutoJump:
            std::snprintf(buf, buf_sz, "%s", reader::kosync::AutoJump() ? "开" : "关");
            break;
        case RowId::kMatchMode:
            std::snprintf(buf, buf_sz, "%s",
                          reader::kosync::MatchMode() == "filename"
                              ? "文件名（两端书名相同即可）"
                              : "文件内容（两端文件须逐字节一致）");
            break;
        case RowId::kAutoSync:
            std::snprintf(buf, buf_sz, "%s",
                          reader::kosync::AutoSync() ? "开（开书拉/存盘推）" : "关（仅手动按钮）");
            break;
    }
}

const char* RowTitle(RowId id) {
    switch (id) {
        case RowId::kEnabled: return "启用 KOReader 同步";
        case RowId::kServer: return "服务器地址";
        case RowId::kUsername: return "用户名";
        case RowId::kPassword: return "密码";
        case RowId::kAutoJump: return "拉取后自动跳转";
        case RowId::kMatchMode: return "书籍匹配方式";
        case RowId::kAutoSync: return "自动同步";
    }
    return "";
}

void OnRowClicked(lv_event_t* e) {
    const auto id = static_cast<RowId>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    switch (id) {
        case RowId::kEnabled:
            reader::kosync::SetEnabled(!reader::kosync::IsEnabled());
            RenderRows();
            break;
        case RowId::kServer:
            ShowServerChooser();  // 预设（官方/自建）或自定义输入
            break;
        case RowId::kUsername:
            OpenKeyboard(1);
            break;
        case RowId::kPassword:
            OpenKeyboard(2);
            break;
        case RowId::kAutoJump:
            reader::kosync::SetAutoJump(!reader::kosync::AutoJump());
            RenderRows();
            break;
        case RowId::kMatchMode:
            reader::kosync::SetMatchMode(reader::kosync::MatchMode() == "filename" ? "content"
                                                                                   : "filename");
            RenderRows();
            break;
        case RowId::kAutoSync:
            reader::kosync::SetAutoSync(!reader::kosync::AutoSync());
            RenderRows();
            break;
    }
}

// ---------------------------------------------------------------------------
// 服务器选择浮层：官方（推荐，默认）/ 我的自建（不推荐）/ 自定义
// ---------------------------------------------------------------------------

lv_obj_t* s_chooser = nullptr;

struct ServerOption {
    const char* title;
    const char* note;
    const char* url;  // nullptr = 自定义（弹键盘）
};

constexpr ServerOption kServerOptions[] = {
    {"官方服务器（推荐）", "https://sync.koreader.rocks · KOReader 官方公共服",
     "https://sync.koreader.rocks"},
    {"我的服务器（不推荐）", "https://koreader.taor.site · 自建实例，稳定性取决于部署",
     "https://koreader.taor.site"},
    {"自定义服务器…", "手动输入地址（须 https 开头）", nullptr},
};

void CloseServerChooser() {
    if (s_chooser == nullptr) return;
    lv_obj_t* c = s_chooser;
    s_chooser = nullptr;
    lv_obj_delete_async(c);
}

void OnServerOptionClicked(lv_event_t* e) {
    const auto idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < sizeof(kServerOptions) / sizeof(kServerOptions[0]) && kServerOptions[idx].url != nullptr) {
        reader::kosync::SetServer(kServerOptions[idx].url);
        CloseServerChooser();
        RenderRows();
        return;
    }
    // 自定义：关掉浮层进键盘
    CloseServerChooser();
    OpenKeyboard(0);
}

void OnChooserBackClicked(lv_event_t* /*e*/) { CloseServerChooser(); }

void ShowServerChooser() {
    if (s_chooser != nullptr || s_ui.screen == nullptr) return;

    lv_obj_t* box = lv_obj_create(s_ui.screen);
    s_chooser = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(box);

    lv_obj_t* title = lv_label_create(box);
    lv_label_set_text(title, "选择服务器");
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kPad * 2, 60);

    const int w = LV_HOR_RES - kPad * 2;
    int y = 120;
    for (size_t i = 0; i < sizeof(kServerOptions) / sizeof(kServerOptions[0]); ++i) {
        const ServerOption& opt = kServerOptions[i];
        lv_obj_t* row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, 128);
        lv_obj_set_pos(row, kPad, y);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnServerOptionClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        DisableScroll(row);

        lv_obj_t* t = lv_label_create(row);
        lv_label_set_text(t, opt.title);
        lv_obj_set_style_text_font(t, UiFont(), 0);
        lv_obj_set_height(t, kRowLabelH);

        lv_obj_t* n = lv_label_create(row);
        lv_label_set_text(n, opt.note);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_width(n, w - 24);
        lv_obj_set_style_text_font(n, UiFont(), 0);
        lv_obj_set_style_text_opa(n, LV_OPA_60, 0);
        y += 128 + kRowGap;
    }

    lv_obj_t* back = lv_btn_create(box);
    lv_obj_set_size(back, w, 60);
    lv_obj_set_pos(back, kPad, y + 8);
    lv_obj_set_style_bg_color(back, lv_color_black(), 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, OnChooserBackClicked, LV_EVENT_CLICKED, nullptr);
}

void RenderRows() {
    if (s_ui.list == nullptr) return;
    lv_obj_clean(s_ui.list);
    const int w = LV_HOR_RES - kPad * 2;

    const RowId ids[] = {RowId::kEnabled, RowId::kServer, RowId::kUsername, RowId::kPassword,
                         RowId::kAutoJump, RowId::kMatchMode, RowId::kAutoSync};
    for (RowId id : ids) {
        lv_obj_t* row = lv_obj_create(s_ui.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, kRowH);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(id)));
        DisableScroll(row);

        lv_obj_t* t = lv_label_create(row);
        lv_label_set_text(t, RowTitle(id));
        lv_obj_set_style_text_font(t, UiFont(), 0);
        lv_obj_set_height(t, kRowLabelH);

        lv_obj_t* v = lv_label_create(row);
        char buf[128];
        RowValueText(id, buf, sizeof(buf));
        lv_label_set_text(v, buf);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_width(v, w - 24);
        lv_obj_set_style_text_font(v, UiFont(), 0);
        lv_obj_set_style_text_opa(v, LV_OPA_60, 0);
    }
}

// ---------------------------------------------------------------------------
// 登录 / 注册（Network 域执行，结果回屏）
// ---------------------------------------------------------------------------

struct AuthMsg {
    bool ok = false;
    bool is_register = false;
    std::string err;
};

void AuthApply(void* user_data) {
    std::unique_ptr<AuthMsg> msg(static_cast<AuthMsg*>(user_data));
    if (msg == nullptr || !s_alive || s_ui.auth_lbl == nullptr) return;
    if (msg->ok) {
        lv_label_set_text(s_ui.auth_lbl, msg->is_register ? "注册成功，已可同步"
                                                          : "登录验证通过，已可同步");
    } else {
        std::string text = (msg->is_register ? "注册失败: " : "登录失败: ") + msg->err;
        lv_label_set_text(s_ui.auth_lbl, text.c_str());
    }
}

void RunAuth(bool is_register) {
    if (s_ui.auth_lbl != nullptr) {
        lv_label_set_text(s_ui.auth_lbl, is_register ? "注册中…" : "验证登录中…");
    }
    Application::GetInstance().Schedule([is_register]() {
        auto* msg = new (std::nothrow) AuthMsg{};
        if (msg == nullptr) return;
        msg->is_register = is_register;
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
        do {
            if (!Board::GetInstance().EnsureNetworkReady(15000)) {
                msg->err = "网络未就绪";
                break;
            }
            msg->ok = is_register ? reader::kosync::RegisterNow(msg->err)
                                  : reader::kosync::AuthorizeNow(msg->err);
        } while (false);
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        if (!ScreenLvAsync(AuthApply, msg)) delete msg;
    }, Application::TaskDomain::Network);
}

void OnAuthClicked(lv_event_t* e) {
    const bool is_register = reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0;
    RunAuth(is_register);
}

bool OnVkKey(const char* key) {
    (void)key;
    if (s_kb_screen != nullptr) return false;  // 键盘页自行处理点击
    return false;                              // HOME 默认返回
}

void OnScreenLoaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_ui.screen) return;
    s_alive = true;
    PowerPolicy::GetInstance().Acquire(PowerNeed::ForegroundNetwork);
    // 预热 NVS 缓存（Loaded 在 LVGL tick 任务，栈深充裕）：后续点击路径全部
    // 命中缓存，触摸线程（4→8KB 栈）不会再触 Flash 操作
    (void)reader::kosync::Server();
    (void)reader::kosync::Username();
    (void)reader::kosync::IsEnabled();
    (void)reader::kosync::AutoJump();
    (void)reader::kosync::AutoSync();
    (void)reader::kosync::MatchMode();
    (void)LansyncScreen::GetSlot(1);
    (void)LansyncScreen::GetSlot(2);
    (void)LansyncScreen::GetSlot(3);
    RenderRows();
}

void OnScreenUnloaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_ui.screen) return;
    s_alive = false;
    PowerPolicy::GetInstance().Release(PowerNeed::ForegroundNetwork);
}

}  // namespace

lv_obj_t* KosyncScreen::Create() {
    s_ui = Ui{};
    s_alive = true;
    // 键盘页不归本屏管：若上次泄漏（极端情况），清掉状态
    s_kb_screen = nullptr;
    s_kb_input_lbl = nullptr;
    s_kb_char_keys.clear();
    s_kb_field = -1;

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

    // 标题行
    lv_obj_t* header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_HOR_RES - kPad * 2, kHeaderH);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, status.height + kPad);
    DisableScroll(header);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "KOReader 同步");
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    // 设置行容器（纵向 flex，均分）
    const int list_y = status.height + kPad + kHeaderH + 8;
    s_ui.list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, LV_HOR_RES - kPad * 2,
                    7 * (kRowH + kRowGap) + kRowGap);
    lv_obj_align(s_ui.list, LV_ALIGN_TOP_MID, 0, list_y);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, kRowGap, 0);
    lv_obj_set_flex_align(s_ui.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    DisableScroll(s_ui.list);

    // 操作行：验证登录 / 注册账号
    const int btn_y = list_y + 7 * (kRowH + kRowGap) + kRowGap + 16;
    const int btn_w = (LV_HOR_RES - kPad * 2 - 16) / 2;
    lv_obj_t* auth = lv_btn_create(scr);
    lv_obj_set_size(auth, btn_w, 56);
    lv_obj_set_pos(auth, kPad, btn_y);
    lv_obj_set_style_bg_color(auth, lv_color_black(), 0);
    lv_obj_t* al = lv_label_create(auth);
    lv_label_set_text(al, "验证登录");
    lv_obj_set_style_text_color(al, lv_color_white(), 0);
    lv_obj_center(al);
    lv_obj_add_event_cb(auth, OnAuthClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(0)));

    lv_obj_t* reg = lv_btn_create(scr);
    lv_obj_set_size(reg, btn_w, 56);
    lv_obj_set_pos(reg, kPad + btn_w + 16, btn_y);
    lv_obj_set_style_bg_color(reg, lv_color_white(), 0);
    lv_obj_set_style_border_color(reg, lv_color_black(), 0);
    lv_obj_set_style_border_width(reg, 2, 0);
    lv_obj_t* rl = lv_label_create(reg);
    lv_label_set_text(rl, "注册账号");
    lv_obj_set_style_text_color(rl, lv_color_black(), 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(reg, OnAuthClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(1)));

    // 结果/提示行
    s_ui.auth_lbl = lv_label_create(scr);
    lv_label_set_text(s_ui.auth_lbl,
                      "与 KOReader 填同一服务器和账号即可互通进度。Reeden 导出等被处理过的"
                      "书请把「书籍匹配方式」改为文件名。自动同步按百分比定位，两端排版不同时"
                      "偶发进度偏差，介意请关自动、用阅读时长页的手动上传/拉取。");
    lv_obj_set_width(s_ui.auth_lbl, LV_HOR_RES - kPad * 2);
    lv_label_set_long_mode(s_ui.auth_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.auth_lbl, UiFont(), 0);
    lv_obj_set_style_text_opa(s_ui.auth_lbl, LV_OPA_60, 0);
    lv_obj_align(s_ui.auth_lbl, LV_ALIGN_TOP_LEFT, kPad, btn_y + 56 + 16);

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{KosyncScreen::Create, OnVkKey,
                                                       nullptr, nullptr, nullptr, nullptr,
                                                       nullptr, nullptr});
    return scr;
}

bool KosyncScreen::IsActive() {
    return s_alive && s_ui.screen != nullptr;
}
