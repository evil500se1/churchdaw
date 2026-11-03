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
            . '<p>Liberação manual, vagas por plataforma e auditoria em tempo real.</p>'
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
        $events = is_array($data['events'] ?? null)
            ? $data['events'] : [];
        $orders = is_array($data['orders'] ?? null)
            ? $data['orders'] : [];
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
            $status = (string) ($license['license_status'] ?? '');
            $active = $status === 'active';
            $userStatus = (string) ($license['user_status'] ?? '');
            $userActive = $userStatus === 'active';
            $activationPolicy = (string) (
                $license['activation_policy'] ?? 'single_device'
            );
            $bundle = $activationPolicy === 'platform_bundle';
            $devices = is_array($license['devices'] ?? null)
                ? $license['devices'] : [];
            $devicesByPlatform = [];
            foreach ($devices as $device) {
                if (is_array($device)) {
                    $devicesByPlatform[(string) ($device['platform'] ?? '')] =
                        $device;
                }
            }
            $platforms = $bundle
                ? [
                    'windows' => 'Windows',
                    'macos' => 'macOS',
                    'android' => 'Android',
                ]
                : ['single_device' => 'Aparelho único'];
            $deviceHtml = '<div class="admin-device-grid">';
            foreach ($platforms as $platformCode => $platformLabel) {
                $device = $bundle
                    ? ($devicesByPlatform[$platformCode] ?? null)
                    : ($devices[0] ?? null);
                if (!is_array($device)) {
                    $deviceHtml .= '<article class="admin-device-slot is-empty">'
                        . '<span>' . self::escape($platformLabel) . '</span>'
                        . '<strong>Vaga livre</strong>'
                        . '<small>Aguardando ativação</small></article>';
                    continue;
                }
                $deviceId = self::escape(
                    (string) ($device['device_id'] ?? '')
                );
                $installation = trim(
                    (string) ($device['installation_id'] ?? '')
                );
                $installationShort = $installation === ''
                    ? '—'
                    : substr($installation, 0, 8) . '…'
                        . substr($installation, -6);
                $actualPlatform = (string) ($device['platform'] ?? '');
                $deviceHtml .= '<article class="admin-device-slot is-active">'
                    . '<span>' . self::escape(
                        $bundle ? $platformLabel : strtoupper($actualPlatform)
                    ) . '</span><strong>'
                    . self::escape((string) ($device['device_label'] ?? ''))
                    . '</strong><small>'
                    . self::escape($installationShort) . '</small>'
                    . '<small>Último contato: ' . self::escape(self::date(
                        (string) ($device['last_validated_at'] ?? '')
                    )) . '</small>';
                if ($active && $deviceId !== '') {
                    $deviceHtml .= '<form method="post" action="/admin/devices/'
                        . $deviceId . '/deactivate" '
                        . 'data-confirm="Desativar este aparelho agora?">'
                        . '<input type="hidden" name="csrf_token" value="'
                        . $csrf . '"><button type="submit" '
                        . 'class="admin-slot-action">Liberar vaga</button></form>';
                }
                $deviceHtml .= '</article>';
            }
            $deviceHtml .= '</div>';
            $actions = '<details class="admin-password-action">'
                . '<summary class="admin-button admin-button-ghost">Alterar senha</summary>'
                . '<form method="post" action="/admin/users/'
                . $userId . '/password" class="admin-mini-form">'
                . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                . '<label>Nova senha<input type="password" '
                . 'name="customer_password" required minlength="8" maxlength="128" '
                . 'autocomplete="new-password"></label>'
                . '<label>Confirmar<input type="password" '
                . 'name="customer_password_confirmation" required minlength="8" '
                . 'maxlength="128" autocomplete="new-password"></label>'
                . '<button type="submit" class="admin-button admin-button-primary">'
                . 'Salvar senha</button></form></details>';
            $actions .= '<form method="post" action="/admin/users/'
                . $userId . '/status" data-confirm="'
                . self::escape($userActive
                    ? 'Bloquear este cliente e encerrar todas as sessões?'
                    : 'Desbloquear este cliente?') . '">'
                . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                . '<input type="hidden" name="status" value="'
                . ($userActive ? 'blocked' : 'active') . '">'
                . '<button type="submit" class="admin-button '
                . ($userActive ? 'admin-button-warn' : 'admin-button-success') . '">'
                . ($userActive ? 'Bloquear cliente' : 'Desbloquear cliente')
                . '</button></form>';
            $actions .= '<details class="admin-password-action">'
                . '<summary class="admin-button admin-button-ghost">Editar licença</summary>'
                . '<form method="post" action="/admin/licenses/'
                . $licenseId . '/policy" class="admin-mini-form">'
                . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                . '<label>Tipo de licença<select name="activation_policy">'
                . '<option value="platform_bundle"'
                . ($bundle ? ' selected' : '')
                . '>Pacote completo — Windows + macOS + Android</option>'
                . '<option value="single_device"'
                . ($bundle ? '' : ' selected')
                . '>Licença antiga — um aparelho</option></select></label>'
                . '<button type="submit" class="admin-button admin-button-primary">'
                . 'Salvar licença</button></form></details>';
            if ($active) {
                $actions .= '<form method="post" action="/admin/licenses/'
                    . $licenseId . '/revoke" '
                    . 'data-confirm="Revogar esta licença e encerrar todos os aparelhos?">'
                    . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                    . '<input type="hidden" name="reason" '
                    . 'value="Revogada manualmente no painel">'
                    . '<button type="submit" class="admin-button admin-button-danger">'
                    . 'Revogar licença</button></form>';
            } else {
                $actions .= '<form method="post" action="/admin/licenses/'
                    . $licenseId . '/reactivate">'
                    . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
                    . '<button type="submit" class="admin-button admin-button-success">'
                    . 'Reativar licença</button></form>';
            }
            $rows .= '<tr data-admin-row data-license-status="'
                . self::escape($active ? 'active' : 'revoked')
                . '" data-user-status="'
                . self::escape($userActive ? 'active' : 'blocked')
                . '"><td><strong>'
                . self::escape((string) ($license['display_name'] ?? ''))
                . '</strong><small>'
                . self::escape((string) ($license['email'] ?? ''))
                . '</small><span class="admin-status admin-status-'
                . ($userActive ? 'active' : 'blocked') . '">CLIENTE '
                . self::escape($userActive ? 'ATIVO' : 'BLOQUEADO')
                . '</span></td><td><span class="admin-status admin-status-'
                . ($active ? 'active' : 'revoked') . '">'
                . self::escape($active ? 'ATIVA' : 'REVOGADA')
                . '</span><span class="admin-plan-badge">'
                . self::escape($bundle
                    ? 'PACOTE 3 PLATAFORMAS'
                    : 'LICENÇA ANTIGA · 1 APARELHO')
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

        $eventRows = '';
        foreach ($events as $event) {
            if (!is_array($event)) {
                continue;
            }
            $metadata = json_decode(
                (string) ($event['metadata_json'] ?? ''),
                true
            );
            $metadata = is_array($metadata) ? $metadata : [];
            $email = (string) ($event['email'] ?? $metadata['email'] ?? '');
            $license = (string) ($event['license_id'] ?? '');
            $eventRows .= '<tr><td><strong>'
                . self::escape(self::eventLabel(
                    (string) ($event['event_type'] ?? '')
                )) . '</strong><small>'
                . self::escape((string) ($event['event_type'] ?? ''))
                . '</small></td><td>'
                . self::escape($email !== '' ? $email : '—')
                . '</td><td><code class="admin-id">'
                . self::escape($license !== '' ? $license : '—')
                . '</code></td><td>'
                . self::escape(self::date(
                    (string) ($event['created_at'] ?? '')
                )) . '</td></tr>';
        }
        if ($eventRows === '') {
            $eventRows = '<tr><td colspan="4" class="admin-empty">'
                . 'Nenhuma ação administrativa registrada.</td></tr>';
        }

        $orderRows = '';
        foreach ($orders as $order) {
            if (!is_array($order)) {
                continue;
            }
            $status = (string) ($order['status'] ?? '');
            $statusClass = match ($status) {
                'paid' => 'active',
                'provider_error', 'paid_review' => 'revoked',
                default => 'pending',
            };
            $plan = (string) ($order['plan_code'] ?? '');
            $planLabel = $plan === 'complete'
                ? 'Completo · 3 plataformas'
                : ($plan === 'single' ? 'Individual · 1 aparelho' : '—');
            $amount = self::money(
                (int) ($order['expected_amount_cents'] ?? 0),
                (string) ($order['currency'] ?? 'BRL')
            );
            $licenseId = (string) ($order['license_id'] ?? '');
            $orderRows .= '<tr><td><strong>'
                . self::escape((string) ($order['display_name'] ?? ''))
                . '</strong><small>'
                . self::escape((string) ($order['email'] ?? ''))
                . '</small></td><td><strong>'
                . self::escape($planLabel) . '</strong><small>'
                . self::escape($amount) . '</small></td><td><span class="admin-status admin-status-'
                . $statusClass . '">'
                . self::escape(self::orderStatusLabel($status))
                . '</span><small>Woovi: '
                . self::escape((string) ($order['provider_status'] ?? '—'))
                . '</small></td><td><code class="admin-id">'
                . self::escape(substr((string) ($order['order_id'] ?? ''), 0, 12))
                . '…</code><small>'
                . self::escape(self::date((string) ($order['created_at'] ?? '')))
                . '</small></td><td>'
                . ($licenseId === ''
                    ? '<span class="admin-muted">Ainda não liberada</span>'
                    : '<span class="admin-status admin-status-active">LIBERADA</span><small><code class="admin-id">'
                        . self::escape(substr($licenseId, 0, 12)) . '…</code></small>')
                . '</td></tr>';
        }
        if ($orderRows === '') {
            $orderRows = '<tr><td colspan="5" class="admin-empty">'
                . 'Nenhuma compra Pix recebida ainda.</td></tr>';
        }

        return '<!doctype html><html lang="pt-BR"><head>'
            . '<meta charset="utf-8"><meta name="viewport" '
            . 'content="width=device-width,initial-scale=1">'
            . '<title>Painel ChurchDAW</title>'
            . '<link rel="stylesheet" href="' . $this->asset() . '">'
            . '<script defer src="' . $this->script() . '"></script></head>'
            . '<body class="admin-body" data-page="admin"><main class="admin-shell">'
            . '<header class="admin-topbar"><div>'
            . '<span class="admin-kicker">CHURCHDAW / ADMIN</span>'
            . '<h1>Painel de licenças</h1></div>'
            . '<form method="post" action="/admin/logout">'
            . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
            . '<button class="admin-button admin-button-ghost" type="submit">'
            . 'Sair</button></form></header>'
            . $flashHtml
            . '<section class="admin-metrics">'
            . self::metric('Clientes', $counts['total_customers'] ?? 0)
            . self::metric('Licenças ativas', $counts['active_licenses'] ?? 0)
            . self::metric('Aparelhos ativos', $counts['active_devices'] ?? 0)
            . self::metric('Demos em curso', $counts['active_trials'] ?? 0)
            . self::metric('Clientes bloqueados', $counts['blocked_customers'] ?? 0)
            . self::metric('Revogadas', $counts['revoked_licenses'] ?? 0)
            . '</section>'
            . '<section class="admin-metrics admin-commerce-metrics">'
            . self::metric('Vendas Pix', $counts['paid_orders'] ?? 0)
            . self::metric('Faturamento Pix', self::money(
                (int) ($counts['gross_revenue_cents'] ?? 0),
                'BRL'
            ))
            . self::metric('Pix aguardando', $counts['pending_orders'] ?? 0)
            . self::metric('Exigem atenção', $counts['attention_orders'] ?? 0)
            . '</section>'
            . '<section class="admin-panel admin-issue">'
            . '<div><span class="admin-kicker">LIBERAÇÃO MANUAL</span>'
            . '<h2>Criar acesso de cliente</h2>'
            . '<p class="admin-muted">Crie manualmente o plano Individual de R$ 129,99 ou o Pacote Completo de R$ 199,90, '
            . 'sem registrar uma cobrança Pix. O pacote completo possui uma vaga '
            . 'para Windows, uma para macOS e uma para Android. Usa '
            . 'a senha que você escolher abaixo. Para uma conta existente, a senha '
            . 'também será atualizada.</p>'
            . '</div><form method="post" action="/admin/licenses/issue" '
            . 'class="admin-form admin-form-grid">'
            . '<input type="hidden" name="csrf_token" value="' . $csrf . '">'
            . '<label>Nome do cliente<input name="display_name" required '
            . 'maxlength="96" autocomplete="off"></label>'
            . '<label>E-mail do cliente<input type="email" name="email" '
            . 'required maxlength="254" autocomplete="off"></label>'
            . '<label>Senha do cliente<input type="password" '
            . 'name="customer_password" required minlength="8" maxlength="128" '
            . 'autocomplete="new-password" spellcheck="false"></label>'
            . '<label>Confirmar senha<input type="password" '
            . 'name="customer_password_confirmation" required minlength="8" '
            . 'maxlength="128" autocomplete="new-password" spellcheck="false"></label>'
            . '<label class="admin-plan-choice">Tipo de licença<select '
            . 'name="activation_policy" required>'
            . '<option value="platform_bundle" selected>Pacote Completo R$ 199,90 — '
            . '1 Windows + 1 macOS + 1 Android</option>'
            . '<option value="single_device">Plano Individual R$ 129,99 — '
            . '1 aparelho por vez</option></select></label>'
            . '<small class="admin-muted">Use de 8 até 128 '
            . 'caracteres. A senha nunca é exibida nem armazenada em texto.</small>'
            . '<button class="primary" type="submit">Liberar licença e definir senha</button>'
            . '</form></section>'
            . '<section class="admin-panel"><div class="admin-section-title">'
            . '<div><span class="admin-kicker">VENDAS / PIX</span>'
            . '<h2>Compras automáticas</h2></div>'
            . '<span class="admin-muted">Últimos 50 pedidos Woovi</span></div>'
            . '<div class="admin-table-wrap"><table class="admin-table admin-orders-table">'
            . '<thead><tr><th>Cliente</th><th>Plano</th><th>Pagamento</th>'
            . '<th>Pedido</th><th>Licença</th></tr></thead><tbody>'
            . $orderRows . '</tbody></table></div></section>'
            . '<section class="admin-panel"><div class="admin-section-title">'
            . '<div><span class="admin-kicker">CLIENTES</span>'
            . '<h2>Ativações e aparelhos</h2></div>'
            . '<span class="admin-muted">Últimas 100 licenças</span></div>'
            . '<div class="admin-toolbar">'
            . '<label class="admin-search">Pesquisar<input id="admin-search" '
            . 'type="search" placeholder="Nome, e-mail, aparelho ou licença"></label>'
            . '<label>Filtrar<select id="admin-status-filter">'
            . '<option value="all">Todos</option>'
            . '<option value="active">Licença ativa</option>'
            . '<option value="revoked">Licença revogada</option>'
            . '<option value="blocked">Cliente bloqueado</option>'
            . '</select></label><strong id="admin-result-count"></strong></div>'
            . '<div class="admin-table-wrap"><table class="admin-table">'
            . '<thead><tr><th>Cliente</th><th>Licença</th>'
            . '<th>Vagas por plataforma</th><th>Ações</th></tr></thead>'
            . '<tbody>' . $rows . '</tbody></table></div></section>'
            . '<section class="admin-panel"><div class="admin-section-title">'
            . '<div><span class="admin-kicker">AUDITORIA</span>'
            . '<h2>Histórico recente</h2></div>'
            . '<span class="admin-muted">Últimas 40 ações</span></div>'
            . '<div class="admin-table-wrap"><table class="admin-table admin-audit-table">'
            . '<thead><tr><th>Ação</th><th>Cliente</th><th>Licença</th>'
            . '<th>Data</th></tr></thead><tbody>' . $eventRows
            . '</tbody></table></div></section>'
            . '<footer class="admin-footer">Release '
            . self::escape($this->config->string('app.release'))
            . ' · checkout automático '
            . ($this->config->bool('checkout.enabled') ? 'ativo' : 'pausado')
            . '</footer>'
            . '</main></body></html>';
    }

    private function asset(): string
    {
        return '/account-assets/'
            . rawurlencode($this->config->string('app.release'))
            . '/app.css';
    }

    private function script(): string
    {
        return '/account-assets/'
            . rawurlencode($this->config->string('app.release'))
            . '/app.js';
    }

    private static function eventLabel(string $type): string
    {
        return match ($type) {
            'admin.license_issued' => 'Licença emitida',
            'admin.license_revoked' => 'Licença revogada',
            'admin.license_reactivated' => 'Licença reativada',
            'admin.license_policy_updated' => 'Tipo da licença atualizado',
            'admin.device_deactivated' => 'Aparelho desativado',
            'admin.customer_access_reset' => 'Código de acesso gerado',
            'admin.customer_password_changed' => 'Senha alterada',
            'admin.customer_blocked' => 'Cliente bloqueado',
            'admin.customer_unblocked' => 'Cliente desbloqueado',
            default => 'Ação administrativa',
        };
    }

    private static function metric(string $label, mixed $value): string
    {
        return '<article class="admin-metric"><span>'
            . self::escape($label) . '</span><strong>'
            . self::escape((string) $value) . '</strong></article>';
    }

    private static function orderStatusLabel(string $status): string
    {
        return match ($status) {
            'paid' => 'PAGO',
            'pending', 'creating', 'provider_ambiguous' => 'AGUARDANDO',
            'expired' => 'EXPIRADO',
            'provider_error' => 'ERRO NO PROVEDOR',
            'paid_review' => 'REVISAR PAGAMENTO',
            default => strtoupper($status !== '' ? $status : 'DESCONHECIDO'),
        };
    }

    private static function money(int $cents, string $currency): string
    {
        $prefix = strtoupper($currency) === 'BRL' ? 'R$ ' : strtoupper($currency) . ' ';
        return $prefix . number_format($cents / 100, 2, ',', '.');
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
