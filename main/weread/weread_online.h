#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 微信读书在线阅读（听趣S1 M2，实验功能）
//
// - 章节目录 POST /book/chapterInfos；正文候选端点按序探测（首个真实账号验证时
//   日志输出各端点状态）：
//     1) GET  /book/chapterContent?bookId=&chapterUid=
//     2) POST /book/chapterContent {bookId, chapterUid}
//     3) GET  /book/content?bookId=&chapterUid=
// - 正文 HTML 经 reader::HtmlBlockParser 转纯文本段落（图片暂跳过，MVP 文本-only）
// - 按 tools/ebook/SPEC.md v1 合成 .ebook（zlib 分块、索引可 fseek、extra.book_id）
//   写入书库目录（/sdcard/metalio/e-ink/books/），复用现有阅读器打开
// - 所有网络函数仅 Network 域执行；UI 事件经 SetUiHandler 上屏
// ---------------------------------------------------------------------------

namespace weread {
namespace online {

struct ChapterInfo {
    int uid = 0;
    std::string title;
};

bool FetchChapterInfos(const std::string& book_id, std::vector<ChapterInfo>& out, std::string& err);
bool FetchChapterParagraphs(const std::string& book_id, int chapter_uid,
                            std::vector<std::string>& paragraphs, std::string& err);

// ---------------- .ebook 合成器（SPEC v1，内存构建 + 原子写盘） ----------------
class EbookWriter {
public:
    bool Begin(const std::string& title, const std::string& author, const std::string& book_id);
    // 段落即 HtmlBlockParser 产出的文本块；内部做前导空白剥离/0x1E 拼接/分块压缩
    bool AddChapter(const std::string& title, const std::vector<std::string>& paragraphs);
    // 合成并原子写入 path；成功返回 true
    bool FinishAndWrite(const char* path, std::string& err);

private:
    struct Chapter {
        std::string title;
        std::vector<std::string> paragraphs;
    };
    std::string title_;
    std::string author_;
    std::string book_id_;
    std::vector<Chapter> chapters_;
};

// ---------------- 一键在线阅读（目录+正文+合成+打开） ----------------
struct UiEvent {
    enum class Type : uint8_t {
        kProgress = 0,  // message=状态文本
        kDone,          // message=.ebook 绝对路径（阅读器据此打开）
        kError,         // message=原因
    };
    Type type;
    std::string message;
};
using UiHandler = std::function<void(const UiEvent&)>;
void SetUiHandler(UiHandler handler);

// 启动在线阅读全流程（幂等）。title/author 用于 .ebook 元数据与文件名。
bool StartOnlineRead(const std::string& book_id, const std::string& title,
                     const std::string& author);
bool IsBusy();

}  // namespace online
}  // namespace weread
