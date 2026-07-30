# Overlay HostGator sem substituir a landing page

Este overlay encaminha somente conta, compra e `/api/v1` ao backend privado.
`public_html/index.html`, os assets da landing e seus redirects continuam sendo
servidos como antes. O checkout deve permanecer pausado (`CHECKOUT_ENABLED=0`)
durante toda a preparacao, migracao, ativacao, smoke e eventual rollback.

## Invariantes

- `APP_RELEASE` e unico, tem no maximo 64 caracteres e usa apenas
  `A-Z`, `a-z`, `0-9`, ponto, `_` ou `-`;
- o nome do diretorio privado e exatamente o `APP_RELEASE`;
- `current` e sempre um link simbolico para `releases/APP_RELEASE`;
- cada HTML referencia
  `/account-assets/APP_RELEASE/app.css` e `app.js`;
- assets de releases anteriores nao sao sobrescritos nem removidos durante o
  deploy: por isso o rollback recupera codigo e assets correspondentes;
- configuracao, chaves, migrations, fontes PHP, logs e backups nunca entram em
  `public_html`;
- uma release nunca e reutilizada com bytes diferentes;
- banco nao sofre rollback cego depois de pagamentos ou webhooks.

Topologia:

```text
/home2/USUARIO/churchdaw/
  deploy-tools/
    preflight-release.php
    stage-account-assets.sh
    switch-release.sh
    smoke.sh
  releases/
    2026-07-28.1/
      bin/
      config/local.php -> ../../../shared/config/local.php
      migrations/
      public/account-assets/app.css
      public/account-assets/app.js
      src/
  shared/
    config/local.php
    keys/churchdaw-license-private.pem
  current -> releases/2026-07-28.1

/home2/USUARIO/public_html/
  index.html
  .htaccess
  churchdaw-account.php
  account-assets/
    .htaccess
    2026-07-28.1/
      app.css
      app.js
```

O front-controller calcula `/home2/USUARIO/churchdaw/current`, resolve o link e
recusa iniciar se o destino nao estiver dentro de `churchdaw/releases`.

## Preparacao sem corte

1. Manter no config privado:

   ```php
   'app.release' => '2026-07-28.1',
   'checkout.enabled' => false,
   ```

2. Fazer backup externo de `public_html`, do `.htaccess` real e do banco.
   Registrar SHA-256 e testar a abertura do backup. Nao guardar o backup em
   `public_html`.
3. Enviar a release completa para
   `/home2/USUARIO/churchdaw/releases/APP_RELEASE`, nunca para `current`.
4. Ligar `config/local.php` ao config compartilhado e conferir permissoes.
5. Instalar os quatro arquivos de `bin/` deste overlay em
   `churchdaw/deploy-tools`, fora da DocumentRoot, com permissao `0750`.
6. Executar com o mesmo PHP CLI usado pelo dominio:

   ```sh
   PHP_BIN=/opt/cpanel/ea-php84/root/usr/bin/php
   "$PHP_BIN" /home2/USUARIO/churchdaw/deploy-tools/preflight-release.php \
     /home2/USUARIO/churchdaw/releases/2026-07-28.1 \
     2026-07-28.1
   ```

   O resultado precisa informar `checkout_enabled:false` e os dois hashes.

## Instalacao inicial do overlay estavel

Estes arquivos mudam raramente e podem ser instalados antes do corte, pois nao
recebem trafego ate as regras serem ativadas:

1. enviar `public_html/churchdaw-account.php` com nome temporario, conferir seu
   SHA-256/permissao e renomear no mesmo diretorio;
2. criar `public_html/account-assets`, instalar seu `.htaccess` da mesma forma
   e conferir que listagem de diretorio esta desativada;
3. preparar os assets versionados:

   ```sh
   /home2/USUARIO/churchdaw/deploy-tools/stage-account-assets.sh \
     /home2/USUARIO/churchdaw \
     /home2/USUARIO/public_html \
     2026-07-28.1
   ```

   O script copia para um diretorio oculto, compara os bytes e usa um unico
   `rename` no mesmo filesystem. Se o nome ja existir com bytes diferentes, o
   deploy falha e exige um novo `APP_RELEASE`.

## Ordem segura do `.htaccess`

Nao substituir o `.htaccess` real. Montar uma copia completa
`.htaccess.churchdaw-next` e inserir o bloco entre os marcadores
`BEGIN/END CHURCHDAW ACCOUNT OVERLAY` nesta ordem:

1. handler PHP gerenciado pelo cPanel (a posicao exata pode ser preservada);
2. redirect canonico explicito para `https://churchdaw.com`;
3. bloco ChurchDAW fornecido em `churchdaw-routes.htaccess`;
4. redirect existente de `/linknabio` e demais redirects especificos da
   landing page;
5. qualquer fallback/catch-all da landing page.

O `.htaccess` auditado em 28 de julho de 2026 tinha exatamente 641 bytes e
SHA-256
`1bdf86d8be64edc09fa28805e06338d03299c628f618fbce3206310b04c32fcc`.
Antes de preparar a copia, aplicar este guard no arquivo real:

```sh
cd /home2/USUARIO/public_html
test "$(wc -c < .htaccess)" -eq 641
printf '%s  %s\n' \
  1bdf86d8be64edc09fa28805e06338d03299c628f618fbce3206310b04c32fcc \
  .htaccess | sha256sum -c -
cp -p .htaccess .htaccess.churchdaw-next
```

Se tamanho ou hash divergirem, parar e auditar novamente; nao montar o arquivo
novo sobre uma versao desconhecida. Na copia, substituir o redirect que usa
`https://%{HTTP_HOST}%{REQUEST_URI}` por:

```apache
<IfModule mod_rewrite.c>
    RewriteEngine On
    RewriteCond %{HTTPS} !=on [OR]
    RewriteCond %{HTTP_HOST} !^churchdaw\.com$ [NC]
    RewriteRule ^ https://churchdaw.com%{REQUEST_URI} [R=301,L]
</IfModule>
```

Isso faz HTTP e `www` convergirem para o apex sem refletir um `Host` arbitrario,
mantendo `APP_BASE_URL`, redirects e cookies no mesmo host. A query string e
preservada pelo Apache.

O bloco usa `[END]`, protege o front-controller direto, reserva somente os dois
arquivos permitidos no namespace versionado, limita o corpo do controller a
64 KiB e aplica aos erros Apache os mesmos headers de seguranca/no-cache do PHP.
O `.htaccess` de `account-assets` desativa indices, bloqueia extensoes perigosas
e marca somente assets versionados como `immutable`.

Antes da troca:

```sh
grep -c 'BEGIN CHURCHDAW ACCOUNT OVERLAY' .htaccess.churchdaw-next
grep -c 'END CHURCHDAW ACCOUNT OVERLAY' .htaccess.churchdaw-next
```

Cada comando deve imprimir exatamente `1`. Guardar a copia anterior fora de
`public_html`. A substituicao final deve ser um `mv -T` no mesmo diretorio, nao
uma sequencia de apagar/criar no File Manager.

No primeiro deploy, preparar banco e `current` antes de trocar o `.htaccess`;
assim a troca do `.htaccess` e o unico instante que torna as rotas publicas.

## Migracao, corte atomico e smoke

1. Com checkout ainda pausado, executar as migrations na release nova.
2. Confirmar `schema_migrations`, produto e compatibilidade do rollback.
3. Fazer o corte:

   ```sh
   /home2/USUARIO/churchdaw/deploy-tools/switch-release.sh \
     /home2/USUARIO/churchdaw \
     /home2/USUARIO/public_html \
     2026-07-28.1 \
     /opt/cpanel/ea-php84/root/usr/bin/php
   ```

   O script repete o preflight, compara os assets publicos e troca `current`
   por um unico `rename`. Ele recusa diretorio comum no lugar de symlink.
4. No primeiro deploy, somente agora promover o `.htaccess` completo preparado.
5. Usar os hashes exibidos pelo preflight:

   ```sh
   /home2/USUARIO/churchdaw/deploy-tools/smoke.sh \
     https://churchdaw.com \
     2026-07-28.1 \
     SHA256_APP_CSS \
     SHA256_APP_JS \
     /opt/cpanel/ea-php84/root/usr/bin/php
   ```

O smoke e nao destrutivo. Ele prova:

- landing existente HTTP 200;
- health HTTP 200, release/schema exatos e checkout falso;
- `/comprar` em manutencao HTTP 503;
- headers CSP, HSTS, no-store, frame/content/referrer/permissions;
- CSS/JS HTTP 200, imutaveis e com hashes exatos;
- controller direto e paths privados 403/404;
- allowlist do namespace de assets;
- Content-Type errado HTTP 415;
- corpo acima de 64 KiB HTTP 413.

O smoke nao prova Mercado Pago, entrega de e-mail, cron nem compra real.

## Rollback atomico

Antes do corte, anotar a saida `previous=` do script e manter:

- release privada anterior;
- seus assets versionados;
- config compartilhado compativel;
- backup do `.htaccess` anterior;
- dump anterior e registro de webhooks/pagamentos ocorridos depois dele.

Se o schema novo continua compativel com o codigo anterior, rollback de codigo:

```sh
/home2/USUARIO/churchdaw/deploy-tools/switch-release.sh \
  /home2/USUARIO/churchdaw \
  /home2/USUARIO/public_html \
  RELEASE_ANTERIOR \
  /opt/cpanel/ea-php84/root/usr/bin/php
```

Rodar novamente `smoke.sh` com a release e hashes anteriores. A troca de
`current` devolve PHP e HTML em um unico passo; o HTML antigo volta a apontar
para seu diretorio de assets, que permaneceu intacto.

Se a migration nao for retrocompativel, nao apontar o codigo antigo para o
schema novo. Manter checkout pausado, corrigir para frente ou restaurar em banco
separado e reconciliar pagamentos/webhooks antes de qualquer restauracao real.
Nunca restaurar dump antigo por cima do banco ativo.

Falha exclusiva da primeira instalacao do overlay permite restaurar
atomicamente o `.htaccess` anterior para retirar as novas rotas, sem apagar a
landing. Nao remover releases/assets no mesmo dia; a limpeza exige janela e
retencao formal separadas.

## Liberacao comercial

`CHECKOUT_ENABLED=1` nao faz parte deste deploy. So pode ser alterado em uma
release/config posterior depois de smoke, crons, e-mail, Mercado Pago,
compra/reembolso controlados, monitoramento e rollback aprovados.
