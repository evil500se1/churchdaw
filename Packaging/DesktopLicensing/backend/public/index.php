<?php

declare(strict_types=1);

use ChurchDAW\Licensing\App;

$projectRoot = dirname(__DIR__);
require $projectRoot . '/src/autoload.php';

(new App($projectRoot))->run();
