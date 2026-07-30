# ChurchDAW Android 1.2.0 — relatório de release

Data: 27/07/2026  
Pacote: `com.churchdaw.app`  
Versão: `1.2.0` / `versionCode 2`  
SDK mínimo/alvo/compilação: `24` / `36` / `36`  
ABI: `arm64-v8a`

## Artefatos atuais

- AAB: `Releases/Android/ChurchDAW-1.2.0-signed.aab`
  - Tamanho: `21.197.953` bytes
  - SHA-256:
    `DAC6F17D079934E07E9FC30427A3248957BDD8DA3D21F5A13404B1805FC12612`
- APK: `Releases/Android/ChurchDAW-1.2.0-signed.apk`
  - Tamanho: `22.101.224` bytes
  - SHA-256:
    `404CC87631C1FEC07C52B1FEB9967FE00369E65526CA05870EBF441158AB8AD2`
- Certificado de upload:
  `Packaging/churchdaw-upload-certificate.pem`
- SHA-256 do certificado:
  `6E:AC:FE:6B:E5:5C:20:6D:66:FC:D9:A6:65:BE:99:7F:82:EE:81:ED:3D:83:F2:F9:7F:F2:E4:F7:7D:71:01:AC`

A chave privada permanece fora do projeto em
`C:\Users\Administrator\ChurchDAW-Secrets`.

## Evidências concluídas

- Release assinada compilada com sucesso.
- Android lint: `0 errors, 4 warnings`.
- APK release: assinatura v2 válida, RSA 4096 bits, certificado ChurchDAW.
- A Activity customizada agora herda diretamente de `android.app.Activity`.
  Isso elimina o `UnsatisfiedLinkError` que o `JuceActivity.onResume()` causava
  quando push notifications estavam desativadas.
- Ícone corrigido em `ldpi`, `mdpi`, `hdpi`, `xhdpi` e `xxhdpi`; o aviso de
  tamanho DIP desapareceu. O RGB permanece igual ao logo original, com cantos
  transparentes e sem fundo verde.
- A ponte Play Core foi movida para `Source/AndroidJava`, portanto não se perde
  ao regenerar o projeto Android.
- Build debug ARM64 foi aberta no TANK 3 sem apagar o acervo externo de
  `9.240` arquivos / `13 GB`; o processo permaneceu ativo, sem crash fatal e
  sem ANR. A inspeção visual final do aparelho físico depende do desbloqueio.
- Emulador Android 16 / API 36 com Google Play e ABI `x86_64` criado no PC.
  Uma variante debug isolada foi usada somente para os testes visuais e de
  integração; o AAB/APK oficial foi regenerado depois como `arm64-v8a`.
- Instalação limpa no emulador: a primeira configuração exibiu corretamente
  `Baixar pack completo` e `Abrir sem baixar`.
- Foi corrigido e provado no emulador que `Abrir sem baixar` é lembrado no
  armazenamento privado. No segundo início o app entra direto na DAW; o pack
  continua disponível em `Bibliotecas`.
- A migração do acervo antigo não sonda mais `Music/ChurchDAW` antes da
  permissão. A oferta inicial é opcional, `Agora não`/negação são lembrados e a
  permissão pode ser solicitada novamente por `Bibliotecas`, sem mover, copiar
  ou apagar samples.
- O catálogo exibiu `Church Live` incluído e `Worship Keys Vol. 1` como
  `Em breve`, sem expor URL de compra ou pack comercial.
- O gate durável de atualização obrigatória foi exercitado com a instalação
  `versionCode 2` e piso `3`. A DAW ficou inacessível, mostrou `Atualizar
  agora` e continuou bloqueada após reinício sem internet. O fixture foi
  removido depois sem apagar cache, configurações ou bibliotecas.
- Manifesto de produção recebido pelo aparelho com 30 partes e
  `13.416.853.492` bytes.
- Os 30 ZIPs foram testados localmente com 7-Zip; hashes e tamanhos conferem.
- Os 30 URLs públicos retornaram HTTP `200`, tamanho exato, `Accept-Ranges:
  bytes` e HTTP `206` para `bytes=0-0`.
- `Packaging/VerifyAndroidRelease.ps1` repetiu toda a auditoria local/remota e
  terminou com `VERIFICACAO_ANDROID_OK`.
- O manifesto público é JSON válido, idêntico ao arquivo local e mantém
  `minimum_version_code: 1`.
- Fixture multipartes de duas partes instalada de ponta a ponta no emulador:
  2 ZIPs, 4 arquivos, `2.820` bytes expandidos e marcador atômico válido.
  Nenhum `.part`, staging, backup ou journal ficou órfão.
- Os dois WAVs reais do fixture foram indexados como `PartA` e `PartB`.
- O helper MIDI Android foi reconhecido como uma entrada:
  `Automated CC Source`. Note On/Off 60 e CC74 retornaram sucesso.
- Ao tocar Note 60, o medidor interno saiu de `-100,0 dBFS` para `-29,4 dBFS`
  e registrou `FALHA-STREAM 0`, comprovando a cadeia MIDI -> sample -> saída
  Oboe. O emulador em SwiftShader apresentou atrasos de callback esperados e
  não substitui o teste de latência na interface física.
- `Packaging/VerifyAndroidRelease.ps1` foi repetido após todas as correções:
  30/30 partes e `VERIFICACAO_ANDROID_OK`.

## Avisos de lint aceitos

1. Existe Gradle mais novo que 8.13; atualizar ferramenta perto da publicação
   sem necessidade aumentaria risco.
2. Android 16 pode ignorar orientação fixa em telas grandes. O produto atual é
   intencionalmente landscape, mas deve ser testado em tablet.
3. Dois avisos de ausência de `x86_64` para ChromeOS. O produto móvel atual é
   ARM64.

## Pendências externas antes de produção aberta

- Enviar este AAB para a faixa interna da Play Console e testar o fluxo
  imediato a partir de uma instalação feita pela própria loja. O Play Core não
  oferece uma prova completa para APK instalado por ADB.
- Desbloquear/reconectar o TANK 3 para instalar este APK ARM64 final e repetir
  o teste MIDI/áudio com a interface física. O emulador é `x86_64` e, por isso,
  não pode instalar o APK final `arm64-v8a`.
- Corrigir a delegação DNS de `churchdaw.com`; o domínio técnico HTTPS funciona
  como fallback.
- Para bibliotecas pagas, implementar compra, validação de entitlement e URLs
  temporárias antes de publicar qualquer pack comercial.
