<?php
function check(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

check(PHP_ZTS === true, 'PHP must be thread safe');
check(extension_loaded('pdo_duckdb'), 'newly built extension must load');
zts_instance_barrier();
$dir = getenv('PDO_DUCKDB_ZTS_DIR');
$inside = "$dir/inside";
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
if ($request === 0) {
    zts_instance_barrier();
    $db = new PDO("duckdb:$inside/shared.db", null, null, $options);
    zts_instance_barrier();
    if ($worker === 0) {
        $db->exec('CREATE TABLE shared_writes (i INTEGER)');
    }
    zts_instance_barrier();
    $db->exec('INSERT INTO shared_writes VALUES (' . ($worker + 1) . ')');
    zts_instance_barrier();
    check($db->query('SELECT sum(i) FROM shared_writes')->fetchColumn() === '3', 'committed writes must be visible across threads');
    $db->beginTransaction();
    $db->exec('INSERT INTO shared_writes VALUES (' . ($worker + 100) . ')');
    zts_instance_barrier();
    check($db->query('SELECT count(*) FROM shared_writes WHERE i >= 100')->fetchColumn() === 1, 'uncommitted writes must remain connection-local');
    zts_instance_barrier();
    $db->rollBack();
    zts_instance_barrier();
    check($db->query('SELECT count(*) FROM shared_writes')->fetchColumn() === 2, 'rollback must discard every uncommitted marker');
    $db = null;
    zts_instance_barrier();
    for ($i = 0; $i < 12; $i++) {
        $db = new PDO("duckdb:$inside/shared.db", null, null, $options);
        check($db->query('SELECT sum(i) FROM shared_writes')->fetchColumn() === '3', 'all committed writes must survive reopen');
        $db = null;
        zts_instance_barrier();
    }
    $other = new PDO("duckdb:$inside/worker-$worker.db", null, null, $options);
    $other->exec('CREATE TABLE own_file AS SELECT ' . $worker . ' AS i');
    check($other->query('SELECT i FROM own_file')->fetchColumn() === $worker, 'distinct files must remain independent');
    $other = null;
}

$persistent = new PDO("duckdb:$inside/persistent.db", null, null, $options + [PDO::ATTR_PERSISTENT => true]);
zts_instance_barrier();
if ($request === 0 && $worker === 0) {
    $persistent->exec('CREATE TABLE persistent_writes (i INTEGER)');
}
zts_instance_barrier();
if ($request === 1) {
    check($persistent->query('SELECT count(*) FROM persistent_writes')->fetchColumn() === 2, 'persistent handles must survive request reuse');
}
zts_instance_barrier();
$persistent->exec('INSERT INTO persistent_writes VALUES (' . ($worker + 10 * $request) . ')');
zts_instance_barrier();
check($persistent->query('SELECT count(*) FROM persistent_writes')->fetchColumn() === 2 * ($request + 1), 'persistent and thread-local handles must share committed state');
$persistent = null;
zts_instance_barrier();
if ($request === 0) {
    return;
}

$db = new PDO("duckdb:$inside/sandbox.db", null, null, $options);
zts_instance_barrier();
$marker = 'pdo_zts_' . basename($dir);
$log = "$dir/outside/query.log";
$profile = "$dir/outside/profile.json";
if ($worker === 0) {
    $db->exec('SET log_query_path=' . $db->quote($log));
    $db->query("SELECT '$marker-before'")->fetchColumn();
    check(str_contains(file_get_contents($log), "$marker-before"), 'logger control must observe the pre-tightening query');
    $db->exec("PRAGMA enable_profiling='json'");
    $db->exec('SET profiling_output=' . $db->quote($profile));
    $db->query("SELECT '$marker-profile-before'")->fetchAll();
    check(str_contains(file_get_contents($profile), "$marker-profile-before"), 'profiler control must observe the pre-tightening query');
}
zts_instance_barrier();
if ($worker === 1) {
    check(ini_set('open_basedir', $inside) !== false, 'open_basedir must tighten in one thread');
    check($db->query('SELECT 41 + 1')->fetchColumn() === 42, 'sandbox escalation must leave its handle usable');
} else {
    check($db->query('SELECT sum(i) FROM range(2000000) AS t(i)')->fetchColumn() === '1999999000000', 'a concurrent query must remain correct during sibling escalation');
}
zts_instance_barrier();
if ($worker === 0) {
    $beforeLog = file_get_contents($log);
    $beforeProfile = file_get_contents($profile);
    check($db->query("SELECT '$marker-after'")->fetchColumn() === "$marker-after", 'sibling handle must remain usable after shared lockdown');
    check(file_get_contents($log) === $beforeLog, 'a sibling file logger must stop after lockdown');
    check(file_get_contents($profile) === $beforeProfile, 'a sibling profiler must stop after lockdown');
    try {
        $db->exec('SET log_query_path=' . $db->quote($log));
        throw new RuntimeException('a sibling must not restart its file logger');
    } catch (PDOException $e) {
        check(str_contains($e->getMessage(), 'locked'), 'logger restart must fail because configuration is locked');
    }
}
zts_instance_barrier();
$db = null;
