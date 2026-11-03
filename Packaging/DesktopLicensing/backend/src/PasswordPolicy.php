<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class PasswordPolicy
{
    // Hash Argon2id de uma senha aleatória que nunca é aceita. É constante para
    // igualar o custo de contas inexistentes sem executar Argon2 ao criar App.
    public const DUMMY_HASH =
        '$argon2id$v=19$m=65536,t=3,p=1$T2FpUzUuNDBGeTNiMUxLUA$bFFIsbAaW4ti4RfCXlMtWMukDxQy0ewuchCYQPeONY0';

    public static function assertValid(string $password, string $email = ''): void
    {
        $length = mb_strlen($password);
        if ($length < 8 || $length > 128) {
            throw new ApiException(
                422,
                'WEAK_PASSWORD',
                'Use uma senha entre 8 e 128 caracteres.'
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
