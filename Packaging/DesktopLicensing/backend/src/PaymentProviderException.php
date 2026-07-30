<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class PaymentProviderException extends \RuntimeException
{
    public function __construct(
        string $message,
        public readonly bool $operationMayHaveSucceeded,
        ?\Throwable $previous = null
    ) {
        parent::__construct($message, 0, $previous);
    }
}
