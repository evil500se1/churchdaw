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
        $singlePrice = number_format(
            $this->config->int('plan.single.price_cents') / 100,
            2,
            ',',
            '.'
        );
        $completePrice = number_format(
            $this->config->int('plan.complete.price_cents') / 100,
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
                <p class="eyebrow">CHURCHDAW / LICENÇA PERPÉTUA</p>
                <h1>Escolha seu plano.<br>Toque sem mensalidade.</h1>
                <p class="hero-copy">Crie sua conta, pague por Pix e receba a licença automaticamente após a confirmação.</p>
                <ul class="benefits">
                  <li>Uma ativação para Windows</li>
                  <li>Uma ativação para macOS</li>
                  <li>Uma ativação para Android</li>
                </ul>
              </section>
              <section class="card">
                <div class="price-row"><div><p class="eyebrow">COMPRA SEGURA</p><h2>ChurchDAW</h2></div><strong>PIX</strong></div>
                <form id="purchase-form" novalidate>
                  <fieldset class="plan-picker">
                    <legend>Escolha seu plano</legend>
                    <label class="plan-option">
                      <input type="radio" name="plan_code" value="single" required>
                      <span><b>Individual</b><small>1 aparelho por vez</small></span>
                      <strong>R$ {$singlePrice}</strong>
                    </label>
                    <label class="plan-option is-featured">
                      <input type="radio" name="plan_code" value="complete" checked required>
                      <span><b>Pacote completo</b><small>1 Windows + 1 Mac + 1 Android</small></span>
                      <strong>R$ {$completePrice}</strong>
                    </label>
                  </fieldset>
                  <label>Nome completo<input name="display_name" autocomplete="name" maxlength="96" required></label>
                  <label>E-mail<input name="email" type="email" autocomplete="email" maxlength="254" required></label>
                  <label>Crie uma senha<input name="password" type="password" autocomplete="new-password" minlength="8" maxlength="128" required></label>
                  <p class="hint">Use pelo menos 8 caracteres. A senha nunca é enviada ao provedor de pagamento.</p>
                  <label class="check"><input name="accept_terms" type="checkbox" required><span>Li e aceito os <a href="/termos">Termos de compra</a> e a <a href="/politica-de-privacidade">Política de privacidade</a>.</span></label>
                  <button class="primary" type="submit">Gerar Pix seguro</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
                <section id="pix-payment" class="pix-payment" hidden>
                  <div class="pix-heading"><div><p class="eyebrow">PIX GERADO</p><h2>Escaneie ou copie</h2></div><span id="pix-live" class="pix-live">Aguardando</span></div>
                  <img id="pix-qr" alt="QR Code Pix da compra ChurchDAW" hidden>
                  <label>Código Pix copia e cola<textarea id="pix-code" readonly rows="4"></textarea></label>
                  <div class="pix-actions"><button id="pix-copy" class="primary" type="button">Copiar código Pix</button><a id="pix-open" class="ghost link-button" target="_blank" rel="noreferrer">Abrir pagamento</a></div>
                  <p id="pix-status" class="status" role="status" aria-live="polite">Aguardando o pagamento…</p>
                  <p class="hint">A confirmação é automática. Não feche esta página até a licença ser liberada.</p>
                </section>
                <p class="signin-link">Já comprou? <a href="/entrar">Acesse sua conta</a></p>
                <div class="secure-note">Pix processado pela Woovi. Sua senha nunca é enviada ao provedor.</div>
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
                <p class="hero-copy">Veja seus aparelhos ativos e libere a vaga certa antes de trocar de equipamento.</p>
              </section>
              <section class="card">
                <h2>Entrar</h2>
                <form id="login-form" novalidate>
                  <label>E-mail<input name="email" type="email" autocomplete="email" required></label>
                  <label>Senha<input name="password" type="password" autocomplete="current-password" required></label>
                  <button class="primary" type="submit">Entrar na minha conta</button>
                  <p id="form-status" class="status" role="status" aria-live="polite"></p>
                </form>
                <p class="signin-link"><a href="/esqueci-senha">Esqueci minha senha</a></p>
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
                <div id="license-plan" class="license-plan"></div>
                <div id="device-list" class="account-device-grid"></div>
                <p id="transfer-note" class="hint"></p>
                <p id="account-status" class="status" role="status" aria-live="polite"></p>
              </section>
            </main>
            HTML
        );
    }

    public function purchaseSuccess(): string
    {
        // Estes links precisam apontar somente para artefatos ja publicados.
        // A troca para uma nova versao ocorre junto com a promocao publica dos
        // tres instaladores, nunca antes dela.
        $windowsUrl = '/app/desktop/ChurchDAW-1.6.26-r2-Setup.exe';
        $macUrl = '/app/desktop/ChurchDAW-1.6.26.dmg';
        $androidUrl = '/app/android/ChurchDAW-1.6.26-signed.apk';

        return $this->layout(
            'Pagamento confirmado — ChurchDAW',
            'purchase-success',
            <<<HTML
            <main class="message-page success-page">
              <section class="card success-card">
                <header class="success-header">
                  <span class="success-check" aria-hidden="true">✓</span>
                  <div>
                    <p class="eyebrow">CHURCHDAW / LICENÇA LIBERADA</p>
                    <h1>Pagamento confirmado!</h1>
                    <p class="success-lead">Seu pagamento foi recebido e sua licença ChurchDAW já está pronta para usar.</p>
                  </div>
                </header>

                <section class="success-guide" aria-labelledby="success-guide-title">
                  <h2 id="success-guide-title">Agora é só começar</h2>
                  <ol class="success-steps">
                    <li><span>1</span><p><strong>Baixe</strong><br>o ChurchDAW para o seu aparelho.</p></li>
                    <li><span>2</span><p><strong>Instale e abra</strong><br>o aplicativo.</p></li>
                    <li><span>3</span><p><strong>Entre</strong><br>com os dados usados na compra.</p></li>
                  </ol>
                  <div class="credentials-note">
                    <span class="credentials-icon" aria-hidden="true">@</span>
                    <p><strong>Use o mesmo e-mail e a mesma senha que você criou na compra.</strong><br>Não precisa criar outra conta nem confirmar seu e-mail.</p>
                  </div>
                </section>

                <section class="success-downloads" aria-labelledby="download-title">
                  <div class="download-heading">
                    <div><p class="eyebrow">ESCOLHA SEU SISTEMA</p><h2 id="download-title">Baixe o ChurchDAW</h2></div>
                    <small>Versão mais recente</small>
                  </div>
                  <div class="download-grid">
                    <a class="download-card" href="{$windowsUrl}" download aria-label="Baixar ChurchDAW para Windows">
                      <span class="download-platform" aria-hidden="true">WIN</span>
                      <span class="download-copy"><strong>Windows</strong><small>Instalador .exe</small></span>
                      <span class="download-arrow" aria-hidden="true">↓</span>
                    </a>
                    <a class="download-card" href="{$macUrl}" download aria-label="Baixar ChurchDAW para macOS">
                      <span class="download-platform" aria-hidden="true">MAC</span>
                      <span class="download-copy"><strong>macOS</strong><small>Instalador .dmg</small></span>
                      <span class="download-arrow" aria-hidden="true">↓</span>
                    </a>
                    <a class="download-card" href="{$androidUrl}" download aria-label="Baixar ChurchDAW para Android">
                      <span class="download-platform" aria-hidden="true">APK</span>
                      <span class="download-copy"><strong>Android</strong><small>Celular ou tablet</small></span>
                      <span class="download-arrow" aria-hidden="true">↓</span>
                    </a>
                  </div>
                </section>

                <footer class="success-footer">
                  <p>Já instalou? Abra o ChurchDAW e faça seu primeiro acesso.</p>
                  <a class="ghost link-button success-account-link" href="/entrar">Acessar minha conta</a>
                </footer>
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
                  <label>Nova senha<input name="password" type="password" autocomplete="new-password" minlength="8" maxlength="128" required></label>
                  <p class="hint">Use pelo menos 8 caracteres.</p>
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
        $singlePrice = number_format(
            $this->config->int('plan.single.price_cents') / 100,
            2,
            ',',
            '.'
        );
        $completePrice = number_format(
            $this->config->int('plan.complete.price_cents') / 100,
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
            <p>Última atualização: 15 de agosto de 2026.</p>
            <h2>Produto e preço</h2>
            <p>O plano Individual custa R$ {$singlePrice} e permite um aparelho ativo por vez. O Pacote Completo custa R$ {$completePrice} e inclui uma vaga Windows, uma vaga macOS e uma vaga Android. Ambos são pagamentos únicos e concedem licença perpétua vinculada à conta cadastrada.</p>
            <h2>Ativação</h2>
            <p>No plano Individual, a vaga pode ser transferida entre aparelhos. No Pacote Completo, cada plataforma tem sua própria vaga e pode ser liberada separadamente pela área da conta, sujeita aos limites de segurança exibidos ali.</p>
            <h2>Conta e segurança</h2>
            <p>Você deve informar dados verdadeiros, manter sua senha em sigilo e não compartilhar a conta ou tentar contornar a ativação.</p>
            <h2>Pagamento e entrega</h2>
            <p>A Woovi processa o Pix. A licença é emitida somente após o servidor confirmar diretamente com o provedor o identificador, o valor e o estado concluído da cobrança. Depois da confirmação, basta entrar no aplicativo com o mesmo e-mail e a mesma senha criados na compra.</p>
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
            <p>Última atualização: 15 de agosto de 2026.</p>
            <h2>Dados tratados</h2>
            <p>Tratamos nome, e-mail, senha protegida por hash, pedidos, estado do pagamento, licença, identificador e nome do computador, plataforma, tokens de sessão protegidos e registros técnicos de segurança. Não recebemos nem armazenamos o número completo do cartão.</p>
            <h2>Finalidades</h2>
            <p>Usamos esses dados para criar a conta, confirmar o pagamento, emitir e proteger a licença, limitar cada ativação à sua vaga de plataforma, recuperar o acesso, prevenir fraude e atender suporte.</p>
            <h2>Compartilhamento</h2>
            <p>Dados estritamente necessários podem ser tratados pela Woovi, pelo provedor de hospedagem e pelo serviço de e-mail. Cada fornecedor também aplica seus próprios termos e políticas.</p>
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
