--TEST--
pdo_duckdb: DuckDB 2.0 extension repositories are forbidden under open_basedir
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) {
    die('skip needs a build against the DuckDB 2.0 headers');
}
?>
--FILE--
<?php
const KEY = 'MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAt9+KeDkWJa7SiBb7AMdXAPV2qpRunztoC2c7DtRmN9+Lq2YQm4rMQmmAQcwyxTxcK8vwPKOiE4mLMwmQMvTty9tL+buREfvg5KJ+FY3Bv05cZYsf7VuYgUDcw41zSEE5W1thh5PiafcrMn7CPF0jN0oFfVvAHB/fAM0yaZGPKs3WYqnHAHa6v/cVMeiWZ/lQORCBqqL2hii1fEiuIqw6AOEoZVsN0/lqm7GfnD899zrzLOPRuP4YFg46drL1UyNdg+4TquJ2OV9kR4CAfdGRexqHFWLmoxkTfnUxJ4yL+gkV57K1H+smEdKrDhDX68pEFM/34E2eVKZoRU/0paxDZQIDAQAB';
$repos = __DIR__ . '/131_repos_' . getmypid() . '.d';
mkdir($repos);
$o = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$allowed = "duckdb::memory:;extension_repository_directory=$repos;allow_extension_repositories=allowed";

function create_repo(PDO $db, string $name): string {
    try {
        $db->query("CREATE EXTENSION REPOSITORY $name WITH PREFIX 'http://127.0.0.1:9/$name' USING PUBLIC KEY '" . KEY . "'")->fetchAll();
        return 'created';
    } catch (PDOException $e) {
        return 'denied';
    }
}

function attempt(string $label, callable $fn): void {
    try {
        $result = $fn();
        echo "$label: $result\n";
    } catch (PDOException $e) {
        echo "$label: ", str_contains($e->getMessage(), 'not allowed when open_basedir is set') ? 'rejected' : 'error', "\n";
    }
}

// Positive control without open_basedir: an opted-in handle stores a trusted
// repository.
$control = new PDO($allowed, null, null, $o);
echo 'control: ', create_repo($control, 'acme'), "\n";
var_dump($control->query("SELECT repository_name FROM duckdb_extension_repositories() WHERE type = 'USER_PROVIDED'")->fetchColumn());

// Opened before the tightening, escalated on its next SQL.
$early = new PDO($allowed, null, null, $o);

ini_set('open_basedir', __DIR__);

var_dump($early->query("SELECT current_setting('allow_extension_repositories')")->fetchColumn());
echo 'escalated create: ', create_repo($early, 'late'), "\n";

attempt('dsn allow_extension_repositories', fn() => new PDO('duckdb::memory:;allow_extension_repositories=allowed', null, null, $o) ? 'opened' : '');
attempt('dsn extension_repository_directory', fn() => new PDO("duckdb::memory:;extension_repository_directory=$repos", null, null, $o) ? 'opened' : '');
attempt('config array', fn() => new PDO('duckdb::memory:', null, null, $o + [PDO::DUCKDB_ATTR_CONFIG => ['allow_extension_repositories' => 'allowed']]) ? 'opened' : '');

$fresh = new PDO('duckdb::memory:', null, null, $o);
var_dump($fresh->query("SELECT current_setting('allow_extension_repositories')")->fetchColumn());
echo 'fresh create: ', create_repo($fresh, 'fresh'), "\n";
attempt('fresh opt-in', fn() => $fresh->exec("SET allow_extension_repositories='allowed'") === false ? '' : 'set');

$early = $fresh = $control = null;
$files = array_map('basename', glob("$repos/*"));
var_dump($files);
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/131_repos_*.d') as $d) {
    foreach (glob("$d/*") as $f) {
        @unlink($f);
    }
    @rmdir($d);
}
?>
--EXPECT--
control: created
string(4) "acme"
string(9) "forbidden"
escalated create: denied
dsn allow_extension_repositories: rejected
dsn extension_repository_directory: rejected
config array: rejected
string(9) "forbidden"
fresh create: denied
fresh opt-in: error
array(1) {
  [0]=>
  string(31) "acme.duckdb_extension.repo.json"
}
