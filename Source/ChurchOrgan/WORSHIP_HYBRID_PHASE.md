# Church Organ — Worship Hybrid Phase

## Escopo

`WorshipHybridEngine` é a quarta família do Church Organ. Ela compõe, sem
alterar os motores congelados:

- `TonewheelOrganEngine`;
- `WorshipLayerEngine`;
- `AutoMixContext`.

O wrapper não depende de JUCE, UI, sistema operacional ou buffers de outros
canais. O mesmo núcleo pode ser usado pelos adapters de Windows, macOS e
Android.

## Fluxo de áudio

1. Uma fila fixa recebe a performance MIDI com `sampleOffset`.
2. Antes do bloco, os eventos que pertencem ao bloco são enviados aos dois
   motores mantendo exatamente o offset original.
3. O Tonewheel renderiza em buffers de trabalho estéreo.
4. Os ganhos lentos do AutoMix esculpem apenas o Tonewheel:
   - zona abaixo de 180 Hz preservada;
   - low-mid 180–900 Hz;
   - mid 900–3200 Hz;
   - agudos acima de 3200 Hz preservados;
   - compensação conservadora de drive e proteção de master.
5. As camadas Worship são somadas ao sinal e escrevem um send de reverb
   estéreo independente.
6. O wrapper aplica ganho final suavizado, bloqueio DC e proteção de pico.

A saída principal é sobrescrita. O send de reverb é opcional e também é
sobrescrito. Para mono, o wrapper entrega a média coerente dos dois canais.
Buffers de main e send devem ser regiões distintas.

## Eventos e sustain

A fila tem armazenamento fixo para 1024 eventos e a mesma prioridade do
Tonewheel:

1. all-notes-off;
2. note-off;
3. sustain;
4. expression/volume;
5. note-on.

Eventos além do bloco são carregados para o bloco seguinte sem perder a
posição. Um evento exatamente em `numSamples` começa no sample zero do bloco
seguinte.

Upper e Lower espelham as camadas Worship por padrão; Pedal é opcional. O
wrapper mantém estado físico e sustentado por manual. Quando a mesma nota está
ativa em dois manuais, existe uma única voz Worship e ela só recebe note-off
após a última referência desaparecer. Sustain é traduzido para as camadas no
offset correto, embora o motor Worship congelado não tenha evento de pedal
próprio.

O mask de manuais só muda com a performance ociosa e sem evento pendente.
Isso evita reinterpretar uma nota no meio do bloco.

## Macro Worship Intensity

`setWorshipIntensity()` controla simultaneamente:

- o parâmetro Worship já usado pelo scanner, amp e rotary do Tonewheel;
- o perfil perceptual das sete camadas Worship;
- uma compensação máxima de apenas 0,72 dB no Tonewheel no topo do macro.

A compensação mantém o órgão presente enquanto o bed cresce e evita que o
macro se comporte como dois faders independentes.

## AutoMix

O AutoMix recebe somente envelopes já calculados:

- piano;
- pad externo;
- strings externas;
- proximidade do master.

Nenhum áudio de outro canal entra ou fica retido no Hybrid. Reaction e
recovery continuam lentos e limitados pelo `AutoMixContext`. O wrapper usa:

- `worshipLayer` nas camadas;
- `organLowMid` e `organMid` no Tonewheel;
- `drive` como trim pós-drive conservador;
- `output` no master;
- `room` no send.

O trim pós-drive é deliberado: a API congelada do Tonewheel não oferece um
offset transitório de drive que preserve ao mesmo tempo a automação feita pelo
usuário. O wrapper não sobrescreve esse parâmetro.

## Qualidade e segurança

| Hybrid | Tonewheel | Worship |
|---|---|---|
| Eco | Eco | Eco |
| Standard | Standard | Standard |
| Ultra | Ultra | Ultra |

Faixas validadas:

- 44,1, 48, 96 e 192 kHz;
- blocos até 4096 samples;
- main abaixo de 0,985;
- send abaixo de 0,500;
- valores não finitos sanitizados;
- bloqueio DC em 8 Hz;
- estado de filtros subnormal limpo;
- render sem mutex e sem alocação dinâmica.

O objeto contém os dois estados completos e buffers fixos; no build MSVC
validado, `sizeof(WorshipHybridEngine) == 1.212.144` bytes. Instâncias devem
preferencialmente viver no heap do canal, não na pilha da callback.

## Estado B e futura troca seamless

`prepareStateB()` prepara fora da callback:

- Tonewheel completo;
- Worship completo;
- AutoMix;
- parâmetros observáveis do Tonewheel;
- parâmetros das sete camadas;
- intensidade;
- controles e parâmetros AutoMix.

O estado B permanece silencioso e acessível para configuração adicional. O
wrapper não oferece `swap`: uma troca realmente seamless precisa renderizar A
e B ao mesmo tempo, fazer crossfade e manter as caudas de A até o silêncio.
Trocar ponteiros diretamente cortaria rotary, amp e releases, por isso essa
operação foi deixada para uma fase futura.

A API congelada de `WorshipLayerEngine` não expõe getters para seu ganho
interno de saída nem para ducking externo. Configurações feitas diretamente
nesses dois setters devem ser repetidas no ponteiro B pelo futuro dono da
transição. Todo estado controlado pelo wrapper e todo parâmetro com getter é
clonado.

## Validação isolada

Projeto:

`Tests/ChurchOrgan/HybridPhase`

Cobertura:

- offsets no bloco, entre blocos e na fronteira;
- blocos de 128 e 257 samples;
- 44,1–192 kHz;
- Upper/Lower com referência compartilhada;
- Pedal configurável;
- sustain, expression e all-notes-off;
- preservação de release/tails;
- determinismo bit a bit;
- preparação e clone do estado B;
- AutoMix lento e limitado;
- mono, estéreo e send opcional;
- NaN/Inf, DC e limites de pico;
- zero alocação em 80 callbacks rastreadas.

Resultados:

- Release: 78 checks, 0 falhas; CTest 2/2.
- Debug: 78 checks, 0 falhas; CTest 2/2.

Benchmark Release, 48 kHz, bloco 128, acorde típico de 14 notas + pedal,
8 segundos de áudio:

| Qualidade | Núcleo | Tempo real | Main peak | Send peak |
|---|---:|---:|---:|---:|
| Eco | 15,89% | 6,29x | 0,3860 | 0,0970 |
| Standard | 14,53% | 6,88x | 0,3629 | 0,0927 |
| Ultra | 16,08% | 6,22x | 0,4086 | 0,1019 |

Os percentuais são de um núcleo lógico da máquina de validação e servem como
baseline comparativo, não como garantia de hardware.

Comandos reproduzíveis:

```powershell
cmake -S Tests\ChurchOrgan\HybridPhase `
      -B build\hybrid-phase `
      -G "Visual Studio 17 2022" -A x64
cmake --build build\hybrid-phase --config Release --parallel 8
ctest --test-dir build\hybrid-phase -C Release --output-on-failure
cmake --build build\hybrid-phase --config Debug --parallel 8
ctest --test-dir build\hybrid-phase -C Debug --output-on-failure
```
