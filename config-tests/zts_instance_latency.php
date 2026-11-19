<?php
function check(bool $ok, string $message): void
{
    if (!$ok) {
        throw new RuntimeException($message);
    }
}

function mark(string $name): void
{
    file_put_contents(getenv('PDO_DUCKDB_ZTS_DIR') . "/mark-$name", (string) hrtime(true));
}

function marked(string $name): int
{
    return (int) file_get_contents(getenv('PDO_DUCKDB_ZTS_DIR') . "/mark-$name");
}

/* Two DuckDB threads keep the slow join's duration similar across runners;
 * it has to outlast the sibling's start delay by a wide margin. */
function slow_query(PDO $db, int $n): void
{
    $sql = "SELECT count(*) FROM range($n) AS a(i) JOIN range($n) AS b(j) ON a.i < b.j";
    check($db->query($sql)->fetchColumn() === intdiv($n * ($n - 1), 2), 'slow query result');
}

check(PHP_ZTS === true, 'PHP must be thread safe');
check(extension_loaded('pdo_duckdb'), 'newly built extension must load');
$scenario = getenv('PDO_DUCKDB_ZTS_SCENARIO');
$dir = getenv('PDO_DUCKDB_ZTS_DIR') . '/inside';
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = "duckdb:$dir/a.db;threads=2";
$b = "duckdb:$dir/b.db;threads=2";

if ($scenario === 'shared-reads') {
    check($workers === 4, 'shared-reads runs four workers');
    $db = new PDO($a, null, null, $options);
    if ($worker === 0) {
        $db->exec('CREATE TABLE counter (id INTEGER PRIMARY KEY, n INTEGER)');
        $db->exec('INSERT INTO counter VALUES (1, 0)');
    }
    zts_instance_barrier();
    if ($worker === 0) {
        mark('slow-start');
        slow_query($db, 12000);
        mark('slow-end');
    } elseif ($worker === 1) {
        usleep(300000);
        mark('fast-start');
        for ($i = 0; $i < 5; $i++) {
            $fast = new PDO($a, null, null, $options);
            check($fast->query('SELECT 1')->fetchColumn() === 1, 'fast query result');
            $fast = null;
        }
        check($db->query('SELECT count(*) FROM counter')->fetchColumn() === 1, 'fast query on an open handle');
        mark('fast-end');
    }
    zts_instance_barrier();
    if ($worker === 0) {
        check(marked('fast-start') > marked('slow-start'), 'the fast reader must start while the slow query runs');
        check(marked('fast-end') < marked('slow-end'), 'a SELECT on the same file must not wait for a sibling SELECT');
    }
    zts_instance_barrier();
    /* Writers still take the instance exclusively: concurrent autocommit
     * UPDATEs of one row queue instead of failing with a write conflict. */
    $errors = 0;
    for ($i = 0; $i < 200; $i++) {
        try {
            if ($i % 2) {
                $db->exec('UPDATE counter SET n = n + 1 WHERE id = 1');
            } else {
                $db->prepare('UPDATE counter SET n = n + CAST(? AS INTEGER) WHERE id = 1')->execute([1]);
            }
        } catch (PDOException $e) {
            $errors++;
            $last = $e->getMessage();
        }
    }
    check($errors === 0, "concurrent UPDATEs must queue, got $errors errors" . ($errors ? ": $last" : ''));
    zts_instance_barrier();
    check($db->query('SELECT n FROM counter')->fetchColumn() === 800, 'every concurrent UPDATE must apply');
} elseif ($scenario === 'registry') {
    check($workers === 3, 'registry runs three workers');
    if ($worker < 2) {
        $db = new PDO($a, null, null, $options);
    }
    zts_instance_barrier();
    if ($worker === 0) {
        mark('slow-start');
        slow_query($db, 12000);
        mark('slow-end');
    } elseif ($worker === 1) {
        usleep(300000);
        mark('close-start');
        $db = null;
    } else {
        usleep(600000);
        mark('open-start');
        $other = new PDO($b, null, null, $options);
        check($other->query('SELECT 1')->fetchColumn() === 1, 'other file query result');
        $other = null;
        mark('open-end');
    }
    zts_instance_barrier();
    if ($worker === 0) {
        check(marked('close-start') > marked('slow-start') && marked('open-start') > marked('close-start'),
            'the close and the open must start while the slow query runs');
        check(marked('open-end') < marked('slow-end'),
            'opening another file must not wait for a busy instance that another thread is closing');
    }
} elseif ($scenario === 'timeout') {
    check($workers === 1, 'timeout runs one worker');
    check((int) ini_get('max_execution_time') === 2, 'timeout runs with max_execution_time=2');
    $db = new PDO("duckdb:$dir/timeout.db", null, null, $options + [PDO::ATTR_PERSISTENT => true]);
    if ($request === 0) {
        $db->exec('SET threads = 2');
        /* The shutdown function checks the fatal error instead of the output. */
        ini_set('display_errors', '0');
        $start = hrtime(true);
        register_shutdown_function(function () use ($start): void {
            $elapsed = (hrtime(true) - $start) / 1e9;
            $error = error_get_last();
            if ((connection_status() & CONNECTION_TIMEOUT) && $elapsed < 4.0
                    && $error !== null && $error['type'] === E_ERROR
                    && str_starts_with($error['message'], 'Maximum execution time of 2 seconds exceeded')) {
                zts_instance_expect_bailout();
                return;
            }
            file_put_contents('php://stderr', sprintf("a query under max_execution_time=2 ended after %.1f s (connection status %d, last error %s)\n",
                $elapsed, connection_status(), $error['message'] ?? 'none'));
        });
        try {
            slow_query($db, 20000);
        } catch (PDOException $e) {
        }
        throw new RuntimeException('the query must end in a max_execution_time error');
    }
    check($db->query('SELECT 42')->fetchColumn() === 42, 'a persistent handle must stay usable after a timed-out query');
} else {
    throw new RuntimeException("unknown scenario $scenario");
}
