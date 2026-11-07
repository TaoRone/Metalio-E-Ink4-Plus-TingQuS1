# 听趣S1 · 项目进展存档

> 最后更新：2026-11-08 · KOReader kosync 同步已真机验证（推送/拉取/手动/跨设备互通）

## 已完成（真机验证 ✅）

- **Phase 0 环境/基线**：WSL2(Ubuntu 24.04, taorone) + ESP-IDF v6.1 + 代理(127.0.0.1:10808)；官方 v2.0.74 零修改构建刷机；原厂固件备份于 `backups/`（不入库）；4G 内置卡=联通物联卡(46011)通用流量
- **Phase 1 软裁剪**：Kconfig 开关 `TINGQU_HIDE_TASK_APP` / `TINGQU_HIDE_ASSISTANT_APP`（默认 y 隐藏，可回开）
- **微信读书 M1**：官方 Agent API（`i.weread.qq.com/api/agent/gateway`，Key 存 NVS weread）
  - 书架/进度/划线数/详情页/书架 SD 缓存(`/sdcard/metalio/e-ink/weread/shelf_cache.json`)
  - 设备键盘（大小写/退格/USEKEY 一键剪贴板 Key/PASTE1-3）+ 音量键翻页接管
- **微信读书封面 + 双模式书架（M1 增强）**：
  - 封面链：cover URL → SD 原始字节缓存（`/sdcard/metalio/e-ink/weread/covers/<book_id>.img`）
    → 解码 L8（≤200×268，PSRAM map）→ 网格/列表槽位；Network 域逐本取数（先 SD 后网），
    当前页优先，epoch 失效制（Create/卸载/新同步 +1），离屏中止，失败本会话不重试
  - 双模式：网格 3×3 大封面（默认，吃满屏高）/ 信息列表 7 行；顶栏切换，NVS `weread/view_mode`
    持久化；列表框高度动态 = LV_VER_RES - 顶栏 - 底栏（消灭底部空白）；行距 SPACE_EVENLY
  - 详情页左侧大封面（200×267）+ 右侧标题/元信息
  - 302 手动跟随：`res.weread.qq.com/wrepub/*_parsecover` 302 到签名 COS URL；响应 Location
    只能经 `HTTP_EVENT_ON_HEADER` 事件回调捕获（`esp_http_client_get_header` 只读请求头）
  - 封面源图上限 4MB（parsecover 全尺寸 PNG 可超 1MB；512K 会误杀）
- **KOReader kosync 同步（Phase 3，跨设备进度互通已验证）**：
  - 协议按 KOReader 官方 kosync.koplugin 实现：注册 POST /users/create、鉴权自定义头
    `x-auth-user`/`x-auth-key=MD5(密码)`、推 PUT /syncs/progress、拉 GET /syncs/progress/{digest}；
    默认官方服 sync.koreader.rocks，预置用户自建服 koreader.taor.site（可选，不推荐）
  - 书的 digest：文件内容=KOReader partialMD5（12 采样点各 1024B：偏移 256/1024/4096/…/1GB，
    遇 EOF 截断）；文件名=md5(文件名含扩展名)。**Reeden 导出会改文件字节（字体精简等），
    内容匹配必失效，须用文件名匹配且 KOReader 端也切按文件名**
  - 进度映射：percentage(0-1)↔progress_x10(0-1000)；TXT 按字节偏移/文件大小（跨设备准），
    EPUB 按全书页表（跨渲染引擎只能到"同章附近"）
  - 推拉策略：开书拉/存盘推（节流 0.3%/180s，force 手动跳过）；**仅前进防回退**（云端记录
    常被另一端落后进度覆盖，回退跳转全是误伤）；本机回显（device_id）忽略
  - UI：首页「KOReader 同步」卡片（7 行设置：启用/服务器三选浮层/用户名/密码/自动跳转/
    匹配方式/自动同步）；全字符键盘（ABC+SYM 两页、SHIFT、PASTE1-3 局域网剪贴板填入）；
    阅读时长页（设置→第4页）手动「上传进度/拉取进度」，失败回执上屏（网络/凭据/无记录）
  - 断网手动同步会主动起网（EnsureNetworkReady 30s 等 4G 附着），有网自然刷新保网窗口
- **局域网剪贴板**：首页卡片，:8080 服务，网页三槽位 + 一键设 Key（NVS clipboard）；槽位
  已加 RAM 缓存（见下方 Flash 栈铁律）
- **官网 V2**：https://readere4p.taor.site （静态站 + MySQL 反馈 + 教程页 + 路线图）
- **关键修复**：esp-tls 需 SetRequestDeadline；cJSON 大 JSON 用 PSRAM 钩子(ParseShelfBody 等)；
  联网页 PowerNeed 保网(ForegroundNetwork/OtaDownload)；`MBEDTLS_SSL_RENEGOTIATION` 必须关
  （esp_ssl.c 注释警告）；对公网 HTTPS 用 SDK esp_http_client（板载 HttpClient/EspSsl 栈会写挂起）；
  ota 分区扩至 0x510000×2，font_data 0x4C2000

## 待办（按优先级）

1. **需求池**：
   - EPUB 字级进度同步：解析 kosync `progress` 字段的 KOReader xpointer（/body/DocFragment[n]/…），
     Reeden 已实现同类方案（changelog 2026-10-22 "EPUB 精确到字"）；工作量较大单列
   - 语音读书/音频播放（蓝牙链路现成，待 TTS 选型）
2. **M2 在线阅读**：微信读书墨水屏非官方接口（weread-omni 参考），实验功能默认关
3. Reeden 侧：用户已关文件加密；需"清空云端"重传去旧密文

## 技术备忘

- 构建：`idf.py -B ~/build/tingqu-s1 build`（构建目录必须纯 ASCII；工程路径中文触发 picolibc specs bug）
- 合并：`BUILD_DIR=~/build/tingqu-s1 ESPTOOL_CHIP=esp32s31 bash merge_firmware.sh`
- 刷机（整机）：`python -m esptool --chip esp32s31 --port COMx --baud 921600 write-flash --compress 0x0 firmware/metalio-e-ink4-2.0.74.bin`（wipes NVS，WiFi/Key 需重配）
- 刷机（仅 app，**保 NVS**，迭代调试首选）：先 `cp "//wsl$/Ubuntu-24.04/home/taorone/build/tingqu-s1/Metalio-E-Ink4-Plus.bin" /tmp/app.bin`，
  再 `py -m esptool --chip esp32s31 --port COM5 --baud 921600 write-flash --compress 0x80000 <app.bin>`（esptool 不认 \\wsl$ 路径须先拷贝；ota_data 指向 ota_0 直写即生效）
- **Flash 栈 sanity 铁律**（2026-11 踩雷实录）：SDK 在关 Flash 缓存前会断言当前任务栈
  （`esp_task_stack_is_sane_cache_disabled`，cache_utils.c:114）。**PSRAM 栈任务
  （book_open worker 16KB SPIRAM 等）与薄栈任务（touch_feed 原 4KB）禁止直碰 NVS/Flash**——
  冷缓存读会触发 Flash 操作直接断言重启。规则：①reader 层对外钩子体内零 NVS（检查下沉
  Network 域任务）②剪贴板等高频读取走 RAM 缓存 ③NVS 命名空间在屏幕 Loaded 时预热
  ④touch_feed 栈已扩 8KB。kosync 崩溃三连（Store access fault ×3）根因即 kosync::Notify*
  在 book_open worker 里读 NVS
- 常用日志 Tag：WereadSvc / WereadScreen / KosyncScreen / **KOSync** / Lansync / PowerPolicy / UartEthModem
- 屏幕生命周期：页面对象归系统导航栈管理，**不要二次 delete**（双重释放→LVGL 堆损坏→再进卡死）
- LVGL 9.5 无 `LV_FLEX_FLOW_NONE`（flow=0 是 ROW）；关 flex 用 `lv_obj_set_style_layout(obj, LV_LAYOUT_NONE, 0)` 再绝对定位
- 墨水屏封面显示：`EpdContentRole::Shelf`（保灰阶不压暗，同本地书架）；云列表封面用 `Monochrome`+Bayer 二值化
- IDF 6.1 = mbedTLS 4.x，`mbedtls/md5.h` 已移除；MD5 用 `esp_rom_md5.h`（`md5_context_t`，无需组件依赖）
- kosync 响应 Location 只能经 `HTTP_EVENT_ON_HEADER` 回调拿（`esp_http_client_get_header` 只读请求头）
- 本机串口：设备=USB VID_303A（COM5，ESP32-S3 原生 USB）；COM3/COM4 是蓝牙虚拟口勿用；Windows 侧用 `py -m esptool`（python 命令无 esptool）
- AMH WAF 拦中文 POST 体：前端已做 \uXXXX 转义；MySQL 走 unix_socket(/tmp/mysql-generic-8.0.sock, skip-networking)
- 官网后台：/admin/feedback.php（admin / TqS1-9kM7-xJ3q，建议更换）
- Reeden 格式：book.json(read_progress=全局段落索引 int) + books/<id>/book_config.json(navigationHistories[0]={sectionIndex,paragraphIndex,elementIndex,timestamp})
