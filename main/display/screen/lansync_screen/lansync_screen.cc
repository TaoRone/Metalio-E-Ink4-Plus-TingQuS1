#include "lansync_screen/lansync_screen.h"

#include <assets/lang_config.h>
#include <power_policy.h>
#include <settings.h>
#include <weread/weread_service.h>

#include "screen_common.h"
#include "vk_key_handler.h"
#include "fontpack_lvgl.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <cJSON.h>
#include <mutex>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// 局域网剪贴板：设备起 :8080 服务，电脑浏览器填写；NVS "clipboard"/slot1..3
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "Lansync";
constexpr const char* kScreenId = "lansync";
constexpr int kPad = 12;
constexpr int kRowGap = 10;

// 极简剪贴板页面：3 槽 + 保存/设为 Key
const char kPageHtml[] = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>听趣S1 剪贴板</title><style>body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px;color:#111}h1{font-size:20px}textarea{width:100%;height:72px;font-size:16px;padding:8px;box-sizing:border-box}button{font-size:15px;padding:8px 14px;margin:6px 6px 0 0;cursor:pointer}.row{margin:14px 0}.key{background:#111;color:#fff}.status{font-size:13px;color:#666}</style></head><body><h1>听趣S1 · 局域网剪贴板</h1><div id="st" class="status">加载中…</div><div class="row"><textarea id="t1"></textarea><br><button onclick="save(1)">保存到粘贴位1</button><button class="key" onclick="setkey(1)">设为微信读书Key</button></div><div class="row"><textarea id="t2"></textarea><br><button onclick="save(2)">保存到粘贴位2</button><button class="key" onclick="setkey(2)">设为微信读书Key</button></div><div class="row"><textarea id="t3"></textarea><br><button onclick="save(3)">保存到粘贴位3</button><button class="key" onclick="setkey(3)">设为微信读书Key</button></div><script>async function refresh(){const r=await fetch("/api/status");const j=await r.json();document.getElementById("st").textContent="Key "+(j.has_key?"已配置":"未配置");t1.value=j.s1||"";t2.value=j.s2||"";t3.value=j.s3||""}async function save(n){const t=document.getElementById("t"+n).value;await fetch("/api/save",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({slot:n,text:t})});refresh()}async function setkey(n){const t=document.getElementById("t"+n).value;const r=await fetch("/api/setkey",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({slot:n,text:t})});const j=await r.json();alert(j.ok?"已设置为微信读书 Key":"设置失败：内容不是有效 Key（需 wrk- 开头）")}refresh();</script></body></html>)HTML";

lv_obj_t* s_screen = nullptr;
httpd_handle_t s_httpd = nullptr;
bool s_alive = false;
lv_timer_t* s_refresh_timer = nullptr;
lv_obj_t* s_status_lbl = nullptr;
lv_obj_t* s_url_lbl = nullptr;
lv_obj_t* s_key_lbl = nullptr;
lv_obj_t* s_slot_lbls[3] = {nullptr, nullptr, nullptr};

std::string SlotKey(int n) { return "slot" + std::to_string(n); }

// 槽位 RAM 缓存：触摸/键盘事件链（touch_feed 仅 4KB 栈）禁止直触 NVS→Flash——
// SDK 在关 Flash 缓存前会校验当前任务栈余量，深调用链下会断言重启。
// 网页端 SaveSlot 时同步更新本缓存，保持一致。
static std::mutex s_slot_mu;
static std::string s_slot_cache[3];
static bool s_slot_valid[3] = {false, false, false};

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
const lv_font_t* UiTitleFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : UiFont();
}

std::string LoadSlot(int n) {
    if (n < 1 || n > 3) return "";
    {
        std::lock_guard<std::mutex> lock(s_slot_mu);
        if (s_slot_valid[n - 1]) return s_slot_cache[n - 1];
    }
    Settings s("clipboard");
    std::string v = s.GetString(SlotKey(n));
    std::lock_guard<std::mutex> lock(s_slot_mu);
    s_slot_cache[n - 1] = v;
    s_slot_valid[n - 1] = true;
    return v;
}

void SaveSlot(int n, const std::string& text) {
    if (n < 1 || n > 3) return;
    {
        std::lock_guard<std::mutex> lock(s_slot_mu);
        s_slot_cache[n - 1] = text;
        s_slot_valid[n - 1] = true;
    }
    Settings s("clipboard", true);
    s.SetString(SlotKey(n), text);
}

// ---------------------------------------------------------------------------
// HTTP 服务（8080）
// ---------------------------------------------------------------------------

esp_err_t SendJson(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandleStatus(httpd_req_t* req) {
    char buf[1024];
    std::string s1 = LoadSlot(1), s2 = LoadSlot(2), s3 = LoadSlot(3);
    // 简单 JSON 转义（仅替换引号与反斜杠，剪贴板文本足够）
    auto esc = [](const std::string& in) {
        std::string out;
        for (char c : in) {
            if (c == '"' || c == '\\') out.push_back('\\');
            if (c == '\n' || c == '\r') { out.push_back(' '); continue; }
            out.push_back(c);
        }
        return out;
    };
    std::snprintf(buf, sizeof(buf),
                  "{\"has_key\":%s,\"s1\":\"%s\",\"s2\":\"%s\",\"s3\":\"%s\"}",
                  weread::HasApiKey() ? "true" : "false", esc(s1).c_str(),
                  esc(s2).c_str(), esc(s3).c_str());
    return SendJson(req, buf);
}

bool ReadBodyJson(httpd_req_t* req, std::string* out) {
    char buf[256];
    std::string body;
    int remaining = req->content_len;
    while (remaining > 0) {
        const int n = httpd_req_recv(req, buf, std::min<int>(remaining, sizeof(buf)));
        if (n <= 0) return false;
        body.append(buf, n);
        remaining -= n;
        if (body.size() > 4096) return false;  // 剪贴板上限 4KB
    }
    *out = std::move(body);
    return true;
}

// 从 JSON 中提取 "slot":N 与 "text":"..."（cJSON）
esp_err_t HandleSave(httpd_req_t* req) {
    std::string body;
    if (!ReadBodyJson(req, &body)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad"); return ESP_FAIL; }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON* slot = cJSON_GetObjectItem(root, "slot");
    const cJSON* text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsNumber(slot) || slot->valueint < 1 || slot->valueint > 3 || !cJSON_IsString(text)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "field");
        return ESP_FAIL;
    }
    SaveSlot(slot->valueint, text->valuestring);
    cJSON_Delete(root);
    ESP_LOGI(kTag, "slot %d saved (%d bytes)", slot->valueint, text->valuestring ? (int)strlen(text->valuestring) : 0);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t HandleSetKey(httpd_req_t* req) {
    std::string body;
    if (!ReadBodyJson(req, &body)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad"); return ESP_FAIL; }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON* slot = cJSON_GetObjectItem(root, "slot");
    const cJSON* text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsNumber(slot) || slot->valueint < 1 || slot->valueint > 3 || !cJSON_IsString(text)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "field");
        return ESP_FAIL;
    }
    const int n = slot->valueint;
    // 直接用输入框内容：顺手保存到槽位，再设为 Key
    SaveSlot(n, text->valuestring);
    const bool ok = weread::SetApiKey(text->valuestring);
    cJSON_Delete(root);
    ESP_LOGI(kTag, "set weread key from web slot %d -> %s", n, ok ? "ok" : "invalid");
    return SendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

bool StartServer() {
    if (s_httpd != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        s_httpd = nullptr;
        return false;
    }
    httpd_uri_t page = {};
    page.uri = "/";
    page.method = HTTP_GET;
    page.handler = HandlePage;
    httpd_register_uri_handler(s_httpd, &page);

    httpd_uri_t status = {};
    status.uri = "/api/status";
    status.method = HTTP_GET;
    status.handler = HandleStatus;
    httpd_register_uri_handler(s_httpd, &status);

    httpd_uri_t save = {};
    save.uri = "/api/save";
    save.method = HTTP_POST;
    save.handler = HandleSave;
    httpd_register_uri_handler(s_httpd, &save);

    httpd_uri_t setkey = {};
    setkey.uri = "/api/setkey";
    setkey.method = HTTP_POST;
    setkey.handler = HandleSetKey;
    httpd_register_uri_handler(s_httpd, &setkey);
    return true;
}

void StopServer() {
    if (s_httpd != nullptr) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void RefreshSlotLabels() {
    if (s_key_lbl != nullptr) {
        lv_label_set_text(s_key_lbl, weread::HasApiKey() ? Lang::Strings::LANSYNC_KEY_OK
                                                          : Lang::Strings::LANSYNC_KEY_NONE);
    }
    for (int i = 0; i < 3; ++i) {
        if (s_slot_lbls[i] == nullptr) continue;
        std::string v = LoadSlot(i + 1);
        std::string preview = v.empty() ? Lang::Strings::LANSYNC_SLOT_EMPTY : v.substr(0, 40);
        std::string line = "PASTE" + std::to_string(i + 1) + "  " + preview;
        lv_label_set_text(s_slot_lbls[i], line.c_str());
    }
}

void OnTimerRefresh(lv_timer_t*) { RefreshSlotLabels(); }

void OnScreenLoaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_screen) return;
    s_alive = true;
    // 本页与 PC 长时间交互：软保网，防止宽限到期断网导致网页连不上
    PowerPolicy::GetInstance().Acquire(PowerNeed::ForegroundNetwork);

    // WiFi STA IP
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_ip4_addr_t ip = {};
    bool has_ip = false;
    if (sta != nullptr) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
            ip = info.ip;
            has_ip = true;
        }
    }

    if (!has_ip) {
        if (s_status_lbl != nullptr) lv_label_set_text(s_status_lbl, Lang::Strings::LANSYNC_NO_WIFI);
        if (s_url_lbl != nullptr) lv_label_set_text(s_url_lbl, "");
        return;
    }

    if (StartServer()) {
        char url[64];
        std::snprintf(url, sizeof(url), "http://%d.%d.%d.%d:8080",
                      esp_ip4_addr1_16(&ip), esp_ip4_addr2_16(&ip),
                      esp_ip4_addr3_16(&ip), esp_ip4_addr4_16(&ip));
        if (s_status_lbl != nullptr) lv_label_set_text(s_status_lbl, Lang::Strings::LANSYNC_STARTED);
        if (s_url_lbl != nullptr) lv_label_set_text(s_url_lbl, url);
        ESP_LOGI(kTag, "server on %s", url);
    } else {
        if (s_status_lbl != nullptr) lv_label_set_text(s_status_lbl, Lang::Strings::LANSYNC_SERVER_FAIL);
    }
    RefreshSlotLabels();
    // 周期刷新槽位显示（电脑端保存后设备端同步显示）；单例，重进不叠加
    if (s_refresh_timer == nullptr) {
        s_refresh_timer = lv_timer_create(OnTimerRefresh, 1500, nullptr);
    }
}

void OnScreenUnloaded(lv_event_t* e) {
    if (lv_event_get_target(e) != s_screen) return;
    s_alive = false;
    PowerPolicy::GetInstance().Release(PowerNeed::ForegroundNetwork);
    if (s_refresh_timer != nullptr) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    StopServer();
}

bool OnVkKey(const char* key) {
    (void)key;
    return false;  // HOME 默认返回
}

}  // namespace

std::string LansyncScreen::GetSlot(int n) {
    if (n < 1 || n > 3) return "";
    return LoadSlot(n);
}

lv_obj_t* LansyncScreen::Create() {
    s_screen = nullptr;
    s_alive = false;
    s_status_lbl = nullptr;
    s_url_lbl = nullptr;
    for (auto& p : s_slot_lbls) p = nullptr;

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, UiFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, OnScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES - kPad * 2, LV_VER_RES - status.height - kPad * 2);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height + kPad);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kRowGap, 0);

    lv_obj_t* title = lv_label_create(body);
    lv_label_set_text(title, Lang::Strings::LANSYNC_TITLE);
    lv_obj_set_style_text_font(title, UiTitleFont(), 0);

    s_status_lbl = lv_label_create(body);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_opa(s_status_lbl, LV_OPA_60, 0);

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, Lang::Strings::LANSYNC_URL_HINT);
    lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);

    s_url_lbl = lv_label_create(body);
    lv_label_set_text(s_url_lbl, "");
    lv_obj_set_style_text_font(s_url_lbl, UiTitleFont(), 0);

    s_key_lbl = lv_label_create(body);
    lv_label_set_text(s_key_lbl, "");
    lv_obj_set_style_text_opa(s_key_lbl, LV_OPA_60, 0);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t* line = lv_obj_create(body);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, LV_HOR_RES - kPad * 2);
        lv_obj_set_height(line, 56);
        lv_obj_set_style_border_color(line, lv_color_black(), 0);
        lv_obj_set_style_border_width(line, 1, 0);
        lv_obj_set_style_pad_all(line, 8, 0);
        s_slot_lbls[i] = lv_label_create(line);
        lv_label_set_text(s_slot_lbls[i], "");
        lv_obj_align(s_slot_lbls[i], LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_long_mode(s_slot_lbls[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_slot_lbls[i], LV_HOR_RES - kPad * 2 - 24);
    }

    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{LansyncScreen::Create, OnVkKey, nullptr, nullptr, nullptr,
                                       nullptr, nullptr, nullptr});
    return scr;
}

bool LansyncScreen::IsActive() {
    return s_alive && s_screen != nullptr;
}
