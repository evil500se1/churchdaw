# ChurchDAW Android — distribuição oficial pelo site

Estado do fonte candidato: `1.5.8` (`versionCode 14`).

Última versão pública verificada antes desta promoção: `1.5.7`
(`versionCode 13`). O fonte não é evidência de publicação: o site e as policies
continuam na versão anterior até o workflow comercial terminar e verificar os
bytes públicos.

Pacote: `com.churchdaw.app`

API mínima/alvo: `24` / `36`

Arquitetura de produção: `arm64-v8a`

## Artefatos oficiais

- Formato canônico do APK público:
  `https://churchdaw.com/app/android/ChurchDAW-MAJOR.MINOR.PATCH-signed.apk`
- SHA-256 e tamanho autoritativos: ficam no
  `android-promotion-manifest.json` do run comercial e devem coincidir com o
  APK baixado novamente por HTTPS.
- Certificado de assinatura SHA-256:
  `6eacfe6be55c206d66fcd9a665be997f82ee81ed3d83f2f97ff2e4f77d7101ac`

O APK público deve continuar assinado com a mesma chave. Trocar a chave impede
que o Android instale uma versão nova por cima da anterior.

O upgrade direto obrigatório desta trilha começa na versão pública 1.5.7,
assinada pelo fingerprint acima. Instalações antigas assinadas por outra chave
não podem receber esse APK por cima: precisam de migração/reinstalação ou de uma
linhagem de assinatura projetada e testada em aparelho físico. Não eleve o piso
para prender essa população antiga no gate de atualização.

## Publicador comercial transacional

O único caminho autorizado para promover Android no HostGator é:

`.github/workflows/release-android-publish.yml`

Ele é manual, precisa ser despachado selecionando a própria tag da release e
exige `confirmation=PUBLISH`. O workflow só aceita:

- tag remota imutável `vMAJOR.MINOR.PATCH`;
- commit exato de 40 hex, igual ao HEAD protegido, à tag e ao builder;
- run concluído com sucesso de `.github/workflows/build-android.yml`;
- artifact ID exato, não expirado, com digest e tamanho do ZIP validados pela
  API do GitHub e por download independente;
- exatamente um APK `-signed.apk` e uma metadata;
- pacote `com.churchdaw.app`, versão da tag, `arm64-v8a`, SHA-256, tamanho,
  commit e certificado coerentes;
- assinatura APK v2 válida e fingerprint igual ao pin protegido.

O job de publicação usa o environment protegido `desktop-production`. Antes do
primeiro uso, configure nele:

Segredos:

- `HOSTGATOR_FTPS_HOST`;
- `HOSTGATOR_FTPS_USERNAME`;
- `HOSTGATOR_FTPS_PASSWORD`;
- `DESKTOP_UPDATE_PRIVATE_KEY_BASE64` — a chave RSA compartilhada pelas
  policies desktop/Android, em Base64; nunca o arquivo público nem uma chave
  dentro do APK.

Variáveis protegidas:

- `ANDROID_EXPECTED_CERTIFICATE_SHA256`;
- `ANDROID_POLICY_SERIAL` — exatamente a serial pública atual mais um;
- `ANDROID_MINIMUM_VERSION_CODE` — nesta promoção precisa ser igual ao piso
  público anterior;
- `HOSTGATOR_FTPS_PORT`;
- `HOSTGATOR_REMOTE_ROOT`;
- `HOSTGATOR_ANDROID_REMOTE_DIR`;
- `PUBLIC_SITE_URL`;
- `PUBLIC_ANDROID_BASE_URL`.

Os quatro hashes públicos anteriores são inputs obrigatórios do dispatch:
site, manifesto, policy legada e policy v2. Eles implementam concorrência
otimista. O grupo `desktop-production-publish` é compartilhado deliberadamente
com o publicador desktop, criando um único escritor global para o HostGator.

O `Packaging/SiteProduction/index.html` do commit da tag representa a primeira
fase: aponta para os novos instaladores desktop e mantém o APK Android anterior,
que já precisa estar acessível. Depois da publicação desktop, o publicador
Android prova que esse baseline é byte a byte igual ao `index.html` remoto,
deriva um `index.release.html` temporário alterando somente URL e rótulos
Android com contagens estritas, e exige que EXE e DMG continuem na mesma versão
da tag. O baseline imutável nunca é editado nem promovido diretamente pela
segunda fase.

Teste local da máquina de estados, sem rede e sem publicação:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Packaging\TestPublishAndroidRelease.ps1
```

## Aplicativo e bibliotecas

O APK/AAB contém interface, DSP, MIDI, áudio e o instalador de conteúdo. Os
samples não ficam dentro do aplicativo. A biblioteca Church Live é descrita
pelo catálogo remoto e instalada no armazenamento privado do app.

Uma alteração de código, interface, DSP ou formato exige:

1. novo `versionCode`;
2. novo APK/AAB assinado;
3. publicação do APK;
4. manifesto e política assinada atualizados.

Uma atualização apenas de samples ou metadados ainda exige um novo catálogo e
uma nova política assinada que vincule o SHA-256 desse catálogo.

## Gate obrigatório antes do login

No Android, a política de atualização é verificada antes da autenticação e
antes da criação do mixer, do motor de áudio e dos callbacks MIDI.

Quando o `versionCode` instalado é menor que `minimum_version_code`:

- o login não é mostrado;
- o motor de áudio não é iniciado;
- o usuário vê o gate de atualização;
- o botão abre exatamente o APK oficial indicado pela política assinada;
- o app continua bloqueado até o `PackageManager` confirmar uma versão
  suficiente.

O endereço assinado só é aceito quando:

- usa HTTPS;
- pertence a `churchdaw.com`;
- aponta para um arquivo APK no diretório oficial;
- não contém query, fragmento, CR/LF ou host alternativo.

Se a URL assinada for inválida, o aplicativo usa somente o endereço oficial
compilado no próprio build.

## Limite inevitável do Android

Um aplicativo distribuído fora da Play Store não pode instalar silenciosamente
outro APK. O ChurchDAW obriga o usuário a iniciar a atualização e bloqueia o uso
da versão antiga, mas o Android ainda exige confirmações no Chrome/Gerenciador
de downloads e no Instalador de pacotes. O Play Protect também pode mostrar o
aviso de aplicativo de desenvolvedor ainda não reconhecido.

Não desative o Play Protect no aparelho do cliente. Assinatura estável, HTTPS,
hashes publicados e futura distribuição por uma loja reconhecida reduzem esse
atrito, mas não removem a confirmação do sistema em sideload.

## Política de atualização assinada

A política autoritativa é:

`https://churchdaw.com/app/android/update-policy-v2.json`

O envelope usa:

- `schema_version: 2`;
- `algorithm: SHA256withRSA`;
- chave pública RSA-3072 embutida no aplicativo;
- `key_id: churchdaw-update-2026-01`;
- payload JSON em Base64;
- assinatura RSA validada antes de interpretar o payload.

Baseline público verificado antes da promoção 1.5.8:

- `policy_serial: 22`;
- `latest_version_code: 13`;
- `minimum_version_code: 9`;
- `latest_version_name: 1.5.7`;
- catálogo/manifesto SHA-256:
  `20BD14BC4357784083C2E0BBE79DA96F0C3BC75652D3E135B26E25B2B9A55061`;
- policy v2 SHA-256:
  `293016AA25F491512D07AB8F2324DF9BF537ED94F2F7EB4B19FB097E7FE8328F`.

O serial é monotônico e evita replay. Um piso aceito não pode diminuir sem uma
política de rollback válida, com novo serial e revogação explícita do serial
anterior. O `catalog_sha256` impede que o catálogo seja alterado sem uma nova
assinatura.

A chave privada de política fica fora do repositório:

`C:\Users\Administrator\ChurchDAW-Secrets\churchdaw-update-policy-private.pem`

Ela nunca deve ser copiada para o APK, site ou repositório.

## Endpoints canônicos

- APK:
  `https://churchdaw.com/app/android/ChurchDAW-1.5.7-signed.apk`
- Manifesto:
  `https://churchdaw.com/app/android/manifest.json`
- Política assinada:
  `https://churchdaw.com/app/android/update-policy-v2.json`
- Política legada:
  `https://churchdaw.com/app/android/update-policy.json`
- Política de privacidade:
  `https://churchdaw.com/politica-de-privacidade.html`
- Arquivos da biblioteca Church Live:
  `https://churchdaw.com/app/android/packs/church-live-v1-segment-01.zip`
  até
  `https://churchdaw.com/app/android/packs/church-live-v1-segment-30.zip`

## Ordem segura de publicação

Nunca aumente o piso antes de o novo APK estar acessível publicamente.

1. aumente `versionCode` e `versionName` e prepare o site baseline com EXE/DMG
   novos e APK anterior ainda válido;
2. gere uma build limpa e assinada a partir da tag imutável;
3. valide o APK exato em aparelho ARM64 real e teste upgrade sobre 1.5.7;
4. publique primeiro o desktop e o baseline do site, que só referencia
   downloads já acessíveis;
5. despache o publicador Android com run ID, commit e hashes anteriores exatos;
6. o publicador baixa os quatro arquivos públicos anteriores, confirma FTPS e
   HTTPS e preserva backups com hashes na auditoria;
7. ele prova que o site do tag é igual ao remoto e deriva o candidato Android
   temporário, sem mudar nenhum outro byte lógico;
8. o APK sobe com nome temporário, volta por download/hash e só então recebe o
   nome versionado final por `RNFR`/`RNTO`;
9. o APK público canônico e com nonce precisa coincidir byte a byte;
10. promovem-se, nesta ordem, `index.html`, `manifest.json` e
   `update-policy.json`;
11. `update-policy-v2.json` é sempre a última mutação do caminho normal;
12. todos os endpoints voltam a ser baixados e comparados após a promoção.

Antes da v2, uma falha restaura site/manifesto/legada somente se o hash ativo
ainda for o anterior ou o candidato desta execução. Depois que a v2 pode ter
sido observada, a policy antiga nunca volta: o publicador restaura o catálogo e
publica uma recovery normal com `policy_serial + 1`. A promoção recusa elevar o
piso porque uma rollback que revogue a serial intermediária seria rejeitada por
aparelhos que ainda não a tivessem visto. Assim, a mesma recovery é aceita por
clientes antes e depois da release. O APK versionado permanece imutável para
auditoria. O grupo global de concorrência e os dois checks de hash imediatamente
antes de cada `RNTO` recusam escritores cooperativos e reduzem a janela de uma
corrida externa. Como o FTPS não oferece compare-and-swap atômico, a publicação
também pressupõe que nenhum escritor externo altere esses quatro arquivos durante
a curta janela entre o último check e o `RNTO`.

Somente depois do upgrade físico 1.5.7 → candidato, cold start e gate terem sido
aprovados deve existir uma execução separada para elevar o piso. Não misture
essa decisão irreversível à promoção do APK.

Se qualquer etapa falhar, não considere o novo `minimum_version_code`
aprovado. O artifact de auditoria contém backups, candidatos, recovery assinada
e o manifesto completo da transação.

## Build e validação

A chave de assinatura fica fora do repositório em:

`C:\Users\Administrator\ChurchDAW-Secrets\churchdaw-upload.jks`

Build limpa:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Packaging\BuildSignedAndroidRelease.ps1
```

Verificação da configuração:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Packaging\AssertAndroidBuildConfiguration.ps1 `
  -ProjectRoot C:\Users\Administrator\MinhaDAW `
  -Bundle C:\Users\Administrator\MinhaDAW\Releases\Android\ChurchDAW-1.5.8-signed.aab
```

Testes obrigatórios antes de publicar:

- assinatura e parsing da política;
- preservação da URL oficial assinada da versão futura;
- rejeição de host externo, query, fragmento e CR/LF;
- recuperação/cancelamento/retomada da biblioteca;
- instalação por cima da versão anterior;
- gate antes do login;
- retorno ao login depois da atualização;
- inicialização de áudio e MIDI somente depois do gate e da autenticação.

## Evidência da versão 1.5.4

O fluxo foi validado no TANK3 e no emulador Android:

1. o APK final foi instalado no TANK3 e confirmou
   `versionName 1.5.4`, `versionCode 9`;
2. em um perfil físico limpo do TANK3, “Preparar biblioteca” abriu o fluxo de
   preparação e download sem chamar `/trial/reserve`;
3. o download real entrou no estado cancelável, sem iniciar os 20 minutos;
4. o APK público coincidiu byte a byte com o artefato release;
5. o APK público passou na assinatura v2 com o certificado oficial;
6. manifesto e política assinada apontam somente para
   `ChurchDAW-1.5.4.apk`, sem o sufixo `-signed`;
7. os 30 segmentos públicos responderam HTTP `206` e reportaram exatamente
   os tamanhos vinculados pelo catálogo, totalizando `13.416.853.492` bytes.

Provas locais:

- `C:\Users\Administrator\Documents\Codex\2026-07-27\ac\tank-qa-154-login.xml`
- `C:\Users\Administrator\Documents\Codex\2026-07-27\ac\tank-qa-154-starter.png`
- `C:\Users\Administrator\Documents\Codex\2026-07-27\ac\tank-qa-154-downloading.png`
- `C:\Users\Administrator\Documents\Codex\2026-07-27\ac\verify-android-154`

## Limites de segurança

O gate reduz versões antigas e força o caminho oficial, mas não torna um APK
impossível de copiar ou modificar. Proteção contra compartilhamento de compras
deve vir também do backend de conta/licença, ativação por dispositivo, tokens
revogáveis e autorização individual para downloads pagos. Não coloque segredos
de servidor ou chave privada dentro do cliente.
