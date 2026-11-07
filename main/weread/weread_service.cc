#include "weread_service.h"

#include <board.h>
#include <http.h>
#include <settings.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <cstdio>

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

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        out_err = "no network";
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        out_err = "http create failed";
        return false;
    }
    http->SetTimeout(20000);
    http->SetHeader("Authorization", "Bearer " + key);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json");
    http->SetKeepAlive(false);
    http->SetContent(std::move(body));

    bool ok = false;
    if (http->Open("POST", kGateway)) {
        const int code = http->GetStatusCode();
        out_body = http->ReadAll();
        ESP_LOGI(kTag, "gateway %s -> http %d, body %u bytes", api_name.c_str(), code,
                 static_cast<unsigned>(out_body.size()));
        if (code == 200) {
            // 业务层错误（如 token 失效）以 errcode 非零表示
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
    } else {
        out_err = "connect failed";
        ESP_LOGE(kTag, "gateway connect failed, last_err=%d", http->GetLastError());
    }
    http->Close();
    return ok;
}

bool FetchShelf(std::vector<ShelfBook>& out, std::string& err) {
    out.clear();
    const int64_t begin_us = esp_timer_get_time();
    std::string body;
    if (!GatewayCall("/shelf/sync", "", body, err)) {
        return false;
    }
    const int64_t got_us = esp_timer_get_time();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = "bad json";
        return false;
    }
    const int64_t parsed_us = esp_timer_get_time();
    const cJSON* books = cJSON_GetObjectItem(root, "books");
    if (!cJSON_IsArray(books)) {
        err = "no books field";
        cJSON_Delete(root);
        return false;
    }
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
    ESP_LOGI(kTag, "shelf: http %lld ms, parse %lld ms, books %u",
             (long long)((got_us - begin_us) / 1000), (long long)((parsed_us - got_us) / 1000),
             static_cast<unsigned>(out.size()));
    if (out.empty()) {
        err = "empty shelf";
        return false;
    }
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
    // 官方返回字段：readingProgress / progress 等，取第一个可用的数值字段
    const char* kCandidates[] = {"readingProgress", "progress", "readProgress"};
    for (const char* name : kCandidates) {
        const cJSON* v = cJSON_GetObjectItem(root, name);
        if (cJSON_IsNumber(v)) {
            double p = v->valuedouble;
            if (p <= 1.0 && p > 0.0) p *= 100.0;  // 0-1 小数 → 百分比
            percent = static_cast<int>(p + 0.5);
            break;
        }
    }
    cJSON_Delete(root);
    if (percent < 0) err = "no progress field";
    return percent;
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
