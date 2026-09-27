--TEST--
pdo_duckdb: failed standalone named binds preserve results while failed execute clears them
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
class WrappedStatement extends PDOStatement
{
    public function execute(?array $params = null): bool
    {
        return parent::execute($params);
    }

    public function bindValue(string|int $param, mixed $value, int $type = PDO::PARAM_STR): bool
    {
        return parent::bindValue($param, $value, $type);
    }

    public function bindParam(string|int $param, mixed &$var, int $type = PDO::PARAM_STR,
        int $maxLength = 0, mixed $driverOptions = null): bool
    {
        return parent::bindParam($param, $var, $type, $maxLength, $driverOptions);
    }
}

foreach ([PDOStatement::class, WrappedStatement::class] as $class) {
    foreach ([false, true] as $unbuffered) {
        foreach (['bindValue', 'bindParam'] as $method) {
            $db = new PDO('duckdb::memory:', null, null, [
                PDO::ATTR_ERRMODE => PDO::ERRMODE_SILENT,
                PDO::ATTR_STATEMENT_CLASS => [$class],
                PDO::DUCKDB_ATTR_UNBUFFERED => $unbuffered,
            ]);
            $stmt = $db->prepare('SELECT i FROM range(3) t(i) WHERE i >= :minimum');
            $stmt->execute(['minimum' => 0]);
            $first = $stmt->fetchColumn();
            $value = 0;
            $bound = $stmt->$method(':missing', $value);
            $error = $stmt->errorCode();
            $columns = $stmt->columnCount();
            $next = $stmt->fetchColumn();
            $stmt->closeCursor();

            $db->exec('CREATE TABLE t (v INTEGER)');
            $insert = $db->prepare('INSERT INTO t VALUES (:value)');
            $insert->execute(['value' => 1]);
            $insertBound = $insert->$method(':missing', $value);
            $changed = $insert->rowCount();

            $stmt->execute(['minimum' => 0]);
            $stmt->fetchColumn();
            $executed = $stmt->execute(['missing' => 0]);
            $failedColumns = $stmt->columnCount();
            $stale = $stmt->fetchColumn();
            $stmt->execute(['minimum' => 2]);
            $recovered = $stmt->fetchColumn();

            printf("%s %s %s: %s\n", $class, $unbuffered ? 'unbuffered' : 'buffered',
                $method, json_encode([$first, $bound, $error, $columns, $next,
                    $insertBound, $changed, $executed, $failedColumns, $stale, $recovered]));
        }
    }
}
?>
--EXPECT--
PDOStatement buffered bindValue: [0,false,"HY000",1,1,false,1,false,0,false,2]
PDOStatement buffered bindParam: [0,false,"HY000",1,1,false,1,false,0,false,2]
PDOStatement unbuffered bindValue: [0,false,"HY000",1,1,false,1,false,0,false,2]
PDOStatement unbuffered bindParam: [0,false,"HY000",1,1,false,1,false,0,false,2]
WrappedStatement buffered bindValue: [0,false,"HY000",1,1,false,1,false,0,false,2]
WrappedStatement buffered bindParam: [0,false,"HY000",1,1,false,1,false,0,false,2]
WrappedStatement unbuffered bindValue: [0,false,"HY000",1,1,false,1,false,0,false,2]
WrappedStatement unbuffered bindParam: [0,false,"HY000",1,1,false,1,false,0,false,2]
