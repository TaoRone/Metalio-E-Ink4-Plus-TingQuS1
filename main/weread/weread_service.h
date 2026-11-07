#pragma once

#include <cstddef>
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

// 同上，同时返回原始响应体（用于 SD 缓存）
bool FetchShelf(std::vector<ShelfBook>& out, std::string& raw_body, std::string& err);

// 解析 /shelf/sync 响应体（缓存恢复用）
bool ParseShelfBody(const std::string& body, std::vector<ShelfBook>& out, std::string& err);

// 单本书阅读进度（/book/getprogress），返回阅读进度百分比 0-100；失败返回 -1
int FetchProgressPercent(const std::string& book_id, std::string& err);

// 单本书阅读时长（/book/getprogress 的 book.readingTime，秒）；失败/无记录返回 -1
int64_t FetchReadingTimeSec(const std::string& book_id, std::string& err);

// 单本书划线/书签数量（/book/bookmarklist）
bool FetchBookmarkCount(const std::string& book_id, int& count, std::string& err);

// 书架 SD 缓存：同步成功后保存原始 JSON，进页先展示缓存再后台刷新
bool SaveShelfCacheBody(const std::string& body);
bool LoadShelfCacheBody(std::string& body);

// 阅读统计（/readdata/detail）
bool FetchReadStats(ReadStats& out, std::string& err);

// ---------------------------------------------------------------------------
// 封面缩略图源数据（书架列表 80x112 槽位用）
// - FetchCoverBytes：公网图片 GET。SDK esp_http_client 可靠模式（crt_bundle /
//   PSRAM 缓冲 / 读截止 / 4MB 上限），同 GatewayCall 不用板载 HttpClient。
//   302 手动跟随：响应 Location 只能经 HTTP_EVENT_ON_HEADER 事件回调捕获
//   （esp_http_client_get_header 只读请求头）。调用方须先确保网络就绪
//   （Board::EnsureNetworkReady），可在网络任务域执行
// - SD 原始字节缓存 covers/<book_id>.img：保留原图分辨率，供本次解码与
//   将来详情页大封面复用；book_id 非法字符转 '_'，无法成文件名时返回 ""
// ---------------------------------------------------------------------------
bool FetchCoverBytes(const std::string& url, std::vector<uint8_t>& out, std::string& err);
std::string CoverCachePath(const std::string& book_id);
bool SaveCoverCache(const std::string& book_id, const uint8_t* data, size_t len);
bool LoadCoverCache(const std::string& book_id, std::vector<uint8_t>& out);

// 书架浏览模式（NVS weread/view_mode，持久化用户上次选择）
bool PreferGridView();
bool SetPreferGridView(bool grid);

}  // namespace weread
