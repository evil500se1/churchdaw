# ChurchDAW — pesquisa e critérios do reverb worship

Este documento registra as decisões técnicas do motor nativo. O objetivo é obter
profundidade e musicalidade comparáveis à categoria de reverbs worship, sem
copiar firmware, código, respostas de impulso ou presets proprietários.

## O defeito que motivou o redesenho

Uma leitura de delay que muda enquanto a cauda está soando produz Doppler. Para
`y(t) = x(t - D(t))`, a frequência instantânea é multiplicada por
`1 - dD(t)/dt`. Portanto, até uma LFO lenta desafina a nota quando sua
profundidade em samples é grande. A arquitetura anterior modulava diretamente
os comprimentos do FDN e reproduzia exatamente a reclamação de piano
desafinando no final da cauda.

Referência:
[Time-Varying Delay Line Reads](https://www.dsprelated.com/freebooks/pasp/Time_Varying_Delay_Line_Reads.html).

## Arquitetura adotada

1. Predelay estéreo fixo, reflexões iniciais e quatro allpasses de difusão por
   canal.
2. FDN de oito linhas com comprimentos inteiros primos e espaçamento
   aproximadamente logarítmico. Isso evita coincidências periódicas e reduz o
   ringing metálico/modal sem interpolador dentro do feedback.
3. Atenuação de três bandas calculada por linha e aplicada antes de uma única
   matriz de mistura: `A(z) = U(t) Γ(z)`.
4. `U(t)` é Hadamard normalizada seguida por quatro rotações ortogonais lentas.
   O movimento alarga modos e aumenta a vivacidade, mas os read heads não se
   movem; logo não há Doppler de delay.
5. Pitch granular somente em ambientes que o pedem de forma explícita. Cloud,
   Hall, Room, Plate e os demais ambientes naturais não recebem harmônicos
   ocultos.
6. Trocas de preset usam fade-out curto, limpeza de estado e fade-in. Uma
   reversão rápida cancela corretamente o pedido intermediário.

A literatura de FDN recomenda uma matriz ortogonal para o protótipo sem perdas
e filtros de atenuação por linha para realizar o T60 dependente da frequência.
Ela também mostra que a modulação de uma matriz unitária pode melhorar a
qualidade da cauda mantendo a estabilidade:

- [RIR2FDN: An Improved Room Impulse Response Analysis and Synthesis](https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_20.pdf)
- [Time-varying feedback matrices in feedback delay networks](https://www.sebastianjiroschlecht.com/publication/schlecht-2015-hi/)
- [Improved Reverberation Time Control for Feedback Delay Networks](https://www.dafx.de/paper-archive/details/JbPmKKJc3yrN_fd36-MBoQ)
- [Optimizing tiny colorless feedback delay networks](https://link.springer.com/article/10.1186/s13636-025-00401-w)

## Curva worship

Um decay longo sozinho não produz um bom reverb worship. O desenho precisa
preservar o ataque do piano, formar a nuvem depois dele, evitar acúmulo grave e
escurecer progressivamente a cauda.

Critérios usados:

- predelay de 48–72 ms nos ambientes grandes para separar o ataque;
- poucas reflexões iniciais nos ambientes Cloud/Shimmer/Swell;
- médios como a parte mais longa da cauda;
- graves um pouco mais curtos para não embolar acordes e sustain;
- agudos muito mais curtos para evitar chiado e aspereza;
- alta difusão para a cauda nascer densa;
- ducking moderado no Cloud e forte no Swell;
- movimento ortogonal lento e raso, abaixo da percepção de chorus;
- largura estéreo ampla, validada também em Mid e Side.

Fabricantes de referência expõem os mesmos grupos de controle. O manual BigSky
define predelay, decay, tone e modulação leve, e recomenda quantidade baixa para
apenas sugerir shimmer. O H90 descreve modulação sutil para reduzir ringing,
Hall com decay independente por banda e Blackhole com ataque suave e cauda
longa:

- [Strymon BigSky — manual oficial](https://www.strymon.net/manuals/BigSky_UserManual_RevD.pdf)
- [Eventide H90 — algoritmos de reverb](https://cdn.eventideaudio.com/manuals/h90/1.3.0/content/algorithms/reverb.html)
- [Pre-delay e clareza do ataque](https://www.izotope.com/community/blog/reverb-pre-delay)

## Calibração dos ambientes

| Ambiente | Predelay | T60 grave | T60 médio | T60 agudo | Função |
| --- | ---: | ---: | ---: | ---: | --- |
| Bloom | 52 ms | 7,0 s | 8,5 s | 4,0 s | nuvem que cresce após o ataque |
| Cloud | 62 ms | 10,0 s | 12,5 s | 5,5 s | piano/pad worship principal |
| Chorale | 48 ms | 7,0 s | 8,5 s | 4,0 s | camada coral com oitava discreta |
| Shimmer | 64 ms | 7,5 s | 9,5 s | 4,0 s | oitava explícita e regeneração limitada |
| Magneto | 34 ms | 4,0 s | 4,8 s | 2,5 s | textura mais curta |
| Nonlinear | 18 ms | 1,8 s | 2,4 s | 1,2 s | envelope especial e controlado |
| Reflections | 22 ms | 2,6 s | 3,4 s | 1,7 s | presença de reflexões |
| Room | 12 ms | 1,4 s | 1,8 s | 0,9 s | sala natural curta |
| Hall | 38 ms | 5,4 s | 6,5 s | 3,2 s | hall amplo sem pitch |
| Plate | 18 ms | 3,2 s | 4,0 s | 2,7 s | brilho controlado |
| Spring | 13 ms | 2,1 s | 2,6 s | 1,6 s | caráter curto |
| Swell | 72 ms | 10,5 s | 13,5 s | 5,8 s | entrada macia e grande sustentação |

Os números são alvos de T60, não apenas rótulos de interface. A calibração é
medida por banda depois de todos os filtros e da matriz de feedback.

## Contrato de validação

Antes de um APK/AAB final:

1. medir afinação sustentada em 44,1/48/96 kHz e blocos pares/ímpares;
2. medir a liberação de nota em L, R, Mid e Side;
3. rejeitar um controle negativo que use delay móvel conhecido;
4. medir T60 em 125 Hz, 1 kHz e 8 kHz;
5. verificar a oitava do Shimmer no início e após 30 s;
6. executar cada teste em processos novos para detectar estado marginal;
7. testar troca e reversão rápida de presets;
8. confirmar finitude, ausência de clip e ausência de alocação no callback;
9. instalar os bytes finais no Android e tocar acordes reais com pausas;
10. conferir CPU, blocos lentos, streams, NaN, crash e ANR no aparelho.

## Resultado do gate 1.3.0

O gate final de 28/07/2026 fechou sobre o header
`31F509D10C3F3A79EA761A2B51BC6D0EEE1E2C32731558115ADCF560A7A983C2`.

- `WORSHIP_REVERB_TESTS_OK` em 44,1/48/96 kHz, blocos pares e ímpares.
- Mudança de afinação início/fim da cauda Cloud: `0,040 cent`.
- Shimmer após 30 s: pior P95 `0,176 cent`.
- Cloud T60 em 125 Hz / 1 kHz / 8 kHz:
  `10,109 / 12,130 / 5,559 s`.
- Reversão Cloud → Hall → Cloud: salto máximo `0,002`, erro assentado zero.
- Debug, Release e AddressSanitizer: zero acesso fora dos limites.
- TANK3 físico: Cloud e Shimmer em `Return 100%`, master `+10 dB`, cauda até
  silêncio, zero NaN, clip, falha de stream, crash ou ANR.

O desenho continua sendo implementação original da ChurchDAW. Nomes de
categorias e equipamentos de referência servem para comunicar intenção sonora;
nenhum firmware, código, resposta de impulso, preset binário ou sample da
Strymon/BigSky foi incluído.
