# Transistor Organ Engine — clean-room

Este núcleo C++17 modela uma família original de órgãos combo/transistorizados.
Os três perfis são linguagens tímbricas genéricas criadas para o ChurchDAW;
não são emulações, medições ou reproduções de circuitos, presets, nomes
comerciais ou instrumentos de terceiros.

## Gerador e divisão compartilhados

`TransistorDividerBank` mantém 12 acumuladores fixed-point livres, um por
classe cromática. Os 8 taps de oitava de cada cadeia derivam dos bits do mesmo
acumulador:

```text
12 master counters
  -> 8 coherent octave taps per pitch class
  -> 96 shared taps (MIDI 24..119)
  -> polyBLEP square / 36% pulse / ramp
  -> tab/rank routing
```

Uma mesma frequência é calculada uma vez por sample e reutilizada por todas as
teclas e tabs. Portanto, acordes e oitavas mantêm fase relativa livre e
coerente; não existem senoides ou osciladores reiniciados por tecla/drawbar.
As bordas de square/pulse e o wrap da ramp recebem correção polyBLEP antes da
mistura.

## Tabs e perfis

As oito tabs extensíveis são:

1. Bass 16
2. Principal 8
3. Reed 8
4. Flute 4
5. String 4
6. Bright 2
7. Mixture
8. Brilliance

Os perfis `britishCombo`, `italianCombo` e `germanCombo` usam matrizes próprias
de square, pulse e ramp, níveis de ranks, filtros de cabinet, vibrato e ganho.
Os nomes descrevem apenas três direções autorais:

- British: centro firme, reed nasal e brilho controlado;
- Italian: pulsos/ramp mais presentes e mixtures incisivas;
- German: corpo arredondado, ramp suave e menos brilho extremo.

## Caminho de sinal

```text
sample-accurate Upper events
  -> gate + deterministic key transient
  -> shared divider taps
  -> profile tab/rank matrix
  -> global generator vibrato
  -> DC coupling
  -> light symmetric drive
  -> DC coupling
  -> two cabinet poles + subtle right all-pass
  -> smooth bounded cabinet protection + safety headroom
  -> mono or stereo output
```

O vibrato modula os 12 geradores antes da derivação das oitavas, preservando
coerência de divisão. O key transient é ruído determinístico band-pass curto;
velocity afeta somente 10% do click, nunca o corpo sustentado.

## Estrutura otimizada

- os acumuladores Q0.32 mantêm erro de afinação abaixo de 0,005 cent no pior
  caso suportado e evitam conversões de 64 bits no hot path;
- uma lista fixa de taps ativos é reconstruída somente em mudanças de notas;
  os 12 contadores continuam sempre livres, mesmo quando nenhum tap é lido;
- a correção polyBLEP da borda inicial é calculada uma vez e compartilhada
  pelas formas square, pulse e ramp;
- acordes normais usam coeficientes de tab/rank pré-combinados e caminho
  direto de baixa latência;
- a partir de 48 vozes, gates são agregados pelas oito rotas de divider e
  depois misturados por tab. A transformação é linearmente equivalente à
  soma voz a voz e reduz trabalho redundante sem remover harmônicos.

No host de referência Windows x64, Release, 48 kHz e blocos de 128 samples,
a medição final foi 6,56% de um core para 10 notas e 19,69% para
128 notas. Os limites automatizados são 8% e 25%, respectivamente. O teste
espectral mediu redução de alias residual de 98,27% contra square ingênua.

## Contrato real-time

- `prepare()` e mudanças estruturais ficam fora do callback;
- eventos do bloco entram pela fila fixa ordenada de 512 posições;
- setters de tabs/perfil/drive/vibrato são `noexcept`, mas devem ser entregues
  pela thread de áudio;
- `render()` aceita no máximo o bloco preparado e não aloca, bloqueia, faz I/O
  ou usa JUCE;
- `right == nullptr` produz mono; em estéreo o canal direito recebe uma
  diferença de fase/cabinet limitada por `stereoWidth`;
- Upper manual é a primeira implementação. O enum de manual e as rotas fixas
  deixam Lower/Pedal para uma fase futura.

## Limitações

- os perfis ainda exigem validação perceptual/ABX em Windows, macOS e Android;
- o polyBLEP reduz aliasing de borda, mas não é um oscilador brick-wall;
- drive é deliberadamente leve e não usa oversampling nesta fase;
- cabinet é uma abstração de polos/all-pass, não uma resposta impulsiva;
- não há leakage, chorus ensemble, reverb, rotary ou serializer de preset;
- foldback é por oitavas para caber no banco 24..119 e precisa de validação
  musical nas extremidades;
- determinismo bit-exato é limitado ao mesmo compilador/plataforma;
- parâmetros simultâneos pertencem à thread de áudio; não há atomics/MPSC.
