<?php
// 听趣S1 官网 · 反馈查看后台（HTTP Basic 认证，MySQL）
require __DIR__ . '/../api/db.php';
const ADMIN_HASH = '__ADMIN_HASH__';   // 部署时由 PHP CLI 注入

$valid = isset($_SERVER['PHP_AUTH_USER'], $_SERVER['PHP_AUTH_PW'])
      && $_SERVER['PHP_AUTH_USER'] === 'admin'
      && password_verify($_SERVER['PHP_AUTH_PW'], ADMIN_HASH);
if (!$valid) {
    header('WWW-Authenticate: Basic realm="tingqu-feedback"');
    http_response_code(401);
    exit('Unauthorized');
}

header('Content-Type: text/html; charset=utf-8');
$rows = [];
$db = tingqu_db();
if ($db !== null) {
    $rows = $db->query('SELECT * FROM feedback ORDER BY id DESC LIMIT 500')->fetchAll();
}
?>
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>反馈管理 · 听趣S1</title>
<style>
body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;max-width:900px;margin:24px auto;padding:0 16px;color:#16181a}
h1{font-size:22px}p.meta{color:#7a7d80;font-size:13px}
table{width:100%;border-collapse:collapse;font-size:14px}
td,th{border:1px solid #e3e1d8;padding:8px 10px;text-align:left;vertical-align:top}
th{background:#efeee7}
td.content{white-space:pre-wrap;word-break:break-word;max-width:420px}
</style></head><body>
<h1>听趣S1 · 反馈管理</h1>
<p class="meta">共 <?= count($rows) ?> 条（最多显示 500 条）</p>
<table>
<tr><th>#</th><th>时间</th><th>昵称</th><th>联系方式</th><th>内容</th><th>IP</th></tr>
<?php foreach ($rows as $r): ?>
<tr>
<td><?= (int)$r['id'] ?></td>
<td><?= htmlspecialchars(date('Y-m-d H:i', (int)$r['ts'])) ?></td>
<td><?= htmlspecialchars((string)$r['nickname']) ?></td>
<td><?= htmlspecialchars((string)$r['contact']) ?></td>
<td class="content"><?= htmlspecialchars((string)$r['content']) ?></td>
<td class="meta"><?= htmlspecialchars((string)$r['ip']) ?></td>
</tr>
<?php endforeach; ?>
</table>
</body></html>
