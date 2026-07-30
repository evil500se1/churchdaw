# Pipe Organ Engine — síntese clean-room

O `PipeOrganEngine` é um instrumento autoral por síntese. Não usa samples,
impulsos, assets externos, medições, tabelas de fabricantes ou código de
instrumentos comerciais.

## Arquitetura

```text
Upper / Lower / Pedal events
  -> fixed coupler matrix
  -> 384 fixed division-note voices
  -> 11 independent pipe ranks per voice
  -> rank speech + chiff + deterministic wind
  -> band-limited additive partials
  -> division tremulant and wind reservoir
  -> swell + progressive crescendo registration
  -> deterministic pipe-field stereo
  -> DC coupling + smooth bounded headroom
```

Cada pipe mantém fase, envelope, fala, chiff e microinstabilidade próprios. Uma
tabela senoidal matemática de 8192 pontos alimenta, durante `prepare()`, um
banco interno de waveforms normal/bright de 1024 pontos por rank e quantidade
permitida de parciais. Essas tabelas são síntese pré-calculada, não samples de
instrumento. No render, o motor seleciona a tabela cujo último harmônico fica
abaixo de 46% da sample rate.

## Divisões, ranks e controles

As divisões implementadas são `upper`, `lower` e `pedal`. Qualquer divisão
pode ser acoplada a outra por uma matriz fonte/destino, sem criar eventos ou
vozes dinamicamente.

Ranks combináveis:

1. Principal
2. Flute
3. Gedackt
4. Bourdon
5. Octave
6. Mixture
7. Strings
8. Reed
9. Trumpet
10. Choir
11. Pedal

Cada rank tem espectro, ataque, release, duração de fala, brilho inicial,
chiff, instabilidade e ganho próprios. Strings usa um segundo pipe levemente
desafinado em Standard/Ultra.

O crescendo não sobrescreve os stops do usuário: ele cria uma registration
paralela progressiva, com limiar e máximo específicos por rank. Swell,
crescendo e níveis de rank possuem smoothing.

## Temperamentos e vento

Quatro temperamentos originais estão disponíveis: Equal, Church Well, Gentle
Meantone e Pure Choir. Eles são offsets cromáticos moderados criados para este
motor, não transcrições de uma afinação histórica específica.

Cada divisão possui um reservatório de vento compartilhado. A carga dos pipes
reduz lentamente a pressão; ruído determinístico de baixa frequência e
microinstabilidade individual movimentam a afinação. Tremulant modula pressão,
pitch e amplitude de forma coerente dentro da divisão.

## Qualidade

- Eco: quatro parciais, lookup direto, sem chiff e sem pipe duplo;
- Standard: oito parciais, interpolação linear, chiff e Strings duplo; acima
  de 95 vozes divisionais o par é colapsado para um pipe coerente, evitando
  sobrecarga em clusters extremos;
- Ultra: doze parciais, interpolação cúbica e campo estéreo completo.

Todos os modos preservam o mesmo estado e API. A qualidade é escolhida em
`prepare()` e não muda dentro do callback. Standard/Ultra são os modos
musicais principais; Eco é também o modo de segurança para testes patológicos
de 128 teclas simultâneas em uma única divisão.

## Contrato real-time

- `prepare()` ocorre fora da thread de áudio;
- a fila sample-accurate tem 1024 eventos fixos e ordenados;
- `render()` rejeita blocos maiores que o máximo preparado;
- nenhum container dinâmico, lock, I/O, JUCE ou texto é usado no render;
- setters pertencem à thread de handoff de áudio e não são concorrentes;
- `right == nullptr` produz mono; estéreo mantém pan determinístico por pipe;
- saída passa por DC blocker e curva suave limitada abaixo de `0.97`.

O objeto contém aproximadamente um megabyte de tabelas/estados fixos. A
integração deve possuí-lo no processor alocado em heap, não como variável local
na pequena stack do callback.

## Limitações

- é um primeiro Upper/Lower/Pedal completo, mas ainda sem serializer de preset;
- mudanças de temperament durante nota sustentada não fazem glide;
- não há reverb/convolution, blower gravado, ação mecânica ou ruído de sala;
- acopladores trabalham em uníssono; sub/super-octave couplers ficam para fase
  futura;
- o modelo de wind reservoir é perceptual, não uma simulação CFD;
- valores precisam de ABX e ajuste musical em caixas, fones e templos reais;
- determinismo bit-exato é garantido somente no mesmo compilador/plataforma;
- Android e macOS ainda precisam de benchmark nos dispositivos reais.
