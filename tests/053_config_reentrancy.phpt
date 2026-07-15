--TEST--
pdo_duckdb: config values are converted without invoking user callbacks
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
final class StringableConfigValue
{
    public static int $calls = 0;

    public function __toString(): string
    {
        self::$calls++;
        return '2';
    }
}

final class ThrowingConfigValue
{
    public static int $calls = 0;

    public function __toString(): string
    {
        self::$calls++;
        throw new RuntimeException('sentinel-secret-value');
    }
}

final class MutatingConfigValue
{
    public static int $calls = 0;
    private mixed $config;

    public function __construct(mixed &$config)
    {
        $this->config =& $config;
    }

    public function __toString(): string
    {
        self::$calls++;
        $this->config = ['threads' => 1];
        ini_set('open_basedir', __DIR__);
        return '2';
    }
}

$threadRef = 3;
$scalarCases = [
    'string' => ['threads' => '2'],
    'int' => ['threads' => 2],
    'float' => ['threads' => 2.0],
    'bool' => ['enable_object_cache' => true],
    'null' => ['temp_directory' => null],
    'reference' => ['threads' => &$threadRef],
];
foreach ($scalarCases as $label => $config) {
    new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_CONFIG => $config]);
    echo "$label accepted\n";
}

$resource = fopen('php://memory', 'r');
$invalidCases = [
    'array' => ['nested'],
    'resource' => $resource,
    'object' => new stdClass(),
    'stringable' => new StringableConfigValue(),
    'throwing' => new ThrowingConfigValue(),
];
foreach ($invalidCases as $label => $value) {
    try {
        new PDO('duckdb::memory:', null, null, [
            PDO::DUCKDB_ATTR_CONFIG => ['threads' => $value],
        ]);
        echo "$label accepted (BUG)\n";
    } catch (PDOException $e) {
        echo "$label rejected\n";
    }
}
fclose($resource);

$mutatingConfig = [];
$mutatingConfig['threads'] = new MutatingConfigValue($mutatingConfig);
$mutatingConfig['enable_external_access'] = true;
try {
    new PDO('duckdb::memory:', null, null, [
        PDO::DUCKDB_ATTR_CONFIG => &$mutatingConfig,
    ]);
    echo "mutating accepted (BUG)\n";
} catch (PDOException $e) {
    echo "mutating rejected\n";
}

try {
    new PDO('duckdb::memory:', null, null, [
        PDO::DUCKDB_ATTR_CONFIG => ["threads\0secret" => 2],
    ]);
    echo "NUL accepted (BUG)\n";
} catch (PDOException $e) {
    echo "NUL rejected\n";
}

echo 'stringable calls=', StringableConfigValue::$calls, "\n";
echo 'throwing calls=', ThrowingConfigValue::$calls, "\n";
echo 'mutating calls=', MutatingConfigValue::$calls, "\n";
?>
--EXPECT--
string accepted
int accepted
float accepted
bool accepted
null accepted
reference accepted
array rejected
resource rejected
object rejected
stringable rejected
throwing rejected
mutating rejected
NUL rejected
stringable calls=0
throwing calls=0
mutating calls=0
