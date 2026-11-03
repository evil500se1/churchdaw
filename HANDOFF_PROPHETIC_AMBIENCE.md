# HANDOFF — Prophetic Ambience (ChurchDAW) — 04/08/2026

Documento de passagem para outro agente assumir. Escrito ao fim de uma sessão Cowork que
converteu o timbre "Ambiência 2" do Ableton para o ChurchDAW. **O problema NÃO está resolvido**:
sobra um ruído metálico de fundo ("como um parafuso solto balançando") que ainda não foi
identificado. Este documento existe para você não refazer o que já foi feito nem cair nas
mesmas armadilhas.

---

## 0. LEIA ISTO PRIMEIRO — as 4 armadilhas que custaram caro

1. **O motor NÃO lê `.sfz` nem `.json`.** Não existe uma única ocorrência de "sfz" em
   `Source/TimbresProcessor.h`. O mapeamento vem do **nome do arquivo WAV**, via
   `TimbresProcessor::parseSampleName()` (~linha 510). Nome errado = som errado, por mais
   correto que esteja o `.sfz`. Os `.sfz`/`.json` servem só para metadado e para o
   `Tools/ValidateTimbrePack.py`.

2. **Existe um índice em cache que mente.** `%APPDATA%\ChurchDAW\timbre-index-v2.bin` guarda os
   **caminhos completos** de cada sample. `scanAllLibraries()` retorna o cache sem varrer o disco.
   A assinatura de validade só olha a raiz da biblioteca e as pastas **imediatamente** abaixo —
   gravar dentro de `Pads\<Timbre>\` **não** invalida. Sintoma: `0 zonas` no log.
   Para invalidar: sobrescrever o `.bin` com <24 bytes, **com o app fechado**.

3. **Cutoff de AutoFilter no `.als` é NOTA MIDI, não Hz.** `f = 440 * 2^((v−69)/12)`.
   Ler como Hz foi o que destruiu o primeiro render deste timbre (gerou WAV com DC travado
   em −1,0). Já `SimplerFilter/Freq` do Sampler **é** em Hz.

4. **Sem chunk `smpl` no WAV, o motor CHUTA o loop** em 40%/95% do arquivo com crossfade de
   50 ms (`resolveLoop`, ~linha 7728). Num pad isso soa metálico/robótico.
   Quando acha loop de verdade, ele **carrega a zona inteira na RAM** (`if (z->hasLoop && z->streamed)`,
   ~linha 7602) — ou seja, streaming de disco deixa de ser suspeito.

---

## 1. Caminhos

| O quê | Onde |
|---|---|
| Repositório | `D:\ChurchDAW_Source` |
| Projeto Ableton de origem | `C:\Users\Fabiopc\Downloads\Telegram Desktop\Live Pianos 3.0` |
| Samples de origem | `...\Live Pianos 3.0\SAMPLES\Imported\` |
| Timbre instalado | `%APPDATA%\ChurchDAW\Libraries\church-live\Pads\Prophetic Ambience\` |
| Pack do repositório | `D:\ChurchDAW_Source\Packaging\Packs\prophetic-ambience-v1.zip` |
| Catálogos (os 3 precisam concordar) | `catalog.json` (raiz), `Packaging\catalog.json`, `Packaging\TimbrePacks\timbre-catalog-v1.json` |
| Log do app | `%APPDATA%\ChurchDAW\ChurchDAW.log` |
| Setup salvo | `%APPDATA%\ChurchDAW\ChurchDAW.settings` (backup em `.settings.bak`) |
| MIDI para exportar do Ableton | `...\Live Pianos 3.0\AmbienciaExport.mid` |

---

## 2. O que é o timbre

"Ambiência 2" **não é pasta nem preset**: é nome de **clipe MIDI** no `Live Pianos 3.0.als`,
na track `AMBIENCIA`, que automatiza o **Chain Selector** do rack de instrumento
(AutomationTarget Id 132050):

- `Off` → 0
- `Ambiencia` → 1 → chain **Ambient**
- `Ambiencia 2` → 2 → chain **Ambient 2**  ← este
- `Ambiencia 3` → 3 → chain **Guitar**

Os samples da chain *Ambient 2* são os de **nome limpo** em `SAMPLES\Imported`:
`E0, A#0, E1, A#1, E2, A#2, E3, A#3` (16-bit, 44,1 kHz, 9,60 s).
**ARMADILHA:** os sufixos `-2` são da chain *Ambient* e os `-1` da chain *Guitar*. São sons
diferentes, não camadas de velocity.

### Mapeamento (roots do `.als`, convenção Ableton C3=60)

| arquivo fonte | nome ChurchDAW | root | zona que o motor monta |
|---|---|---|---|
| E0.wav | `Prophetic Ambience-E1-V127-01.wav` | 40 | 0–43 |
| A#0.wav | `...-A#1-V127-02.wav` | 46 | 44–49 |
| E1.wav | `...-E2-V127-03.wav` | 52 | 50–55 |
| A#1.wav | `...-A#2-V127-04.wav` | 58 | 56–61 |
| E2.wav | `...-E3-V127-05.wav` | 64 | 62–67 |
| A#2.wav | `...-A#3-V127-06.wav` | 70 | 68–73 |
| E3.wav | `...-E4-V127-07.wav` | 76 | 74–79 |
| A#3.wav | `...-A#4-V127-08.wav` | 82 | 80–127 |

Fórmula das zonas (`computeRanges`, ramo não-layerIndex):
`keyLo[i] = i==0 ? 0 : (root[i-1]+root[i])/2 + 1` · `keyHi[i] = último ? 127 : (root[i]+root[i+1])/2`
Confirmado por FFT/HPS nos 5 primeiros samples: 82,89 Hz = E1, 117,17 = A#1, 164,95 = E2,
233,29 = A#2, 329,56 = E3. Bate com o metadado.

---

## 3. A cadeia do Ableton, medida no `.als` (não estimada)

**Chain "Ambient 2"**, por zona (`MultiSamplePart`):
- `SampleStart = 60000` (1,36 s) — **o Ableton não toca o início do arquivo**
- `SustainLoop Mode=1`, com Start/End próprios e **Crossfade de 47.913 a 105.332 amostras (1,1–2,4 s)**
- `ReleaseLoop Mode=3` (sem loop no release)

Valores por zona (`SampleStart, loopStart, loopEnd, crossfade`, em amostras do arquivo original):
```
E0    60000  118264  391906   47913
A#0   60000  125813  419585   68157
E1    60000  115748  420214   94559
A#1   60000  122038  396938  105332
E2    60000  118893  418956   94608
A#2   60000  121409  393793   91382
E3    60000  146572  391277   98385
A#3   60000  115119  406960   88328
```

**Devices, na ordem:**
1. **Sampler**: LP 24 dB @ **1112,28 Hz** (Res 0,068), volume **−1,125 dB**
   Envelope: attack 0,1 ms · decay 5892 ms até 28,4% · release **1927,4 ms**
2. **Reverb dentro da chain**: RoomSize 500, DecayTime **9377,86 ms**, PreDelay 2,5 ms,
   MixDirect 0,619 (dry 62%), shelf hi 4500 Hz ×0,7, shelf lo 72 Hz ×0,6,
   Chorus e Spin ligados, StereoSeparation 120, **sem shimmer**
3. **Track** (depois do rack): AutoFilter **HP 114,96 Hz** (Cutoff 45,7695) →
   AutoFilter **LP 2137,90 Hz** (Cutoff 96,3672, Res 0,138) →
   **Compressor2**: thr −11,25 dB, ratio 4,34:1, atk **0,098 ms**, rel 85,7 ms, knee 6 dB,
   auto-gain ligado (+8,66 dB pela fórmula padrão)

---

## 4. Estado atual do que está instalado

Os 8 WAVs em `Libraries\church-live\Pads\Prophetic Ambience\` são 24-bit / 44,1 kHz / estéreo,
363.360 frames (8,24 s), gerados assim:

1. Fonte 16-bit → filtros **LP 1112,28 → HP 114,96 → LP 2137,90**, todos 4ª ordem
   (`scipy.signal.butter(4,...) + sosfilt`, forward-only) → volume −1,125 dB
2. Corte em `SampleStart = 60000`
3. **Crossfade longo do Ableton assado no áudio** (potência constante, `sqrt`), na região
   `[loopEnd−X, loopEnd)` misturada com `[loopStart−X, loopStart)`,
   `X = min(Crossfade, loopStart, (loopEnd−loopStart)/2)`
4. Ganho estático **+3,02 dB** (pico 0,89)
5. Gravados com **chunk `smpl`** contendo loopStart/loopEnd corrigidos (menos 60000) e
   `midiUnityNote` = root

**Compressor NÃO foi assado** — ver seção 5.

Também presentes na pasta: 8 arquivos `Prophetic Ambience-C0..D3-V127-AMB.wav` de **78 bytes**.
São marcadores inertes que substituíram um render quebrado; o motor os ignora
(`createReaderFor` → nullptr → `continue`). **Podem ser apagados** — a ponte do Cowork não
apaga arquivos, por isso ficaram assim.

### Setup salvo (canal índice 0)
`attack=0.001` `release=1.93` `lcut=0.0` `hcut=0.0` `comp=0.0` `rvb=0.6` `vol=0.30`
Global: `rvbpreset=1` (Cloud), `rvbreturn=0.72`, `masterDb=10.0`, `masterPunch=1`.

**Pendências conhecidas nesses ajustes:**
- `comp` foi zerado quando o compressor estava assado; **precisa voltar a ligar** (~0,12).
- `vol` pode estar alto/baixo — os samples mudaram de nível três vezes.

---

## 5. O que JÁ FOI DESCARTADO (não repita)

| Suspeita | Veredito | Como foi verificado |
|---|---|---|
| Pasta errada dentro do zip | **Não era** | O instalador (`TimbreCatalog.h` ~575) extrai para staging e copia **por nome**, achatando. A estrutura interna do zip não afeta a instalação. |
| Notas erradas | **Era, e foi corrigido** | Roots eram 24,26,36,38,48,50,60,62; os reais são 40,46,52,58,64,70,76,82. |
| Render antigo quebrado | **Era** | Os 8 arquivos tinham **SHA-256 idêntico** (1 arquivo copiado 8×), DC −0,49, amostras cravadas em −1,0, L==R. Causado por ler o cutoff em Hz. |
| Timbre com 0 zonas | **Era cache** | Índice apontava para os arquivos antigos. Invalidado. Log confirmou `8 zonas` depois. |
| Compressor assado | **Causava metálico** | Ganho modulando até −5,45 dB (desvio 0,71 dB); resíduo a −17,1 dB do sinal com **0,94% da energia acima de 1,2 kHz** — recriando agudo numa faixa que o LP de 1112 Hz esvaziou. **Removido.** |
| Meu crossfade de loop | **Não é** | Comparados 0 / 0,10 / 0,25 / 0,50 / 1,00 / 1,09 s. O de 1,09 s (Ableton) dá a MENOR modulação (12,4% contra 28,5% sem crossfade) e emenda contínua (salto 1,1× o passo normal). |
| Streaming de disco | **Não é** | Com loop no `smpl`, o motor relê a zona inteira para a RAM (`z->streamed = false`). |
| Balanço lento de 0,81 Hz | **É da fonte** | Mesma amplitude (17,5% vs 16,8%) no meu arquivo e na fonte filtrada. É movimento musical do pad. |

---

## 6. O PROBLEMA EM ABERTO

Usuário relata: **"ainda tem um som no fundo tipo como se fosse um parafuso solto balançando,
tipo algo metálico"**. Persiste depois de todas as correções acima.

### Suspeitos ainda não testados

1. **Reverb do ChurchDAW** (mais provável). Preset global **Cloud**: 12,5 s de decaimento,
   `rate 0.16`, `deg 5.0` (a maior modulação da tabela em `WorshipReverb.h` ~905).
   Num pad sustentado o FDN é realimentado sem parar e os modos ressoam → zumbido metálico.
   Envio do canal em 0,6 e return 0,72 é bastante reverb.
2. **Cadeia do master**: `masterDb=10.0` com `masterPunch=1` (compressão paralela + saturação
   `tanh(p*1.95)*0.45` em `MasterProcessor.h`). Nível alto entrando em saturação = harmônicos.
3. **Batimento entre vozes** em acordes (duas notas na mesma zona, alturas diferentes).
4. **Não ser do timbre**: pode ser driver/buffer/xrun ou algo do master, presente em todos os timbres.

### Diagnóstico pedido ao usuário (ainda sem resposta)

- Zerar o **envio de reverb do canal** — se sumir, é o reverb.
- **Uma nota vs acorde** — se só no acorde, é batimento entre vozes.
- **Desligar o loop** do canal e segurar uma nota — se só aparece depois de ~8 s, é o loop.
- **Carregar outro timbre** (piano, New Pad) — se o ruído estiver lá também, **nunca foi deste timbre**.
- **Gravar o áudio** e mandar o WAV (Audacity com host *Windows WASAPI* + dispositivo
  *"(loopback)"*, ou a interface dele). 20 s de nota única + 20 s de acorde.

**A gravação é o caminho mais rápido.** Com o áudio dá para medir periodicidade, harmônicos e
correlação com o ciclo do loop (6,2 s) e acabar com o chute.

---

## 7. Plano B — exportar do Ableton (fidelidade garantida)

Já existe `...\Live Pianos 3.0\AmbienciaExport.mid`: 8 notas raiz (40,46,52,58,64,70,76,82),
9 s cada, 4 s de intervalo, 1min50 no total.

Procedimento para o usuário:
1. Arrastar o MIDI para a track `AMBIENCIA`
2. Chain Selector em **Ambiência 2** (valor 2)
3. **Desligar só o device Reverb de dentro da chain** (manter Sampler, os dois AutoFilter e o Compressor)
4. Export Audio, 44,1 kHz / 24-bit

Depois: fatiar nas 8 notas, aplicar o loop da tabela da seção 3 (ajustando os offsets, já que o
render começa do zero e não de 60000), gravar `smpl`, montar o pack.

Desligar o reverb na exportação é proposital: com a cauda assada não dá para fazer loop e o
usuário perde o controle de reverb ao vivo.

---

## 8. Diferenças estruturais que não têm como fechar

- **Ordem do reverb.** No Ableton ele está *antes* dos filtros da track, então a cauda sai
  filtrada. No ChurchDAW é um send *depois* do canal.
- **Decay do envelope.** O Ableton cai 11 dB ao longo de ~5,9 s enquanto a nota é segurada.
  O ChurchDAW só tem attack/release, não tem decay/sustain.
- **Inclinação dos filtros do canal.** `lowcut`/`highcut` do ChurchDAW são **1 polo** (6 dB/oitava):
  `Low Cut = 20 * 40^x`, `High Cut = 18000 / 22,5^x`. Por isso os filtros foram assados.

---

## 9. Como mexer no setup salvo (se precisar)

`%APPDATA%\ChurchDAW\ChurchDAW.settings` → `<VALUE name="chain">` →
`<SLOT index="0" id="internal:timbres" state="...">`.

O `state` é base64 do JUCE: `"<tamanho>.<dados>"`, tabela
`".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"`, 6 bits por char, LSB primeiro.
Decodificado: magic `0x21324356` + uint32 com o tamanho do XML + **XML em texto puro**
(não comprimido) + 1 byte de sobra.

Editar por substituição de texto no XML e reencodar. Sempre gravar `.bak` antes e **com o app
fechado** — senão ele regrava por cima ao sair. Ler em modo texto converte CRLF→LF e encolhe o
arquivo (inofensivo).

---

## 10. Pendências não relacionadas ao ruído

- **O pack nunca subiu para o servidor.** `download_url` aponta para
  `https://churchdaw.com/app/android/timbres/prophetic-ambience-v1.zip`, que não existe.
  Enquanto não subir (`Packaging\UploadAllTimbres.ps1`), o timbre só existe nesta máquina.
- **Catálogos divergentes:** `new_pad` está em `catalog.json` e `Packaging\catalog.json` mas
  **falta** em `timbre-catalog-v1.json`; `mellow_upright` e `pad_worship_2_0_ed_bento` só existem
  no `timbre-catalog-v1.json`.
- **Timbre "Ambiencia" bagunçado:** existe outro timbre na biblioteca montado por
  `parseNoteOnlyName` a partir de **28 FLACs** em `Libraries\church-live\Imported\`, misturando
  as três chains do Ableton (`E0.flac` junto com `E0-2.flac`) e 12 arquivos de nota solta
  (`A.flac`, `B.flac`…). Vale separar.
- **`ValidateTimbrePack.py --catalog` tem o id hardcoded** como `pad_worship_2_0_ed_bento`
  (~linha 398). Rodar com `--catalog` reprova qualquer outro timbre. Validar **sem** `--catalog`.
- Os 8 marcadores de 78 bytes (`*-AMB.wav`) podem ser apagados.

---

## 11. Estado dos arquivos (última gravação)

- Pack: `prophetic-ambience-v1.zip`, 17.005.285 bytes,
  sha256 `692384fb572fb47f...` (valor completo no `.sha256.txt` ao lado), 10 arquivos
- Os três catálogos foram atualizados com esse sha256, `download_bytes` e `file_count = 10`
- `ValidateTimbrePack.py --pack` (sem `--catalog`): **100% PASS nos 5 checks**

---

## 12. Limitação que motivou este handoff

O agente Cowork **não consegue ouvir áudio** nem controlar aplicativos — só ler e escrever
arquivos. Por isso o ruído metálico foi atacado três vezes por dedução, e ainda não foi
identificado. Rodar em Claude Code **não muda isso**. O desbloqueio real é o usuário gravar o
áudio, ou responder os testes de isolamento da seção 6.
