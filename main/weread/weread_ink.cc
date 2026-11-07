#include "weread_ink.h"

#include <settings.h>
#include <application.h>
#include <board.h>
#include <power_policy.h>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <rom/sha.h>

#include <cJSON.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace weread {
namespace ink {

static const char* kTag = "WereadInk";
static const char* kNvsNs = "weread";

// BOOX 墨水屏设备身份（weread-omni einkDevice 固定值）
static const char* kUserAgent =
    "WeRead/2.1.2 WRBrand/Onyx wr_eink Dalvik/2.1.0 (Linux; U; Android 11; BOOX Build/onyx)";
static const char* kAppVer = "2.1.2.10245900";
static const char* kDeviceName = "BOOX";
static const char* kWxAppId = "wxab9b71ad2b90ff34";
static const char* kScope = "snsapi_userinfo,snsapi_timeline,snsapi_friend";

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static std::string Sha256Hex(const std::string& input) {
    SHA_CTX ctx;
    ets_sha_enable();
    ets_sha_init(&ctx, SHA2_256);
    ets_sha_update(&ctx, reinterpret_cast<const unsigned char*>(input.data()),
                   static_cast<uint32_t>(input.size()), true);
    unsigned char digest[32];
    ets_sha_finish(&ctx, digest);
    ets_sha_disable();
    char hex[65];
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 64);
}

static std::string RandomDigits(int n) {
    std::string s;
    s.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) s.push_back(static_cast<char>('0' + esp_random() % 10));
    return s;
}

static std::string NewDeviceId() {
    // weread-omni leaf3 习惯前缀 + 19 位十进制（无符号 63bit）
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | (esp_random() & 0xFF);
    v &= 0x7FFFFFFFFFFFFFFFULL;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "eink334691225%019llu", static_cast<unsigned long long>(v));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// 凭证（NVS weread / ink_*）
// ---------------------------------------------------------------------------

namespace {

std::string Load(const char* key) {
    Settings settings(kNvsNs);
    return settings.GetString(key);
}

void Save(const char* key, const std::string& value) {
    Settings settings(kNvsNs, true);
    settings.SetString(key, value);
}

}  // namespace

bool HasCredentials() {
    return !Load("ink_access").empty() && !Load("ink_vid").empty() &&
           !Load("ink_refresh").empty() && !Load("ink_device").empty();
}

void ClearCredentials() {
    Save("ink_access", "");
    Save("ink_refresh", "");
}

std::string Vid() { return Load("ink_vid"); }

static std::string DeviceId() {
    std::string id = Load("ink_device");
    if (id.empty()) {
        id = NewDeviceId();
        Save("ink_device", id);
    }
    return id;
}

// ---------------------------------------------------------------------------
// HTTP（SDK esp_http_client 可靠模式；i.weread.qq.com / open.weixin.qq.com 两个主机）
// ---------------------------------------------------------------------------

namespace {

struct HttpResult {
    long status = 0;
    std::string body;
};

// 通用请求。extra_headers 为 "\n" 分隔的 "Key: Value" 列表（可为空）。
bool RawRequest(const char* method, const std::string& url, const std::string& extra_headers,
                const std::string& json_body, HttpResult& out, std::string& err,
                const char* user_agent) {
    out = HttpResult{};
    err.clear();

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = std::strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = 20000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        err = "http init failed";
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", user_agent);
    if (extra_headers.empty()) {
        esp_http_client_set_header(client, "Accept", "application/json");
    } else {
        size_t pos = 0;
        while (pos < extra_headers.size()) {
            const size_t end = extra_headers.find('\n', pos);
            const std::string line = extra_headers.substr(
                pos, end == std::string::npos ? std::string::npos : end - pos);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                esp_http_client_set_header(client, line.substr(0, colon).c_str(),
                                           line.substr(colon + 1).c_str());
            }
            pos = end == std::string::npos ? extra_headers.size() : end + 1;
        }
    }

    bool ok = false;
    if (esp_http_client_open(client, static_cast<int>(json_body.size())) != ESP_OK) {
        err = "open failed";
    } else {
        if (!json_body.empty() &&
            esp_http_client_write(client, json_body.data(),
                                  static_cast<int>(json_body.size())) < 0) {
            err = "write failed";
        } else {
            esp_http_client_fetch_headers(client);
            out.status = esp_http_client_get_status_code(client);
            char buf[1024];
            const int64_t deadline_us = esp_timer_get_time() + 25000000LL;
            while (esp_timer_get_time() <= deadline_us) {
                const int r = esp_http_client_read(client, buf, sizeof(buf));
                if (r < 0) {
                    err = "read failed";
                    break;
                }
                if (r == 0) break;
                out.body.append(buf, static_cast<size_t>(r));
                if (out.body.size() > 1024 * 1024) {
                    err = "response too large";
                    break;
                }
            }
            if (err.empty()) ok = true;
        }
    }
    esp_http_client_cleanup(client);
    return ok;
}

std::string VersionHeaders() {
    // "Key: Value\n..."（RawRequest 按行解析）
    std::string h;
    h += "baseapi: 30\n";
    h += std::string("appver: ") + kAppVer + "\n";
    h += std::string("basever: ") + kAppVer + "\n";
    h += "osver: 11\n";
    h += "channelId: 900\n";
    return h;
}

std::string AuthHeaders() {
    std::string h = VersionHeaders();
    h += std::string("vid: ") + Load("ink_vid") + "\n";
    h += std::string("accessToken: ") + Load("ink_access");
    return h;
}

// ---------------------------------------------------------------------------
// 令牌刷新（POST /login mint；失败清空 access 迫使重新登录由调用方决定）
// ---------------------------------------------------------------------------

bool RefreshTokensLocked(std::string& err) {
    const std::string device_id = DeviceId();
    const int64_t timestamp = static_cast<int64_t>(esp_timer_get_time() / 1000ULL);
    const int random = static_cast<int>(esp_random() % 1000 + 1);
    const std::string signature =
        Sha256Hex(std::to_string(timestamp) + device_id + std::to_string(random));

    char body[768];
    std::snprintf(body, sizeof(body),
                  "{\"deviceId\":\"%s\",\"deviceName\":\"%s\",\"inBackground\":0,"
                  "\"kickType\":1,\"random\":%d,\"refCgi\":\"\",\"refreshToken\":\"%s\","
                  "\"signature\":\"%s\",\"timestamp\":%lld,\"trackId\":\"\","
                  "\"deviceType\":3}",
                  device_id.c_str(), kDeviceName, random, Load("ink_refresh").c_str(),
                  signature.c_str(), static_cast<long long>(timestamp));

    HttpResult res;
    std::string headers = VersionHeaders();
    headers += "content-type: application/json; charset=UTF-8";
    if (!RawRequest("POST", "https://i.weread.qq.com/login", headers, body, res, err,
                    kUserAgent)) {
        return false;
    }
    cJSON* root = cJSON_Parse(res.body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return false;
    }
    const cJSON* access = cJSON_GetObjectItem(root, "accessToken");
    bool ok = false;
    if (res.status == 200 && cJSON_IsString(access) && access->valuestring[0] != '\0') {
        Save("ink_access", access->valuestring);
        const cJSON* refresh = cJSON_GetObjectItem(root, "refreshToken");
        if (cJSON_IsString(refresh) && refresh->valuestring[0] != '\0') {
            Save("ink_refresh", refresh->valuestring);
        }
        const cJSON* vid = cJSON_GetObjectItem(root, "vid");
        if (cJSON_IsString(vid) && vid->valuestring[0] != '\0') {
            Save("ink_vid", vid->valuestring);
        }
        ok = true;
        ESP_LOGI(kTag, "tokens refreshed");
    } else {
        const cJSON* msg = cJSON_GetObjectItem(root, "errMsg");
        err = std::string("refresh http ") + std::to_string(res.status) + " " +
              (cJSON_IsString(msg) ? msg->valuestring : "");
    }
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------------------
// 认证 GET/POST（401 → 刷新 → 重试一次）
// ---------------------------------------------------------------------------

std::mutex g_auth_mu;  // 串行化刷新，避免并发刷爆

template <typename Fn>
bool AuthedCallWithRetry(Fn&& fn, std::string& err) {
    {
        std::lock_guard<std::mutex> lock(g_auth_mu);
        if (fn()) return true;
    }
    // 401/403：刷新令牌后重试一次
    ESP_LOGW(kTag, "auth fail, refreshing tokens");
    {
        std::lock_guard<std::mutex> lock(g_auth_mu);
        std::string refresh_err;
        if (!RefreshTokensLocked(refresh_err)) {
            err = "令牌刷新失败: " + refresh_err;
            return false;
        }
        if (fn()) return true;
    }
    if (err.empty()) err = "请求失败";
    return false;
}

}  // namespace

bool InkGet(const std::string& path, std::string& body, std::string& err) {
    if (!HasCredentials()) {
        err = "未登录";
        return false;
    }
    return AuthedCallWithRetry(
        [&]() -> bool {
            HttpResult res;
            std::string e;
            if (!RawRequest("GET", "https://i.weread.qq.com" + path, AuthHeaders(), "", res, e,
                            kUserAgent)) {
                err = e;
                return false;
            }
            if (res.status == 401 || res.status == 403) {
                err = "unauthorized";
                return false;
            }
            if (res.status != 200) {
                err = "http " + std::to_string(res.status);
                return false;
            }
            body = std::move(res.body);
            return true;
        },
        err);
}

bool InkPost(const std::string& path, const std::string& json_body, std::string& body,
             std::string& err) {
    if (!HasCredentials()) {
        err = "未登录";
        return false;
    }
    return AuthedCallWithRetry(
        [&]() -> bool {
            HttpResult res;
            std::string e;
            std::string headers = AuthHeaders();
            headers += "\ncontent-type: application/json; charset=UTF-8";
            if (!RawRequest("POST", "https://i.weread.qq.com" + path, headers, json_body, res, e,
                            kUserAgent)) {
                err = e;
                return false;
            }
            if (res.status == 401 || res.status == 403) {
                err = "unauthorized";
                return false;
            }
            if (res.status != 200) {
                err = "http " + std::to_string(res.status);
                return false;
            }
            body = std::move(res.body);
            return true;
        },
        err);
}

// ---------------------------------------------------------------------------
// QR 登录（Network 域任务内跑完整流程）
// ---------------------------------------------------------------------------

namespace {

LoginEventHandler g_handler;
std::mutex g_handler_mu;
std::atomic<bool> s_login_busy{false};
std::atomic<bool> s_login_cancel{false};

void Emit(const LoginEvent& ev) {
    LoginEventHandler fn;
    {
        std::lock_guard<std::mutex> lock(g_handler_mu);
        fn = g_handler;
    }
    if (fn) fn(ev);
}

void EmitError(const std::string& msg) {
    LoginEvent ev;
    ev.type = LoginEvent::Type::kError;
    ev.message = msg;
    Emit(ev);
}

// 解析 "k":"v" 简单 JSON 字段（响应极小，直接用 cJSON）
bool ParseTicket(const std::string& body, std::string& signature, int64_t& timestamp) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;
    const cJSON* sig = cJSON_GetObjectItem(root, "signature");
    const cJSON* ts = cJSON_GetObjectItem(root, "timeStamp");
    bool ok = cJSON_IsString(sig) && cJSON_IsNumber(ts);
    if (ok) {
        signature = sig->valuestring;
        timestamp = static_cast<int64_t>(ts->valuedouble);
    }
    cJSON_Delete(root);
    return ok;
}

bool QrTicket(std::string& signature, int64_t& timestamp, std::string& err) {
    HttpResult res;
    if (!RawRequest("GET", "https://i.weread.qq.com/wxticket?nonceStr=weread", VersionHeaders(),
                    "", res, err, kUserAgent)) {
        return false;
    }
    if (res.status != 200 || !ParseTicket(res.body, signature, timestamp)) {
        err = "ticket http " + std::to_string(res.status);
        return false;
    }
    return true;
}

bool QrUuid(const std::string& signature, int64_t timestamp, std::string& uuid, std::string& err) {
    char url[512];
    std::snprintf(url, sizeof(url),
                  "https://open.weixin.qq.com/connect/sdk/qrconnect?appid=%s&noncestr=weread"
                  "&timestamp=%lld&scope=%s&signature=%s",
                  kWxAppId, static_cast<long long>(timestamp), kScope, signature.c_str());
    HttpResult res;
    if (!RawRequest("GET", url, "", "", res, err, kUserAgent)) {
        return false;
    }
    cJSON* root = cJSON_Parse(res.body.c_str());
    if (root == nullptr) {
        err = "qrconnect bad json";
        return false;
    }
    const cJSON* code = cJSON_GetObjectItem(root, "errcode");
    const cJSON* u = cJSON_GetObjectItem(root, "uuid");
    const bool ok = res.status == 200 && cJSON_IsNumber(code) && code->valuedouble == 0 &&
                    cJSON_IsString(u) && u->valuestring[0] != '\0';
    if (ok) uuid = u->valuestring;
    cJSON_Delete(root);
    if (!ok) err = "qrconnect http " + std::to_string(res.status);
    return ok;
}

// 轮询扫码：返回 wx_code 或失败。cancelled 由调用方查 s_login_cancel。
bool PollQr(const std::string& uuid, std::string& wx_code, std::string& err) {
    const int64_t deadline_us = esp_timer_get_time() + 5LL * 60 * 1000000;
    int last = 0;
    while (esp_timer_get_time() < deadline_us) {
        if (s_login_cancel.load()) {
            err = "已取消";
            return false;
        }
        char url[384];
        if (last != 0) {
            std::snprintf(url, sizeof(url),
                          "https://long.open.weixin.qq.com/connect/l/qrconnect?f=json&uuid=%s&last=%d",
                          uuid.c_str(), last);
        } else {
            std::snprintf(url, sizeof(url),
                          "https://long.open.weixin.qq.com/connect/l/qrconnect?f=json&uuid=%s",
                          uuid.c_str());
        }
        HttpResult res;
        std::string e;
        if (!RawRequest("GET", url, "", "", res, e, "Mozilla/5.0")) {
            // 长轮询可能被超时掐断：非致命，继续（除非总超时）
            ESP_LOGW(kTag, "poll transport: %s", e.c_str());
            vTaskDelay(pdMS_TO_TICKS(800));
            continue;
        }
        cJSON* root = cJSON_Parse(res.body.c_str());
        if (root == nullptr) {
            ESP_LOGW(kTag, "poll bad json: %.80s", res.body.c_str());
            vTaskDelay(pdMS_TO_TICKS(800));
            continue;
        }
        const cJSON* code = cJSON_GetObjectItem(root, "wx_errcode");
        const int c = cJSON_IsNumber(code) ? static_cast<int>(code->valuedouble) : 0;
        ESP_LOGI(kTag, "poll code=%d", c);
        bool done = false;
        bool failed = false;
        if (c == 405) {
            const cJSON* wc = cJSON_GetObjectItem(root, "wx_code");
            if (cJSON_IsString(wc) && wc->valuestring[0] != '\0') {
                wx_code = wc->valuestring;
                done = true;
            } else {
                err = "confirm 缺 wx_code";
                failed = true;
            }
        } else if (c == 404) {
            if (last != 404) {
                LoginEvent ev;
                ev.type = LoginEvent::Type::kScanned;
                Emit(ev);
            }
        } else if (c == 402) {
            err = "二维码已过期";
            failed = true;
        } else if (c == 403) {
            err = "微信端拒绝了登录";
            failed = true;
        } else if (c == 408) {
            // 等待扫码
        }
        cJSON_Delete(root);
        last = c;
        if (done) return true;
        if (failed) return false;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    err = "扫码超时（5 分钟）";
    return false;
}

bool Exchange(const std::string& wx_code, const std::string& device_id, std::string& err) {
    const int64_t timestamp = static_cast<int64_t>(esp_timer_get_time() / 1000ULL);
    const int random = static_cast<int>(esp_random() % 1000);
    const std::string signature =
        Sha256Hex(std::to_string(timestamp) + device_id + std::to_string(random));
    const std::string install_id = "eink31" + RandomDigits(26);

    char body[1024];
    std::snprintf(body, sizeof(body),
                  "{\"appFirstInstall\":1,\"code\":\"%s\",\"deviceId\":\"%s\","
                  "\"deviceName\":\"%s\",\"installId\":\"%s\",\"isAutoLogout\":0,"
                  "\"isFromQrcode\":1,\"random\":%d,\"signature\":\"%s\","
                  "\"timestamp\":%lld,\"trackId\":\"\",\"deviceType\":3}",
                  wx_code.c_str(), device_id.c_str(), kDeviceName, install_id.c_str(), random,
                  signature.c_str(), static_cast<long long>(timestamp));

    HttpResult res;
    std::string headers = VersionHeaders();
    headers += "content-type: application/json; charset=UTF-8";
    if (!RawRequest("POST", "https://i.weread.qq.com/login", headers, body, res, err,
                    kUserAgent)) {
        return false;
    }
    cJSON* root = cJSON_Parse(res.body.c_str());
    if (root == nullptr) {
        err = "login bad json http " + std::to_string(res.status);
        ESP_LOGW(kTag, "login resp: %.300s", res.body.c_str());
        return false;
    }
    const cJSON* access = cJSON_GetObjectItem(root, "accessToken");
    const cJSON* refresh = cJSON_GetObjectItem(root, "refreshToken");
    const cJSON* vid = cJSON_GetObjectItem(root, "vid");
    const bool vid_ok = (cJSON_IsString(vid) && vid->valuestring[0] != '\0') || cJSON_IsNumber(vid);
    const bool ok = res.status == 200 && cJSON_IsString(access) && access->valuestring[0] != '\0' &&
                    cJSON_IsString(refresh) && refresh->valuestring[0] != '\0' && vid_ok;
    if (ok) {
        Save("ink_vid", cJSON_IsNumber(vid) ? std::to_string(static_cast<long long>(vid->valuedouble))
                                            : vid->valuestring);
        Save("ink_access", access->valuestring);
        Save("ink_refresh", refresh->valuestring);
        Save("ink_device", device_id);
        ESP_LOGI(kTag, "login ok vid=%s", Vid().c_str());
    } else {
        const cJSON* msg = cJSON_GetObjectItem(root, "errMsg");
        const cJSON* ecode = cJSON_GetObjectItem(root, "errCode");
        err = std::string("login http ") + std::to_string(res.status) + " errCode=" +
              (cJSON_IsNumber(ecode) ? std::to_string(static_cast<long long>(ecode->valuedouble))
                                     : "?") +
              " " + (cJSON_IsString(msg) ? msg->valuestring : "");
        ESP_LOGW(kTag, "login fail resp: %.300s", res.body.c_str());
    }
    cJSON_Delete(root);
    return ok;
}

void QrLoginJob();

// 交换阶段沿用同一会话的网络；若中途断网则重试一次
bool EnsureNetworkReadyForLogin() {
    if (Board::GetInstance().IsNetworkReady()) return true;
    return Board::GetInstance().EnsureNetworkReady(15000);
}

void QrLoginJob() {
    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    bool ok = false;
    do {
        if (!Board::GetInstance().EnsureNetworkReady(30000)) {
            EmitError("网络未就绪");
            break;
        }
        std::string signature;
        int64_t timestamp = 0;
        std::string err;
        if (!QrTicket(signature, timestamp, err)) {
            EmitError("获取扫码票据失败: " + err);
            break;
        }
        std::string uuid;
        if (!QrUuid(signature, timestamp, uuid, err)) {
            EmitError("生成二维码失败: " + err);
            break;
        }
        {
            LoginEvent ev;
            ev.type = LoginEvent::Type::kQrUrl;
            ev.url = "https://open.weixin.qq.com/connect/confirm?uuid=" + uuid;
            Emit(ev);
        }
        std::string wx_code;
        if (!PollQr(uuid, wx_code, err)) {
            EmitError(err);
            break;
        }
        if (!EnsureNetworkReadyForLogin()) {
            EmitError("网络中断");
            break;
        }
        if (!Exchange(wx_code, DeviceId(), err)) {
            EmitError("登录失败: " + err);
            break;
        }
        ok = true;
    } while (false);
    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
    s_login_busy.store(false);
    s_login_cancel.store(false);
    if (ok) {
        LoginEvent ev;
        ev.type = LoginEvent::Type::kSuccess;
        Emit(ev);
    }
}

}  // namespace

void SetLoginEventHandler(LoginEventHandler handler) {
    std::lock_guard<std::mutex> lock(g_handler_mu);
    g_handler = std::move(handler);
}

bool StartQrLogin() {
    if (s_login_busy.exchange(true)) return false;
    s_login_cancel.store(false);
    if (!Application::GetInstance().Schedule([]() { QrLoginJob(); },
                                             Application::TaskDomain::Network)) {
        s_login_busy.store(false);
        return false;
    }
    return true;
}

void CancelQrLogin() { s_login_cancel.store(true); }

bool IsLoginBusy() { return s_login_busy.load(); }

}  // namespace ink
}  // namespace weread
