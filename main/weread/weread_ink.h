#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 微信读书墨水屏登录/认证（听趣S1 M2 在线阅读）
//
// - 协议逆向自 weread-omni（teng-lin/weread-omni）eink provider：
//   QR 流程 GET /wxticket?nonceStr=weread → 拼 open.weixin.qq.com qrconnect URL
//   → 轮询 long.open.weixin.qq.com/connect/l/qrconnect（408 等待/404 已扫/405 确认）
//   → POST /login 换 {vid, accessToken, refreshToken}
// - 设备身份伪装 BOOX 墨水屏（deviceType=3；UA/版本头见 .cc）
// - 签名：sha256_hex(timestamp + deviceId + random)（ROM ets_sha，SHA2_256）
// - 凭证存 NVS namespace "weread"（ink_vid/ink_access/ink_refresh/ink_device）
// - 所有网络函数必须在 Network 域任务上下文执行（调用方 EnsureNetworkReady）
// ---------------------------------------------------------------------------

namespace weread {
namespace ink {

bool HasCredentials();
void ClearCredentials();
std::string Vid();

// 登录事件（回调发生在 Network 任务上下文；屏幕须自行转 LVGL 线程）
struct LoginEvent {
    enum class Type : uint8_t {
        kQrUrl = 0,  // url = 二维码内容（confirmUrl）
        kScanned,    // 用户已扫码，等待手机端确认
        kSuccess,    // 登录完成，凭证已落 NVS
        kError,      // message = 失败原因（超时/拒绝/网络）
    };
    Type type;
    std::string url;
    std::string message;
};
using LoginEventHandler = std::function<void(const LoginEvent&)>;
void SetLoginEventHandler(LoginEventHandler handler);

// 启动 QR 登录（幂等：进行中返回 false）。内部转 Network 域。
bool StartQrLogin();
void CancelQrLogin();
bool IsLoginBusy();

// 认证 HTTP（仅 Network 域调用；accessToken 失效自动刷新并重试一次）
// path 以 '/' 开头（i.weread.qq.com）；body 为响应体。
bool InkGet(const std::string& path, std::string& body, std::string& err);
bool InkPost(const std::string& path, const std::string& json_body, std::string& body,
             std::string& err);

// ---------------- AI 大纲（官方 /web/book/briefOutline，网页 Cookie 鉴权） ----------------
// 网页 Cookie（wr_vid=...; wr_skey=...）存 NVS weread/web_cookie；可由 QR 屏一键粘贴剪贴板
bool HasWebCookie();
bool SetWebCookie(const std::string& cookie);
// 拉取 AI 精读大纲：summary + 主题问答列表（markdown 文本，Network 域调用）
struct BriefQA {
    std::string question;
    std::string answer_md;
};
bool FetchBriefOutline(const std::string& book_id, std::string& summary,
                       std::vector<BriefQA>& qa, std::string& err);

}  // namespace ink
}  // namespace weread
