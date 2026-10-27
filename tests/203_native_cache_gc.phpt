--TEST--
pdo_duckdb: private native-row cleanup defers unrelated cycle destructors and restores GC state
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$inside = $total = 0;
class PendingCycle {
    public $self;
    public function __destruct() {
        global $inside, $total;
        $total++;
        foreach (debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS) as $frame) {
            if (($frame['class'] ?? '') === PDOStatement::class
                && in_array($frame['function'], ['closeCursor', 'fetchColumn', 'execute'], true)) {
                $inside++;
                break;
            }
        }
    }
}
function markRoots($db, $stmt, $saved): void {}
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES=>true]);
foreach (['scalar'=>'i', 'list'=>'[i]', 'json'=>'to_json([i])', 'variant'=>'[i]::VARIANT'] as $kind=>$expression) {
    foreach (['close', 'next', 'execute'] as $action) {
        foreach ([false, true] as $mutate) {
            $stmt = $db->query("SELECT $expression AS v FROM range(3) r(i)");
            $saved = $stmt->fetchColumn();
            if ($mutate && is_object($saved)) $saved->annotation = 1;
            gc_collect_cycles();
            gc_disable();
            markRoots($db, $stmt, $saved);
            $target = gc_status()['threshold'];
            $created = 0;
            $before = $total;
            while (gc_status()['roots'] < $target) {
                $cycle = new PendingCycle;
                $cycle->self = $cycle;
                unset($cycle);
                $created++;
            }
            gc_enable();
            $inside = 0;
            if ($action === 'close') $stmt->closeCursor();
            elseif ($action === 'next') $next = $stmt->fetchColumn();
            else $stmt->execute();
            if ($inside !== 0) throw new Exception("$kind/$action callbacks=$inside");
            if (!gc_enabled()) throw new Exception('GC left disabled');
            gc_collect_cycles();
            if ($created < 1 || $total - $before !== $created) throw new Exception('pending cycles lost');
            unset($stmt, $saved, $next);
        }
    }
    echo "$kind cleanup OK\n";
}
$stmt = $db->query('SELECT [1]');
$saved = $stmt->fetchColumn();
$saved->annotation = 1;
gc_disable();
$stmt->closeCursor();
if (gc_enabled()) throw new Exception('disabled collector enabled');
gc_enable();
echo "collector state restored OK\n";
?>
--EXPECT--
scalar cleanup OK
list cleanup OK
json cleanup OK
variant cleanup OK
collector state restored OK
