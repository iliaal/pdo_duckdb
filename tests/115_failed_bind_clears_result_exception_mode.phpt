--TEST--
pdo_duckdb: a failed bind round clears the previous result and rowCount in exception mode
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function state(PDOStatement $st): string {
    return 'columns=' . $st->columnCount()
        . ' fetch=' . var_export($st->fetch(PDO::FETCH_NUM), true)
        . ' rowCount=' . $st->rowCount();
}

function failBind(PDOStatement $st, array $params): void {
    try {
        $st->execute($params);
        echo "no exception\n";
    } catch (PDOException $e) {
        echo get_class($e), ' ', $e->getMessage(), "\n";
    }
}

foreach (['buffered' => false, 'unbuffered' => true] as $mode => $unbuffered) {
    $db = new PDO('duckdb::memory:', null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::DUCKDB_ATTR_UNBUFFERED => $unbuffered,
    ]);
    $st = $db->prepare('SELECT i FROM range(5) t(i) WHERE i >= ? ORDER BY i');
    $st->execute([0]);
    echo "$mode first=", $st->fetchColumn(), "\n";
    failBind($st, [0, 999]);
    echo "$mode after failure: ", state($st), "\n";
    $st->execute([3]);
    echo "$mode re-execute: ", implode(',', $st->fetchAll(PDO::FETCH_COLUMN)), "\n";
}

foreach (['buffered' => false, 'unbuffered' => true] as $mode => $unbuffered) {
    $db = new PDO('duckdb::memory:', null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::DUCKDB_ATTR_UNBUFFERED => $unbuffered,
    ]);
    $db->exec('CREATE TABLE t (i INTEGER)');
    $ins = $db->prepare('INSERT INTO t VALUES (?)');
    $ins->execute([1]);
    echo "$mode insert rowCount=", $ins->rowCount(), "\n";
    failBind($ins, [2, 999]);
    echo "$mode insert after failure: rowCount=", $ins->rowCount(), ' columns=', $ins->columnCount(), "\n";
    $ins->execute([3]);
    echo "$mode insert re-execute rowCount=", $ins->rowCount(), "\n";
    echo "$mode table=", implode(',', $db->query('SELECT i FROM t ORDER BY i')->fetchAll(PDO::FETCH_COLUMN)), "\n";
}

foreach (['buffered' => false, 'unbuffered' => true] as $mode => $unbuffered) {
    $db = new PDO('duckdb::memory:', null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::DUCKDB_ATTR_UNBUFFERED => $unbuffered,
    ]);
    $st = $db->prepare('SELECT ? AS v');
    failBind($st, [1, 999]);
    echo "$mode first-execution failure: ", state($st), "\n";
    $st->execute([7]);
    echo "$mode first-execution recovery: ", $st->fetchColumn(), "\n";
}
?>
--EXPECT--
buffered first=0
PDOException SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
buffered after failure: columns=0 fetch=false rowCount=0
buffered re-execute: 3,4
unbuffered first=0
PDOException SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
unbuffered after failure: columns=0 fetch=false rowCount=0
unbuffered re-execute: 3,4
buffered insert rowCount=1
PDOException SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
buffered insert after failure: rowCount=0 columns=0
buffered insert re-execute rowCount=1
buffered table=1,3
unbuffered insert rowCount=1
PDOException SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
unbuffered insert after failure: rowCount=0 columns=0
unbuffered insert re-execute rowCount=1
unbuffered table=1,3
PDOException SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
buffered first-execution failure: columns=0 fetch=false rowCount=0
buffered first-execution recovery: 7
PDOException SQLSTATE[HY000]: General error: 1 Failed to bind string parameter
unbuffered first-execution failure: columns=0 fetch=false rowCount=0
unbuffered first-execution recovery: 7