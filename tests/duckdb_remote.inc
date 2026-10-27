<?php
function duckdb_remote_connection(): PDO
{
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($directory = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($directory));
    }
    $db->exec('LOAD httpfs; LOAD quack');
    return $db;
}

function duckdb_remote_skipif(): void
{
    require_once __DIR__ . '/duckdb_v2.inc';
    if (!pdo_duckdb_v2_headers()) {
        die('skip needs a build against the DuckDB 2.0 headers');
    }
    try {
        duckdb_remote_connection();
    } catch (PDOException $e) {
        die('skip quack extension not available: ' . strtok($e->getMessage(), "\n"));
    }
}

final class DuckdbRemoteFixture
{
    public PDO $server;
    public PDO $client;
    public string $endpoint;
    public string $token;

    public function __construct()
    {
        $socket = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
        if (!$socket) {
            throw new RuntimeException("Cannot reserve loopback port: $error");
        }
        $port = (int)substr(strrchr(stream_socket_get_name($socket, false), ':'), 1);
        fclose($socket);
        $this->endpoint = "quack:127.0.0.1:$port";
        $this->token = bin2hex(random_bytes(16));
        $this->server = duckdb_remote_connection();
        $this->client = duckdb_remote_connection();
        $this->server->exec('CREATE TABLE remote_t (id INTEGER PRIMARY KEY, s VARCHAR, repeated VARCHAR, b BLOB)');
        $this->server->exec('CREATE TABLE remote_counts (id INTEGER, v INTEGER)');
        $this->server->query('CALL quack_serve(' . $this->server->quote($this->endpoint)
            . ', token=' . $this->server->quote($this->token) . ', disable_ssl=true)')->fetchAll();
        try {
            $this->client->exec('ATTACH ' . $this->client->quote($this->endpoint)
                . ' AS q (TOKEN ' . $this->client->quote($this->token) . ', DISABLE_SSL true)');
        } catch (Throwable $error) {
            $this->stop();
            throw $error;
        }
    }

    public function stop(): void
    {
        $this->server->query('SELECT * FROM quack_stop(' . $this->server->quote($this->endpoint) . ')')->fetchAll();
    }
}
