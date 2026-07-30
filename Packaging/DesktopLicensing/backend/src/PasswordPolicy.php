<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class PasswordPolicy
{
    // Hash Argon2id de uma senha aleatória que nunca é aceita. É constante para
    // igualar o custo de contas inexistentes sem executar Argon2 ao criar App.
    public const DUMMY_HASH =
        '$argon2id$v=19$m=65536,t=3,p=1$T2FpUzUuNDBGeTNiMUxLUA$bFFIsbAaW4ti4RfCXlMtWMukDxQy0ewuchCYQPeONY0';

    /** @var list<string> */
    private const BLOCKED = [
        '12345678',
        '123456789',
        '1234567890',
        'password',
        'senha123',
        'churchdaw',
        'qwerty123',
        '11111111',
    ];

    public static function assertValid(string $password, string $email = ''): void
    {
        $length = mb_strlen($password);
        if ($length < 12 || $length > 128) {
            throw new ApiException(
                422,
                'WEAK_PASSWORD',
                'Use uma senha entre 12 e 128 caracteres.'
            );
        }

        $normalised = mb_strtolower(trim($password));
        $emailLocal = mb_strtolower(explode('@', $email)[0] ?? '');
        if (in_array($normalised, self::BLOCKED, true)
            || ($emailLocal !== '' && mb_strlen($emailLocal) >= 4
                && str_contains($normalised, $emailLocal))) {
            throw new ApiException(
                422,
                'WEAK_PASSWORD',
                'Escolha uma senha menos previsivel.'
            );
        }
    }

    public static function hash(string $password): string
    {
        $hash = password_hash($password, PASSWORD_ARGON2ID, [
            'memory_cost' => 65536,
            'time_cost' => 3,
            'threads' => 1,
        ]);
        if (!is_string($hash)) {
            throw new \RuntimeException('Falha ao proteger a senha.');
        }
        return $hash;
    }
}
