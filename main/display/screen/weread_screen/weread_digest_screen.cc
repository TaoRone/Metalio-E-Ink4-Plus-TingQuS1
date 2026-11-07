#include "weread_screen/weread_digest_screen.h"

#include <application.h>
#include <board.h>
#include <power_policy.h>
#include <weread/weread_ink.h>

#include "screen_common.h"
#include "vk_key_handler.h"
#include "fontpack_lvgl.h"

#include <esp_log.h>

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AI 大纲阅读页
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "WereadDigest";

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_body = nullptr;
lv_obj_t* s_title = nullptr;
std::string s_book_id;
std::string s_book_title;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
const lv_font_t* UiTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : UiFont();
}

struct DigestMsg {
    std::string summary;
    std::vector<weread::ink::BriefQA> qa;
    std::string err;
};

void Close() {
    lv_obj_t* self = s_screen;
    s_screen = nullptr;
    s_body = nullptr;
    s_title = nullptr;
    if (self != nullptr) lv_obj_delete_async(self);
}

void OnCancel(lv_event_t* /*e*/) { Close(); }

void DigestApply(void* user_data) {
    std::unique_ptr<DigestMsg> msg(static_cast<DigestMsg*>(user_data));
    if (msg == nullptr || s_screen == nullptr || s_body == nullptr) return;
    lv_obj_clean(s_body);
    auto add_para = [&](const std::string& text, bool bold_title) {
        if (text.empty()) return;
        lv_obj_t* l = lv_label_create(s_body);
        lv_label_set_text(l, text.c_str());
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_HOR_RES - 48);
        lv_obj_set_style_text_font(l, bold_title ? UiTitleFont() : UiFont(), 0);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    };
    if (!msg->err.empty()) {
        add_para("获取失败: " + msg->err, false);
        return;
    }
    if (!msg->summary.empty()) {
        add_para("全书概要", true);
        add_para(msg->summary, false);
    }
    for (const auto& item : msg->qa) {
        add_para("", false);
        add_para("◆ " + item.question, true);
        add_para(item.answer_md, false);
    }
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
}

}  // namespace

void WereadDigestScreen::Show(const char* book_id, const char* book_title) {
    if (s_screen != nullptr || book_id == nullptr) return;
    s_book_id = book_id;
    s_book_title = book_title != nullptr ? book_title : "";

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    lv_obj_t* title = lv_label_create(scr);
    s_title = title;
    std::string head = s_book_title + " · AI大纲";
    lv_label_set_text(title, head.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_HOR_RES - 220);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 60);

    lv_obj_t* close = lv_btn_create(scr);
    lv_obj_set_size(close, 130, 48);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -24, 56);
    lv_obj_set_style_bg_color(close, lv_color_black(), 0);
    lv_obj_t* cl = lv_label_create(close);
    lv_label_set_text(cl, "返回");
    lv_obj_set_style_text_font(cl, UiFont(), 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, OnCancel, LV_EVENT_CLICKED, nullptr);

    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, LV_HOR_RES, LV_VER_RES - 140);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 14, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* loading = lv_label_create(s_body);
    lv_label_set_text(loading, "正在拉取 AI 大纲…");
    lv_obj_set_style_text_font(loading, UiFont(), 0);

    lv_screen_load(scr);

    // Network 域拉取
    const std::string bid = s_book_id;
    Application::GetInstance().Schedule([bid]() {
        auto* msg = new (std::nothrow) DigestMsg{};
        if (msg == nullptr) return;
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
        do {
            if (!Board::GetInstance().EnsureNetworkReady(30000)) {
                msg->err = "网络未就绪";
                break;
            }
            msg->err.clear();
            if (!weread::ink::FetchBriefOutline(bid, msg->summary, msg->qa, msg->err)) break;
        } while (false);
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        if (!ScreenLvAsync(DigestApply, msg)) delete msg;
    }, Application::TaskDomain::Network);
}
