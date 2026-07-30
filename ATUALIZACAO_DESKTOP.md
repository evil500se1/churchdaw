# Atualização automática e obrigatória — Windows e macOS

Traz para o desktop a ideia que já existe no Android, com uma diferença que
manda em todo o resto do desenho: **no Android quem instala é a Play Store, e no
desktop não existe loja.**

O `ChurchDAWUpdateBridge.java` fixa a regra de ouro do Android:

> o app nunca baixa nem executa um APK vindo da hospedagem

A loja confere a assinatura, o app só pede. No Windows e no Mac esse trabalho
volta para nós — e com ele volta o risco que a regra evitava: se a hospedagem
for invadida, um instalador falso chega em todas as máquinas dos compradores de
uma vez.

---

## Como a confiança é construída

Nada é executado por vir da nossa hospedagem. São **duas provas independentes**,
e as duas precisam passar:

| # | Prova | Windows | macOS |
|---|-------|---------|-------|
| 1 | Assinatura RS256 nossa sobre o manifesto | BCrypt (`crypt32`, `bcrypt`) | Security.framework |
| 2 | Assinatura do sistema no instalador | Authenticode (`WinVerifyTrust`) | Gatekeeper (`spctl --assess`) |

A prova 1 usa a **mesma chave RSA-3072 do Android** (`churchdaw-update-2026-01`,
`Packaging/AndroidUpdatePolicyPublicKey.pem`). Uma chave só para guardar, um
script só para assinar, e uma policy que não pode ser válida numa plataforma e
inválida na outra.

A prova 2 existe porque as duas falham de formas diferentes: se um dia a nossa
chave privada vazar, o Authenticode/Gatekeeper ainda segura; se a cadeia da
Microsoft/Apple falhar, a nossa segura.

O SHA-256 do instalador viaja **dentro** do payload assinado, e o app inteiro
vai embutido no instalador como recurso PE — então a corrente fecha inteira:

```
assinatura RSA → payload → SHA-256 → instalador → Authenticode → ChurchDAW.exe
```

O payload é recurso `RCDATA` (dentro da imagem PE), não dados grudados no fim do
arquivo. Isso importa: assim o hash Authenticode **cobre também o app**, e trocar
o binário embutido invalida a assinatura.

---

## As três regras que valem mais que as outras

### 1. Nunca interromper quem está tocando

Isto é um instrumento, usado ao vivo. Uma atualização obrigatória disparando no
meio do culto é pior do que ficar desatualizado — o tecladista fica sem som na
frente da igreja e não tem o que fazer.

O portão **só é avaliado no arranque**, em `Main.cpp`, antes de existir motor de
áudio. `mayEnforceNow()` recusa com qualquer voz soando.

### 2. Trava anti-tijolo

`minimum_version_code` sozinho não bloqueia nada. O portão só arma quando existe,
para aquela plataforma, um instalador anunciado, baixado, com SHA-256 conferido,
assinado pelo sistema e de versão suficiente. Faltando qualquer elo — rede caiu,
hospedagem fora do ar, assinatura recusada, instalador não publicado — o app
**abre normalmente** e apenas avisa.

Casos que falham para o lado seguro, todos cobertos por teste:

- policy vencida → não bloqueia
- build sem `CHURCHDAW_VERSION_CODE` → não bloqueia
- abaixo do mínimo mas **sem instalador publicado** → só avisa
- policy não validada → não bloqueia

### 3. Trava de loop

Se a policy exigir uma versão que o instalador publicado **não entrega** — um
`minimum_version_code` digitado alto demais, ou um binário antigo subido por
engano — o app instalaria, reabriria ainda abaixo do mínimo, instalaria de novo,
para sempre. O músico veria o ChurchDAW reiniciando sozinho sem parar, e nada
indicaria o motivo.

Por isso o SHA-256 do instalador executado fica gravado em
`%APPDATA%\ChurchDAW\update-state.json`. Se o mesmo instalador já rodou e a
versão continua abaixo do mínimo, ele não resolve: vira aviso. Um instalador
**novo** (outro hash) é tentado normalmente.

---

## O instalador

`Installer/ChurchDAWSetup.cpp` — nativo, sem dependência externa, com o
`ChurchDAW.exe` inteiro embutido.

**Instala em `%LOCALAPPDATA%\Programs\ChurchDAW`, não em Arquivos de Programas.**
Isso é deliberado e vale mais que a aparência: num PC de igreja, quem senta na
frente do teclado quase nunca é administrador. Se a atualização obrigatória
abrisse um pedido de senha de administrador que o músico não tem, o portão
viraria exatamente o tijolo que ele foi feito para evitar. É o mesmo motivo pelo
qual o Chrome e o VS Code fazem assim.

Troca o executável mesmo com a versão antiga ainda fechando: o Windows não deixa
sobrescrever a imagem de um processo vivo, mas deixa **renomear** — então o
antigo vira `.old`, o novo entra no lugar, e o `.old` some depois.

| Argumento | Efeito |
|---|---|
| *(nenhum)* | instalação interativa |
| `/SILENT` | sem janela nenhuma — é o que a atualização automática usa |
| `/RELAUNCH` | reabre o ChurchDAW ao terminar |
| `/UNINSTALL` | desinstala (atalhos, registro, arquivos) |

---

## Publicar uma atualização

Tudo de uma vez:

```powershell
cd Packaging
.\BuildDesktopRelease.ps1 -PolicySerial 1 -MinimumVersionCode 10300 `
    -PfxPath C:\ChurchDAW-Secrets\codesign.pfx -PfxPassword ***
```

Compila, monta o instalador, assina, assina a policy e diz o que subir.

Ou passo a passo: `SignDesktopInstaller.ps1` e `SignDesktopUpdatePolicy.ps1`.

O script de policy recusa **antes de gravar** se:

- o serial não for maior que o da policy anterior (anti-replay);
- uma policy `normal` tentar **baixar** o `minimum_version_code`;
- um `rollback` não revogar exatamente o serial anterior, ou não reduzir o mínimo;
- o nome do arquivo na URL não bater com o arquivo assinado;
- a chave privada não corresponder à pública oficial.

O SHA-256 sai sempre do arquivo real — nunca digitado.

**Version code:** `maior*10000 + menor*100 + correção` (1.3.0 → 10300), derivado
do `project()` do CMake. Contador próprio do desktop; não misturar com o
`versionCode` do Android, que anda em outro ritmo.

### Desfazer uma policy ruim

```powershell
.\SignDesktopUpdatePolicy.ps1 -Mode rollback -PolicySerial 6 -RevokesPolicySerial 5 `
    -LatestVersionCode 10400 -MinimumVersionCode 10200 ...
```

---

## Testes

```powershell
cmake -S . -B build -DCHURCHDAW_BUILD_UPDATE_TESTS=ON
cmake --build build --config Release --target ChurchDAWUpdatePolicyTests
.\build\ChurchDAWUpdatePolicyTests_artefacts\Release\ChurchDAWUpdatePolicyTests.exe `
    . <policy-serial-4.json> <policy-serial-5.json>
```

27 verificações. Destaques:

- **Grupo A** usa a **policy Android real já publicada** como vetor conhecido. Se
  o algoritmo, a codificação da mensagem ou a chave embutida estivessem errados,
  A1 reprovaria. "Compilou" nunca teria percebido.
- **Grupo E** testa os **dois lados** do Authenticode. O modo de falha perigoso
  ali é o silencioso: um `WinVerifyTrust` mal configurado que aprovasse tudo
  deixaria o app "funcionando" com metade da proteção perdida.

### Teste ponta a ponta do fluxo obrigatório

Feito em 28/07/2026 com certificado de teste. Resultado: policy buscada →
decisão `mandatory` → diálogo "Atualização obrigatória" → download de 13,7 MB →
SHA-256 conferido → Authenticode conferido → instalação silenciosa → app
encerrado → app reaberto já atualizado, tudo em ~1 segundo. O binário instalado
passou a ser byte a byte o novo. Na reabertura, a trava de loop impediu a
segunda tentativa.

Para repetir: compilar com
`-DCHURCHDAW_UPDATE_ALLOW_LOCALHOST=ON -DCHURCHDAW_UPDATE_POLICY_URL=http://127.0.0.1:8099/update-policy.json`,
servir policy e instalador em 127.0.0.1, e assinar o instalador com um
certificado temporário confiado em `CurrentUser\Root`. **Remover o certificado e
apagar `update-state.json` depois** — o serial de teste recusaria policies reais.

`CHURCHDAW_UPDATE_ALLOW_LOCALHOST` é desligado por padrão e faz o CMake gritar
quando ligado; segue a mesma convenção do `CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST`
que já existia.

### Validação no macOS (28/07/2026, MacinCloud, macOS 26.3)

O código do `Security.framework` em `DesktopUpdater.cpp` tinha sido escrito sem
nunca ter sido compilado. Foi testado isolado, com um programa que reproduz
exatamente a mesma lógica e verifica a **policy Android real já publicada**:

```
payload 522 bytes | assinatura 384 bytes | SPKI 422 bytes | PKCS1 398 bytes
  SPKI     import OK, assinatura VALIDA
  PKCS1    import OK, assinatura VALIDA
RESULTADO: SPKI FUNCIONA - o codigo atual esta correto no Mac
```

- **Compila** com `clang++ -std=c++17 -framework Security -framework CoreFoundation`,
  sem um aviso sequer.
- **`SecKeyCreateWithData` aceita SPKI.** Havia um receio real aqui: a API
  historicamente exigia PKCS#1 (`RSAPublicKey`), e a chave embutida é X.509
  (`SubjectPublicKeyInfo`). No macOS atual os dois formatos importam e verificam.
  O mesmo vale para `DesktopLeaseSignature_mac.mm`, que faz igual.
- **Gatekeeper:** `/usr/sbin/spctl` existe no caminho codificado e recusa
  (`exit=3`) tanto o `ChurchDAW-final.dmg` ad-hoc quanto um binário sem
  assinatura. O caminho de recusa está correto — falha fechado, como deve.

**O que continua sem teste no Mac:** o caso POSITIVO do Gatekeeper, que exige um
DMG assinado com Developer ID e notarizado. É o mesmo bloqueio do item 2. Também
não foi possível compilar o app inteiro lá: aquele Mac não tem `cmake` nem árvore
de fontes, e é máquina compartilhada com a outra sessão.

Transferência sem git e sem disco compartilhado: base64 digitado em 23 pedaços,
com SHA-256 conferido nas duas pontas (`5768eac8722d5d2f…`, idêntico). A pasta de
teste foi removida do Mac; o `$HOME` ficou exatamente como estava.

---

## Pendências

1. **Certificado de assinatura de código para Windows.** É o único item que
   separa o sistema de estar no ar. O fluxo inteiro já foi provado funcionando
   com um certificado de teste; falta o real. Ele já era necessário de qualquer
   forma, senão o SmartScreen assusta cada comprador.
   Depois de comprado: `BuildDesktopRelease.ps1 -PfxPath ...` e acabou.

2. **macOS: assinatura + notarização** (`Signature=adhoc`, `TeamIdentifier=not
   set`, Apple Developer US$ 99/ano). Único item que falta para o Mac.

   Validado no MacinCloud (macOS 26.3) em 28/07/2026 — ver abaixo.

3. **Hospedar `app/desktop/update-policy-v1.json`** e os instaladores. Hoje o
   endereço responde 404, e o app trata isso corretamente como "não deu para
   saber" e abre normal (verificado com o app rodando).

---

## Correção relacionada: o catálogo de bibliotecas no desktop

`verifyAndroidSignedPolicyEnvelope` devolvia `false` fora do Android. Parecia
detalhe, mas travava o produto inteiro no desktop:

```
verifyAndroidSignedPolicyEnvelope → false        (ChurchDAWServices.h)
  └─ policy nunca fica cryptographicallyVerified
      └─ catalogSha256 nunca é preenchido
          └─ o estado aceito em disco também nunca é salvo (exige verificação)
              └─ checkRemoteManifest() aborta:
                 "Catalogo bloqueado: politica assinada sem SHA-256 confiavel"
```

Ou seja: **no Windows e no Mac o app nem chegava a pedir o `manifest.json`** —
ninguém conseguia baixar timbre nenhum. Não era rede: `churchdaw.com` serve o
catálogo normalmente (HTTP 200, 7629 bytes).

A correção liga aquele `#else` ao verificador RS256 nativo que já existe em
`DesktopUpdater.cpp` — mesma chave, mesmo algoritmo, mesmo envelope do Android.
São 3 linhas em `ChurchDAWServices.h` (arquivo da outra sessão; há backup no
scratchpad da sessão).

Antes: *"Nao foi possivel validar o catalogo agora. Downloads ficam pausados
para sua seguranca."* — botão de download escondido.
Depois: *"PACK COMPLETO / download 13.4 GB"* — botão **Baixar pack completo**.

Coberto pelo grupo F dos testes (F1 valida o envelope Android real).

**Decidido em 28/07/2026: um catálogo só para todas as plataformas.** O desktop
usa o mesmo `/app/android/manifest.json` e os mesmos 30 segmentos de ~447 MB em
`/app/android/packs/` que o celular. Não há catálogo separado de desktop.

Consequências que valem lembrar antes de mudar isso um dia:

- Publicar um pack novo aparece nas três plataformas de uma vez — não dá para
  lançar só no PC sem lançar no celular.
- O `catalog_sha256` da policy assinada cobre esse manifesto único, então
  qualquer alteração no catálogo exige assinar uma policy nova (o app recusa um
  catálogo cujo SHA-256 não bate com o assinado).
- O caminho continua com `/android/` no nome. É só um endereço — não vale
  renomear e quebrar as instalações que já existem.
