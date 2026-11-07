<?php
// 听趣S1 官网 · 意见反馈接口（SQLite，数据文件存于 webroot 之外）
header('Content-Type: application/json; charset=utf-8');

const DB_PATH = '/home/wwwroot/lnmp01/tingqu-data/feedback.db';
const RATE_LIMIT = 5;          // 每窗口最多提交次数
const RATE_WINDOW_SEC = 600;   // 限流窗口（10 分钟）

function fail(string $msg, int $code = 400): void {
    http_response_code($code);
    echo json_encode(['ok' => false, 'error' => $msg]);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') fail('method not allowed', 405);

$raw = file_get_contents('php://input');
$in = json_decode($raw, true);
if (!is_array($in)) fail('bad request');

$nickname = trim((string)($in['nickname'] ?? ''));
$contact  = trim((string)($in['contact'] ?? ''));
$content  = trim((string)($in['content'] ?? ''));

if ($content === '') fail('内容不能为空');
if (mb_strlen($content) > 2000) fail('内容过长');
if (mb_strlen($nickname) > 32 || mb_strlen($contact) > 64) fail('字段过长');

// 简单人机校验：内容不得为纯空白/单字符
if (mb_strlen($content) < 2) fail('内容过短');

$ip = $_SERVER['REMOTE_ADDR'] ?? '0.0.0.0';
$ua = substr((string)($_SERVER['HTTP_USER_AGENT'] ?? ''), 0, 200);

if (!is_dir(dirname(DB_PATH))) {
    mkdir(dirname(DB_PATH), 0750, true);
}

$db = new PDO('sqlite:' . DB_PATH);
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    ip TEXT NOT NULL,
    ua TEXT,
    nickname TEXT,
    contact TEXT,
    content TEXT NOT NULL
)');
$db->exec('CREATE INDEX IF NOT EXISTS idx_feedback_ts ON feedback(ts)');

// 限流：同 IP 窗口内计数
$stmt = $db->prepare('SELECT COUNT(*) FROM feedback WHERE ip = ? AND ts > ?');
$stmt->execute([$ip, time() - RATE_WINDOW_SEC]);
if ((int)$stmt->fetchColumn() >= RATE_LIMIT) {
    fail('提交太频繁，请 10 分钟后再试', 429);
}

$stmt = $db->prepare('INSERT INTO feedback (ts, ip, ua, nickname, contact, content) VALUES (?,?,?,?,?,?)');
$stmt->execute([time(), $ip, $ua, $nickname, $contact, $content]);

echo json_encode(['ok' => true]);
