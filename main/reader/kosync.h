#pragma once

#include <cstdint>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// KOReader kosync 客户端（听趣S1 Phase 3）
//
// - 协议参考 KOReader 官方 kosync.koplugin（api.json / KOSyncClient.lua）：
//   注册 POST /users/create（密码先 MD5）；鉴权自定义头 x-auth-user / x-auth-key
//   (=MD5(密码))，accept: application/vnd.koreader.v1+json；推 PUT /syncs/progress，
//   拉 GET /syncs/progress/{document}
// - 书的标识 = KOReader partialMD5（12 个采样点各 1024 字节：偏移 256/1024/4096/
//   16384/65536/262144/1048576/4194304/16777216/67108864/268435456/1073741824，
//   遇 EOF 截断），与 KOReader「文件内容」匹配方式逐字节一致
// - 进度映射：percentage(0-1) <-> progress_x10(0-1000)；progress 字段写
//   "para:<全局段落索引>"（对齐 Reeden book.json 语义，供精确同步演进）
// - 配置存 NVS namespace "kosync"：enabled / server / username / userkey(md5) /
//   auto_jump / device_id；密码不明文落盘
// - 所有网络函数：调用方须先确保网络就绪（Board::EnsureNetworkReady），
//   且必须在非 LVGL 任务的 DRAM 栈上下文执行（Application::Schedule 等）
// ---------------------------------------------------------------------------

namespace reader {
namespace kosync {

// ---------- 配置（NVS） ----------
bool IsEnabled();
void SetEnabled(bool enabled);
// 服务器地址，默认 https://sync.koreader.rocks
std::string Server();
bool SetServer(const std::string& server);
std::string Username();
bool SetUsername(const std::string& username);
// 设置明文密码（内部存 MD5；成功 true）。清空用户名/密码 = 清除凭据。
bool SetPassword(const std::string& plaintext);
void ClearCredentials();
bool HasCredentials();
// 拉取到更新的云端进度后是否自动跳转（同 Reeden「自动跳转当前书云端进度」）
bool AutoJump();
void SetAutoJump(bool enabled);
// 自动同步（开书拉取/存进度推送）。关闭后仅手动按钮生效（见 PushNow/PullNow）。
// 注意：自动同步按百分比定位，两端排版不同时偶发进度偏差。
bool AutoSync();
void SetAutoSync(bool enabled);
// 书籍匹配方式："content"=文件内容（默认；KOReader partialMD5，要求两端文件逐字节一致）
// / "filename"=按文件名（md5(文件名含扩展名)；适合 Reeden 导出等被重处理过的书，
// 两端文件名相同即可，KOReader 端也需切到按文件名匹配）
std::string MatchMode();
bool SetMatchMode(const std::string& mode);

// 本机设备标识（NVS 一次性生成），用于区分"进度来自本机"忽略回显
std::string DeviceId();

// ---------- 对阅读器的钩子（任意线程调用，内部转 Network 域，失败静默） ----------
void NotifyBookOpened(const std::string& book_path);                    // 触发拉取
void NotifyProgressSaved(const std::string& book_path, int pct_x10);    // 节流推送

// 手动推送当前进度（跳过节流；阅读界面「上传进度」按钮，任意线程调用）
void PushNow(const std::string& book_path, int pct_x10);
// 手动拉取云端进度（跳过小幅差值门槛；仍遵守仅前进防回退，结果经 StatusHandler 回执）
void PullNow(const std::string& book_path);

// 进度跳转回调：由 book_screen 注册（在 LVGL 线程执行）。
// book_path 为打开时的路径；执行方需自行校验该书仍在读（防过期跳转）。
using JumpHandler =
    std::function<void(const std::string& book_path, int pct_x10, const std::string& source_device)>;
void SetJumpHandler(JumpHandler handler);

// 手动同步结果回执（仅手动 force 操作触发；失败时由 book_screen 转 LVGL 弹提示）
using SyncStatusHandler = std::function<void(const char* message)>;
void SetStatusHandler(SyncStatusHandler handler);

// ---------- 设置页用的阻塞操作（由设置页在 Network 域任务里调用） ----------
// 校验凭据：GET /users/auth。成功 true；err 带失败原因。
bool AuthorizeNow(std::string& err);
// 注册新账号：POST /users/create；402 = 已存在（视为可用，转 authorize 校验）。
// 成功 true；err 带失败原因。
bool RegisterNow(std::string& err);

}  // namespace kosync
}  // namespace reader
