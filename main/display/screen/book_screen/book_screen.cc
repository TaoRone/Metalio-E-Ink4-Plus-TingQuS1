#pragma GCC optimize("O1")

#include "book_screen/book_screen.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/shelf/book_home.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/book_vk.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_orient.h"
#include "book_screen/settings/book_settings_sheet.h"
#include "book_screen/settings/book_font_multi.h"

#include <lvgl.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <new>
#include <string>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "reader/book_home_snapshot.h"
#include "reader/kosync.h"
#include "screen_common.h"
#include "vk_key_handler.h"

ScreenPaintCoalesce s_reader_paint{};
ScreenPaintCoalesce s_library_paint{};
ScreenPaintCoalesce s_shelf_check_paint{};
ScreenPaintCoalesce s_toc_paint{};
ScreenPaintCoalesce s_settings_sheet_paint{}; // 设置卡：连点只记状态，抬起后一次刷控件
// 盖板键/长按定时器只累加；真正 Next/Prev 在 LVGL 绘制里做，避免跨章重建 pages_ 时 UAF

BookUiState& Book_State() {
    static BookUiState s;
    return s;
}

namespace {

// KOReader 同步：云端进度跳转消息（kosync 服务层在 Network 线程回调，经 ScreenLvAsync 转来）
struct KosyncJumpMsg {
    std::string path;
    int pct_x10 = 0;
    std::string source_device;
};

void KosyncJumpApply(void* user_data) {
    std::unique_ptr<KosyncJumpMsg> msg(static_cast<KosyncJumpMsg*>(user_data));
    if (msg == nullptr) return;
    auto& st = Book_State();
    if (!st.reader.session || !st.reader.session->IsOpen()) return;
    if (st.reader.session->Info().path != msg->path) return;  // 已换书：丢弃过期跳转
    char tip[96];
    std::snprintf(tip, sizeof(tip), "云端进度 %.1f%%（来自 %s）", msg->pct_x10 / 10.0,
                  msg->source_device.empty() ? "其他设备" : msg->source_device.c_str());
    ScreenShowToast(tip);
    if (reader::kosync::AutoJump()) {
        st.reader.session->GoToReadingProgress(msg->pct_x10);
    }
}

void EnsureKosyncHandlerRegistered() {
    static bool done = false;
    if (done) return;
    done = true;
    reader::kosync::SetJumpHandler([](const std::string& book_path, int pct_x10,
                                      const std::string& source_device) {
        auto* msg = new (std::nothrow) KosyncJumpMsg{book_path, pct_x10, source_device};
        if (msg == nullptr) return;
        if (!ScreenLvAsync(KosyncJumpApply, msg)) delete msg;
    });
    // 手动同步失败回执：Network 线程 → LVGL toast
    reader::kosync::SetStatusHandler([](const char* message) {
        struct StatusMsg {
            std::string text;
        };
        auto* msg = new (std::nothrow) StatusMsg{message != nullptr ? message : ""};
        if (msg == nullptr) return;
        if (!ScreenLvAsync(
                [](void* data) {
                    std::unique_ptr<StatusMsg> m(static_cast<StatusMsg*>(data));
                    if (m != nullptr && !m->text.empty()) ScreenShowToast(m->text.c_str());
                },
                msg)) {
            delete msg;
        }
    });
}

}  // namespace

static lv_obj_t* CreateBookHome(bool continue_latest) {
    auto& st = Book_State();
    ReleaseReaderSessionForLeave();
    EnsureKosyncHandlerRegistered();
    st.shelf.shelf_multi = false;
    st.shelf.shelf_selected.clear();
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.multi_bar = nullptr;
    st.shelf.back_root = BookUiState::NavRoot::kHome;
    CancelListCoverFill();
    st.shelf.recent_covers.clear();
    st.shelf.shelf_covers.clear();

    ScreenSetIsHome(false);

    if (continue_latest && !ReaderWorkersBusy() && !st.reader.deferred_cleanup && SdCardManager::GetInstance().IsMounted()) {
        const std::string latest = reader::book_home_snapshot::LoadLatestAbsPath();
        if (!latest.empty()) {
            reader::LoadBookLibrary(reader::kDefaultBooksDir, st.shelf.books);
            for (size_t i = 0; i < st.shelf.books.size(); ++i) {
                if (st.shelf.books[i].path == latest) {
                    st.shelf.selected = static_cast<int>(i);
                    return CreateReaderScreen(st.shelf.books[i]);
                }
            }
        }
    }

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    st.shelf.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = Book_State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.shelf.list_scr == tgt) {
                CancelListCoverFill();
                s.shelf.list_scr = nullptr;
                s.shelf.list_body = nullptr;
                s.shelf.list_footer = nullptr;
                s.shelf.multi_bar = nullptr;
                s.shelf.shelf_multi = false;
                s.shelf.shelf_selected.clear();
                s.shelf.recent_covers.clear();
                s.shelf.shelf_covers.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_label = status.status_label;
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(body, kListPad, 0);
    lv_obj_set_style_pad_right(body, kListPad, 0);
    lv_obj_set_style_pad_top(body, kListPad, 0);
    lv_obj_set_style_pad_bottom(body, kListPad + kHomeBottomPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kHomeGap, 0);
    Book_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, Book_ContentWidth());
    lv_obj_set_flex_grow(list_body, 1);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, 0, 0);
    Book_DisableScroll(list_body);
    st.shelf.list_body = list_body;
    st.shelf.list_footer = nullptr;
    st.shelf.multi_bar = nullptr;

    if (!SdCardManager::GetInstance().IsMounted()) {
        Book_ShowMessage(list_body, Lang::Strings::BOOK_NO_SD, Book_ListFont());
        VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
        return scr;
    }

    st.shelf.books.clear();
    // 优先复用开机/上次扫库缓存，未命中再扫盘
    reader::LoadBookLibrary(reader::kDefaultBooksDir, st.shelf.books);
    RenderReadingHome();

    // factory=Create：进百问后返回阅读首页
    VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
    return scr;
}

lv_obj_t* BookScreen::Create() {
    return CreateBookHome(false);
}

lv_obj_t* BookScreen::CreateLatest() {
    return CreateBookHome(true);
}
