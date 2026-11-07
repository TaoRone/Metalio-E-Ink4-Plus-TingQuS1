#include "weread_screen/weread_qr_screen.h"

#include <weread/weread_ink.h>

#include "screen_common.h"
#include "fontpack_lvgl.h"

#include <esp_log.h>

#include <cstring>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// 微信读书扫码登录屏（M2 在线阅读登录入口）
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "WereadQr";

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_qr = nullptr;
lv_obj_t* s_return_to = nullptr;
void (*s_on_done)(void*) = nullptr;
void* s_on_done_user = nullptr;
std::string s_qr_url;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
const lv_font_t* UiTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : UiFont();
}

void SetStatus(const char* text) {
    if (s_status != nullptr) lv_label_set_text(s_status, text ? text : "");
}

// 事件投递：ink 服务层在 Network 线程回调 → ScreenLvAsync 转 LVGL
struct InkMsg {
    weread::ink::LoginEvent ev;
};

void InkApply(void* user_data) {
    std::unique_ptr<InkMsg> msg(static_cast<InkMsg*>(user_data));
    if (msg == nullptr || s_screen == nullptr) return;
    switch (msg->ev.type) {
        case weread::ink::LoginEvent::Type::kQrUrl:
            s_qr_url = msg->ev.url;
            if (s_qr != nullptr) {
                lv_qrcode_update(s_qr, s_qr_url.data(), s_qr_url.size());
                lv_obj_clear_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
            }
            SetStatus("请用微信扫码授权（读书墨水屏登录）");
            break;
        case weread::ink::LoginEvent::Type::kScanned:
            SetStatus("已扫码，请在手机上确认登录…");
            break;
        case weread::ink::LoginEvent::Type::kSuccess:
            SetStatus("登录成功");
            ScreenShowToast("微信读书登录成功");
            if (s_on_done != nullptr) s_on_done(s_on_done_user);
            break;
        case weread::ink::LoginEvent::Type::kError:
            SetStatus(msg->ev.message.empty() ? "登录失败" : msg->ev.message.c_str());
            break;
    }
}

void Close() {
    lv_obj_t* self = s_screen;
    lv_obj_t* back = s_return_to;
    s_screen = nullptr;
    s_status = nullptr;
    s_qr = nullptr;
    s_return_to = nullptr;
    auto* done = s_on_done;
    s_on_done = nullptr;
    (void)done;
    if (back != nullptr) lv_screen_load(back);
    if (self != nullptr) lv_obj_delete_async(self);
}

void OnCancel(lv_event_t* /*e*/) {
    weread::ink::CancelQrLogin();
    Close();
}

}  // namespace

void WereadQrScreen::Show(const char* /*back_screen_id*/, void (*on_done)(void*),
                          void* on_done_user) {
    if (s_screen != nullptr) return;
    s_return_to = lv_screen_active();
    s_on_done = on_done;
    s_on_done_user = on_done_user;
    s_qr_url.clear();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "微信读书登录");
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);

    // 二维码（内容未就绪前隐藏；lv_qrcode 需要初始内容建对象）
    lv_obj_t* qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, 300);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, " ", 1);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
    s_qr = qr;

    s_status = lv_label_create(scr);
    SetStatus("正在获取二维码…");
    lv_obj_set_style_text_font(s_status, UiFont(), 0);
    lv_obj_set_width(s_status, LV_HOR_RES - 48);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 470);

    lv_obj_t* cancel = lv_btn_create(scr);
    lv_obj_set_size(cancel, 200, 56);
    lv_obj_align(cancel, LV_ALIGN_TOP_MID, 0, 560);
    lv_obj_set_style_bg_color(cancel, lv_color_black(), 0);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, UiFont(), 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, OnCancel, LV_EVENT_CLICKED, nullptr);

    lv_screen_load(scr);

    // 事件回调 + 启动登录流程（幂等）
    weread::ink::SetLoginEventHandler([](const weread::ink::LoginEvent& ev) {
        auto* msg = new (std::nothrow) InkMsg{ev};
        if (msg == nullptr) return;
        if (!ScreenLvAsync(InkApply, msg)) delete msg;
    });
    if (!weread::ink::StartQrLogin()) {
        SetStatus("登录流程已在进行中");
    }
}

bool WereadQrScreen::IsActive() {
    return s_screen != nullptr;
}
