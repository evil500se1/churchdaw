# ChurchDAW Android 1.3.0 — relatório de liberação

Data da validação: 28 de julho de 2026

Pacote: `com.churchdaw.app`

Versão: `1.3.0` (`versionCode 3`)

Android: mínimo 24, alvo 36, `arm64-v8a`

## Artefatos oficiais

| Artefato | Bytes | SHA-256 |
| --- | ---: | --- |
| `Releases/Android/ChurchDAW-1.3.0-signed.aab` | 22.817.479 | `9169315F2E8E75ABE28B6BCEEBCC9CE104C3627F712C84F59C7C64C6BACE2186` |
| `Releases/Android/ChurchDAW-1.3.0-signed.apk` | 21.561.100 | `8AFA63FD5F2F30CEC9CE29CF3FAFC7FB57301BD3C8291C9FAAF2548EF510882E` |

- Build release limpo, lint e configuração Android aprovados.
- APK com assinatura v2 válida, chave ChurchDAW RSA-4096 e certificado SHA-256
  `6EACFE6BE55C206D66FCD9A665BE997F82EE81ED3D83F2F97FF2E4F77D7101AC`.
- AAB aprovado pelo `bundletool 1.18.1`; APK aprovado pelo `apksigner`.
- APK e AAB contêm um DEX, uma biblioteca nativa `arm64-v8a` e nenhum sample
  WAV/FLAC/AIFF embutido.
- Os artefatos em `Releases/Android` são idênticos aos resultados finais do build.

## Atualização e catálogo

A atualização do Android é nativa do ecossistema ChurchDAW, mas a instalação de
novas versões é exclusivamente pela Google Play. O app não baixa nem instala APK
por conta própria.

- Política: schema 2, `SHA256withRSA`, chave `churchdaw-update-2026-01`.
- Serial vigente: 3, modo `normal`, versão mais recente 3 e versão mínima 1.
- SHA-256 do catálogo vinculado:
  `788787B8531E60D16121FB9526D7F738DD1DC0A6001185D0136D1AC9CD9B2182`.
- Política assinada verificada com a chave pública empacotada.
- Catálogo: 30 partes, 13.416.853.492 bytes para download e
  13.415.017.502 bytes instalados.
- Instalador com hashes e tamanhos exatos, extração em streaming, cancelamento,
  retomada e recuperação transacional.

Arquivos publicados e comparados byte a byte com a cópia local:

| Endpoint | Bytes | SHA-256 |
| --- | ---: | --- |
| `/app/android/manifest.json` | 7.629 | `788787B8531E60D16121FB9526D7F738DD1DC0A6001185D0136D1AC9CD9B2182` |
| `/app/android/update-policy-v2.json` | 1.320 | `670E664A43FB2351E55089B35B49EAD5639B3D7992D0E432A8947BE1C8DC14C5` |
| `/app/android/update-policy.json` | 390 | `66C1EF5DA349756F51AF288941B448E04C1D2F197D2CE3BDC9EF03CD4971F241` |

Os endpoints respondem `200`, `application/json`, `no-store` e
`X-Content-Type-Options: nosniff` tanto no domínio raiz quanto em `www`.

## Reverb worship nativo

A versão 1.3.0 inclui um reverb estéreo nativo e original da ChurchDAW,
inspirado na profundidade e musicalidade das unidades worship, sem copiar
firmware, código, presets ou recursos proprietários.

- Barramento de retorno 100% molhado, com envio independente por canal e
  `Return` global suavizado.
- Predelay estéreo, reflexões iniciais, quatro allpasses por canal e FDN de
  oito linhas com matriz Hadamard.
- Decay independente em graves, médios e agudos, taps cúbicos fixos, largura
  estéreo, ducking/swell e filtros de entrada/saída.
- O movimento da cauda ocorre numa matriz ortogonal lenta. Os comprimentos dos
  delays permanecem fixos enquanto a nota soa, eliminando o Doppler que
  desafinava o fim da cauda em pianos.
- Shimmer em +12 semitons, com realimentação limitada e voz de quinta
  disponível para desenho futuro, mas desligada nos presets atuais.
- Doze ambientes: Bloom, Cloud, Chorale, Shimmer, Magneto, Nonlinear,
  Reflections, Room, Hall, Plate, Spring e Swell.
- Troca de preset, envios e retorno sem saltos abruptos; preset e retorno são
  restaurados com o projeto.
- Troca rápida Cloud → Hall → Cloud cancela corretamente o pedido intermediário
  sem carregar o preset obsoleto nem silenciar duas vezes.
- `PANIC` limpa imediatamente vozes e cauda. No desktop, um BigSky externo
  continua suportado em 100% wet, com fallback nativo coerente.

## Teste físico — TANK3

O APK oficial foi copiado e somente a cópia de teste foi re-assinada com a chave
de debug já presente no aparelho. O SHA-256 oficial foi conferido antes e depois
da operação e permaneceu `8AFA63FD...0882E`. A atualização ocorreu com `adb
install -r`, sem desinstalar, limpar dados ou alterar `firstInstallTime`.

- TANK3 físico, Android 14, `arm64-v8a`.
- AAudio em 48 kHz com buffer real de 1.024 samples.
- Índice existente: 58 timbres e 9.215 samples.
- Felt Upright ficou pronto em 0,72 s e Pad2 em 0,82 s no cold start final.
- `Automated CC Source` e a porta física do Launchkey foram reconhecidas.
- Processo e Activity permaneceram vivos e em primeiro plano até o fim.

Ensaio de pior caso do retorno:

- Cloud em `Return 100%` e master `+10,0 dB`;
- acorde controlado de quatro notas, velocity 72, seguido da cauda completa;
- 59 relatórios consecutivos até `PICO -100,0 dBFS`;
- `FALHA-STREAM 0` em todos os relatórios;
- zero NaN, proteção DSP, clip, crash, `Fatal signal` ou ANR;
- nenhum pico de saída acima de 0 dBFS e nenhum bloco lento persistente.

O mesmo fluxo foi repetido em Shimmer, velocity 76:

- todas as oito mensagens Note On/Off retornaram sucesso;
- cauda observada de `-39,1` a `-97,7 dBFS` e depois silêncio;
- pico de callback observado de 44,2%;
- zero falha de stream, NaN, clip, crash ou ANR.

Também foram executadas 12 trocas rápidas de ambiente durante nota sustentada e
12 trocas reais de piano entre Felt, Pearl, Amber e Black Upright. Os pianos
ficaram prontos em 0,75–0,90 s, sem instalação obsoleta, OOM, crash ou ANR.

O primeiro smoke desta rodada revelou um defeito que os testes sintéticos ainda
não cobriam: a janela final do sample continha todo o trecho legal restante, mas
o consumidor exigia espaço para um callback inteiro e silenciava o último bloco
(`FALHA-STREAM`, motivo 3). Esse artefato foi descartado. O limite passou a usar
`min(span, samples restantes)`, foi revisado em 5.544 combinações / 552.285
leituras e o build foi refeito do zero. O smoke final acima chegou ao silêncio
com zero falhas.

Ao concluir, o aparelho foi restaurado para Felt Upright, Cloud e `Return 82%`.
A fonte MIDI e os APKs temporários de teste foram removidos. Os APK/AAB oficiais
continuam assinados exclusivamente com a chave ChurchDAW.

Capturas reais:

- `Packaging/ChurchDAW-1.3.0-TANK3-Final.png`
- `Packaging/ChurchDAW-1.3.0-TANK3-Final-Libraries.png`
- `Packaging/ChurchDAW-1.3.0-TANK3-Final-Master.png`
- `Packaging/ChurchDAW-1.3.0-TANK3-Reverb-Shimmer.png`
- `Packaging/ChurchDAW-1.3.0-TANK3-Reverb-Stress.log`

## Validações automatizadas

- Configuração Android: aprovada para `com.churchdaw.app`, versão 1.3.0/code 3.
- Reverb DSP: `WORSHIP_REVERB_TESTS_OK` em L/R/Mid/Side, 44,1/48/96 kHz e
  blocos de 64 a 511 samples.
- O controle negativo que reproduz o delay móvel antigo (`5 ms / 0,13 Hz`) foi
  rejeitado nos quatro caminhos, inclusive quando as bandas laterais se
  cancelam no Mid.
- Afinação sustentada: pior P95 observado de `0,275 cent`; Cloud tail com mudança
  início/fim de `0,040 cent`; Shimmer em 30 s abaixo de `0,176 cent`.
- Cloud T60 medido em 125 Hz / 1 kHz / 8 kHz:
  `10,109 / 12,130 / 5,559 s`.
- Reversão rápida Cloud → Hall → Cloud: salto máximo `0,002`, sem carregar o
  preset intermediário e com erro assentado zero.
- Header DSP validado: SHA-256
  `31F509D10C3F3A79EA761A2B51BC6D0EEE1E2C32731558115ADCF560A7A983C2`.
- Teste DSP: SHA-256
  `D641115F381A61B29809D82A1590225F6356B6FD9520447BE416B2C3736F337D`.
- Sonda de limites/reprepare: Debug, Release e AddressSanitizer sem acesso fora
  de faixa; SHA-256 `6AE6249249EB166CCD806862026FA507753F355BC6A8F784A54CC9E473ECC08F`.
- Limiter master: `MASTER_LIMITER_TESTS_OK`, estéreo linkado, 144 samples de
  latência em 48 kHz, contenção de NaN/Inf e blocos internos sem alocação.
  Header SHA-256
  `3D23C73F465BDDA634C4701D94D600D532CB3CC878083F135CFAE97D72C44B14`.
- Streaming/cancelamento: 100 pedidos em cinco canais, 880 samples FLAC e AIFF,
  pico de 578–579 MiB e shutdown de 86–91 ms.
- `TimbresProcessor.h`: SHA-256
  `F55DDEE9E167906079F37424A01985926649CC12D2BBFE066F370DB24422E2C2`.
- `SampleStream.h`: SHA-256
  `54B31212E838C9B1FE89297E870D46EEF7F05A2B40C74A475183AD2E6F0E863E`.
- Política assinada: 12/12 testes aprovados.
- Instalador e recuperação: 23/23 testes aprovados, sem falhas.
- Release Android: `VERIFICACAO_ANDROID_OK`.
- A validação final observou 30/30 pacotes locais, com tamanho e SHA-256
  conferidos; a evidência permanece em `Packaging/android-final-verify.stdout.log`.
- Estado operacional posterior, 28/07/2026 16:13: outra sessão removeu 29 ZIPs
  locais para liberar espaço enquanto o verificador mantinha o segmento 25
  aberto. Isso não alterou o APK/AAB nem o conteúdo publicado. O cache local
  atual tem 1/30 ZIP; o acervo-base preservado contém os 9.240 FLAC
  (13.415.017.502 bytes), e as 30 listas cobrem esse acervo sem ausências,
  duplicatas ou arquivos extras. Uma nova validação local completa exige
  recuperar os ZIPs publicados e conferir novamente os hashes do manifesto.

## Materiais da loja

- Ícone: PNG 512 × 512 válido.
- Feature graphic: PNG RGB 1024 × 500 sem alpha.
- Quatro screenshots reais do TANK3: PNG RGB 1920 × 1080 sem alpha.
- A captura de Bibliotecas mostra os valores finais `13.4 GB` e `Expansão`.
- A quarta captura é um recorte do canal ativo da tela real, sem interface
  inventada.

## Hospedagem e domínio

- `https://churchdaw.com` e `https://www.churchdaw.com`: HTTP 200.
- HTTP redireciona para HTTPS.
- Certificado Let's Encrypt válido para `churchdaw.com` e `*.churchdaw.com`,
  de 28/07/2026 a 26/10/2026.
- Política pública: `https://churchdaw.com/politica-de-privacidade.html`.
- Pacote de hospedagem:
  `Packaging/AndroidHosting/android-hosting-upload.zip`, 6.640 bytes,
  SHA-256 `26CED00EDFF0B242C2C06125CF400704284A5835178ED65738DE62296056962D`.
- O ZIP contém catálogo, políticas, regras `.htaccess` e a política de
  privacidade HTML.

## Estado da Google Play

A versão ainda não foi publicada.

A conta pessoal ChurchDaw está conectada, mas `Criar app` permanece desativado
enquanto a aprovação dos documentos de identidade não termina. Depois disso:

1. verificar acesso a um aparelho Android físico;
2. verificar o telefone da conta;
3. criar o app e enviar primeiro o AAB para a faixa interna, confirmando que o
   `versionCode 3` está livre;
4. concluir ficha, classificação, App access, público-alvo, Ads e Data safety;
5. executar o teste fechado exigido para conta pessoal nova, com pelo menos
   12 testadores inscritos continuamente por 14 dias;
6. solicitar acesso à produção somente após o teste fechado.

Conta pessoal não exige D-U-N-S. Ainda precisam de decisão do responsável:

- entidade/nome público e e-mail de contato da política;
- retenção e uso dos logs de HostGator/Cloudflare para preencher Data safety sem
  suposições.

Não cancelar nem alterar produtos HostGator ou itens financeiros durante esse
processo sem confirmação específica do alvo.
