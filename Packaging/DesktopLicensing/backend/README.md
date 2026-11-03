# ChurchDAW Desktop Licensing Backend

Backend de conta, compra, pagamento, licença e ativação do ChurchDAW Desktop.
Este documento é o runbook de produção para HostGator compartilhada/cPanel,
Cloudflare e Mercado Pago.

## Estado de liberação

Não publicar enquanto qualquer item em **Bloqueios atuais** permanecer aberto.
Em 28 de julho de 2026, `churchdaw.com` ainda servia o site existente diretamente
pela HostGator e `/api/v1/health` retornava a página 404 da hospedagem.

O backend exige:

- HTTPS de ponta a ponta;
- apenas `public/` acessível pela Web;
- PHP 8.2 ou superior;
- MySQL 5.7.44 ou superior, ou MariaDB 10.6 ou superior;
- os workers de webhook, e-mail e manutenção ativos;
- credenciais produtivas do Mercado Pago;
- chave privada RSA-3072 correspondente à chave pública embutida no aplicativo;
- backup externo e restauração testada.

## Topologia segura

Estrutura recomendada:

```text
/home/USUARIO/churchdaw/
  releases/
    20260728-150000/
      bin/
      config/
      migrations/
      public/
      src/
  shared/
    config/local.php
    keys/churchdaw-license-private.pem
  current -> releases/20260728-150000

/home/USUARIO/logs/
/home/USUARIO/tmp/
```

A DocumentRoot deve ser:

```text
/home/USUARIO/churchdaw/current/public
```

Nunca publicar `src/`, `config/`, `migrations/`, `bin/`, backups, logs ou chaves.
Nunca colocar a árvore inteira do backend sob `public_html`.

### Restrição do domínio principal no cPanel

O usuário comum do cPanel não consegue alterar a DocumentRoot do domínio
principal. A mudança depende da HostGator/WHM. Referência:

<https://docs.cpanel.net/cpanel/domains/domains/manage-the-domain/110/>

Antes do deploy, confirmar em **cPanel > Domínios** se `churchdaw.com` é o domínio
principal e abrir um chamado para apontar a DocumentRoot para `current/public`.

Se a HostGator não fizer a alteração, há somente estas opções seguras:

1. criar em `public_html/index.php` um front-controller versionado que carregue
   `/home/USUARIO/churchdaw/current/src/autoload.php` e instancie `App` com esse
   project root, mantendo todo o restante fora de `public_html`; ou
2. hospedar a aplicação em um subdomínio com DocumentRoot própria e alterar de
   forma coordenada `APP_BASE_URL` e a URL usada pelos clientes desktop.

O front-controller alternativo deve equivaler a:

```php
<?php

declare(strict_types=1);

use ChurchDAW\Licensing\App;

$projectRoot = '/home/USUARIO/churchdaw/current';
require $projectRoot . '/src/autoload.php';

(new App($projectRoot))->run();
```

Os assets de `current/public/account-assets` também precisam ser publicados sem expor a
árvore privada.

### Site existente

`public/.htaccess` define `DirectoryIndex index.php`, e o backend atende `/`.
Portanto, a instalação padrão substitui a home existente. Antes do corte:

- decidir formalmente se o site atual será substituído;
- salvar uma cópia dos arquivos atuais;
- verificar conflitos com `/assets`;
- se os dois sites precisarem coexistir, criar roteamento explícito e não usar o
  `.htaccess` atual sem adaptação.

## Runtime

### PHP e Apache

Usar a mesma versão de PHP no domínio e nos crons. Preferir PHP 8.5 quando
disponível; o mínimo aceito pelo código é PHP 8.2.

Extensões obrigatórias:

```text
pdo_mysql
curl
openssl
sodium
mbstring
```

Também são necessários:

- Apache com `mod_rewrite`;
- `mod_headers` para todos os headers adicionais;
- saída TCP/443 para `api.mercadopago.com`;
- função `mail()` operacional enquanto não houver transporte SMTP/API;
- suporte a `.htaccess`, `Require` e `Options -Indexes`.

Validar no shell:

```bash
/opt/cpanel/ea-php84/root/usr/bin/php -v
/opt/cpanel/ea-php84/root/usr/bin/php -m
```

Nesta conta, o runtime auditado é `ea-php84`. Referência dos binários
de cron:

<https://suporte.hostgator.com.br/hc/pt-br/articles/30812660878867-Quais-s%C3%A3o-os-caminhos-para-a-execu%C3%A7%C3%A3o-de-comandos-em-um-Cron-Job>

Configuração PHP recomendada, por MultiPHP INI ou `.user.ini`:

```ini
display_errors=Off
display_startup_errors=Off
log_errors=On
error_log=/home/USUARIO/logs/churchdaw-php.log
expose_php=Off
date.timezone=UTC
zend.exception_ignore_args=On
```

Não exibir erros em respostas de produção.

### Permissões

```text
diretórios privados                 0750
arquivos PHP e configuração         0640
chave privada RSA                   0600
public/                             0755
arquivos públicos                   0644
```

O usuário que executa o PHP e o cron precisa ler `shared/config/local.php` e a
chave privada. Nunca usar permissão `777`.

## Configuração e segredos

Preferir um único `config/local.php` privado, compartilhado pelas requisições
Web e pelos crons. Outra opção é definir `CHURCHDAW_CONFIG_FILE`, mas a variável
precisa chegar de forma idêntica ao PHP Web e ao PHP CLI.

Configuração mínima de produção:

```php
<?php

declare(strict_types=1);

return [
    'app.env' => 'production',
    'app.release' => '2026-07-28.1',
    'app.base_url' => 'https://churchdaw.com',
    'app.trusted_proxy' => 'CIDRS_ATUAIS_DA_CLOUDFLARE',
    'app.encryption_key' => 'BASE64_DE_32_BYTES_ALEATORIOS',
    'app.ip_hash_key' => 'MINIMO_32_CARACTERES_ALEATORIOS',

    'db.dsn' => 'mysql:host=localhost;dbname=USUARIO_churchdaw;charset=utf8mb4',
    'db.user' => 'USUARIO_churchdaw_app',
    'db.password' => 'SENHA_FORTE_E_UNICA',

    'license.private_key_path' =>
        '/home/USUARIO/churchdaw/shared/keys/churchdaw-license-private.pem',
    'license.key_id' => 'churchdaw-2026-01',
    'license.lease_hours' => 24,
    'license.offline_days' => 7,
    'license.refresh_days' => 30,
    'license.max_transfers_30d' => 2,

    'mercado_pago.access_token' => 'APP_USR-...',
    'mercado_pago.webhook_secret' => 'SEGREDO_DO_WEBHOOK',
    'mercado_pago.collector_id' => 'ID_NUMERICO_DO_VENDEDOR',
    'mercado_pago.application_id' => 'ID_NUMERICO_DA_APLICACAO',
    'mercado_pago.sandbox' => false,

    'checkout.expiration_minutes' => 4320,
    'checkout.claim_ttl_seconds' => 120,
    'checkout.enabled' => false,

    'product.code' => 'churchdaw-desktop-perpetual',
    'product.name' => 'ChurchDAW Windows, macOS e Android',
    'product.currency' => 'BRL',
    'product.price_cents' => 19999,

    'legal.terms_version' => '2026-08-15',
    'legal.privacy_version' => '2026-08-15',

    'mail.from' => 'contato@churchdaw.com',
    'mail.reply_to' => 'contato@churchdaw.com',
    'mail.enabled' => true,
    'support.url' => 'https://churchdaw.com/#duvidas',
];
```

Gerar segredos localmente e transferi-los por canal seguro:

```bash
php -r 'echo base64_encode(random_bytes(32)), PHP_EOL;'
php -r 'echo bin2hex(random_bytes(32)), PHP_EOL;'
```

Não executar esses comandos em terminal gravado ou compartilhado. Não registrar
segredos no Git, em tickets, screenshots, logs ou histórico de shell.

### Segredos que não podem ser perdidos

- `APP_ENCRYPTION_KEY` cifra dados de dispositivo, sessão e fila de e-mail;
- a chave RSA assina leases offline;
- `IP_HASH_KEY` mantém a consistência de identificadores derivados;
- `MP_WEBHOOK_SECRET` autentica notificações.

Uma restauração sem as mesmas chaves pode deixar dados cifrados ilegíveis ou
impedir o aplicativo de validar licenças. Guardar cópia externa criptografada.

## Banco de dados

Criar banco e usuário pelo cPanel, usando InnoDB e `utf8mb4`. Separar, quando
possível:

- usuário de migração, com privilégios de DDL;
- usuário da aplicação, com `SELECT`, `INSERT`, `UPDATE` e `DELETE`.

### Compatibilidade obrigatória

Antes de qualquer DDL, `bin/migrate.php` consulta `SELECT VERSION()` e aceita:

- MySQL 5.7.44 ou superior;
- MariaDB 10.6 ou superior.

Versões inferiores são rejeitadas. As invariantes de slot não dependem de
`CHECK`, que o MySQL 5.7 apenas analisa e ignora: `open_slot` e os dois
`active_slot` são colunas `STORED GENERATED` cobertas por índices únicos. O
estado autoritativo que decide se `paid_review` ainda reserva checkout fica em
`orders.reserves_checkout_slot`; o PHP nunca grava as três colunas geradas.
As migrations fixam todas as tabelas em InnoDB, `utf8mb4_unicode_ci` e
`ROW_FORMAT=DYNAMIC`, mesmo quando o banco do cPanel foi criado com outro
charset padrão.

Confirmar também manualmente:

```sql
SELECT VERSION() AS version, @@sql_mode AS sql_mode;
```

### Migração inicial e upgrades

1. colocar checkout em manutenção;
2. fazer dump consistente e transferi-lo para fora da HostGator;
3. validar lint e configuração da nova release;
4. executar a migração com o PHP CLI correto;
5. verificar todas as versões aplicadas;
6. executar os smoke tests;
7. somente então reabrir o checkout.

Comando:

```bash
/opt/cpanel/ea-php84/root/usr/bin/php -q \
  /home/USUARIO/churchdaw/current/bin/migrate.php
```

Saída esperada:

```text
DATABASE MySQL 5.7.44 SUPPORTED
APPLIED 001_initial.sql
...
READY product=churchdaw-desktop-perpetual
```

Verificação:

```sql
SELECT version, applied_at
FROM schema_migrations
ORDER BY version;

SELECT code, currency, price_cents, status
FROM products;
```

Não executar `tests/integration.php` contra produção. As migrações contêm DDL
com autocommit e não têm down migrations; testar antes em uma cópia do banco.

## Deploy versionado

1. enviar a nova release para `releases/AAAAmmdd-HHMMSS`;
2. verificar que não contém `config/local.php`, `.env`, chaves ou logs;
3. ligar `release/config/local.php` ao arquivo privado compartilhado;
4. rodar lint em todos os PHP;
5. validar `Config::assertRuntimeReady()` em CLI;
6. executar migrações;
7. trocar `current` de forma atômica;
8. limpar ou revalidar OPcache, se necessário;
9. executar os smoke tests;
10. observar logs e filas antes de reabrir compras.

O rollback de código pode apontar `current` para a release anterior somente se
ela for compatível com o schema já migrado.

Não restaurar cegamente um dump antigo depois de receber pagamentos: webhooks
já confirmados com HTTP 200 podem desaparecer do banco. Para rollback de dados,
congelar compras, restaurar em ambiente separado e reconciliar o período com o
Mercado Pago antes do corte.

## Workers e cron

Os scripts são exclusivamente CLI e não devem ficar acessíveis pela Web.

### Limite da HostGator

Hospedagem compartilhada permite cron no mínimo a cada 15 minutos:

<https://suporte.hostgator.com.br/hc/pt-br/articles/30810523318419-Como-gerenciar-um-trabalho-Cron>

Isso implica atraso de até aproximadamente 15 minutos tanto na licença após o
pagamento quanto no envio de confirmação/recuperação de e-mail. Se esse SLA for
inaceitável, usar VPS, fila gerenciada ou scheduler externo autenticado.

### Webhooks

```cron
*/15 * * * * /usr/bin/flock -n /home/USUARIO/tmp/churchdaw-webhooks.lock /opt/cpanel/ea-php84/root/usr/bin/php -q /home/USUARIO/churchdaw/current/bin/process-webhooks.php 25 >> /home/USUARIO/logs/churchdaw-webhooks.log 2>&1
```

O worker busca dados canônicos no Mercado Pago, usa backoff exponencial e sai
com código não-zero enquanto houver retry/dead-letter. Monitorar eventos
`failed`, eventos com `attempts >= 10` e itens `processing` há mais de cinco
minutos. Depois de corrigir a causa de um dead-letter:

```bash
php bin/requeue-webhook.php UUID_DO_EVENTO
```

### Reconciliação de pagamentos

```cron
7,22,37,52 * * * * /usr/bin/flock -n /home/USUARIO/tmp/churchdaw-reconcile.lock /opt/cpanel/ea-php84/root/usr/bin/php -q /home/USUARIO/churchdaw/current/bin/reconcile-payments.php 100 >> /home/USUARIO/logs/churchdaw-reconcile.log 2>&1
```

Esse worker recupera preferências com resultado ambíguo, encontra pagamentos
mesmo sem webhook, revisa reembolsos/mediações, detecta Merchant Orders
duplicados e libera checkout abandonado somente quando o GET canônico informa
`preference_expired`, a margem de segurança já passou e não existe pagamento
pendente/pago. Uma Merchant Order com `status=expired`, valores pago/reembolsado
zerados e todos os pagamentos rejeitados/cancelados/devolvidos também pode ser
encerrada por essa confirmação canônica. Falhas registram
tentativa e erro sem impedir que os pedidos seguintes sejam examinados.

`paid_review` é deliberadamente sticky: webhooks posteriores atualizam as
evidências e podem revogar o entitlement, mas nunca concedem/reativam licença
nem removem a revisão. `paid_review`, `partial_refund`, `in_mediation` e
`charged_back` bloqueiam nova cobrança até resolução administrativa. Um
pagamento rejeitado mantém a preferência original e o slot de checkout enquanto
sua expiração canônica não for confirmada.

Os agregados da Merchant Order precisam corresponder exatamente aos Payments
canônicos, IDs repetidos são rejeitados e sobrepagamento concede no máximo uma
licença, mantendo o pedido em `paid_review`. Reembolsos são somados entre
Merchant Orders duplicadas; qualquer disputa durante a revisão revoga o
entitlement de forma conservadora. Chargebacks com vários pagamentos são
validados por inteiro antes de uma única transação atômica.

### Fila de e-mail

```cron
2,17,32,47 * * * * /usr/bin/flock -n /home/USUARIO/tmp/churchdaw-emails.lock /opt/cpanel/ea-php84/root/usr/bin/php -q /home/USUARIO/churchdaw/current/bin/process-emails.php 50 >> /home/USUARIO/logs/churchdaw-emails.log 2>&1
```

Destinatário, assunto e corpo ficam cifrados no banco. O worker faz até dez
tentativas com atraso crescente. A entrega ainda depende de `mail()`; validar
SPF, DKIM, DMARC, Return-Path e entrega real em Gmail/Outlook. O processo sai
com código não-zero enquanto houver retry/dead-letter; depois de corrigir a
causa, use `php bin/requeue-email.php UUID_DO_JOB`.

### Manutenção

```cron
17 3 * * * /usr/bin/flock -n /home/USUARIO/tmp/churchdaw-maintenance.lock /opt/cpanel/ea-php84/root/usr/bin/php -q /home/USUARIO/churchdaw/current/bin/maintenance.php >> /home/USUARIO/logs/churchdaw-maintenance.log 2>&1
```

Confirmar `/usr/bin/flock` no servidor. Se não existir, implementar lock global
no próprio worker antes de ativar crons concorrentes.

Rotacionar os quatro logs fora da DocumentRoot e alertar se qualquer cron deixar
de produzir saída.

## Mercado Pago

Criar uma aplicação Checkout Pro separada para produção. Configurar:

```text
URL:
https://churchdaw.com/api/v1/webhooks/mercado-pago
```

### Tópicos

Alvo obrigatório de produção:

| Evento no painel | Tópico | Situação atual |
|---|---|---|
| Pagamentos | `payment` | Implementado |
| Chargebacks | `topic_chargebacks_wh` | Implementado |

O mesmo receptor aceita `type=payment` e `type=topic_chargebacks_wh`. O ACK
somente valida e persiste. No worker, pagamentos são conferidos pelo Payment e
Merchant Order canônicos; contestações consultam também
`GET /v1/chargebacks/{id}`, validam o vínculo ao pagamento e revogam pedido,
licença, ativação e refresh token numa transação local. Configure os dois
tópicos na mesma URL.

O código depende da configuração central em **Suas integrações** e não envia
`notification_url` na preferência. Configurar modo de produção, tópico
`payment`, `topic_chargebacks_wh`, URL HTTPS e segredo exato. Documentação:

<https://www.mercadopago.com.br/developers/pt/docs/checkout-pro/payment-notifications>

O endpoint:

1. exige JSON;
2. valida `X-Signature`, `X-Request-ID` e `data.id`;
3. persiste o evento;
4. responde HTTP 200;
5. deixa consulta e efeito financeiro para o worker.

O Mercado Pago espera 200/201 em até 22 segundos e inicia retries quando não
recebe confirmação. Não consultar a API do provedor dentro do request de
notificação.

### Teste

1. salvar URL e tópico no painel;
2. usar o simulador de webhook;
3. confirmar HTTP 200;
4. verificar a linha em `webhook_events`;
5. executar `process-webhooks.php`;
6. confirmar `processed`;
7. realizar compra produtiva controlada;
8. validar pagamento, licença e e-mail;
9. executar reembolso controlado e validar revogação;
10. executar chargeback controlado e validar a revogação completa.

### Reconciliação

Após restore, indisponibilidade longa ou falha de fila, comparar:

- pedidos locais pendentes/pagos/em revisão;
- pagamentos e pedidos comerciais canônicos do Mercado Pago;
- reembolsos e chargebacks;
- licenças ativas e revogadas.

Executar `php bin/reconcile-payments.php 500` repetidamente até cobrir a fila.
O comando consulta preferências, Merchant Orders e Payments canônicos, registra
erros por pedido e retorna código não-zero se algum item falhar. A busca de
Merchant Orders por referência cobre os últimos 90 dias; ordens mais antigas
com `provider_order_id` conhecido continuam sendo consultadas diretamente.

## Cloudflare

Antes de trocar nameservers:

1. exportar/inventariar a zona HostGator;
2. copiar A, AAAA, CNAME, MX, SPF, DKIM, DMARC e validações;
3. confirmar e-mail e AutoSSL;
4. deixar somente A/AAAA/CNAME de Web como **Proxied**;
5. manter registros de e-mail como **DNS only**.

Usar SSL/TLS **Full (strict)**, nunca Flexible:

<https://developers.cloudflare.com/ssl/origin-configuration/ssl-modes/full-strict/>

O certificado do origin precisa estar válido, não expirado e cobrir
`churchdaw.com` e `www.churchdaw.com`.

### Proxy confiável

Copiar os CIDRs atuais IPv4 e IPv6 da fonte oficial antes de cada deploy:

<https://www.cloudflare.com/ips/>

Colocá-los, separados por vírgula, em `app.trusted_proxy`. Sem isso, rate limit e
auditoria verão IPs da Cloudflare, não do cliente. Nunca confiar globalmente em
`CF-Connecting-IP` ou `X-Forwarded-For`.

Se a HostGator permitir, restringir o origin aos IPs da Cloudflare. Caso não
permita, registrar que o IP histórico do origin pode contornar WAF/CDN.

### Cache e WAF

Criar regra **Bypass cache** para:

```text
/api/*
/entrar
/minha-conta
/comprar
/compra/*
/verificar-email
/redefinir-senha
/esqueci-senha
/reenviar-confirmacao
```

Permitir cache somente para assets públicos versionados.

No webhook, remover desafios de navegador/bot somente para:

```text
host = churchdaw.com
path = /api/v1/webhooks/mercado-pago
method = POST
```

Não remover a validação HMAC e não transformar `X-Signature`,
`X-Request-ID`, query string ou corpo JSON.

O aplicativo envia HSTS com `includeSubDomains`; confirmar TLS em todos os
subdomínios Web antes do corte.

## E-mail

- criar `contato@churchdaw.com`;
- publicar SPF, DKIM e DMARC;
- manter registros de e-mail como DNS-only no Cloudflare;
- confirmar alinhamento do envelope/Return-Path produzido por `mail()`;
- testar Gmail, Outlook e uma caixa HostGator;
- monitorar `email_jobs` falhos e `churchdaw-emails.log`.

Links de confirmação e redefinição usam fragmento `#token=...`. O JavaScript
remove o fragmento imediatamente e envia o token somente no corpo JSON do POST;
as rotas não aceitam fallback pela query string. Em produção `MAIL_ENABLED=1` é
obrigatório e a inicialização falha se o envio estiver desativado.

## Backups

Não depender apenas da rotina interna da HostGator. Em hospedagem compartilhada,
restauração completa pode exigir chamado:

<https://suporte.hostgator.com.br/hc/pt-br/articles/30813484137491-Como-fazer-um-backup-completo>

Política mínima:

| Backup | Retenção |
|---|---:|
| Banco diário criptografado e externo | 14 dias |
| Banco semanal | 8 semanas |
| Banco mensal | 12 meses |
| Snapshot pré-migração | 90 dias |
| Releases | atual e duas anteriores |
| Configuração/chaves | cópia externa criptografada |

Usar dump consistente de InnoDB, checksum e transferência para outro provedor.
Não manter o único backup na mesma conta cPanel.

### Restore drill

Trimestralmente:

1. criar banco vazio isolado;
2. restaurar o dump;
3. usar uma configuração de staging;
4. executar migrações restantes;
5. validar schema e produto;
6. executar smoke de conta/licença sem cobrança real;
7. registrar tempo, resultado, checksum e responsável.

### Restauração de produção

1. bloquear checkout, mas não devolver falso HTTP 200 no webhook;
2. preservar logs e fazer snapshot do estado quebrado;
3. restaurar banco e release compatíveis;
4. restaurar exatamente as mesmas chaves;
5. aplicar migrações;
6. iniciar workers;
7. reconciliar pagamentos, reembolsos e chargebacks do período;
8. conferir licenças;
9. somente então reabrir checkout.

RPO do dump diário: até 24 horas. Após restore, o reconciliador recupera estados
canônicos disponíveis no Mercado Pago, mas ainda é obrigatório rodar o comando,
resolver todos os erros e conferir manualmente o período restaurado antes de
reabrir vendas.

## Retenção

`bin/maintenance.php` implementa:

| Dado | Retenção/ação |
|---|---|
| pedidos abertos vencidos | reconciliados; só ficam `expired` após confirmação canônica |
| nonces vencidos | removidos no próximo maintenance |
| rate limits | 2 dias |
| tokens de usuário usados/vencidos | 7 dias |
| sessões Web expiradas/revogadas | 7 dias |
| refresh tokens expirados/revogados | 30 dias |
| webhooks processados | 90 dias |
| e-mails enviados | 7 dias |
| e-mails falhos após 10 tentativas | 30 dias |
| eventos de auditoria | 365 dias |

Sem expiração automática:

- usuários;
- pedidos e pagamentos;
- licenças, dispositivos, ativações e transferências;
- webhooks falhos/dead-letter;
- logs Apache, PHP e Cloudflare;
- backups externos.

Definir retenção operacional para logs e backups e manter a política pública de
privacidade coerente. Webhooks falhos hoje podem ficar indefinidamente e exigem
correção ou justificativa.

## Monitoramento

Alertar para:

```sql
SELECT status, COUNT(*) FROM webhook_events GROUP BY status;
SELECT id, attempts, received_at, last_error
FROM webhook_events
WHERE status = 'failed' OR attempts >= 10;

SELECT status, COUNT(*) FROM email_jobs GROUP BY status;
SELECT id, type, attempts, created_at, last_error
FROM email_jobs
WHERE status = 'failed' OR attempts >= 10;

SELECT status, COUNT(*) FROM orders GROUP BY status;
SELECT id, user_id, updated_at
FROM orders
WHERE status IN (
    'paid_review', 'partial_refund', 'in_mediation', 'charged_back'
);

SELECT id, provider_reconcile_failures, provider_reconcile_attempted_at,
       provider_reconcile_error
FROM orders
WHERE provider_reconcile_failures > 0;
```

Sinais de incidente:

- health fora do ar;
- cron sem execução;
- qualquer `paid_review`, `partial_refund`, `in_mediation` ou `charged_back`;
- webhook com três ou mais falhas;
- dead-letter de webhook/e-mail;
- crescimento anormal de `provider_error`;
- falha de login em massa;
- disco/cota cPanel acima de 80%;
- certificado perto de expirar;
- segredo ou chave exposta.

O health expõe o identificador da release, a lista completa de migrations
aplicadas/esperadas e se o checkout está habilitado. Ele responde 503 se houver
migration ausente, inesperada ou fora da lista exata da release. Ainda não
prova conectividade real com Mercado Pago nem entrega de e-mail; não automatizar
rollback somente por esse endpoint.

## Smoke tests

### HTTP e segurança

```bash
curl -fsS https://churchdaw.com/api/v1/health
curl -i http://churchdaw.com/
curl -i https://churchdaw.com/config/local.php
curl -i https://churchdaw.com/migrations/001_initial.sql
curl -i -X POST -H 'Content-Type: text/plain' \
  https://churchdaw.com/api/v1/auth/login
```

Esperado:

- health HTTP 200 com JSON, release correta e schema atual igual ao esperado;
- HTTP redireciona para HTTPS;
- config, migrations, chave e logs retornam 403/404;
- payload não JSON retorna 415;
- APIs enviam `Cache-Control: no-store`;
- resposta inclui `X-Request-ID`, CSP e HSTS;
- com Cloudflare, API fica `DYNAMIC`/`BYPASS`, nunca HIT.

### Conta e licença

- cadastro e aceite das versões jurídicas;
- e-mail de confirmação e consumo único do token;
- login Web e desktop;
- recuperação de senha;
- compra aprovada;
- Pix/boleto pendente;
- split payment, reembolso parcial/total e pagamento tardio;
- revogação em chargeback;
- uma única licença ativa por produto;
- ativação em um computador;
- bloqueio do segundo computador;
- desativação e transferência;
- rotação e rejeição de replay do refresh token;
- lease online e janela offline;
- expiração/revogação refletida no cliente.

### Operação

- migração em banco vazio e banco atualizado;
- worker de webhook;
- worker de e-mail;
- manutenção;
- restore drill;
- simulação de webhook;
- compra produtiva controlada e reembolso;
- SPF/DKIM/DMARC e entrega real;
- corte e rollback de release.

## Checklist de liberação

- [ ] decisão documentada: substituir ou coexistir com o site atual;
- [ ] DocumentRoot expõe somente `public/`, ou front-controller seguro aprovado;
- [ ] nenhum segredo/arquivo privado acessível por HTTP;
- [ ] PHP Web e CLI na mesma versão suportada;
- [ ] extensões obrigatórias confirmadas;
- [x] MySQL 5.7.44 da conta é compatível em laboratório; MariaDB >= 10.6 também
      passou a mesma suíte;
- [ ] migrations aplicadas e registradas;
- [ ] config e chaves fora da DocumentRoot e em backup;
- [ ] chave RSA confere com o cliente;
- [ ] crons de webhook, reconciliação, e-mail e manutenção ativos;
- [ ] atraso de 15 minutos aceito ou arquitetura substituída;
- [ ] Mercado Pago produtivo, `payment` configurado e simulado;
- [x] suporte canônico ao tópico dedicado de chargeback implementado;
- [x] reconciliador de pagamentos/restores implementado;
- [ ] e-mail com SPF, DKIM e DMARC validado;
- [x] tokens removidos da query string;
- [x] pausa operacional de checkout implementada com `CHECKOUT_ENABLED=0`;
- [ ] `CHECKOUT_ENABLED=1` somente após smoke e compra controlada;
- [ ] Cloudflare Full (strict), proxy confiável, bypass e WAF validados;
- [ ] backup externo e restore drill aprovados;
- [ ] retenção de logs, backups e dead-letter definida;
- [ ] smoke completo e compra/reembolso controlados aprovados;
- [ ] monitoramento e contatos de incidente definidos;
- [ ] política de privacidade corresponde à retenção real.

## Bloqueios atuais

1. O overlay seguro de coexistência foi confrontado com a árvore e o
   `.htaccess` auditados, mas ainda não foi enviado nem ativado. O guard de hash
   deve passar novamente imediatamente antes do corte.
2. A conta usa MySQL 5.7.44-48 compatível, porém ainda não há banco, usuário,
   crons nem backup/restore drill configurados no cPanel.
3. `/comprar` e `/api/v1/health` ainda retornam 404 porque o overlay não foi
   publicado; `CHECKOUT_ENABLED` deve permanecer `0`.
4. SPF está inválido/PERMERROR, o cPanel não reconhece a chave pública DKIM e
   DMARC está ausente; e-mail real não pode ser aprovado nesse estado.
5. Cloudflare, headers globais da landing, proxy confiável, bypass e WAF ainda
   precisam ser ativados e validados.
6. A chave RSA de assinatura fica como PEM legível pelo PHP na hospedagem
   compartilhada; não há KMS/HSM nem rotação com múltiplos key IDs.
7. O cron mínimo de 15 minutos atrasa licença e e-mail.
8. O mailer usa `mail()`, sem SMTP/API e telemetria de entrega.
9. Health não testa dependências externas (Mercado Pago e entrega de e-mail).
10. Retenção de dead-letter, access logs, Cloudflare e backups ainda não está
    automatizada de ponta a ponta.
