# Church Organ Engine Core — fundação clean-room

Esta árvore contém a fundação C++17 independente de JUCE e de interface do
Church Organ Engine. Nenhum código, tabela, amostra, resposta impulsiva ou
preset de fabricante foi usado.

## Escopo implementado

- banco compartilhado de 96 tonewheels cromáticos, de C0 a B7;
- fase contínua por recorrência complexa;
- fase inicial e tolerância de frequência determinísticas por seed;
- nove drawbars com curva calibrada em passos de 3 dB e smoothing;
- foldback ChurchDAW original, apenas por oitavas;
- 128 estados fixos de tecla do Upper manual;
- eventos note-on, note-off e all-notes-off com offset dentro do bloco;
- saída mono ou dual-mono estéreo;
- diagnósticos POD publicados através de atomics de 32/64 bits;
- nenhum container dinâmico, lock, I/O, texto ou chamada JUCE no render.

Fluxo da Fase 1:

```text
Upper note events
  -> contact ramps
  -> 9 x foldback map
  -> shared TonewheelBank
  -> calibrated drawbar sum
  -> fixed headroom
  -> mono / dual-mono output
```

## Foldback original

As janelas abaixo são uma decisão de projeto ChurchDAW. Elas não são uma
transcrição de tabelas históricas ou comerciais. Cada pedido fora da janela é
movido por oitavas até entrar nela, preservando a classe cromática.

| Drawbar | Offset | Mínimo | Máximo |
|---|---:|---:|---:|
| 16' | -12 | 24 | 95 |
| 5 1/3' | +7 | 31 | 102 |
| 8' | 0 | 24 | 107 |
| 4' | +12 | 36 | 107 |
| 2 2/3' | +19 | 43 | 110 |
| 2' | +24 | 48 | 111 |
| 1 3/5' | +28 | 52 | 112 |
| 1 1/3' | +31 | 55 | 114 |
| 1' | +36 | 60 | 119 |

## Contrato real-time

1. Chame `prepare()` fora da thread de áudio.
2. O adapter copia os eventos MIDI do bloco com `queueUpperNoteOn/Off()`.
3. O mesmo chamador executa `render()`.
4. Alterações de drawbar podem vir de outra thread: posições são quantizadas em
   atomics de 32 bits e lidas uma vez por bloco.
5. A fila de eventos é SPSC por contrato e tem 512 posições fixas.
6. `render()` rejeita blocos maiores que `maximumBlockSize`, em vez de alocar.

## Limitações assumidas desta fundação

- apenas Upper manual;
- núcleo tonewheel tradicional não responde à velocity;
- saída ainda é dual-mono;
- sem key click, percussion, leakage, scanner, amp, rotary ou Worship Layer;
- foldback ainda precisa de validação perceptual/ABX;
- não há state serializer nem adapter JUCE;
- não há otimização SIMD;
- a meta de CPU precisa ser validada também em Android real e macOS.

Os testes isolados ficam em `Tests/ChurchOrgan` e não dependem do CMake raiz.
