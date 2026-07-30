<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class Views
{
    public function __construct(private readonly Config $config)
    {
    }

    public function purchase(): string
    {
        $price = number_format(
            $this->config->int('product.price_cents') / 100,
            2,
            ',',
            '.'
        );
        return $this->layout(
            'Comprar ChurchDAW',
            'purchase',
            <<<HTML
            <main class="shell purchase-shell">
              <section class="hero">
                <div class="brand-mark" aria-hidden="true">†<span>▰▰▰</span></div>
                <p class="eyebrow">CHURCHDAW / DESKTOP</p>
                <h1>Seu instrumento.<br>Sua licença.</h1>
                <p class="hero-copy">Pianos, pads, órgãos, layers e plugins em um ambiente feito para tocar ao vivo.</p>
                <ul class="benefits">
                  <li>Licença perpétua para Windows e macOS</li>
                  <li>Um computador ativo por vez</li>
                  <li>Troca segura de máquina pela sua conta</li>
                </ul>
              </section>
              <section class="card">
                <div class="price-row">
                  <div><p class="eyebrow">LICENÇA DESKTOP</p><h2>ChurchDAW</h2></div>
                  <strong>R$ {$price}</strong>
                </div>
                <form id="purchase-form" novalidate>
                  <label>Nome completo<input name="display_name" autocomplete="name" maxlength="96" required></label>
                  <label>E-mail<input name="email" type="email" autocomplete="email" maxlength="254" required></label>
                  <label>Crie uma senha<input name="password" type="password" autocomplete="new-password" minlength="12" maxlength="128" required></label>
                  <p class="hint">Use pelo menos 12 caracteres. A senha nunca é enviada ao provedor de pagamento.</p>
                  <label class="check"><input name="accept_terms" type="checkbox" required><span>Li e aceito os <a href="/termos">Termos de compra</a> e a <a href="/politica-de-privacidade">Política de privacidade</a>.</span></label>
                  <button class="primary" type="submit">Continuar para o pagamento</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
                <p class="signin-link">Já comprou? <a href="/entrar">Acesse sua conta</a></p>
                <div class="secure-note">Pagamento processado no ambiente seguro do Mercado Pago.</div>
              </section>
            </main>
            HTML
        );
    }

    public function login(): string
    {
        return $this->layout(
            'Minha conta ChurchDAW',
            'login',
            <<<HTML
            <main class="shell account-shell">
              <section class="hero compact-hero">
                <p class="eyebrow">CHURCHDAW / CONTA</p>
                <h1>Gerencie sua licença.</h1>
                <p class="hero-copy">Veja o computador ativo e libere a licença antes de trocar de máquina.</p>
              </section>
              <section class="card">
                <h2>Entrar</h2>
                <form id="login-form" novalidate>
                  <label>E-mail<input name="email" type="email" autocomplete="email" required></label>
                  <label>Senha<input name="password" type="password" autocomplete="current-password" required></label>
                  <button class="primary" type="submit">Entrar na minha conta</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
                <p class="signin-link"><a href="/esqueci-senha">Esqueci minha senha</a> · <a href="/reenviar-confirmacao">Reenviar confirmação</a></p>
              </section>
            </main>
            HTML
        );
    }

    public function account(): string
    {
        return $this->layout(
            'Licença ChurchDAW',
            'account',
            <<<HTML
            <main class="dashboard">
              <header><div><p class="eyebrow">CHURCHDAW / CONTA</p><h1>Minha licença</h1></div><button id="logout-button" class="ghost">Sair</button></header>
              <section id="account-loading" class="card">Carregando sua conta…</section>
              <section id="account-content" class="card" hidden>
                <div class="identity"><div><span>Conta</span><strong id="account-name"></strong><small id="account-email"></small></div><span id="license-status" class="pill"></span></div>
                <div class="device-card">
                  <div class="device-icon">▣</div>
                  <div><span>COMPUTADOR ATIVO</span><strong id="device-label"></strong><small id="device-detail"></small></div>
                </div>
                <p id="transfer-note" class="hint"></p>
                <button id="deactivate-button" class="danger">Desativar este computador</button>
                <p id="account-status" class="status" role="status" aria-live="polite"></p>
              </section>
            </main>
            HTML
        );
    }

    public function resetPassword(): string
    {
        return $this->layout(
            'Redefinir senha ChurchDAW',
            'reset',
            <<<HTML
            <main class="shell account-shell">
              <section class="hero compact-hero"><p class="eyebrow">CHURCHDAW / SEGURANÇA</p><h1>Nova senha.</h1></section>
              <section class="card">
                <h2>Redefinir senha</h2>
                <form id="reset-form" novalidate>
                  <label>Nova senha<input name="password" type="password" autocomplete="new-password" minlength="12" maxlength="128" required></label>
                  <button class="primary" type="submit">Salvar nova senha</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
              </section>
            </main>
            HTML
        );
    }

    public function forgotPassword(): string
    {
        return $this->layout(
            'Recuperar senha ChurchDAW',
            'forgot',
            <<<HTML
            <main class="shell account-shell">
              <section class="hero compact-hero"><p class="eyebrow">CHURCHDAW / SEGURANÇA</p><h1>Recupere seu acesso.</h1><p class="hero-copy">Enviaremos um link de uso único, válido por 30 minutos.</p></section>
              <section class="card">
                <h2>Esqueci minha senha</h2>
                <form id="forgot-form" novalidate>
                  <label>E-mail<input name="email" type="email" autocomplete="email" maxlength="254" required></label>
                  <button class="primary" type="submit">Enviar link seguro</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
                <p class="signin-link"><a href="/entrar">Voltar para entrar</a></p>
              </section>
            </main>
            HTML
        );
    }

    public function resendVerification(): string
    {
        return $this->layout(
            'Confirmar e-mail ChurchDAW',
            'resend',
            <<<HTML
            <main class="shell account-shell">
              <section class="hero compact-hero"><p class="eyebrow">CHURCHDAW / CONTA</p><h1>Confirme seu e-mail.</h1><p class="hero-copy">Peça um novo link caso a primeira mensagem não tenha chegado.</p></section>
              <section class="card">
                <h2>Reenviar confirmação</h2>
                <form id="resend-form" novalidate>
                  <label>E-mail<input name="email" type="email" autocomplete="email" maxlength="254" required></label>
                  <button class="primary" type="submit">Reenviar link</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
                <p class="signin-link"><a href="/entrar">Voltar para entrar</a></p>
              </section>
            </main>
            HTML
        );
    }

    public function verifyEmail(): string
    {
        return $this->layout(
            'Confirmar e-mail ChurchDAW',
            'verify',
            <<<HTML
            <main class="message-page">
              <section class="card">
                <p class="eyebrow">CHURCHDAW / CONTA</p>
                <h1>Confirme seu e-mail.</h1>
                <p>Para evitar confirmações automáticas por leitores de e-mail, conclua a ação neste botão.</p>
                <form id="verify-form">
                  <button class="primary" type="submit">Confirmar meu e-mail</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
              </section>
            </main>
            HTML
        );
    }

    public function terms(): string
    {
        $price = number_format(
            $this->config->int('product.price_cents') / 100,
            2,
            ',',
            '.'
        );
        $support = htmlspecialchars(
            $this->config->string('support.url'),
            ENT_QUOTES | ENT_SUBSTITUTE,
            'UTF-8'
        );
        return $this->legalLayout(
            'Termos de compra',
            <<<HTML
            <h1>Termos de compra do ChurchDAW Desktop</h1>
            <p>Última atualização: 28 de julho de 2026.</p>
            <h2>Produto e preço</h2>
            <p>A compra de R$ {$price} concede uma licença perpétua de uso do ChurchDAW Desktop para Windows e macOS, vinculada à conta informada no pagamento.</p>
            <h2>Ativação</h2>
            <p>A licença permite um computador ativo por vez. A máquina pode ser liberada pela área da conta, sujeita aos limites de segurança exibidos ali.</p>
            <h2>Conta e segurança</h2>
            <p>Você deve informar dados verdadeiros, manter sua senha em sigilo e não compartilhar a conta ou tentar contornar a ativação.</p>
            <h2>Pagamento e entrega</h2>
            <p>O Mercado Pago processa o pagamento. A licença é emitida após a confirmação canônica do provedor; o acesso e a ativação ficam liberados depois da confirmação do e-mail da conta.</p>
            <h2>Cancelamento e suporte</h2>
            <p>Pedidos de cancelamento, reembolso, correção de cobrança ou problemas de ativação devem ser enviados pelo <a href="{$support}">suporte oficial</a>. Os direitos obrigatórios previstos na legislação aplicável permanecem preservados.</p>
            <h2>Disponibilidade</h2>
            <p>Atualizações, compatibilidade com sistemas e plugins de terceiros podem variar. A licença não transfere propriedade intelectual nem autoriza revenda.</p>
            HTML
        );
    }

    public function privacy(): string
    {
        $email = htmlspecialchars(
            $this->config->string('mail.reply_to'),
            ENT_QUOTES | ENT_SUBSTITUTE,
            'UTF-8'
        );
        return $this->legalLayout(
            'Política de privacidade',
            <<<HTML
            <h1>Política de privacidade do ChurchDAW</h1>
            <p>Última atualização: 28 de julho de 2026.</p>
            <h2>Dados tratados</h2>
            <p>Tratamos nome, e-mail, senha protegida por hash, pedidos, estado do pagamento, licença, identificador e nome do computador, plataforma, tokens de sessão protegidos e registros técnicos de segurança. Não recebemos nem armazenamos o número completo do cartão.</p>
            <h2>Finalidades</h2>
            <p>Usamos esses dados para criar a conta, confirmar o pagamento, emitir e proteger a licença, limitar a ativação a um computador, recuperar o acesso, prevenir fraude e atender suporte.</p>
            <h2>Compartilhamento</h2>
            <p>Dados estritamente necessários podem ser tratados pelo Mercado Pago, pelo provedor de hospedagem e pelo serviço de e-mail. Cada fornecedor também aplica seus próprios termos e políticas.</p>
            <h2>Retenção e segurança</h2>
            <p>Sessões expiradas são removidas após o período operacional; cargas de webhook processadas são mantidas por até 90 dias e eventos de auditoria por até 365 dias. Dados de compra e licença podem ser mantidos por prazos legais, fiscais, antifraude e de suporte. Segredos de dispositivo são cifrados e endereços IP são armazenados em forma derivada.</p>
            <h2>Seus direitos</h2>
            <p>Para solicitar acesso, correção, exclusão quando cabível ou esclarecer o tratamento, escreva para <a href="mailto:{$email}">{$email}</a>. Poderemos confirmar sua identidade antes de atender a solicitação.</p>
            HTML
        );
    }

    public function message(string $title, string $message, string $tone = 'ok'): string
    {
        $safeTitle = htmlspecialchars($title, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
        $safeMessage = htmlspecialchars($message, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
        $safeTone = preg_replace('/[^a-z-]/', '', $tone) ?: 'ok';
        return $this->layout(
            $title,
            'message',
            "<main class=\"message-page\"><section class=\"card {$safeTone}\"><h1>{$safeTitle}</h1><p>{$safeMessage}</p><a class=\"primary link-button\" href=\"/entrar\">Ir para minha conta</a></section></main>"
        );
    }

    private function legalLayout(string $title, string $content): string
    {
        return $this->layout(
            $title . ' — ChurchDAW',
            'legal',
            "<main class=\"message-page\"><article class=\"card legal-card\">{$content}<p><a href=\"/comprar\">Voltar para a compra</a></p></article></main>"
        );
    }

    private function layout(string $title, string $page, string $content): string
    {
        $safeTitle = htmlspecialchars($title, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
        $safePage = htmlspecialchars($page, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
        // APP_RELEASE is restricted by Config to [A-Za-z0-9._-]. Keeping it in
        // the path makes every release immutable at the CDN/browser boundary
        // and lets a rollback reuse the exact CSS/JS that shipped with it.
        $assetRelease = rawurlencode($this->config->string('app.release'));
        return <<<HTML
        <!doctype html>
        <html lang="pt-BR">
        <head>
          <meta charset="utf-8">
          <meta name="viewport" content="width=device-width, initial-scale=1">
          <meta name="color-scheme" content="dark">
          <title>{$safeTitle}</title>
          <link rel="stylesheet" href="/account-assets/{$assetRelease}/app.css">
          <script src="/account-assets/{$assetRelease}/app.js" defer></script>
        </head>
        <body data-page="{$safePage}">
          {$content}
        </body>
        </html>
        HTML;
    }
}
