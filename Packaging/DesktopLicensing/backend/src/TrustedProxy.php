<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class TrustedProxy
{
    public static function assertSafeConfiguration(
        string $configuredRanges
    ): void {
        foreach (preg_split('/[\s,]+/', trim($configuredRanges)) ?: [] as $range) {
            if ($range === '') {
                continue;
            }
            if (!str_contains($range, '/')) {
                if (!filter_var($range, FILTER_VALIDATE_IP)) {
                    throw new \RuntimeException(
                        "TRUSTED_PROXY contem endereco invalido: {$range}"
                    );
                }
                continue;
            }

            [$network, $prefixText] = array_pad(
                explode('/', $range, 2),
                2,
                ''
            );
            $packedNetwork = @inet_pton($network);
            if ($packedNetwork === false || !ctype_digit($prefixText)) {
                throw new \RuntimeException(
                    "TRUSTED_PROXY contem CIDR invalido: {$range}"
                );
            }
            $prefix = (int) $prefixText;
            $maximumBits = strlen($packedNetwork) * 8;
            $minimumSafePrefix = $maximumBits === 32 ? 8 : 16;
            if ($prefix < $minimumSafePrefix || $prefix > $maximumBits) {
                throw new \RuntimeException(
                    "TRUSTED_PROXY contem faixa ampla ou invalida: {$range}"
                );
            }
        }
    }

    public static function contains(string $configuredRanges, string $address): bool
    {
        $packedAddress = @inet_pton($address);
        if ($packedAddress === false) {
            return false;
        }

        foreach (preg_split('/[\s,]+/', trim($configuredRanges)) ?: [] as $range) {
            if ($range === '') {
                continue;
            }
            if (!str_contains($range, '/')) {
                if (hash_equals($range, $address)) {
                    return true;
                }
                continue;
            }

            [$network, $prefixText] = array_pad(explode('/', $range, 2), 2, '');
            $packedNetwork = @inet_pton($network);
            if ($packedNetwork === false
                || strlen($packedNetwork) !== strlen($packedAddress)
                || !ctype_digit($prefixText)) {
                continue;
            }

            $prefix = (int) $prefixText;
            $maximumBits = strlen($packedAddress) * 8;
            if ($prefix < 0 || $prefix > $maximumBits) {
                continue;
            }
            if (self::prefixMatches($packedAddress, $packedNetwork, $prefix)) {
                return true;
            }
        }
        return false;
    }

    private static function prefixMatches(
        string $address,
        string $network,
        int $prefix
    ): bool {
        $wholeBytes = intdiv($prefix, 8);
        if ($wholeBytes > 0
            && !hash_equals(
                substr($network, 0, $wholeBytes),
                substr($address, 0, $wholeBytes)
            )) {
            return false;
        }

        $remainingBits = $prefix % 8;
        if ($remainingBits === 0) {
            return true;
        }
        $mask = (0xff << (8 - $remainingBits)) & 0xff;
        return (ord($address[$wholeBytes]) & $mask)
            === (ord($network[$wholeBytes]) & $mask);
    }
}
