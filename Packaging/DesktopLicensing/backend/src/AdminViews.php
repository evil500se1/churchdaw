<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class AdminViews
{
    public function __construct(private readonly Config $config)
    {
    }

    public function login(string $error = ''): string
    {
        $asset = $this->asset();
        $errorHtml = $error === ''
            ? ''
            : '<p class="admin-alert admin-alert-error">'
                . self::escape($error) . '</p>';
        return '<!doctype html><html lang="pt-BR"><head>'
            . '<meta charset="utf-8"><meta name="viewport" '
            . 'content="width=device-width,initial-scale=1">'
            . '<title>ChurchDAW Admin</title>'
            . '<link rel="stylesheet" href="' . $asset . '"></head>'
            . '<body class="admin-body"><main class="admin-login">'
            . '<section class="admin-login-brand">'
            . '<span class="admin-kicker">CHURCHDAW / ADMIN</span>'
            . '<h1>Licenças sob seu controle.</h1>'
            . '<p>Liberação manual, um aparelho ativo e auditoria em tempo real.</p>'
            . '</section><section class="admin-login-card">'
            . '<div class="admin-mark" aria-hidden="true">⌁</div>'
            . '<h2>Entrar no painel</h2>'
            . '<p class="admin-muted">Acesso restrito ao administrador ChurchDAW.</p>'
            . $errorHtml
            . '<form method="post" action="/admin/login" class="admin-form">'
            . '<label>Usuário<input name="username" autocomplete="username" '
            . 'required maxlength="64"></label>'
            . '<label>Senha<input type="password" name="password" '
            . 'autocomplete="current-password" required maxlength="256"></label>'
            . '<button class="primary" type="submit">Entrar com segurança</button>'
            . '</form><p class="admin-security">Sessão protegida · expira por inatividade</p>'
            . '</section></main></body></html>';
    }

    /** @param array<string, mixed> $data */
    public function dashboard(array $data): string
    {
        $csrf = self::escape((string) ($data['csrf_token'] ?? ''));
        $counts = is_array($data['counts'] ?? null)
            ? $data['counts'] : [];
        $licenses = is_array($data['licenses'] ?? null)
            ? $data['licenses'] : [];
        $flash = is_array($data['flash'] ?? null) ? $data['flash'] : null;
        $flashHtml = '';
        if ($flash !== null) {
            $type = ($flash['type'] ?? '') === 'success'
                ? 'success' : 'error';
            $flashHtml = '<section class="admin-alert admin-alert-'
                . $type . '"><strong>'
                . self::escape((string) ($flash['message'] ?? ''))
                . '</strong>';
            if (!empty($flash['email'])) {
                $flashHtml .= '<span>Cliente: '
                    . self::escape((string) $flash['email']) . '</span>';
            }
            if (!empty($flash['temporary_code'])) {
                $flashHtml .= '<span>Código/senha temporária — copie agora:</span>'
                    . '<code class="admin-code">'
                    . self::escape((string) $flash['temporary_code'])
                    . '</code>';
            }
            if (!empty($flash['license_id'])) {
                $flashHtml .= '<span>Licença: '
                    . self::escape((string) $flash['license_id']) . '</span>';
            }
            $flashHtml .= '</section>';
        }

        $rows = '';
        foreach ($licenses as $license) {
            if (!is_array($license)) {
                continue;
            }
            $licenseId = self::escape(
                (string) ($license['license_id'] ?? '')
            );
            $userId = self::escape((string) ($license['user_id'] ?? ''));
            $deviceId = self::escape(
                (string) ($license['device_id'] ?? '')
            );
            $status = (string) ($license['license_status'] ?? '');
            $active = $status === 'active';
            $device = trim((string) ($license['device_label'] ?? ''));
            $platform = trim((string) ($license['platform'] ?? ''));
            $installation = trim(
                (string) ($license['installation_id'] ?? '')
            );
            $installationShort = $installation === ''
                ? '—'
                : substr($installation, 0, 8) . '…'
                    . substr($installation, -6);
            $deviceHtml = $device === ''
                ? '<span class="admin-muted">Ainda não ativada</span>'
                : '<strong>' . self::escape($device) . '</strong>'
                    . '<small>' . self::escape(
                        strtoupper($platform) . ' · ' . $installationShort
                    ) . '</small>'
                    . '<small>Ativado: ' . self::escape(
                        self::date((string) ($license['activated_at'] ?? ''))
                    ) . '</small>'
                    . '<small>Último contato: ' . self::escape(
                        self::date((string) ($license['last_validated_at'] ?? ''))
                    ) . '</small>';
            $actions = '<form method="post" action="/admin/users/'
                . $userId . '/reset-access">'
                . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                . '<button type="submit" class="admin-button admin-button-ghost">'
                . 'Novo código</button></form>';
            if ($active && $deviceId !== '') {
                $actions .= '<form method="post" action="/admin/devices/'
                    . $deviceId . '/deactivate">'
                    . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                    . '<button type="submit" class="admin-button admin-button-warn">'
                    . 'Trocar aparelho</button></form>';
            }
            if ($active) {
                $actions .= '<form method="post" action="/admin/licenses/'
                    . $licenseId . '/revoke">'
                    . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                    . '<input type="hidden" name="reason" '
                    . 'value="Revogada manualmente no painel">'
                    . '<button type="submit" class="admin-button admin-button-danger">'
                    . 'Revogar licença</button></form>';
            }
            $rows .= '<tr><td><strong>'
                . self::escape((string) ($license['display_name'] ?? ''))
                . '</strong><small>'
                . self::escape((string) ($license['email'] ?? ''))
                . '</small></td><td><span class="admin-status admin-status-'
                . ($active ? 'active' : 'revoked') . '">'
                . self::escape($active ? 'ATIVA' : 'REVOGADA')
                . '</span><small>Emitida: '
                . self::escape(self::date(
                    (string) ($license['issued_at'] ?? '')
                )) . '</small></td><td>' . $deviceHtml
                . '</td><td><div class="admin-actions">'
                . $actions . '</div></td></tr>';
        }
        if ($rows === '') {
            $rows = '<tr><td colspan="4" class="admin-empty">'
                . 'Nenhuma licença manual emitida ainda.</td></tr>';
        }

        return '<!doctype html><html lang="pt-BR"><head>'
            . '<meta charset="utf-8"><meta name="viewport" '
            . 'content="width=device-width,initial-scale=1">'
            . '<title>Painel ChurchDAW</title>'
            . '<link rel="stylesheet" href="' . $this->asset() . '"></head>'
            . '<body class="admin-body"><main class="admin-shell">'
            . '<header class="admin-topbar"><div>'
            . '<span class="admin-kicker">CHURCHDAW / ADMIN</span>'
            . '<h1>Painel de licenças</h1></div>'
            . '<form method="post" action="/admin/logout">'
            . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
            . '<button class="admin-button admin-button-ghost" type="submit">'
            . 'Sair</button></form></header>'
            . $flashHtml
            . '<section class="admin-metrics">'
            . self::metric('Licenças ativas', $counts['active_licenses'] ?? 0)
            . self::metric('Aparelhos ativos', $counts['active_devices'] ?? 0)
            . self::metric('Demos em curso', $counts['active_trials'] ?? 0)
            . self::metric('Revogadas', $counts['revoked_licenses'] ?? 0)
            . '</section>'
            . '<section class="admin-panel admin-issue">'
            . '<div><span class="admin-kicker">LIBERAÇÃO MANUAL</span>'
            . '<h2>Criar acesso de cliente</h2>'
            . '<p class="admin-muted">Gera uma licença para um aparelho e, '
            . 'se for cliente novo, um código/senha temporária exibido uma única vez.</p>'
            . '</div><form method="post" action="/admin/licenses/issue" '
            . 'class="admin-form admin-form-grid">'
            . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
            . '<label>Nome do cliente<input name="display_name" required '
            . 'maxlength="96" autocomplete="off"></label>'
            . '<label>E-mail do cliente<input type="email" name="email" '
            . 'required maxlength="254" autocomplete="off"></label>'
            . '<button class="primary" type="submit">Liberar licença</button>'
            . '</form></section>'
            . '<section class="admin-panel"><div class="admin-section-title">'
            . '<div><span class="admin-kicker">CLIENTES</span>'
            . '<h2>Ativações e aparelhos</h2></div>'
            . '<span class="admin-muted">Últimas 100 licenças</span></div>'
            . '<div class="admin-table-wrap"><table class="admin-table">'
            . '<thead><tr><th>Cliente</th><th>Licença</th>'
            . '<th>Aparelho</th><th>Ações</th></tr></thead>'
            . '<tbody>' . $rows . '</tbody></table></div></section>'
            . '<footer class="admin-footer">Release '
            . self::escape($this->config->string('app.release'))
            . ' · checkout automático desligado</footer>'
            . '</main></body></html>';
    }

    private function asset(): string
    {
        return '/account-assets/'
            . rawurlencode($this->config->string('app.release'))
            . '/app.css';
    }

    private static function metric(string $label, mixed $value): string
    {
        return '<article class="admin-metric"><span>'
            . self::escape($label) . '</span><strong>'
            . self::escape((string) $value) . '</strong></article>';
    }

    private static function date(string $value): string
    {
        $time = strtotime($value);
        return $time === false ? '—' : gmdate('d/m/Y H:i', $time) . ' UTC';
    }

    private static function escape(string $value): string
    {
        return htmlspecialchars(
            $value,
            ENT_QUOTES | ENT_SUBSTITUTE,
            'UTF-8'
        );
    }
}
