<?php
// 听趣S1 官网 · 数据库连接（MySQL）
// 凭据文件位于 webroot 之外，不入库；部署时由维护者创建：
//   /home/wwwroot/lnmp01/tingqu-data/db-config.php
// 内容形如：<?php return ['dsn'=>'mysql:host=127.0.0.1;dbname=e4p;charset=utf8mb4','user'=>'e4p_user','pass'=>'xxxx'];
function tingqu_db(): ?PDO {
    static $pdo = null;
    static $tried = false;
    if ($tried) return $pdo;
    $tried = true;
    $cfg_file = '/home/wwwroot/lnmp01/tingqu-data/db-config.php';
    if (!file_exists($cfg_file)) return null;
    $cfg = require $cfg_file;
    try {
        $pdo = new PDO($cfg['dsn'], $cfg['user'], $cfg['pass'], [
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        ]);
        $pdo->exec('CREATE TABLE IF NOT EXISTS feedback (
            id INT AUTO_INCREMENT PRIMARY KEY,
            ts INT NOT NULL,
            ip VARCHAR(45) NOT NULL,
            ua VARCHAR(200),
            nickname VARCHAR(32),
            contact VARCHAR(64),
            content TEXT NOT NULL,
            INDEX idx_ts (ts)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4');
    } catch (Throwable $e) {
        error_log('tingqu db: ' . $e->getMessage());
        $pdo = null;
    }
    return $pdo;
}
