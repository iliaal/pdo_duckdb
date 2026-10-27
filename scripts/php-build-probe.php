<?php
declare(strict_types=1);

$options = getopt('', ['version:', 'thread-safety:']);
$version = $options['version'] ?? null;
$threadSafety = $options['thread-safety'] ?? null;
if (!is_string($version) || !preg_match('/^8\.[1-9][0-9]*\.[0-9]+(?:RC[0-9]+)?$/D', $version)
    || !in_array($threadSafety, ['nts', 'zts'], true)) {
    fwrite(STDERR, "Usage: php -n php-build-probe.php --version VERSION --thread-safety nts|zts\n");
    exit(64);
}

$actualThreadSafety = PHP_ZTS ? 'zts' : 'nts';
$checks = [
    'version' => PHP_VERSION === $version,
    'thread safety' => $actualThreadSafety === $threadSafety,
    '64-bit integers' => PHP_INT_SIZE === 8,
    'CLI SAPI' => PHP_SAPI === 'cli',
    'release build' => !PHP_DEBUG,
    'PDO' => extension_loaded('PDO'),
];
foreach ($checks as $label => $passed) {
    if (!$passed) {
        fwrite(STDERR, sprintf("PHP build check failed: %s (actual PHP %s, %s, %d-bit, %s, debug=%d, PDO=%d)\n",
            $label, PHP_VERSION, $actualThreadSafety, PHP_INT_SIZE * 8, PHP_SAPI, PHP_DEBUG, extension_loaded('PDO')));
        exit(1);
    }
}
printf("PHP %s %s %d-bit CLI release with PDO: verified\n", PHP_VERSION, $actualThreadSafety, PHP_INT_SIZE * 8);
