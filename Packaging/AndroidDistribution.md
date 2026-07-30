# ChurchDAW Android — distribuição oficial pelo site

Versão publicada: `1.5.4` (`versionCode 9`)

Pacote: `com.churchdaw.app`

API mínima/alvo: `24` / `36`

Arquitetura de produção: `arm64-v8a`

## Artefatos oficiais

- APK público:
  `https://churchdaw.com/app/android/ChurchDAW-1.5.4.apk`
- APK SHA-256:
  `AA5D119CB9ECF3883C8561055CC7BD64DB42C882D2D0C911416D0D261117DC65`
- APK tamanho:
  `21.109.858` bytes
- AAB SHA-256:
  `A98442BE4951AFC88518A13D08290A2C5DE0F3A563FA11376583A41B84FDA0F4`
- Certificado de assinatura SHA-256:
  `6eacfe6be55c206d66fcd9a665be997f82ee81ed3d83f2f97ff2e4f77d7101ac`

O APK público deve continuar assinado com a mesma chave. Trocar a chave impede
que o Android instale uma versão nova por cima da anterior.

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

Produção atual:

- `policy_serial: 10`;
- `latest_version_code: 9`;
- `minimum_version_code: 9`;
- `latest_version_name: 1.5.4`;
- catálogo SHA-256:
  `24eea0c98cff9654bb815830e4deb0da0f7ad1ca5471e40e60170a8e5154224d`;
- política SHA-256:
  `BF3137893B234FBC0842A83CFC753ECAC2E5FDA12F6B19F256294B890A00B1F8`.

O serial é monotônico e evita replay. Um piso aceito não pode diminuir sem uma
política de rollback válida, com novo serial e revogação explícita do serial
anterior. O `catalog_sha256` impede que o catálogo seja alterado sem uma nova
assinatura.

A chave privada de política fica fora do repositório:

`C:\Users\Administrator\ChurchDAW-Secrets\churchdaw-update-policy-private.pem`

Ela nunca deve ser copiada para o APK, site ou repositório.

## Endpoints canônicos

- APK:
  `https://churchdaw.com/app/android/ChurchDAW-1.5.4.apk`
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

1. aumente `versionCode` e `versionName`;
2. gere uma build release limpa e assinada;
3. valide assinatura, pacote, versão, tamanho e SHA-256;
4. instale o APK exato em aparelho real;
5. publique primeiro o novo APK;
6. baixe o APK público e compare os bytes com o artefato local;
7. publique o `manifest.json`;
8. assine e publique `update-policy-v2.json`;
9. publique `update-policy.json` somente como compatibilidade;
10. atualize os links do site;
11. teste uma versão antiga em aparelho real, desde o cold start até a
    confirmação do Instalador de pacotes;
12. instale a versão nova e confirme que o gate desapareceu.

Se qualquer etapa falhar, não eleve `minimum_version_code`.

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
  -Bundle C:\Users\Administrator\MinhaDAW\Releases\Android\ChurchDAW-1.5.4-signed.aab
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
