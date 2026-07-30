# Church Organ Engine — fase de articulação

Esta fase é uma implementação clean-room e independente do núcleo atual. Os
três módulos não dependem de JUCE, não fazem I/O e usam somente armazenamento
fixo depois de `prepare()`.

## Módulos

### `MultiContactKeying`

- nove contatos independentes por tecla, um para cada drawbar;
- sequência original ChurchDAW de fechamento e abertura;
- dispersão temporal e jitter determinístico por seed/nota/contato/evento;
- ataque, release e bounce de contato limitados a `[0, 1]`;
- lista ativa fixa para evitar percorrer 128 teclas inativas;
- `advance()` deve ser chamado uma vez por sample antes da soma de drawbars.

Parâmetros padrão: spread de 1,65 ms no ataque e 1,10 ms no release, jitter de
0,16 ms, bounce de 5,5% com decaimento de 2,8 ms.

### `KeyClickGenerator`

- click de note-on e note-off sem samples;
- ruído determinístico xorshift64*, filtrado por high-pass + low-pass;
- envelopes independentes por nota e armazenamento máximo de 128 vozes;
- pequena influência opcional de velocity apenas no click, não no tonewheel;
- `renderSample()` retorna um sinal para mistura com trim próprio.

Parâmetros padrão: 850–6800 Hz, 7,5 ms no ataque, 4,5 ms no release, níveis
0,075 e 0,032.

### `HarmonicPercussion`

- seleção de 2º harmônico (`+12`, drawbar 4') ou 3º (`+19`, drawbar 2 2/3');
- tempos fast/slow e níveis soft/normal;
- single-trigger tradicional ou retrigger por cada note-on;
- ataque curto, decay exponencial e release quando a nota proprietária sai;
- o chamador fornece a amostra do tonewheel indicado por
  `harmonicDrawbarIndex()` e `triggeredMidiNote()`.

No modo single-trigger, uma nova percussão só arma quando não havia tecla
pressionada antes do note-on. No modo retrigger, todo note-on válido reinicia o
envelope e muda a nota proprietária.

## Contrato real-time e integração

1. Chame `prepare()` fora da thread de áudio.
2. Na posição sample-accurate do evento, chame `noteOn()`/`noteOff()` nos três
   módulos.
3. Por sample, chame `MultiContactKeying::advance()`.
4. Multiplique cada contribuição de drawbar por
   `contactGain(note, drawbar)`.
5. Some `KeyClickGenerator::renderSample()` com trim controlado.
6. Para percussão, mapeie `harmonicDrawbarIndex()` e
   `triggeredMidiNote()` pelo foldback existente, leia a roda compartilhada e
   passe o carrier a `HarmonicPercussion::processSample()`.

Os setters de percussão são RT-safe, mas pertencem à thread de áudio. O adapter
de parâmetros deve entregar mudanças nessa thread; os módulos não prometem
acesso concorrente.

## Limitações deliberadas

- ainda não estão conectados ao `ChurchOrganCore`;
- os valores padrão precisam de validação perceptual/ABX em caixas e fones;
- não há modelagem de desgaste por instrumento, leakage ou crosstalk;
- o click é sintético e mono; a espacialização pertence ao estágio de amp/
  rotary;
- a percussão expõe envelope/carrier, mas não decide redução do drawbar 1';
- single-trigger mantém uma nota proprietária; ao soltá-la o envelope entra em
  release mesmo que outra tecla continue pressionada;
- não há serializer de preset nesta fase;
- determinismo bit-exato é garantido apenas no mesmo compilador/plataforma.
