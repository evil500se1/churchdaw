<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;

final class Mailer
{
    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto
    ) {
    }

    public function sendVerification(string $email, string $name, string $token): void
    {
        $url = $this->config->string('app.base_url')
             . '/verificar-email#token=' . rawurlencode($token);
        $subject = 'Confirme seu e-mail no ChurchDAW';
        $body = "Ola, {$name}!\n\n"
              . "Confirme o e-mail da sua conta ChurchDAW:\n{$url}\n\n"
              . "O link expira em 24 horas. Se voce nao iniciou esta compra, ignore.";
        $this->enqueue(
            $email,
            $subject,
            $body,
            'email_verification',
            Crypto::tokenHash($token)
        );
    }

    public function sendPasswordReset(string $email, string $name, string $token): void
    {
        $url = $this->config->string('app.base_url')
             . '/redefinir-senha#token=' . rawurlencode($token);
        $subject = 'Redefina sua senha do ChurchDAW';
        $body = "Ola, {$name}!\n\n"
              . "Use este link para criar uma nova senha:\n{$url}\n\n"
              . "O link expira em 30 minutos e funciona uma unica vez. "
              . "Se voce nao pediu a troca, ignore.";
        $this->enqueue(
            $email,
            $subject,
            $body,
            'password_reset',
            Crypto::tokenHash($token)
        );
    }

    public function processPending(int $limit = 25): int
    {
        if (!$this->config->bool('mail.enabled')) {
            return 0;
        }
        $limit = max(1, min(100, $limit));
        $sent = 0;
        for ($index = 0; $index < $limit; ++$index) {
            $job = $this->claimNext();
            if ($job === null) {
                break;
            }
            try {
                $this->deliver(
                    $this->crypto->decrypt((string) $job['recipient_ciphertext']),
                    $this->crypto->decrypt((string) $job['subject_ciphertext']),
                    $this->crypto->decrypt((string) $job['body_ciphertext'])
                );
                $this->database->pdo()->prepare(
                    "UPDATE email_jobs
                     SET status = 'sent', sent_at = CURRENT_TIMESTAMP(6),
                         processing_started_at = NULL, last_error = NULL
                     WHERE id = :id AND status = 'processing'"
                )->execute(['id' => $job['id']]);
                ++$sent;
            } catch (\Throwable $error) {
                $attempts = (int) $job['attempts'];
                $delaySeconds = min(21600, 60 * (2 ** min(8, $attempts)));
                $this->database->pdo()->prepare(
                    "UPDATE email_jobs
                     SET status = 'failed', processing_started_at = NULL,
                         available_at = :available_at, last_error = :last_error
                     WHERE id = :id"
                )->execute([
                    'available_at' => gmdate(
                        'Y-m-d H:i:s',
                        time() + $delaySeconds
                    ),
                    'last_error' => mb_substr($error->getMessage(), 0, 500),
                    'id' => $job['id'],
                ]);
                error_log('ChurchDAW: falha no job de e-mail ' . $job['id']);
            }
        }
        return $sent;
    }

    private function enqueue(
        string $email,
        string $subject,
        string $body,
        string $type,
        string $dedupeMaterial
    ): void {
        if (!$this->config->bool('mail.enabled')) {
            return;
        }
        $this->assertSafeEnvelope($email, $subject);
        $this->database->pdo()->prepare(
            "INSERT INTO email_jobs
                (id, type, dedupe_key, recipient_ciphertext,
                 subject_ciphertext, body_ciphertext)
             VALUES
                (:id, :type, :dedupe_key, :recipient_ciphertext,
                 :subject_ciphertext, :body_ciphertext)
             ON DUPLICATE KEY UPDATE id = id"
        )->execute([
            'id' => Crypto::uuid(),
            'type' => $type,
            'dedupe_key' => hash('sha256', $type . '|' . $dedupeMaterial),
            'recipient_ciphertext' => $this->crypto->encrypt($email),
            'subject_ciphertext' => $this->crypto->encrypt($subject),
            'body_ciphertext' => $this->crypto->encrypt($body),
        ]);
    }

    /** @return array<string, mixed>|null */
    private function claimNext(): ?array
    {
        return $this->database->transaction(function (PDO $pdo): ?array {
            $statement = $pdo->query(
                "SELECT * FROM email_jobs
                 WHERE attempts < 10
                   AND available_at <= CURRENT_TIMESTAMP(6)
                   AND (
                        status IN ('queued', 'failed')
                        OR (
                            status = 'processing'
                            AND processing_started_at
                                < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 5 MINUTE)
                        )
                 )
                 ORDER BY created_at ASC
                 LIMIT 1 FOR UPDATE"
            );
            $job = $statement->fetch();
            if (!is_array($job)) {
                return null;
            }
            $pdo->prepare(
                "UPDATE email_jobs
                 SET status = 'processing',
                     processing_started_at = CURRENT_TIMESTAMP(6),
                     attempts = attempts + 1, last_error = NULL
                 WHERE id = :id"
            )->execute(['id' => $job['id']]);
            $job['attempts'] = (int) $job['attempts'] + 1;
            return $job;
        });
    }

    private function deliver(string $email, string $subject, string $body): void
    {
        $this->assertSafeEnvelope($email, $subject);
        $from = $this->config->string('mail.from');
        $replyTo = $this->config->string('mail.reply_to');
        $this->assertSafeEnvelope($from, 'ChurchDAW');
        $this->assertSafeEnvelope($replyTo, 'ChurchDAW');
        $headers = [
            'From: ChurchDAW <' . $from . '>',
            'Reply-To: ' . $replyTo,
            'Content-Type: text/plain; charset=UTF-8',
            'Content-Transfer-Encoding: 8bit',
            'X-Auto-Response-Suppress: All',
        ];
        if (!mail($email, $subject, $body, implode("\r\n", $headers))) {
            throw new \RuntimeException('O provedor de e-mail recusou a mensagem.');
        }
    }

    private function assertSafeEnvelope(string $email, string $subject): void
    {
        if (!filter_var($email, FILTER_VALIDATE_EMAIL)
            || str_contains($email, "\r")
            || str_contains($email, "\n")
            || $subject === ''
            || mb_strlen($subject) > 200
            || str_contains($subject, "\r")
            || str_contains($subject, "\n")) {
            throw new \RuntimeException('Envelope de e-mail invalido.');
        }
    }
}
