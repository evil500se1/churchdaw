<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class Money
{
    public static function toCents(int|float|string $amount): int
    {
        if (is_float($amount)) {
            $amount = number_format($amount, 2, '.', '');
        } elseif (is_int($amount)) {
            $amount = (string) $amount;
        }

        $amount = trim($amount);
        if (!preg_match('/^([0-9]{1,9})(?:\.([0-9]{1,2}))?$/', $amount, $matches)) {
            throw new \RuntimeException('Valor monetario invalido.');
        }
        $fraction = str_pad($matches[2] ?? '', 2, '0');
        return ((int) $matches[1] * 100) + (int) $fraction;
    }

    public static function fromCents(int $cents): float
    {
        return round($cents / 100, 2);
    }
}
