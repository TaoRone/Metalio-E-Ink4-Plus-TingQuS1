#include "weread_service.h"

#include <settings.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// cJSON 默认走片上内存（运行时仅剩 ~66KB），书架 JSON（~100KB）的 DOM 需要
// ~150-200KB，必须临时切到 PSRAM 解析；解析完恢复默认钩子。
class CjsonPsramGuard {
public:
    CjsonPsramGuard() {
        cJSON_Hooks hooks = {};
        hooks.malloc_fn = [](size_t sz) {
            return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        };
        hooks.free_fn = [](void* p) { heap_caps_free(p); };
        cJSON_InitHooks(&hooks);
    }
    ~CjsonPsramGuard() { cJSON_InitHooks(nullptr); }
};

}  // namespace

namespace weread {

static const char* kTag = "WereadSvc";
static const char* kGateway = "https://i.weread.qq.com/api/agent/gateway";
static const char* kSkillVersion = "1.0.4";
static const char* kNvsNs = "weread";
static const char* kNvsKey = "api_key";

namespace {

std::string LoadKey() {
    Settings settings(kNvsNs);
    return settings.GetString(kNvsKey);
}

}  // namespace

bool HasApiKey() {
    return !LoadKey().empty();
}

bool SetApiKey(const std::string& key_raw) {
    // 去首尾空白（网页输入框常带换行/空格）
    size_t begin = key_raw.find_first_not_of(" \t\r\n");
    size_t end = key_raw.find_last_not_of(" \t\r\n");
    const std::string key = (begin == std::string::npos) ? "" : key_raw.substr(begin, end - begin + 1);
    if (key.size() < 20 || key.rfind("wrk-", 0) != 0) {
        ESP_LOGE(kTag, "invalid api key format");
        return false;
    }
    Settings settings(kNvsNs, true);
    settings.SetString(kNvsKey, key);
    return true;
}

void ClearApiKey() {
    Settings settings(kNvsNs, true);
    settings.EraseKey(kNvsKey);
}

bool GatewayCall(const std::string& api_name, const std::string& params_json,
                 std::string& out_body, std::string& out_err) {
    out_body.clear();
    out_err.clear();

    const std::string key = LoadKey();
    if (key.empty()) {
        out_err = "api key missing";
        return false;
    }

    // 请求体：api_name / skill_version 与业务参数同层平铺
    std::string body;
    body.reserve(128 + params_json.size());
    body = "{\"api_name\":\"";
    body += api_name;
    body += "\",\"skill_version\":\"";
    body += kSkillVersion;
    body += "\"";
    if (!params_json.empty()) {
        body += ",";
        body += params_json;
    }
    body += "}";

    // 使用 SDK 原生 esp_http_client。原因：板载自定义 HttpClient 栈（esp-ml307 组件）
    // 的 TLS 写入路径在本设备上会无日志永久挂起（握手成功、请求发出后卡死），
    // 而 SDK 客户端超时语义可靠；WiFi/4G 均走标准 netif，与板卡抽象解耦。
    esp_http_client_config_t cfg = {};
    cfg.url = kGateway;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        out_err = "http init failed";
        return false;
    }
    esp_http_client_set_header(client, "Authorization", ("Bearer " + key).c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    const int64_t deadline_us = esp_timer_get_time() + 25000000LL;
    int code = -1;
    char* buf = nullptr;  // PSRAM 响应缓冲
    size_t buf_len = 0;
    size_t buf_cap = 0;

    const esp_err_t open_err = esp_http_client_open(client, static_cast<int>(body.size()));
    bool transport_ok = false;
    if (open_err == ESP_OK) {
        const int written = esp_http_client_write(client, body.c_str(), static_cast<int>(body.size()));
        if (written >= 0) {
            esp_http_client_fetch_headers(client);
            code = esp_http_client_get_status_code(client);
            const int total = esp_http_client_get_content_length(client);
            buf_cap = total > 0 ? static_cast<size_t>(total) : (64 * 1024);
            buf = static_cast<char*>(heap_caps_malloc(buf_cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (buf != nullptr) {
                while (buf_len < buf_cap) {
                    if (esp_timer_get_time() > deadline_us) {
                        if (out_err.empty()) out_err = "read deadline";
                        break;
                    }
                    const int r = esp_http_client_read(client, buf + buf_len, static_cast<int>(buf_cap - buf_len));
                    if (r <= 0) break;  // 0=EOF，<0=错误/超时
                    buf_len += static_cast<size_t>(r);
                }
                buf[buf_len] = '\0';
                transport_ok = true;
            } else {
                out_err = "psram oom";
            }
            ESP_LOGI(kTag, "gateway %s -> http %d, body %u bytes", api_name.c_str(), code,
                     static_cast<unsigned>(buf_len));
        } else {
            out_err = "write failed";
        }
    } else {
        out_err = "open failed: ";
        out_err += esp_err_to_name(open_err);
    }
    esp_http_client_cleanup(client);

    if (!transport_ok) {
        if (buf != nullptr) heap_caps_free(buf);
        if (out_err.empty()) out_err = "transport";
        return false;
    }
    out_body.assign(buf, buf_len);
    heap_caps_free(buf);

    bool ok = false;
    if (code == 200) {
        // 业务层错误（如 token 失效）以 errcode 非零表示
        CjsonPsramGuard guard;
        cJSON* root = cJSON_Parse(out_body.c_str());
        if (root != nullptr) {
            const cJSON* errcode = cJSON_GetObjectItem(root, "errcode");
            if (cJSON_IsNumber(errcode) && errcode->valueint != 0) {
                const cJSON* errmsg = cJSON_GetObjectItem(root, "errmsg");
                out_err = (errmsg != nullptr && cJSON_IsString(errmsg))
                              ? errmsg->valuestring
                              : "gateway errcode " + std::to_string(errcode->valueint);
                ESP_LOGE(kTag, "gateway biz error: %s", out_err.c_str());
            } else {
                ok = true;
            }
            cJSON_Delete(root);
        } else {
            // 非 JSON 响应（正常情况下网关总返回 JSON）；按成功透传由调用方解析判断
            ok = !out_body.empty();
            if (!ok) out_err = "empty response";
        }
    } else {
        out_err = "http " + std::to_string(code);
        ESP_LOGE(kTag, "gateway http %d, body head: %.160s", code, out_body.c_str());
    }
    return ok;
}

bool FetchShelf(std::vector<ShelfBook>& out, std::string& err) {
    std::string discard;
    return FetchShelf(out, discard, err);
}

bool FetchShelf(std::vector<ShelfBook>& out, std::string& raw_body, std::string& err) {
    out.clear();
    raw_body.clear();
    const int64_t begin_us = esp_timer_get_time();
    std::string body;
    if (!GatewayCall("/shelf/sync", "", body, err)) {
        return false;
    }
    raw_body = body;
    const int64_t got_us = esp_timer_get_time();

    if (!ParseShelfBody(body, out, err)) {
        return false;
    }
    const int64_t parsed_us = esp_timer_get_time();
    ESP_LOGI(kTag, "shelf: http %lld ms, parse %lld ms, books %u",
             (long long)((got_us - begin_us) / 1000), (long long)((parsed_us - got_us) / 1000),
             static_cast<unsigned>(out.size()));
    return true;
}

bool ParseShelfBody(const std::string& body, std::vector<ShelfBook>& out, std::string& err) {
    CjsonPsramGuard guard;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return false;
    }
    const cJSON* books = cJSON_GetObjectItem(root, "books");
    if (!cJSON_IsArray(books)) {
        err = "no books field";
        cJSON_Delete(root);
        return false;
    }
    out.clear();
    out.reserve(cJSON_GetArraySize(books));
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, books) {
        ShelfBook b;
        const cJSON* v = nullptr;
        if ((v = cJSON_GetObjectItem(item, "bookId")) && cJSON_IsString(v)) b.book_id = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "title")) && cJSON_IsString(v)) b.title = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "author")) && cJSON_IsString(v)) b.author = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "cover")) && cJSON_IsString(v)) b.cover = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "category")) && cJSON_IsString(v)) b.category = v->valuestring;
        if ((v = cJSON_GetObjectItem(item, "finishReading")) && cJSON_IsNumber(v)) b.finish_reading = v->valueint;
        if ((v = cJSON_GetObjectItem(item, "readUpdateTime")) && cJSON_IsNumber(v)) b.read_update_time = (int64_t)v->valuedouble;
        if (!b.book_id.empty() && !b.title.empty()) {
            out.push_back(std::move(b));
        }
    }
    cJSON_Delete(root);
    if (out.empty()) {
        err = "empty shelf";
        return false;
    }
    return true;
}

bool FetchBookmarkCount(const std::string& book_id, int& count, std::string& err) {
    count = 0;
    std::string body;
    const std::string params = "\"bookId\":\"" + book_id + "\"";
    if (!GatewayCall("/book/bookmarklist", params, body, err)) {
        return false;
    }
    CjsonPsramGuard guard;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return false;
    }
    for (const char* key : {"updated", "bookmarks", "items", "list", "data"}) {
        const cJSON* arr = cJSON_GetObjectItem(root, key);
        if (cJSON_IsArray(arr)) {
            count = cJSON_GetArraySize(arr);
            break;
        }
    }
    cJSON_Delete(root);
    return true;
}

bool SaveShelfCacheBody(const std::string& body) {
    const char* path = "/sdcard/metalio/e-ink/weread/shelf_cache.json";
    if (mkdir("/sdcard/metalio/e-ink/weread", 0755) != 0 && errno != EEXIST) {
        ESP_LOGW("WereadSvc", "cache mkdir failed errno=%d", errno);
    }
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        ESP_LOGW("WereadSvc", "cache save open failed");
        return false;
    }
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    ESP_LOGI("WereadSvc", "cache saved %u bytes", static_cast<unsigned>(body.size()));
    return true;
}

bool LoadShelfCacheBody(std::string& body) {
    const char* path = "/sdcard/metalio/e-ink/weread/shelf_cache.json";
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        body.append(buf, n);
    }
    fclose(f);
    ESP_LOGI("WereadSvc", "cache loaded %u bytes", static_cast<unsigned>(body.size()));
    return !body.empty();
}

// ---------------------------------------------------------------------------
// 封面缩略图源数据
// ---------------------------------------------------------------------------

// 封面源图上限 4MB：parsecover 类全尺寸封面 PNG 可超过 1MB（PSRAM 充裕）；
// 仅作用于下载/缓存，解码仍按 200x268 缩小
constexpr size_t kCoverMaxBytes = 4 * 1024 * 1024;
constexpr const char* kCoverDir = "/sdcard/metalio/e-ink/weread/covers";

static std::string CoverFileStem(const std::string& book_id) {
    std::string s;
    s.reserve(book_id.size());
    for (char c : book_id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        s.push_back(ok ? c : '_');
    }
    return s;
}

// 响应头捕获回调：esp_http_client 的响应 Location 只能经 HTTP_EVENT_ON_HEADER 拿到
extern "C" esp_err_t CoverRedirectHeaderHandler(esp_http_client_event_t* evt) {
    if (evt != nullptr && evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key != nullptr && evt->header_value != nullptr &&
        strcasecmp(evt->header_key, "Location") == 0) {
        auto* out = static_cast<std::string*>(evt->user_data);
        if (out != nullptr) *out = evt->header_value;
    }
    return ESP_OK;
}

bool FetchCoverBytes(const std::string& url, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    // 书架 cover 均为 https（myqcloud / cdn.weread.qq.com）；不放行其它协议。
    // res.weread.qq.com/wrepub/*_parsecover 会 302 到签名 COS URL，手动跟随（≤2 跳）。
    // 响应头只能经 HTTP_EVENT_ON_HEADER 事件回调捕获（esp_http_client_get_header
    // 只读请求头，实测拿不到 Location）。
    std::string current = url;
    for (int hop = 0; hop < 3; ++hop) {
        err.clear();
        if (current.rfind("https://", 0) != 0) {
            err = "bad url";
            return false;
        }

        esp_http_client_config_t cfg = {};
        cfg.url = current.c_str();
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 15000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.keep_alive_enable = false;
        cfg.buffer_size = 2048;
        cfg.buffer_size_tx = 1024;
        std::string redirect_loc;
        cfg.event_handler = CoverRedirectHeaderHandler;
        cfg.user_data = &redirect_loc;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == nullptr) {
            err = "http init failed";
            return false;
        }

        bool done = false;
        bool ok = false;
        if (esp_http_client_open(client, 0) != ESP_OK) {
            err = "open failed";
            done = true;
        } else {
            esp_http_client_fetch_headers(client);
            const int code = esp_http_client_get_status_code(client);
            const int total = esp_http_client_get_content_length(client);
            if (code >= 300 && code < 400) {
                if (!redirect_loc.empty() && redirect_loc.size() < 1024) {
                    std::string next;
                    if (redirect_loc.rfind("https://", 0) == 0) {
                        next = redirect_loc;
                    } else if (redirect_loc.rfind("//", 0) == 0) {
                        next = "https:" + redirect_loc;
                    } else if (redirect_loc[0] == '/') {
                        const size_t host_end = current.find('/', current.find("://") + 3);
                        next = (host_end == std::string::npos ? current : current.substr(0, host_end)) +
                               redirect_loc;
                    }
                    if (!next.empty()) {
                        current = next;
                        ESP_LOGI(kTag, "cover redirect %d -> %.80s", code, current.c_str());
                    } else {
                        err = "bad redirect";
                        done = true;
                    }
                } else {
                    err = "redirect " + std::to_string(code) + " no location";
                    done = true;
                }
            } else if (code < 200 || code >= 300) {
                err = "http " + std::to_string(code);
                done = true;
            } else if (total > static_cast<int>(kCoverMaxBytes)) {
                err = "cover too large";
                done = true;
            } else {
                // 有 Content-Length 按其分配（封顶 512KB）；无则按上限读，断连截短由解码器判坏图
                const size_t cap = total > 0 ? static_cast<size_t>(total) : kCoverMaxBytes;
                out.resize(cap);
                size_t len = 0;
                const int64_t deadline_us = esp_timer_get_time() + 20000000LL;
                while (len < cap) {
                    if (esp_timer_get_time() > deadline_us) {
                        err = "read deadline";
                        break;
                    }
                    const int r =
                        esp_http_client_read(client, reinterpret_cast<char*>(out.data()) + len,
                                             static_cast<int>(cap - len));
                    if (r < 0) {
                        err = "read failed";
                        break;
                    }
                    if (r == 0) break;  // EOF
                    len += static_cast<size_t>(r);
                }
                if (err.empty() && len > 0) {
                    out.resize(len);
                    ok = true;
                } else {
                    out.clear();
                    if (err.empty()) err = "empty body";
                }
                done = true;
            }
        }
        esp_http_client_cleanup(client);
        if (done) {
            if (!ok) {
                ESP_LOGW(kTag, "cover dl fail: %s <- %.80s", err.c_str(), current.c_str());
            }
            return ok;
        }
        // 3xx 已解析出新 URL：下一跳
    }
    err = "too many redirects";
    return false;
}

std::string CoverCachePath(const std::string& book_id) {
    const std::string stem = CoverFileStem(book_id);
    if (stem.empty() || stem == "." || stem == "..") return "";
    return std::string(kCoverDir) + "/" + stem + ".img";
}

bool SaveCoverCache(const std::string& book_id, const uint8_t* data, size_t len) {
    const std::string path = CoverCachePath(book_id);
    if (path.empty() || data == nullptr || len == 0 || len > kCoverMaxBytes) return false;
    // covers 是 weread 子目录：父目录可能不存在（从未同步过），逐层建
    mkdir("/sdcard/metalio/e-ink/weread", 0755);
    if (mkdir(kCoverDir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "cover cache mkdir failed errno=%d", errno);
        return false;
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) {
        unlink(path.c_str());
        return false;
    }
    return true;
}

bool LoadCoverCache(const std::string& book_id, std::vector<uint8_t>& out) {
    out.clear();
    const std::string path = CoverCachePath(book_id);
    if (path.empty()) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    fclose(f);
    if (out.empty() || out.size() > kCoverMaxBytes) {
        out.clear();
        return false;
    }
    return true;
}

bool PreferGridView() {
    Settings settings(kNvsNs);
    return settings.GetString("view_mode") == "grid";
}

bool SetPreferGridView(bool grid) {
    Settings settings(kNvsNs, true);
    settings.SetString("view_mode", grid ? "grid" : "list");
    return true;
}

int FetchProgressPercent(const std::string& book_id, std::string& err) {
    std::string body;
    const std::string params = "\"bookId\":\"" + book_id + "\"";
    if (!GatewayCall("/book/getprogress", params, body, err)) {
        return -1;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return -1;
    }
    int percent = -1;
    // 官方字段在 book 子对象：progress（0-100 int；0=无记录）
    const cJSON* book = cJSON_GetObjectItem(root, "book");
    if (cJSON_IsObject(book)) {
        const cJSON* p = cJSON_GetObjectItem(book, "progress");
        if (cJSON_IsNumber(p)) {
            double v = p->valuedouble;
            if (v <= 1.0 && v > 0.0) v *= 100.0;  // 0-1 小数 → 百分比
            percent = static_cast<int>(v + 0.5);
        }
    }
    cJSON_Delete(root);
    if (percent < 0) err = "no progress field";
    return percent;
}

// 单本书阅读时长（秒；无记录/失败返回 -1）。字段同在 getprogress 的 book 子对象
int64_t FetchReadingTimeSec(const std::string& book_id, std::string& err) {
    std::string body;
    const std::string params = "\"bookId\":\"" + book_id + "\"";
    if (!GatewayCall("/book/getprogress", params, body, err)) {
        return -1;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return -1;
    }
    int64_t sec = -1;
    const cJSON* book = cJSON_GetObjectItem(root, "book");
    if (cJSON_IsObject(book)) {
        const cJSON* t = cJSON_GetObjectItem(book, "readingTime");
        if (cJSON_IsNumber(t)) sec = static_cast<int64_t>(t->valuedouble);
    }
    cJSON_Delete(root);
    return sec;
}

bool FetchReadStats(ReadStats& out, std::string& err) {
    out = ReadStats{};
    std::string body;
    if (!GatewayCall("/readdata/detail", "\"mode\":\"weekly\"", body, err)) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return false;
    }
    const cJSON* v = nullptr;
    if ((v = cJSON_GetObjectItem(root, "readingDayCount")) && cJSON_IsNumber(v)) {
        out.reading_days = v->valueint;
    } else if ((v = cJSON_GetObjectItem(root, "readingDays")) && cJSON_IsNumber(v)) {
        out.reading_days = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "totalReadingTime")) && cJSON_IsNumber(v)) {
        out.total_reading_time_min = static_cast<int>(v->valuedouble / 60.0 + 0.5);
    } else if ((v = cJSON_GetObjectItem(root, "totalReadingTimeMin")) && cJSON_IsNumber(v)) {
        out.total_reading_time_min = v->valueint;
    }
    cJSON_Delete(root);
    return true;
}

}  // namespace weread
