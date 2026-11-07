#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 微信读书官方 Agent API 服务层（听趣S1 · M1）
//
// - 网关：https://i.weread.qq.com/api/agent/gateway（HTTPS POST JSON，
//   业务参数与 api_name / skill_version 平铺，Bearer API Key 认证）
// - API Key 存 NVS namespace "weread" / key "api_key"（Settings "weread"）
// - 所有网络函数：调用方必须先确保网络就绪（Board::EnsureNetworkReady），
//   且必须在非 LVGL 任务的 DRAM 栈上下文执行（Application::Schedule 等）
// ---------------------------------------------------------------------------

namespace weread {

// 书架条目（/shelf/sync 的精简字段）
struct ShelfBook {
    std::string book_id;
    std::string title;
    std::string author;
    std::string cover;        // https://wfqqreader-...myqcloud.com 封面 URL（可能为空）
    std::string category;
    int finish_reading = 0;   // 0=在读/未读 1=读完
    int64_t read_update_time = 0;  // 最近阅读时间（秒）
};

// 阅读统计（/readdata/detail，mode=weekly/monthly）
struct ReadStats {
    int reading_days = 0;
    int total_reading_time_min = 0;  // 阅读时长（分钟）
};

bool HasApiKey();
// 校验并保存 Key（ wrk- 前缀，长度 >= 20）；失败返回 false
bool SetApiKey(const std::string& key);
void ClearApiKey();

// 通用网关调用。params_json 为平铺参数片段（可为空），例如 "\"count\":5"。
// 成功返回 true 且 out_body 为响应体；失败 out_err 带原因。
bool GatewayCall(const std::string& api_name, const std::string& params_json,
                 std::string& out_body, std::string& out_err);

// 全量书架（官方接口当前一次返回全部；含读完标记与最近阅读时间）
bool FetchShelf(std::vector<ShelfBook>& out, std::string& err);

// 单本书阅读进度（/book/getprogress），返回阅读进度百分比 0-100；失败返回 -1
int FetchProgressPercent(const std::string& book_id, std::string& err);

// 阅读统计（/readdata/detail）
bool FetchReadStats(ReadStats& out, std::string& err);

}  // namespace weread
