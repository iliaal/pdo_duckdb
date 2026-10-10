<?php
// Keep PIE's argument modes aligned with the configure help, without needing
// DuckDB or a compiler. A valueless PIE option cannot forward a bundle path.
declare(strict_types=1);

$root = dirname(__DIR__);
$composer = json_decode(file_get_contents($root . '/composer.json'), true, 512, JSON_THROW_ON_ERROR);
$config = file_get_contents($root . '/config.m4');
preg_match_all('/AS_HELP_STRING\(\[--([a-z0-9-]+)(=[^\]]+)?\]/', $config, $matches, PREG_SET_ORDER);
$modes = [];
foreach ($matches as $match) {
    $modes[$match[1]] = isset($match[2]) && $match[2] !== '';
}

foreach ($composer['php-ext']['configure-options'] as $option) {
    $name = $option['name'];
    if (!array_key_exists($name, $modes)) {
        fwrite(STDERR, "PIE option --$name is missing from config.m4 help\n");
        exit(1);
    }
    if (($option['needs-value'] ?? false) !== $modes[$name]) {
        fwrite(STDERR, "PIE option --$name has the wrong needs-value mode\n");
        exit(1);
    }
    echo "ok: --$name\n";
}
