# Church Organ Listening QA

## Escopo

Este harness offline gera nove frases autorais sem samples, IRs ou assets
externos. Os WAVs usam diretamente `TonewheelOrganEngine` e
`TransistorOrganEngine`, com eventos sample-accurate, blocos fixos de 128
samples e seed determinística.

- formato: PCM WAV estéreo, 24-bit, 48 kHz;
- duração: 16 s nos seis exemplos Tonewheel e 15,5 s nos três Transistor;
- conteúdo: acordes, staccato/release, glissando cromático, palm smear,
  expressão e transições Stop/Slow/Fast/Brake distribuídas pelo pack;
- o vetor final e a escrita WAV ficam fora do callback;
- cada chamada de `render()` é cercada por contador global de alocações;
- todo exemplo é renderizado novamente do zero e o PCM 24-bit deve repetir
  exatamente o checksum FNV-1a.

O diretório `outputs` contém os WAVs, `listening_metrics.csv` e
`loudness_matrix.csv`.

## Limites automatizados

| Medida | Limite |
|---|---:|
| Sample peak | 0,015 a 0,9995 |
| True-peak aproximado 4x | <= 1,05 |
| RMS | -50 a -3 dBFS |
| DC absoluto | <= 0,005 |
| DC/RMS | <= 3,5% |
| Crest factor | 1 a 30 dB |
| Salto em borda de bloco | <= 0,40 e <= 14x delta RMS |
| Perda no fold-down mono | <= 12 dB |
| Últimos 500 ms | RMS <= -40 dBFS; pico <= 0,10 |
| NaN/Inf, invalid e alocações RT | zero |
| Determinismo | checksum PCM idêntico em duas renders |

O true-peak 4x é aproximado por interpolação cúbica Catmull-Rom nas fases
0,25/0,50/0,75. Ele é um detector conservador de overshoot, não substitui um
medidor BS.1770 de produção.

## Resultado do listening pack

Todos os nove arquivos passaram.

| Cena | RMS dBFS | Pico | TP 4x | Crest dB | Mono dB | Tail dBFS | Checksum PCM |
|---|---:|---:|---:|---:|---:|---:|---|
| Tonewheel Clean | -34,639 | 0,167815 | 0,167817 | 19,135 | -0,496 | -119,69 | `6B775A3E71882A87` |
| Tonewheel Aged | -30,442 | 0,179257 | 0,179283 | 15,512 | -0,578 | -87,12 | `968E5AD0F0F6DE39` |
| Tonewheel Road | -28,666 | 0,154217 | 0,154217 | 12,429 | -0,890 | -70,85 | `FB915D7986AD7332` |
| Classic Dry | -31,196 | 0,163542 | 0,163542 | 15,469 | 0,000 | -120,71 | `A2C566CF896F3A7B` |
| Gospel Motion | -29,566 | 0,131478 | 0,131672 | 11,943 | -1,459 | -77,62 | `0B61C383FE171C3F` |
| Soft Worship | -34,016 | 0,112331 | 0,112334 | 15,026 | -0,608 | -107,69 | `44AA4F04145D93D8` |
| Transistor British | -20,260 | 0,571198 | 0,571611 | 15,395 | -0,007 | silêncio digital | `186108327E2E08B4` |
| Transistor Italian | -21,556 | 0,542795 | 0,543156 | 16,249 | -0,011 | silêncio digital | `418C484B8EDCEC63` |
| Transistor German | -26,233 | 0,296168 | 0,296376 | 15,663 | -0,004 | silêncio digital | `7A53A8E80FF8471D` |

DC máximo: `0,00014243`. Alocações dentro de `render()`: `0` em todas as
cenas. Invalid/NaN/Inf: `0`.

## Matriz de loudness 1/4/8/16 notas

O probe Tonewheel usa registro Full (nove drawbars em 8), master interno em
0 dB, amp Clean, scanner/rotary bypass e sem leakage/crosstalk/hum. Não há
o headroom legado de -4 dB nem trim externo. O Transistor usa o perfil
British e seus ganhos internos padrão.

| Motor | Notas | RMS dBFS | Pico | Crest dB | Delta RMS |
|---|---:|---:|---:|---:|---:|
| Tonewheel | 1 | -13,181 | 0,499786 | 7,157 | — |
| Tonewheel | 4 | -12,045 | 0,570873 | 7,176 | +1,136 dB |
| Tonewheel | 8 | -10,462 | 0,603214 | 6,071 | +1,583 dB |
| Tonewheel | 16 | -9,429 | 0,610160 | 5,138 | +1,032 dB |
| Transistor | 1 | -25,016 | 0,121522 | 6,709 | — |
| Transistor | 4 | -19,098 | 0,373943 | 10,554 | +5,919 dB |
| Transistor | 8 | -14,646 | 0,568151 | 9,736 | +4,451 dB |
| Transistor | 16 | -11,495 | 0,639835 | 7,616 | +3,151 dB |

Leitura perceptual inferível:

- Tonewheel mantém crescimento contido e pico praticamente estabilizado de
  8 para 16 notas. O auto-headroom evita salto de acordes e clipping sem
  colapsar o RMS;
- Transistor preserva crescimento mais próximo da soma polifônica. A passagem
  1→4 notas é perceptualmente maior (+5,9 dB), mas o crescimento comprime
  progressivamente e o pico de 16 notas continua seguro;
- não se deve inserir compensação fixa por contagem de notas: os dois motores
  deliberadamente têm respostas polifônicas diferentes.

## A/B com o sampled legado

Referência somente para medição; nenhum sample entrou nos WAVs gerados:

`B3 Full-C3-V127-EFZO.flac`

- SHA-256:
  `70F7776A8A917124A81B525B687D253EB8A8688859F19210B23F0B4DB331837E`;
- fonte: estéreo 24-bit/44,1 kHz, 7,2722 s;
- janela estável: 1,000 a 3,000 s;
- medição `ffmpeg astats`, canais combinados: RMS `-14,8235 dBFS`,
  pico `-1,3451 dBFS`, DC máximo `0,000045`;
- após o `polyphonyHeadroom=0,6309573` do runtime legado: RMS efetivo
  `-18,8235 dBFS`, pico efetivo `-5,3451 dBFS`.

Novo Tonewheel Full, uma nota, janela 1,000 a 3,000 s, master interno 0 dB e
sem -4 dB adicional: RMS `-13,1813 dBFS`, pico `0,499786`
(`-6,0243 dBFS`).

Contra a saída efetiva antiga, o novo motor fica `+5,642 dB` em RMS e
`-0,679 dB` em pico. Isso confirma que não é necessário reaplicar o trim de
-4 dB: o pico já está praticamente alinhado e o novo envelope é mais denso,
com crest menor que o sample legado.

Comando da janela de referência:

```powershell
ffmpeg -hide_banner -nostats -ss 1.0 -t 2.0 `
  -i "...\B3 Full\B3 Full-C3-V127-EFZO.flac" `
  -af "astats=metadata=1:reset=0" -f null NUL
```

## Problemas audíveis inferíveis

Nenhum defeito bloqueante foi detectado:

- true-peak ficou praticamente igual ao sample peak, sem overshoot oculto;
- nenhum salto de borda excedeu o comportamento sample-a-sample da própria
  frase; não há evidência de click por tamanho de bloco;
- DC, NaN/Inf, tail e mono fold-down passaram com ampla margem;
- Gospel é a cena mais larga, mas perde apenas 1,46 dB em mono;
- o ruído residual intencional de Road termina em -70,85 dBFS, seguro;
- os três Transistor chegam a silêncio digital após release.

Ponto para avaliação humana: o Transistor cresce mais com polifonia que o
Tonewheel. Isso é mensurável e seguro, mas deve ser ouvido no contexto do
mix antes de definir presets finais. Não há fundamento nos testes para
adicionar AGC rápido ou trim fixo ao novo Church Organ.

## Execução

```powershell
cmake -S Tests/ChurchOrgan/ListeningPhase `
  -B <build> -A x64
cmake --build <build> --config Release --parallel
ctest --test-dir <build> -C Release --output-on-failure
```
