#include "weread_online.h"

#include "weread_ink.h"

#include <application.h>
#include <board.h>
#include <power_policy.h>
#include <reader/html_content.h>
#include <reader/image_util.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <zlib.h>
#include <cJSON.h>

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace weread {
namespace online {

static const char* kTag = "WereadOnline";
static const char* kBooksDir = "/sdcard/metalio/e-ink/books";

// ---------------------------------------------------------------------------
// 章节目录
// ---------------------------------------------------------------------------

bool FetchChapterInfos(const std::string& book_id, std::vector<ChapterInfo>& out, std::string& err) {
    out.clear();
    err.clear();
    const std::string body = "{\"bookIds\":[\"" + book_id +
                             "\"],\"synckeys\":[0],\"updateTimes\":[0],\"maxfreeIdx\":[0]}";
    std::string resp;
    if (!ink::InkPost("/book/chapterInfos", body, resp, err)) {
        return false;
    }
    // weread-omni 约定：data[].updated[] 为章节数组；字段防御性解析
    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        err = "chapterInfos bad json";
        return false;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    bool ok = false;
    if (cJSON_IsArray(data)) {
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, data) {
            const cJSON* updated = cJSON_GetObjectItem(entry, "updated");
            if (!cJSON_IsArray(updated)) continue;
            const cJSON* ch = nullptr;
            cJSON_ArrayForEach(ch, updated) {
                const cJSON* uid = cJSON_GetObjectItem(ch, "chapterUid");
                const cJSON* title = cJSON_GetObjectItem(ch, "title");
                if (!cJSON_IsNumber(uid)) continue;
                ChapterInfo info;
                info.uid = static_cast<int>(uid->valuedouble);
                info.title = cJSON_IsString(title) ? title->valuestring : "";
                out.push_back(std::move(info));
            }
            ok = true;  // 取首个 entry（单 bookIds 查询）
            break;
        }
    }
    cJSON_Delete(root);
    if (!ok || out.empty()) {
        err = "chapterInfos 无章节数据";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 章节正文（端点探测 + HTML 转段落）
// ---------------------------------------------------------------------------

namespace {

// 探测成功的端点模式缓存（0=未探测 1=GET chapterContent 2=POST 3=GET content）
int s_content_probe = 0;

// 从响应中提取正文 HTML：优先 content/htmlContent/data 字符串字段；整体像 HTML 也行
bool ExtractHtml(const std::string& body, std::string& html) {
    const std::string trimmed = body;
    if (trimmed.find('<') != std::string::npos) {
        // 找第一个 '<'：可能是裸 HTML 或 JSON 内嵌
        size_t pos = trimmed.find("\"content\"");
        if (pos == std::string::npos) pos = trimmed.find("\"htmlContent\"");
        if (pos == std::string::npos) {
            if (trimmed.find_first_not_of(" \t\r\n") == trimmed.find('<')) {
                html = trimmed;  // 裸 HTML
                return true;
            }
            return false;
        }
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;
    const char* keys[] = {"content", "htmlContent", "data", "html"};
    bool ok = false;
    for (const char* key : keys) {
        const cJSON* v = cJSON_GetObjectItem(root, key);
        if (cJSON_IsString(v) && v->valuestring[0] != '\0') {
            html = v->valuestring;
            ok = true;
            break;
        }
        if (cJSON_IsArray(v)) {
            // data: [{content: "..."}] 形态
            const cJSON* item = cJSON_GetArrayItem(v, 0);
            if (item != nullptr) {
                const cJSON* c = cJSON_GetObjectItem(item, "content");
                if (cJSON_IsString(c) && c->valuestring[0] != '\0') {
                    html = c->valuestring;
                    ok = true;
                    break;
                }
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}

}  // namespace

bool FetchChapterParagraphs(const std::string& book_id, int chapter_uid,
                            std::vector<std::string>& paragraphs, std::string& err) {
    paragraphs.clear();
    err.clear();

    std::string html;
    bool got = false;

    // 探测顺序按 s_content_probe 记忆，命中后不再换
    const std::string qu = std::to_string(chapter_uid);
    auto try_get = [&](const std::string& path) -> bool {
        std::string resp;
        std::string e;
        ESP_LOGI(kTag, "probe GET %s", path.c_str());
        if (!ink::InkGet(path, resp, e)) {
            ESP_LOGW(kTag, "probe GET fail: %s", e.c_str());
            return false;
        }
        ESP_LOGI(kTag, "probe GET ok, %u bytes", static_cast<unsigned>(resp.size()));
        return ExtractHtml(resp, html);
    };
    auto try_post = [&](const std::string& path, const std::string& json) -> bool {
        std::string resp;
        std::string e;
        ESP_LOGI(kTag, "probe POST %s", path.c_str());
        if (!ink::InkPost(path, json, resp, e)) {
            ESP_LOGW(kTag, "probe POST fail: %s", e.c_str());
            return false;
        }
        ESP_LOGI(kTag, "probe POST ok, %u bytes", static_cast<unsigned>(resp.size()));
        return ExtractHtml(resp, html);
    };

    if (s_content_probe == 1 || s_content_probe == 0) {
        got = try_get("/book/chapterContent?bookId=" + book_id + "&chapterUid=" + qu);
        if (got) s_content_probe = 1;
    }
    if (!got && (s_content_probe == 2 || s_content_probe == 0)) {
        got = try_post("/book/chapterContent",
                       "{\"bookId\":\"" + book_id + "\",\"chapterUid\":" + qu + "}");
        if (got) s_content_probe = 2;
    }
    if (!got && (s_content_probe == 3 || s_content_probe == 0)) {
        got = try_get("/book/content?bookId=" + book_id + "&chapterUid=" + qu);
        if (got) s_content_probe = 3;
    }
    if (!got) {
        err = "正文端点不可用（详见 WereadOnline 探测日志）";
        return false;
    }

    // HTML → 段落（图片块跳过并计数）
    struct Sink {
        std::vector<std::string>* paras;
        int images = 0;
    } sink{&paragraphs, 0};
    reader::HtmlBlockParser parser("", [&sink](reader::ContentBlock&& block) {
        if (block.kind == reader::ContentKind::kText) {
            if (!block.text.empty()) sink.paras->push_back(std::move(block.text));
        } else {
            ++sink.images;
        }
        return true;
    });
    bool ok = parser.Feed(reinterpret_cast<const uint8_t*>(html.data()), html.size());
    ok = parser.Finish() && ok;
    if (!ok) {
        err = "正文 HTML 解析失败";
        return false;
    }
    if (sink.images > 0) {
        ESP_LOGI(kTag, "chapter %d: %d 图片跳过（MVP 文本-only）", chapter_uid, sink.images);
    }
    if (paragraphs.empty()) {
        err = "本章无文本内容";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// .ebook 合成（SPEC v1）
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kChunkMax = 4096;
constexpr uint16_t kFontPx = 25;

void PutU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void PutBytes(std::vector<uint8_t>& v, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    v.insert(v.end(), p, p + len);
}

// 段前导空白剥离（SPEC：转换时剥掉段首空格，含 U+3000 全角空格）
std::string StripLeadingWs(std::string s) {
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (static_cast<uint8_t>(c) == 0xEF && i + 2 < s.size() &&
            static_cast<uint8_t>(s[i + 1]) == 0xBC &&
            static_cast<uint8_t>(s[i + 2]) == 0x80) {
            i += 3;  // U+3000
            continue;
        }
        break;
    }
    return s.substr(i);
}

// UTF-8 安全截断：从 max 处向前找非 continuation 字节；保底前进 1 字节防死循环
size_t Utf8SafeCut(const std::vector<uint8_t>& data, size_t max, size_t min_pos) {
    if (max >= data.size()) return data.size();
    size_t cut = max;
    while (cut > min_pos && (data[cut] & 0xC0) == 0x80) --cut;
    if (cut == min_pos) return max > min_pos ? max : min_pos + 1;
    return cut;
}

}  // namespace

bool EbookWriter::Begin(const std::string& title, const std::string& author,
                        const std::string& book_id) {
    title_ = title;
    author_ = author;
    book_id_ = book_id;
    chapters_.clear();
    return !title_.empty() && !book_id_.empty();
}

bool EbookWriter::AddChapter(const std::string& title, const std::vector<std::string>& paragraphs) {
    if (paragraphs.empty()) return false;  // SPEC：不写空章
    Chapter ch;
    ch.title = title.empty() ? " " : title;
    ch.paragraphs = paragraphs;
    for (auto& p : ch.paragraphs) p = StripLeadingWs(std::move(p));
    chapters_.push_back(std::move(ch));
    return true;
}

bool EbookWriter::FinishAndWrite(const char* path, std::string& err) {
    err.clear();
    if (chapters_.empty()) {
        err = "无章节";
        return false;
    }

    // 1) 各章文本 → 0x1E 段落串 → ≤4KB zlib 块
    struct Chunk {
        std::vector<uint8_t> comp;
        uint16_t raw = 0;
    };
    struct ChOut {
        std::string title;
        std::vector<Chunk> chunks;
        uint32_t uncomp_total = 0;
    };
    std::vector<ChOut> outs;
    outs.reserve(chapters_.size());
    for (const Chapter& ch : chapters_) {
        ChOut co;
        co.title = ch.title;
        std::vector<uint8_t> text;
        for (const std::string& p : ch.paragraphs) {
            text.insert(text.end(), p.begin(), p.end());
            text.push_back(0x1E);
        }
        co.uncomp_total = static_cast<uint32_t>(text.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t limit = std::min(text.size(), pos + static_cast<size_t>(kChunkMax));
            const size_t cut = Utf8SafeCut(text, limit, pos);
            const size_t n = std::max(cut, pos + 1) - pos;
            Chunk chunk;
            chunk.raw = static_cast<uint16_t>(n);
            // 不压缩（SPEC v1 允许 bflags=0 未压缩块）：app 分区已贴近 0x510000 上限，
            // 链 deflate 会超容；SD 充裕、下载大头在网络不在磁盘
            chunk.comp.assign(text.begin() + static_cast<long>(pos),
                              text.begin() + static_cast<long>(pos + n));
            co.chunks.push_back(std::move(chunk));
            pos += n;
        }
        outs.push_back(std::move(co));
    }

    // 2) 布局：Header(64) + Meta + Index(32N) + TitleBlob + Data
    // Meta
    std::vector<uint8_t> meta;
    const std::string extra = "{\"book_id\":\"" + book_id_ + "\",\"source\":\"weread-online\"}";
    auto put_str = [&](const std::string& s) {
        PutU16(meta, static_cast<uint16_t>(s.size()));
        PutBytes(meta, s.data(), s.size());
    };
    put_str(title_);
    put_str(author_);
    put_str("zh");
    put_str(extra);
    // TitleBlob
    std::vector<uint8_t> titles;
    std::vector<uint32_t> title_offsets;
    for (const ChOut& co : outs) {
        title_offsets.push_back(static_cast<uint32_t>(titles.size()));
        PutBytes(titles, co.title.data(), co.title.size());
    }
    const uint32_t header_size = 64;
    const uint32_t meta_offset = header_size;
    const uint32_t index_offset = meta_offset + static_cast<uint32_t>(meta.size());
    const uint32_t index_size = static_cast<uint32_t>(outs.size()) * 32;
    const uint32_t title_blob_offset = index_offset + index_size;
    const uint32_t title_blob_size = static_cast<uint32_t>(titles.size());
    const uint32_t data_offset = title_blob_offset + title_blob_size;

    // Data 区 + Index 填充
    std::vector<uint8_t> file;
    file.reserve(data_offset);
    std::vector<uint8_t> index;
    index.reserve(index_size);
    std::vector<uint8_t> data;
    uint32_t cursor = data_offset;
    for (size_t i = 0; i < outs.size(); ++i) {
        const ChOut& co = outs[i];
        const uint32_t ch_data_offset = cursor;
        uint32_t ch_len = 0;
        for (const Chunk& chunk : co.chunks) {
            std::vector<uint8_t> block;
            const bool compressed = chunk.comp.size() != chunk.raw;
            block.reserve(8 + chunk.comp.size());
            block.push_back(0x01);  // TEXT
            block.push_back(compressed ? 0x01 : 0x00);
            PutU16(block, static_cast<uint16_t>(chunk.comp.size()));
            PutU16(block, chunk.raw);
            PutU16(block, 0);
            PutBytes(block, chunk.comp.data(), chunk.comp.size());
            data.insert(data.end(), block.begin(), block.end());
            ch_len += static_cast<uint32_t>(block.size());
            cursor += static_cast<uint32_t>(block.size());
        }
        PutU32(index, ch_data_offset);
        PutU32(index, ch_len);
        PutU32(index, co.uncomp_total);
        PutU16(index, static_cast<uint16_t>(co.chunks.size()));
        PutU16(index, static_cast<uint16_t>(co.title.size()));
        PutU32(index, title_blob_offset + title_offsets[i]);
        PutU32(index, 0);                          // flags：无图
        index.insert(index.end(), 8, 0);           // reserved
    }

    // Header（crc32 最后回填）
    std::vector<uint8_t> header(64, 0);
    auto set_u16 = [&](int off, uint16_t v) {
        header[off] = static_cast<uint8_t>(v & 0xFF);
        header[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    auto set_u32 = [&](int off, uint32_t v) {
        for (int i = 0; i < 4; ++i) header[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    };
    std::memcpy(header.data(), "EBOK", 4);
    set_u16(4, 1);            // version
    set_u16(6, 0x000C);       // flags = ZLIB(4) | HAS_TOC(8)
    set_u32(8, static_cast<uint32_t>(outs.size()));
    set_u16(12, kFontPx);
    set_u16(14, static_cast<uint16_t>(kChunkMax));
    set_u32(16, meta_offset);
    set_u32(20, static_cast<uint32_t>(meta.size()));
    set_u32(24, index_offset);
    set_u32(28, index_size);
    set_u32(32, title_blob_offset);
    set_u32(36, title_blob_size);
    set_u32(40, data_offset);
    set_u32(44, static_cast<uint32_t>(data.size()));
    set_u32(48, 0);           // cover_offset
    set_u32(52, 0);           // cover_size
    const uLong crc = crc32(0L, header.data(), 56);
    set_u32(56, static_cast<uint32_t>(crc));

    // 3) 组装 + 原子写盘
    file.insert(file.end(), header.begin(), header.end());
    file.insert(file.end(), meta.begin(), meta.end());
    file.insert(file.end(), index.begin(), index.end());
    file.insert(file.end(), titles.begin(), titles.end());
    file.insert(file.end(), data.begin(), data.end());

    if (!reader::WriteFileAtomic(path, file.data(), file.size())) {
        err = std::string("写盘失败 errno=") + std::to_string(errno);
        return false;
    }
    ESP_LOGI(kTag, "ebook written %s (%u bytes, %u chapters)", path,
             static_cast<unsigned>(file.size()), static_cast<unsigned>(outs.size()));
    return true;
}

// ---------------------------------------------------------------------------
// 一键在线阅读
// ---------------------------------------------------------------------------

namespace {

UiHandler g_ui;
std::mutex g_ui_mu;
std::atomic<bool> s_busy{false};

void Emit(UiEvent::Type type, const std::string& message) {
    UiHandler fn;
    {
        std::lock_guard<std::mutex> lock(g_ui_mu);
        fn = g_ui;
    }
    if (fn) {
        UiEvent ev;
        ev.type = type;
        ev.message = message;
        fn(ev);
    }
}

std::string SafeFileStem(const std::string& title) {
    std::string s;
    for (char c : title) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                        c == '(' || c == ')' || c == '[' || c == ']' || c == ' ' ||
                        static_cast<unsigned char>(c) >= 0x80;  // 保留中文
        s.push_back(ok ? c : '_');
    }
    if (s.empty() || s == "." || s == "..") s = "weread_book";
    if (s.size() > 80) s.resize(80);
    return s;
}

void OnlineReadJob(std::string book_id, std::string title, std::string author) {
    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    bool ok = false;
    std::string result_path;
    do {
        if (!Board::GetInstance().EnsureNetworkReady(30000)) {
            Emit(UiEvent::Type::kError, "网络未就绪");
            break;
        }
        Emit(UiEvent::Type::kProgress, "拉取章节目录…");
        std::vector<ChapterInfo> chapters;
        std::string err;
        if (!FetchChapterInfos(book_id, chapters, err)) {
            Emit(UiEvent::Type::kError, "目录拉取失败: " + err);
            break;
        }
        Emit(UiEvent::Type::kProgress, "共 " + std::to_string(chapters.size()) + " 章，开始下载正文…");

        EbookWriter writer;
        writer.Begin(title, author, book_id);
        int fetched = 0;
        int skipped = 0;
        for (const ChapterInfo& ch : chapters) {
            if ((fetched + skipped) % 10 == 0) {
                Emit(UiEvent::Type::kProgress,
                     "下载正文 " + std::to_string(fetched + skipped) + "/" +
                         std::to_string(chapters.size()) + " 章…");
            }
            std::vector<std::string> paras;
            if (FetchChapterParagraphs(book_id, ch.uid, paras, err)) {
                if (!writer.AddChapter(ch.title, paras)) ++skipped;
                else ++fetched;
            } else {
                ESP_LOGW(kTag, "chapter %d fail: %s（跳过）", ch.uid, err.c_str());
                ++skipped;
            }
            vTaskDelay(pdMS_TO_TICKS(30));  //  gentle：避免打满网络任务
        }
        if (fetched == 0) {
            Emit(UiEvent::Type::kError, "正文拉取全部失败（详见日志 WereadOnline）");
            break;
        }
        if (skipped > 0) {
            ESP_LOGW(kTag, "%d 章未取到（文本-only/接口限制），已跳过", skipped);
        }

        if (mkdir(kBooksDir, 0755) != 0 && errno != EEXIST) {
            Emit(UiEvent::Type::kError, "书库目录不可用");
            break;
        }
        const std::string path = std::string(kBooksDir) + "/" + SafeFileStem(title) + "[微信读书].ebook";
        Emit(UiEvent::Type::kProgress, "合成电子书…");
        if (!writer.FinishAndWrite(path.c_str(), err)) {
            Emit(UiEvent::Type::kError, "合成失败: " + err);
            break;
        }
        result_path = path;
        ok = true;
    } while (false);
    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
    s_busy.store(false);
    if (ok) Emit(UiEvent::Type::kDone, result_path);
}

}  // namespace

void SetUiHandler(UiHandler handler) {
    std::lock_guard<std::mutex> lock(g_ui_mu);
    g_ui = std::move(handler);
}

bool StartOnlineRead(const std::string& book_id, const std::string& title,
                     const std::string& author) {
    if (book_id.empty() || s_busy.exchange(true)) return false;
    if (!Application::GetInstance().Schedule(
            [book_id, title, author]() { OnlineReadJob(book_id, title, author); },
            Application::TaskDomain::Network)) {
        s_busy.store(false);
        return false;
    }
    return true;
}

bool IsBusy() { return s_busy.load(); }

}  // namespace online
}  // namespace weread
