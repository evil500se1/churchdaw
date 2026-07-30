# ChurchDAW — Arquitetura Premium de Timbres

Status: especificação de produto e implementação  
Escopo: Windows, macOS e Android  
Princípio central: tocar primeiro; editar sem poluir; avançado sem esconder o essencial.

## 1. Resultado da auditoria atual

O mixer já oferece cinco canais, timbre por canal, ON, volume, medidor,
Mute, Solo, Loop, Compressor, Reverb, Attack, Release, EQ Low/Mid/High,
Auto EQ, Transpose por canal, Transpose global, curva de velocity, sustain,
inversão de pedal, master e reverb nativo.

Entretanto, a interface e o motor não estão completamente alinhados:

- o parâmetro e o DSP de Delay existem, mas não há controle visível para ele;
- Transpose por canal existe e é salvo, mas foi removido da maioria dos layouts;
- a faixa visual é chamada de `EQ / FX` mesmo quando mostra COMP, REVERB,
  ATK e REL;
- `Auto EQ` ocupa espaço permanente em todos os canais, embora seja uma ação
  secundária da página de EQ;
- no desktop, cinco cópias de controles avançados competem com o fader e o
  nome do timbre;
- no mobile, a decisão correta de mostrar um canal por vez já existe, mas a
  organização ainda precisa separar performance, forma do som e efeitos;
- Attack e Release só moldam timbres nativos; não devem parecer operantes em
  um VST/AU que não exponha esses parâmetros;
- não há Pan, Stereo Width, Key Range/Split, Fine Tune nem sensibilidade de
  velocity por timbre;
- não há preset de usuário do timbre, A/B, reset por seção ou indicação clara
  de que o timbre foi modificado.

## 2. Evidência de produto

As decisões abaixo usam padrões de instrumentos e software profissionais:

- Kontakt usa envelopes AHDSR para modelar o comportamento dinâmico e separa
  fontes de modulação de seus destinos; controles complexos aparecem sob
  demanda.
- Ableton mantém o mixer simples, move a cadeia detalhada do instrumento para
  uma área dedicada e usa Macros para expor apenas os parâmetros mais úteis.
- Pianoteq separa performance de engenharia de áudio, oferece curva de
  velocity, dinâmica, ruídos e ressonâncias, e permite salvar alterações como
  novo preset.
- Nord Stage organiza Piano, Organ e Synth como famílias diferentes; envelope,
  filtro e parâmetros específicos aparecem somente na família que os usa.
- Hammond expõe drawbars, key click, percussion e rotary como identidade do
  órgão, em vez de forçar controles genéricos de piano.
- Apple e Microsoft recomendam divulgação progressiva: os controles mais
  usados ficam sempre visíveis e os avançados aparecem em contexto.
- Android recomenda alvo tocável mínimo de 48 × 48 dp e layout baseado no
  tamanho real da janela, não no nome do aparelho.

Fontes oficiais:

- https://www.native-instruments.com/ni-tech-manuals/kontakt-manual/en/modulation.html
- https://www.native-instruments.com/ni-tech-manuals/kontakt-manual/en/filter-reference
- https://www.ableton.com/en/live-manual/11/working-with-instruments-and-effects/
- https://www.modartt.com/user_manual?product=pianoteq
- https://www.nordkeyboards.com/products/nord-stage-4/downloads/
- https://hammondorganco.com/a162b162-specs
- https://developer.apple.com/design/human-interface-guidelines/disclosure-controls
- https://learn.microsoft.com/windows/apps/design/guidelines-overview
- https://developer.android.com/develop/adaptive-apps/guides/use-window-size-classes
- https://developer.android.com/develop/ui/compose/accessibility/api-defaults

## 3. Modelo mental do produto

O músico precisa responder rapidamente a quatro perguntas:

1. Qual timbre está tocando?
2. Qual canal está selecionado e em qual região do teclado?
3. Ele está alto, clipando ou enviando ambiente demais?
4. Como deixo o som mais macio, presente, longo ou curto?

A interface deve ter dois modos:

### Modo Palco

Visão geral dos canais e do master. Somente controles que podem ser necessários
durante uma música.

### Editar Timbre

Um canal em foco, com páginas `TOCAR`, `SOM` e `FX`. É aberto por uma ação
explícita `Editar som`; não é uma nova janela e não interrompe o áudio.

O mesmo modelo vale para desktop, tablet e celular. A diferença é apenas a
disposição espacial.

## 4. Hierarquia dos controles

### 4.1 Sempre visível no Modo Palco

Em cada canal:

1. Número e categoria do canal;
2. ON;
3. nome do timbre/preset;
4. volume e medidor estéreo;
5. Mute e Solo;
6. indicador de região do teclado;
7. dois Macros Rápidos:
   - `TONE`: macro musical de brilho/cor, com faixa segura;
   - `SPACE`: envio de reverb;
8. ação `Editar som`;
9. ponto de modificação quando o preset divergir do padrão.

Não ficam permanentemente no strip:

- compressor;
- Attack/Release;
- EQ de três bandas;
- Auto EQ;
- Delay;
- Pan/Width;
- controles específicos por categoria.

Esses controles continuam a um toque/clique, mas não são repetidos cinco vezes
na visão geral.

### 4.2 Página TOCAR

Ordem:

1. volume;
2. região do teclado `LOW KEY` / `HIGH KEY`, com mini teclado;
3. octave/transpose por canal;
4. Fine Tune;
5. resposta de velocity por timbre;
6. Pan e Stereo Width;
7. prioridade/limite de vozes somente em `Avançado`;
8. Mute, Solo e ON continuam no cabeçalho.

Por que: essas propriedades definem onde e como o timbre responde ao músico.
Elas não são efeitos.

### 4.3 Página SOM

Bloco principal, sempre igual:

1. Attack;
2. Release;
3. Tone/Brightness;
4. Dynamics.

Divulgação `Envelope avançado`, apenas para categorias compatíveis:

- Hold;
- Decay;
- Sustain;
- curva do Attack;
- curva do Release.

Bloco `Filtro`, para Pads, Synths e timbres híbridos:

- tipo;
- Cutoff;
- Resonance;
- Envelope Amount;
- Key Tracking;
- Velocity to Filter.

Pianos acústicos não exibem ADSR completo ou ressonância de filtro por padrão.
Eles recebem controles musicais da categoria, descritos na seção 5.

### 4.4 Página FX

Ordem visual segue a ordem do sinal:

1. Dynamics:
   - Compressor;
   - opcionalmente threshold/ratio em `Avançado`;
2. Tone:
   - EQ Low;
   - EQ Mid;
   - EQ High;
   - Auto EQ como ação contextual;
3. Space:
   - Reverb Send;
   - Delay Send;
   - tempo/divisão e feedback somente em `Avançado`;
4. Image:
   - Pan;
   - Stereo Width.

O Auto EQ não deve parecer um quarto botão primário do canal. Ele fica junto ao
EQ e comunica três estados:

- desligado;
- aplicado;
- personalizado depois da aplicação.

## 5. Controles específicos por categoria

Controles específicos só aparecem quando o timbre e o motor realmente os
suportam.

### Piano acústico / Upright / Felt

Prioridade:

- Tone;
- Dynamics;
- Velocity Sensitivity;
- Release;
- Pedal Resonance;
- Sympathetic Resonance;
- Key/Action Noise;
- Pedal Noise;
- Stereo Width;
- perspectiva `Close / Player / Room`, quando existirem múltiplas fontes.

Não colocar todos no strip. `Pedal Resonance`, `Noise` e `Perspective` ficam na
página SOM, seção `Caráter do piano`.

### Electric Piano / Keys

- Tone;
- Bell/Tine;
- Bite/Drive;
- Key-off Noise;
- Tremolo Rate/Depth;
- Chorus;
- Amp/DI Character;
- Stereo Width.

### Pad / Ambient / Strings

- Attack;
- Release;
- Filter Cutoff;
- Resonance;
- Movement;
- Modulation Rate;
- Reverb Send;
- Delay Send;
- Stereo Width;
- Loop, quando o conteúdo realmente depender de loop.

### Organ

- Drawbars ou registro simplificado;
- Key Click;
- Percussion On/Soft/Fast/2nd/3rd;
- Rotary Slow/Fast/Stop;
- Drive;
- Chorus/Vibrato;
- Leakage/Character em `Avançado`;
- Reverb.

Attack/Release genéricos só aparecem quando alterarem o instrumento de fato.

### Leads / Synth / Bass

- Amp Envelope;
- Filter Envelope;
- Cutoff/Resonance;
- Glide;
- Mono/Poly/Legato;
- Drive;
- Mod Wheel destination;
- Pitch Bend range.

### Ambiências / One-shot

- Trigger Mode;
- Loop;
- Attack/Release;
- Fade In/Fade Out;
- Sync/tempo se houver conteúdo rítmico;
- Reverb/Delay;
- Stop seguro.

## 6. Parâmetros recomendados

| Parâmetro | Faixa | Padrão | Observação |
|---|---:|---:|---|
| Volume | -∞ a +6 dB | 0 dB | número sempre em dB |
| Pan | L100 a R100 | C | centro com detent |
| Stereo Width | 0 a 150% | 100% | limitar graves em estéreo numa fase futura |
| Attack | 0 a 2 s | 3 ms | já implementado |
| Release | 50 ms a 12 s | 550 ms | já implementado; escala por nota preservada |
| EQ Low | -12 a +12 dB | 0 dB | shelf em 120 Hz |
| EQ Mid | -12 a +12 dB | 0 dB | bell largo em 850 Hz |
| EQ High | -12 a +12 dB | 0 dB | shelf em 8 kHz |
| Dynamics | 0 a 100% | por preset | macro musical; não apenas compressor |
| Compressor | 0 a 100% | 0% | avançado quando Dynamics existir |
| Reverb Send | 0 a 100% | por preset | `SPACE` no Modo Palco |
| Delay Send | 0 a 100% | 0% | DSP já existe; falta UI |
| Transpose | -24 a +24 st | 0 | por canal |
| Fine Tune | -100 a +100 cents | 0 | novo |
| Velocity Sens. | 0 a 200% | 100% | por timbre; curva global continua no Áudio/MIDI |
| Low/High Key | MIDI 0 a 127 | total | com Learn por duas teclas |
| Cutoff | 20 Hz a 20 kHz | aberto | somente categorias compatíveis |
| Resonance | 0 a faixa segura | 0 | proteção contra picos |

Todos os contínuos precisam de smoothing no DSP. Nenhuma alocação, lock, leitura
de disco ou criação de coeficiente com heap pode ocorrer no callback de áudio.

## 7. Layout exato

### 7.1 Desktop — Modo Palco

```text
--------------------------------------------------------------------------------+
| Header / MIDI / Áudio / CPU / Salvo                                            |
+--------------------------------------------------------------------------------+
| Visor: timbre selecionado | canais ativos | alertas de clip/download           |
+-------------+-------------+-------------+-------------+-------------+----------+
| CANAL 1     | CANAL 2     | CANAL 3     | CANAL 4     | CANAL 5     | MASTER   |
| Piano       | Pad         | EP          | Organ       | Strings     |          |
| Pearl Upr.  | Cloud Pad   | Vintage EP  | Worship B3  | Warm Str.   |          |
|             |             |             |             |             |          |
| FADER+METER | FADER+METER | FADER+METER | FADER+METER | FADER+METER | LEVEL    |
| M      S    | M      S    | M      S    | M      S    | M      S    | PANIC    |
| TONE SPACE  | TONE SPACE  | TONE SPACE  | TONE SPACE  | TONE SPACE  | SUSTAIN  |
| [Editar som]| [Editar som]| [Editar som]| [Editar som]| [Editar som]| FX       |
+-------------+-------------+-------------+-------------+-------------+----------+
```

Os strips mantêm a mesma largura e a mesma ordem. O canal selecionado recebe
somente borda/acento e título mais claro; não aumenta de tamanho.

### 7.2 Desktop — Editar Timbre

```text
+--------------------------------------------------------------------------------+
| C1 | C2 | C3 | C4 | C5 | MASTER                         [Voltar ao mixer]       |
+--------------------------------------------------------------------------------+
| [ícone] CANAL 1 — PIANO                        [Pearl Upright v]          [ON]   |
| mini teclado / região ativa / status de biblioteca                              |
+--------------------------------------------------------------------------------+
| TOCAR | SOM | FX                                                                |
+--------------------------------------------------------------------------------+
| Conteúdo da página: controles grandes, valor/unidade e agrupamento semântico     |
|                                                  [Reset seção] [A/B] [Salvar]    |
+--------------------------------------------------------------------------------+
```

No desktop, o modo focado pode reutilizar a hierarquia mobile já existente,
mantendo o mesmo código de componentes.

### 7.3 Android retrato

- tabs `C1 C2 C3 C4 C5 MST` no topo;
- um canal por vez;
- cabeçalho compacto com nome, preset e ON;
- volume horizontal + meter;
- tabs `TOCAR`, `SOM`, `FX`;
- quatro controles no máximo por linha;
- ações de 48 dp;
- sem miniatura de cinco strips simultâneos.

### 7.4 Android paisagem baixa

- cabeçalho e preset na primeira faixa;
- volume/meter no primeiro terço;
- conteúdo da página nos dois terços restantes;
- ações numa barra inferior;
- não mostrar master e canal lado a lado;
- não reduzir alvos abaixo de 48 dp; reduzir texto secundário antes.

### 7.5 Tablet / foldable

- rail de canais à esquerda ou tabs no topo;
- canal focado no centro;
- painel auxiliar de preset/valores à direita quando houver largura;
- largura máxima do editor 760–840 dp;
- não esticar knobs para ocupar espaços vazios.

## 8. Linguagem visual

Paleta:

- fundo: `#0E0F13`;
- superfície principal: `#181C22`;
- superfície elevada: `#20252D`;
- input/rail: `#22272F`;
- borda: `#39414D`;
- texto: `#F5F6F8`;
- secundário: `#A9AFB8`;
- ChurchDAW Red: `#EA1747`;
- sucesso: `#54C77A`;
- aviso: `#F2B84B`;
- erro: `#FF4D67`.

Uso do vermelho:

- canal selecionado;
- estado ON/ativo;
- ponto/anel de valor principal;
- foco;
- PANIC.

Não usar vermelho simultaneamente em todos os botões. Ações secundárias ficam
charcoal; ligadas recebem estado de fundo, ícone e texto, não apenas cor.

Geometria:

- grid base de 4 dp; espaçamentos principais de 8/12/16/24;
- card desktop: raio 12–14;
- card mobile/tablet: raio 16–20;
- borda normal: 1 px/dp;
- foco: 1,5–2 px/dp;
- sombra: discreta e somente em superfícies elevadas;
- sem glow gamer;
- knob desktop: 44–52;
- knob touch: 56–72 com hit target mínimo de 48;
- botão touch: mínimo 48 de altura;
- transições: 140–180 ms, sem pulsação contínua.

Tipografia:

- título do timbre é o elemento textual dominante;
- categoria é eyebrow/label;
- valor numérico usa dígitos tabulares;
- unidade é visualmente secundária;
- abreviações `ATK`, `REL`, `REV`, `COMP` podem ser usadas no mixer;
- a página focada usa nomes completos em português/inglês técnico.

## 9. Interação

- um clique/toque no strip seleciona;
- `Editar som` entra no modo focado;
- tocar novamente na tab do canal não abre ação oculta;
- duplo clique no desktop restaura o parâmetro;
- long press no Android abre: valor exato, Reset, Mapear MIDI e ajuda curta;
- durante o arraste, valor e unidade aparecem acima do controle;
- alterações exibem um ponto `•` ao lado do preset;
- `A/B` compara sem perder o estado editado;
- `Salvar como preset` nunca sobrescreve o preset de fábrica silenciosamente;
- `Reset seção` restaura somente a página atual;
- `Reset timbre` exige confirmação local simples;
- mudança de página, canal ou rotação preserva valores e foco.

### Modo Seguro de Palco

Quando ativo:

- bloqueia troca acidental de preset, EQ e envelope;
- mantém Volume, Mute, Solo, Sustain, troca de canal e PANIC;
- long press temporário destrava um canal;
- um ícone de cadeado comunica o estado.

## 10. Acessibilidade

- alvo touch mínimo de 48 × 48 dp;
- navegação completa por Tab, setas, D-pad e teclado;
- ordem: canal → preset → volume → M/S → macros → editar;
- cada knob anuncia nome, valor, unidade, mínimo, máximo e ação de reset;
- não depender apenas de cor para ON, erro, clip ou modificação;
- foco visível;
- labels reais, não apenas tooltip;
- alto contraste;
- escalas de fonte reorganizam a página focada em vez de cortar texto;
- reduzir movimento respeita a preferência do sistema.

## 11. Sequência de implementação

### Fase 1 — arquitetura e polimento sem novo DSP

- corrigir título dinâmico `ENV / FX` e `EQ`;
- criar `Modo Palco` e `Editar Timbre`;
- mover controles existentes para `TOCAR`, `SOM` e `FX`;
- expor Delay e Transpose que já existem;
- esconder controles que não operam em VST/AU;
- introduzir Reset por seção, estado modificado e valores/unidades consistentes.

### Fase 2 — performance essencial

- Pan;
- Stereo Width;
- Low/High Key e Learn por MIDI;
- Velocity Sensitivity por timbre;
- Fine Tune;
- preset de usuário e A/B.

### Fase 3 — sound design

- filtro seguro;
- envelope avançado por categoria;
- macros TONE, SPACE, DYNAMICS e MOVEMENT;
- parâmetros específicos de Pad, Piano, EP e Organ.

### Fase 4 — segurança de palco e QA

- modo seguro;
- undo local;
- automação/MIDI learn completa;
- testes de layout, áudio e desempenho;
- aprovação visual antes de qualquer auto-update.

## 12. Critérios de aceitação

1. O músico identifica canal, timbre, nível e estado em menos de dois segundos.
2. Volume, Mute, Solo, preset, Tone e Space ficam acessíveis no Modo Palco.
3. Qualquer edição profunda fica a no máximo um clique/toque de `Editar som`.
4. Nenhum strip exibe mais de dois Macros Rápidos no overview final.
5. Nenhum controle visível é inoperante para o tipo de timbre atual.
6. Páginas e nomes refletem exatamente o conteúdo mostrado.
7. Todos os parâmetros exibem valor e unidade ao editar.
8. A troca de página não muda o áudio.
9. O layout mobile usa um canal por vez e alvo mínimo de 48 dp.
10. O desktop mantém os cinco canais e o master visíveis no Modo Palco.
11. Alterações de timbre podem ser comparadas, salvas e restauradas.
12. Não há alocação ou lock no callback de áudio.
13. O som padrão dos setups existentes permanece idêntico após migração.
14. Windows, macOS e Android compartilham o mesmo modelo mental.
15. Nada é enviado ao auto-update antes de aprovação visual e teste tocado.

## 13. Estado da primeira implementação local — 29/07/2026

Implementado e validado em prévia local:

- `Attack` e `Release` reais por canal nativo, com valores padrão que preservam
  o comportamento anterior (`3 ms` e `550 ms`);
- EQ manual de três bandas por canal: `Low`, `Mid` e `High`, todos bipolares
  em dB e com suavização;
- botão `Editar som` no desktop, reutilizando a hierarquia focada do Android;
- Modo Palco continua mostrando as cinco réguas e o Master sem adicionar
  controles ao overview;
- editor focado mostra Volume e Transpose juntos na área de performance;
- página `ENV / FX`: Compressor, Reverb, Delay, Attack e Release;
- página `EQ 3 BANDAS`: Low, Mid e High;
- Delay já existente no motor foi exposto somente no editor focado;
- Attack/Release são ocultados para VST/AU, pois o envelope pertence ao
  instrumento externo; Compressor, Reverb, Delay e EQ pós-instrumento
  continuam disponíveis;
- retorno do editor focado restaura corretamente os faders verticais;
- persistência de Attack, Release e EQ coberta por teste;
- build Windows e compilação nativa Android aprovadas.

Ainda não implementado nesta fase:

- Pan, Stereo Width, Key Range, Fine Tune e Velocity por timbre;
- macros rápidas `Tone` e `Space`;
- presets de usuário, A/B, Reset por seção e indicador de modificação;
- parâmetros acústicos específicos de Piano/EP/Organ/Pad;
- Modo Seguro de Palco.

Esta implementação é local. Nenhum instalador, APK, política remota ou
auto-update de cliente foi alterado/publicado.
