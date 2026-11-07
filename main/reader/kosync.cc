#include "kosync.h"

#include "book_session.h"

#include <settings.h>
#include <application.h>
#include <board.h>
#include <power_policy.h>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_md5.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace reader {
namespace kosync {

static const char* kTag = "KOSync";
static const char* kNvsNs = "kosync";
static const char* kDefaultServer = "https://sync.koreader.rocks";
static const char* kDeviceName = "TingQu-S1";

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static std::string Md5Hex(const uint8_t* data, size_t len) {
    md5_context_t ctx;
    uint8_t digest[16];
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, data, static_cast<uint32_t>(len));
    esp_rom_md5_final(digest, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 32);
}

// KOReader util.partialMD5：12 个采样点各取 1024 字节做 MD5。
// 偏移 = i=-1 时 1024>>2=256，其余 1024<<(2i)（i=0..10）；
// 读取短于 1024 仍入链（Lua file:read 部分读行为），读到 0 字节即停。
static std::string PartialMD5(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return "";
    md5_context_t ctx;
    uint8_t digest[16];
    esp_rom_md5_init(&ctx);
    uint8_t buf[1024];
    for (int i = -1; i <= 10; ++i) {
        const long off = (i < 0) ? (1024 >> 2) : (1024L << (2 * i));
        if (fseek(f, off, SEEK_SET) != 0) break;
        const size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        esp_rom_md5_update(&ctx, buf, static_cast<uint32_t>(n));
    }
    fclose(f);
    esp_rom_md5_final(digest, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 32);
}

// 配置键合法性：用户名/密码拒绝引号与反斜杠（JSON 转义从简）；服务器必须 https
static bool IsSafeCredentialText(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// NVS 配置
// ---------------------------------------------------------------------------

namespace {

std::string LoadString(const char* key) {
    Settings settings(kNvsNs);
    return settings.GetString(key);
}

void SaveString(const char* key, const std::string& value) {
    Settings settings(kNvsNs, true);
    settings.SetString(key, value);
}

}  // namespace

bool IsEnabled() { return LoadString("enabled") == "1"; }
void SetEnabled(bool enabled) { SaveString("enabled", enabled ? "1" : "0"); }

std::string Server() {
    const std::string s = LoadString("server");
    return s.empty() ? std::string(kDefaultServer) : s;
}

bool SetServer(const std::string& server_raw) {
    std::string s = server_raw;
    // 去首尾空白 + 去尾部斜杠（拼接 path 用）
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n/");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    if (s.rfind("https://", 0) != 0 || s.size() > 96) {
        ESP_LOGW(kTag, "reject server url");
        return false;
    }
    SaveString("server", s);
    return true;
}

std::string Username() { return LoadString("username"); }

bool SetUsername(const std::string& username) {
    if (!IsSafeCredentialText(username)) return false;
    SaveString("username", username);
    return true;
}

bool SetPassword(const std::string& plaintext) {
    if (plaintext.empty()) {
        SaveString("userkey", "");
        return true;
    }
    if (!IsSafeCredentialText(plaintext)) return false;
    SaveString("userkey", Md5Hex(reinterpret_cast<const uint8_t*>(plaintext.data()),
                                 plaintext.size()));
    return true;
}

void ClearCredentials() {
    SaveString("username", "");
    SaveString("userkey", "");
}

bool HasCredentials() { return !Username().empty() && !LoadString("userkey").empty(); }

bool AutoJump() { return LoadString("auto_jump") != "0"; }
void SetAutoJump(bool enabled) { SaveString("auto_jump", enabled ? "1" : "0"); }

std::string MatchMode() {
    const std::string m = LoadString("match_mode");
    return m == "filename" ? "filename" : "content";
}

bool SetMatchMode(const std::string& mode) {
    if (mode != "content" && mode != "filename") return false;
    SaveString("match_mode", mode);
    return true;
}

bool AutoSync() { return LoadString("auto_sync") != "0"; }
void SetAutoSync(bool enabled) { SaveString("auto_sync", enabled ? "1" : "0"); }

// 书的 digest：按匹配方式取 KOReader partialMD5（内容）或 md5(文件名)（文件名）
static std::string BookDigest(const char* path) {
    if (MatchMode() == "filename") {
        const char* base = std::strrchr(path, '/');
        base = (base != nullptr && base[1] != '\0') ? (base + 1) : path;
        return Md5Hex(reinterpret_cast<const uint8_t*>(base), std::strlen(base));
    }
    return PartialMD5(path);
}

std::string DeviceId() {
    std::string id = LoadString("device_id");
    if (!id.empty()) return id;
    uint8_t raw[8];
    esp_fill_random(raw, sizeof(raw));
    char hex[17];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", raw[i]);
    }
    id.assign(hex, 16);
    SaveString("device_id", id);
    return id;
}

// ---------------------------------------------------------------------------
// HTTP（SDK esp_http_client，同 weread 可靠模式；x-auth 自定义头鉴权）
// ---------------------------------------------------------------------------

namespace {

struct HttpResult {
    long status = 0;
    std::string body;
};

// path 以 '/' 开头；json_body 空 = 无请求体。失败返回 false 且 err 带原因。
bool KosyncRequest(const char* method, const std::string& path,
                   const std::string& json_body, HttpResult& out, std::string& err) {
    out = HttpResult{};
    err.clear();
    const std::string server = Server();
    const std::string url = server + path;

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = std::strcmp(method, "PUT") == 0   ? HTTP_METHOD_PUT
                 : std::strcmp(method, "POST") == 0 ? HTTP_METHOD_POST
                                                    : HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        err = "http init failed";
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-auth-user", Username().c_str());
    esp_http_client_set_header(client, "x-auth-key", LoadString("userkey").c_str());

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
            const int64_t deadline_us = esp_timer_get_time() + 20000000LL;
            while (esp_timer_get_time() <= deadline_us) {
                const int r = esp_http_client_read(client, buf, sizeof(buf));
                if (r < 0) {
                    err = "read failed";
                    break;
                }
                if (r == 0) break;  // EOF
                out.body.append(buf, static_cast<size_t>(r));
                if (out.body.size() > 64 * 1024) {
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

// ---------------------------------------------------------------------------
// 推拉任务（Network 域执行）
// ---------------------------------------------------------------------------

struct BookSyncState {
    int64_t last_push_us = 0;
    int last_pushed_x10 = -1;
    int last_local_x10 = -1;
};
std::mutex g_state_mu;
std::map<std::string, BookSyncState> g_state;

// 跳转 handler 本体（SetJumpHandler 在匿名空间外定义，供 book_screen 注册）
JumpHandler g_jump_handler;
std::mutex g_handler_mu;

// 手动同步结果回执（仅 force 操作触发；失败时上屏提示）
SyncStatusHandler g_status_handler;
std::mutex g_status_mu;

void CallJumpHandler(const std::string& path, int pct_x10, const std::string& device) {
    JumpHandler fn;
    {
        std::lock_guard<std::mutex> lock(g_handler_mu);
        fn = g_jump_handler;
    }
    if (fn) fn(path, pct_x10, device);
}

void CallStatusHandler(const char* message) {
    SyncStatusHandler fn;
    {
        std::lock_guard<std::mutex> lock(g_status_mu);
        fn = g_status_handler;
    }
    if (fn) fn(message);
}

void PushJob(const std::string book_path, int pct_x10, bool force) {
    if (!IsEnabled() || !HasCredentials() || pct_x10 < 0) return;
    if (!force && !AutoSync()) return;  // 自动同步被关闭：仅手动 force 生效
    if (!force) {
        std::lock_guard<std::mutex> lock(g_state_mu);
        BookSyncState& st = g_state[book_path];
        st.last_local_x10 = pct_x10;
        // 节流：与上次推送差 <0.3% 且距上次 <180s 则跳过
        if (st.last_pushed_x10 >= 0 && std::abs(pct_x10 - st.last_pushed_x10) < 3 &&
            esp_timer_get_time() - st.last_push_us < 180LL * 1000000) {
            return;
        }
    } else {
        std::lock_guard<std::mutex> lock(g_state_mu);
        g_state[book_path].last_local_x10 = pct_x10;
    }
    const std::string digest = BookDigest(book_path.c_str());
    if (digest.empty()) {
        ESP_LOGW(kTag, "book digest fail: %s", book_path.c_str());
        return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    bool pushed = false;
    do {
        // 手动操作给足起网时间（4G 附着可达 30s）；有网则迅速返回并刷新保网窗口
        if (!Board::GetInstance().EnsureNetworkReady(force ? 30000 : 10000)) {
            if (force) CallStatusHandler("上传失败：网络未就绪");
            break;
        }
        char body[256];
        std::snprintf(body, sizeof(body),
                      "{\"document\":\"%s\",\"progress\":\"%d\",\"percentage\":%.4f,"
                      "\"device\":\"%s\",\"device_id\":\"%s\"}",
                      digest.c_str(), pct_x10, pct_x10 / 1000.0, kDeviceName,
                      DeviceId().c_str());
        HttpResult res;
        std::string err;
        if (!KosyncRequest("PUT", "/syncs/progress", body, res, err)) {
            ESP_LOGW(kTag, "push transport fail: %s", err.c_str());
            if (force) CallStatusHandler("上传失败：网络错误");
            break;
        }
        if (res.status == 200 || res.status == 202) {
            pushed = true;
            ESP_LOGI(kTag, "push ok %s pct=%.1f%%", digest.c_str(), pct_x10 / 10.0);
        } else {
            ESP_LOGW(kTag, "push http %ld", res.status);
            if (force) {
                CallStatusHandler(res.status == 401 ? "上传失败：账号或密码不符"
                                                    : "上传失败：服务器错误");
            }
        }
    } while (false);
    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
    if (pushed) {
        std::lock_guard<std::mutex> lock(g_state_mu);
        BookSyncState& st = g_state[book_path];
        st.last_push_us = esp_timer_get_time();
        st.last_pushed_x10 = pct_x10;
    }
}

void PullJob(const std::string book_path, bool force) {
    if (!IsEnabled() || !HasCredentials()) return;
    if (!force && !AutoSync()) return;  // 自动同步被关闭：仅手动 force 生效
    const std::string digest = BookDigest(book_path.c_str());
    if (digest.empty()) {
        ESP_LOGW(kTag, "book digest fail: %s", book_path.c_str());
        return;
    }
    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    do {
        if (!Board::GetInstance().EnsureNetworkReady(force ? 30000 : 10000)) {
            if (force) CallStatusHandler("拉取失败：网络未就绪");
            break;
        }
        HttpResult res;
        std::string err;
        if (!KosyncRequest("GET", "/syncs/progress/" + digest, "", res, err)) {
            ESP_LOGW(kTag, "pull transport fail: %s", err.c_str());
            if (force) CallStatusHandler("拉取失败：网络错误");
            break;
        }
        if (res.status != 200) {
            // 404=云端无记录；401=凭据失效：静默（避免刷日志）；手动操作回执
            if (res.status == 401) {
                if (force) CallStatusHandler("拉取失败：账号或密码不符");
            } else if (res.status != 404) {
                ESP_LOGW(kTag, "pull http %ld", res.status);
                if (force) CallStatusHandler("拉取失败：服务器错误");
            } else if (force) {
                CallStatusHandler("云端暂无本书进度");
            }
            break;
        }
        cJSON* root = cJSON_Parse(res.body.c_str());
        if (root == nullptr) break;
        const cJSON* pct = cJSON_GetObjectItem(root, "percentage");
        const cJSON* device = cJSON_GetObjectItem(root, "device");
        const cJSON* device_id = cJSON_GetObjectItem(root, "device_id");
        if (cJSON_IsNumber(pct)) {
            const std::string remote_dev = cJSON_IsString(device) ? device->valuestring : "?";
            const std::string remote_id =
                cJSON_IsString(device_id) ? device_id->valuestring : "";
            const int remote_x10 = static_cast<int>(pct->valuedouble * 1000.0 + 0.5);
            int local_x10 = BookSession::PeekProgress(book_path.c_str()).progress_x10;
            if (local_x10 < 0) local_x10 = 0;
            {
                std::lock_guard<std::mutex> lock(g_state_mu);
                const auto it = g_state.find(book_path);
                if (it != g_state.end() && it->second.last_local_x10 >= 0) {
                    local_x10 = it->second.last_local_x10;
                }
            }
            ESP_LOGI(kTag, "pull %s remote=%.1f%% local=%.1f%% from=%s", digest.c_str(),
                     remote_x10 / 10.0, local_x10 / 10.0, remote_dev.c_str());
            // 仅前进：远端须明显领先本地 +0.3%；跨渲染引擎百分比不可比，
            // 回退跳转几乎都是误伤（云端记录常被另一端的落后进度覆盖）。
            // 手动拉取同样遵守，但通过 StatusHandler 回执说明原因。
            const int delta = remote_x10 - local_x10;
            const bool should_jump = remote_id != DeviceId() && remote_x10 <= 1000 && delta > 3;
            if (should_jump) {
                CallJumpHandler(book_path, remote_x10, remote_dev);
            } else if (force) {
                CallStatusHandler(remote_id == DeviceId() ? "云端进度来自本机，无需跳转"
                              : (delta <= 3 ? "云端与本地进度一致"
                                            : "云端进度落后于本地，未跳转（防回退）"));
            }
        }
        cJSON_Delete(root);
    } while (false);
    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
}

}  // namespace

// book_screen 注册进度跳转回调（注册一次；内部副本线程安全）
void SetJumpHandler(JumpHandler handler) {
    std::lock_guard<std::mutex> lock(g_handler_mu);
    g_jump_handler = std::move(handler);
}

// book_screen 注册手动同步失败回执（注册一次）
void SetStatusHandler(SyncStatusHandler handler) {
    std::lock_guard<std::mutex> lock(g_status_mu);
    g_status_handler = std::move(handler);
}

// ---------------------------------------------------------------------------
// 对外钩子（任意线程调用；**本函数体内禁止任何 NVS 读写**——调用方可能是
// PSRAM 栈任务（book_open worker 16KB PSRAM），SDK 在关 Flash 缓存前会做
// 栈 sanity 校验直接断言重启。NVS 检查下沉到 Network 域任务内执行。
// ---------------------------------------------------------------------------

void NotifyBookOpened(const std::string& book_path) {
    if (book_path.empty()) return;
    Application::GetInstance().Schedule([book_path]() { PullJob(book_path, false); },
                                        Application::TaskDomain::Network);
}

void NotifyProgressSaved(const std::string& book_path, int pct_x10) {
    if (book_path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_state_mu);
        g_state[book_path].last_local_x10 = pct_x10;
    }
    Application::GetInstance().Schedule(
        [book_path, pct_x10]() { PushJob(book_path, pct_x10, false); },
        Application::TaskDomain::Network);
}

void PushNow(const std::string& book_path, int pct_x10) {
    if (book_path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_state_mu);
        g_state[book_path].last_local_x10 = pct_x10;
    }
    Application::GetInstance().Schedule(
        [book_path, pct_x10]() { PushJob(book_path, pct_x10, true); },
        Application::TaskDomain::Network);
}

void PullNow(const std::string& book_path) {
    if (book_path.empty()) return;
    Application::GetInstance().Schedule([book_path]() { PullJob(book_path, true); },
                                        Application::TaskDomain::Network);
}

// ---------------------------------------------------------------------------
// 设置页用的阻塞操作（设置页自行保证在 Network 域任务里调用）
// ---------------------------------------------------------------------------

bool AuthorizeNow(std::string& err) {
    if (!HasCredentials()) {
        err = "未配置用户名/密码";
        return false;
    }
    if (!Board::GetInstance().EnsureNetworkReady(15000)) {
        err = "网络未就绪";
        return false;
    }
    HttpResult res;
    if (!KosyncRequest("GET", "/users/auth", "", res, err)) return false;
    if (res.status == 200) return true;
    err = "http " + std::to_string(res.status) + (res.status == 401 ? "（账号或密码不符）" : "");
    return false;
}

bool RegisterNow(std::string& err) {
    if (!HasCredentials()) {
        err = "先填写用户名和密码";
        return false;
    }
    if (!Board::GetInstance().EnsureNetworkReady(15000)) {
        err = "网络未就绪";
        return false;
    }
    const std::string body = "{\"username\":\"" + Username() +
                             "\",\"password\":\"" + LoadString("userkey") + "\"}";
    HttpResult res;
    if (!KosyncRequest("POST", "/users/create", body, res, err)) return false;
    if (res.status == 201) return true;
    if (res.status == 402) {
        // 账号已存在：转鉴权验证密码是否匹配
        return AuthorizeNow(err);
    }
    err = "http " + std::to_string(res.status);
    return false;
}

}  // namespace kosync
}  // namespace reader
