# Church Organ Engine — Design System

Status: especificação visual e de interação para implementação futura. Não altera a UI atual do ChurchDAW.

## 1. Direção de produto

O painel deve parecer parte do ChurchDAW:

- console profissional de palco;
- escuro, legível e confiável em ambiente de culto;
- moderno, mas com referências funcionais ao órgão clássico;
- denso o suficiente para músicos experientes;
- claro o suficiente para operação sob pressão;
- expressivo sem estética gamer, neon excessivo ou madeira/vintage artificial.

O volume visual segue esta ordem:

1. preset, família e estado do instrumento;
2. drawbars e expressão;
3. rotary speaker e Worship Intensity;
4. percussão, character, scanner e drive;
5. diagnóstico, qualidade e MIDI.

## 2. Princípios

### 2.1 Um olhar, uma decisão

A página LIVE deve responder imediatamente:

- qual preset está ativo;
- se o motor está ligado;
- qual família sonora está ativa;
- como os drawbars estão configurados;
- se o rotary está em Slow, Stop ou Fast;
- quanto de expressão, Worship e ambiente está aplicado;
- se existe alerta de CPU, MIDI ou carregamento.

### 2.2 Estado nunca depende só de cor

Cor, forma, rótulo, posição e ícone trabalham juntos. Um botão ativo recebe cor e também:

- texto/ícone de estado;
- mudança de preenchimento;
- borda ou indicador;
- feedback de foco independente.

### 2.3 Palco primeiro

- alvos grandes;
- leitura a distância;
- mudanças críticas protegidas;
- Panic sempre alcançável;
- nenhum diálogo modal durante performance;
- ações destrutivas ou estruturais não ficam ao lado de controles contínuos.

### 2.4 Profundidade funcional

Sombras e highlights indicam hierarquia e material. Não são decoração:

- superfície recuada: trilho, display, medidor;
- superfície elevada: card, seletor, popover;
- elemento físico: drawbar, knob e half-moon switch;
- foco: halo fino de alto contraste, não glow difuso.

## 3. Tokens

### 3.1 Cores

| Token | Valor | Uso |
|---|---:|---|
| `color.canvas` | `#090A0D` | fundo global |
| `color.surface.1` | `#101217` | painel principal |
| `color.surface.2` | `#171A20` | cards e grupos |
| `color.surface.3` | `#20242C` | controle elevado/hover |
| `color.well` | `#08090C` | trilhos, displays e áreas recuadas |
| `color.edge.subtle` | `#282D36` | divisores |
| `color.edge.strong` | `#3A414D` | contorno de controle |
| `color.text.primary` | `#F1EFEA` | títulos e valores |
| `color.text.secondary` | `#A9ADB6` | rótulos |
| `color.text.muted` | `#737985` | ajuda e metadados |
| `color.text.disabled` | `#555B65` | desabilitado |
| `color.amber` | `#D99A3E` | drawbars, rotary, drive e caráter |
| `color.amber.bright` | `#F2B85B` | valor ativo/foco musical |
| `color.amber.dim` | `#805824` | trilho/estado baixo |
| `color.red` | `#D7434D` | seleção, gravação de learn e ações |
| `color.red.bright` | `#FF5B65` | Panic/alerta crítico |
| `color.red.dim` | `#72262D` | fundo de alerta |
| `color.green` | `#54B980` | pronto/conectado/salvo |
| `color.blue` | `#5798D6` | informação de sistema não musical |
| `color.warning` | `#E3A447` | CPU alta/atenção |
| `color.scrim` | `#000000B8` | backdrop de popover |

Regras:

- âmbar identifica timbre, movimento e energia sonora;
- vermelho identifica seleção explícita, automação/learn, Panic e erro crítico;
- vermelho não preenche todos os controles ativos;
- gradientes são curtos e usados apenas para material/volume;
- texto principal precisa alcançar contraste WCAG AA sobre a superfície usada;
- qualquer combinação que falhe contraste deve ser corrigida no token, não localmente.

### 3.2 Tipografia

Família: fonte sans do sistema com fallback do ChurchDAW. Não embutir fonte sem licença.

| Token | Tamanho desktop | Tamanho mobile | Peso | Uso |
|---|---:|---:|---:|---|
| `type.display` | 28 | 22 | 700 | nome do preset |
| `type.title` | 20 | 18 | 700 | título de página |
| `type.section` | 14 | 13 | 700 | título de grupo |
| `type.control` | 12 | 12 | 650 | nome de controle |
| `type.value` | 13 | 13 | 700 | valor numérico |
| `type.meta` | 11 | 11 | 500 | CPU, qualidade, MIDI |
| `type.micro` | 10 | 10 | 600 | marcações de drawbar |

Regras:

- valores numéricos usam dígitos tabulares quando disponíveis;
- nomes de preset podem truncar em uma linha somente se houver tooltip/acesso ao nome completo;
- caixa alta é reservada a títulos curtos e estados;
- textos explicativos usam caixa normal;
- nenhuma informação crítica usa corpo menor que 11 px equivalentes.

### 3.3 Espaçamento e dimensões

Base de 4 unidades:

```text
space.1 = 4
space.2 = 8
space.3 = 12
space.4 = 16
space.5 = 20
space.6 = 24
space.8 = 32
space.10 = 40
space.12 = 48
```

| Token | Desktop | Android |
|---|---:|---:|
| alvo mínimo | 40 x 40 | 48 x 48 |
| knob mínimo | 56 | 60 |
| drawbar mínimo | 34 x 148 | 30 x 118 |
| radius pequeno | 5 | 5 |
| radius card | 10 | 10 |
| radius painel | 14 | 12 |
| borda | 1 | 1 |
| foco | 2 | 2 |

Alvos podem se sobrepor visualmente ao redor de controles estreitos, mas suas regiões interativas não podem conflitar.

### 3.4 Elevação

| Nível | Tratamento |
|---|---|
| `0` | canvas sem sombra |
| `1` | borda sutil + highlight superior de 1 px |
| `2` | sombra `0 4 14 #00000066` + borda |
| `3` | sombra `0 10 28 #00000099` para popover |

Sem blur luminoso amplo. Controles físicos usam highlight direcional discreto.

### 3.5 Movimento

| Token | Duração | Uso |
|---|---:|---|
| `motion.instant` | 0–60 ms | pressão/feedback |
| `motion.fast` | 120 ms | hover/foco |
| `motion.normal` | 180 ms | tabs e cards |
| `motion.slow` | 260 ms | painel avançado |

O desenho do rotary pode acompanhar a velocidade real por telemetria limitada, mas não tenta animar cada amostra. Respeitar “reduzir movimento”:

- retirar parallax;
- evitar rotação decorativa contínua;
- preservar mudança de estado;
- não remover medição ou feedback necessário.

## 4. Arquitetura de informação

Páginas principais:

1. `LIVE`
2. `DRAWBARS`
3. `CHARACTER`
4. `ROTARY`
5. `WORSHIP`

Páginas opcionais quando o escopo estiver implementado:

- `PIPE`
- `TRANSISTOR`
- `PRESETS`
- `SETTINGS`

As cinco páginas principais mantêm a mesma barra de contexto:

- voltar/fechar;
- nome e categoria do preset;
- indicador de modificação;
- família;
- power;
- MIDI;
- qualidade/CPU;
- Panic.

## 5. Shell do instrumento

### 5.1 Header

Ordem desktop:

```text
[Voltar] [Família] [Preset + categoria + dirty] [MIDI] [Quality/CPU] [Power] [Panic]
```

Ordem mobile:

```text
[Voltar] [Preset] [Power] [Panic]
[Família] [MIDI] [Quality/CPU]
```

Comportamentos:

- nome do preset abre browser de preset;
- ponto/asterisco de “editado” tem label acessível;
- Power faz fade, não corta abruptamente;
- Panic responde no primeiro toque e não pede confirmação;
- CPU alta abre diagnóstico por toque, sem modal.

### 5.2 Navegação

Desktop:

- tabs horizontais ou rail curto;
- página ativa com linha/preenchimento vermelho discreto;
- atalhos de teclado quando o foco não estiver editando valor.

Android portrait:

- bottom navigation de cinco itens;
- label sempre visível;
- área segura respeitada;
- LIVE não rola.

Android landscape/tablet:

- rail lateral;
- conteúdo ocupa o restante;
- controles críticos permanecem fixos.

## 6. Componentes

### 6.1 Drawbar

Anatomia:

- label harmônico no topo;
- trilho escuro recuado;
- haste/handle âmbar;
- escala `0..8`;
- valor textual acessível;
- área de toque ampliada;
- reset por gesto secundário/duplo clique somente com alternativa acessível.

Comportamento:

- arrastar em direção ao músico aumenta o valor, reproduzindo a lógica física;
- teclado: setas alteram 1 passo, Page Up/Down alteram 2, Home/End vão aos extremos;
- touch: drag vertical com captura do ponteiro;
- tooltip/announcer: “Drawbar 8 pés, nível 6 de 8”;
- automação externa move o handle suavemente sem roubar foco;
- conflito touch x MIDI mostra origem temporária, sem bloquear.

Variações:

- normal;
- hover;
- pressed;
- focused;
- MIDI learned;
- automated;
- disabled.

### 6.2 Knob

Uso: drive, click, leakage, mix, room, width e controles contínuos.

Anatomia:

- corpo escuro com volume sutil;
- arco de valor âmbar;
- marca central clara;
- label e valor sempre legíveis;
- default marcado quando útil.

Interação:

- drag vertical principal;
- Shift/fine em desktop;
- teclado e acessibilidade;
- edição numérica por ação secundária;
- duplo clique para default somente com menu/teclado equivalente;
- sem “bolinhas vermelhas” em todos os knobs.

### 6.3 Half-moon Rotary

Estados:

- `SLOW`
- `STOP`
- `FAST`

Anatomia:

- corpo horizontal físico;
- três posições claramente rotuladas;
- posição ativa por deslocamento, âmbar e texto;
- estado de transição mostra “ACELERANDO” ou “DESACELERANDO” em telemetria discreta.

Nunca representar Stop como bypass. Mix/bypass é controle separado.

### 6.4 Worship Intensity

Controle hero da página LIVE:

- slider/macropad amplo;
- escala `0–100`;
- indicação das regiões `Base`, `Lift` e `Open`;
- âmbar crescente, sem arco-íris;
- valor, descrição curta e estado de automação;
- feedback de compensação/AutoMix apenas informativo.

No mobile, deve ser operável com o polegar sem cobrir o valor.

### 6.5 Expression

- medidor/slider vertical ou horizontal conforme layout;
- valor `0–100%`;
- origem indicada: pedal, MIDI, UI;
- curva configurada exibida em Settings, não na LIVE;
- perda de pedal não zera abruptamente o som.

### 6.6 Rotary Visualizer

Visualizer técnico, não decorativo:

- horn e drum;
- velocidade atual e alvo;
- mic distance;
- stereo width;
- indicação de room;
- animação limitada por FPS;
- modo reduzir movimento substitui rotação por barras/valores.

### 6.7 Preset selector

Inclui:

- nome;
- categoria;
- favorito;
- factory/user;
- próximo/anterior;
- search fora da LIVE;
- estado carregando;
- estado incompatível/migrado.

Troca em andamento exibe progresso indeterminado curto e mantém o preset anterior utilizável até confirmação.

### 6.8 Toggle/segmented control

Uso para valores discretos como percussion harmonic, scanner mode e family.

- no máximo cinco opções visíveis;
- labels textuais;
- seleção vermelha discreta;
- foco separado da seleção;
- toque mínimo de 48 px no Android.

### 6.9 Meter e diagnóstico

- meter não usa vermelho antes de região realmente crítica;
- peak hold curto;
- CPU mostra média e overload;
- Quality é badge clicável;
- informações avançadas ficam em popover;
- nenhuma string é construída pela thread de áudio.

### 6.10 MIDI Learn

Estados:

- não mapeado;
- aprendendo;
- mapeado;
- conflito;
- controlador desconectado.

Durante learn:

- borda vermelha pulsada em frequência baixa;
- texto “Mova um controle MIDI”;
- cancelar sempre disponível;
- Panic/notas reservadas nunca são capturados;
- confirmação mostra CC/canal e modo pickup.

## 7. Páginas

### 7.1 LIVE

Conteúdo obrigatório:

- preset/categoria/power;
- nove drawbars;
- percussão;
- chorus/vibrato;
- drive;
- rotary;
- expression;
- Worship Intensity;
- reverb/room;
- botão de edição avançada;
- MIDI;
- engine status;
- CPU/quality discretos.

Desktop, grade de 12 colunas:

```text
Header                                                12
Preset/context                                         4
Drawbars                                               8
Expression     Rotary       Worship Intensity          12
Percussion     Scanner      Drive      Reverb           12
```

Android portrait, sem scroll:

```text
Header compacto
Preset + family
9 drawbars compactos
Expression | Rotary | Worship
Perc | Scanner | Drive | Reverb
Bottom navigation
```

Técnicas permitidas para caber:

- labels abreviados com acessibilidade completa;
- valores em overlay durante edição;
- controles secundários em segmented compact;
- header em duas linhas;
- rail inferior respeitando safe area.

Não esconder Power, Panic, drawbars, rotary ou Worship atrás de scroll/menu.

### 7.2 DRAWBARS

- drawbars maiores;
- Upper/Lower/Pedal quando disponíveis;
- presets de registration originais;
- foldback/keyboard map como visual técnico opcional;
- copy/paste entre manuais;
- reset com confirmação não modal;
- nenhuma imitação gráfica de marca concorrente.

### 7.3 CHARACTER

Grupos:

- perfil Clean/Aged/Road;
- key contact/click;
- leakage/crosstalk;
- percussion;
- scanner chorus/vibrato;
- transformer/preamp/drive;
- output character.

Progressive disclosure:

- controles essenciais visíveis;
- ajustes técnicos em `Advanced`;
- valores avançados mantêm labels e ranges claros.

### 7.4 ROTARY

- half-moon em destaque;
- visualizer;
- horn/drum balance;
- acceleration/deceleration quando expostos;
- mic distance;
- stereo width;
- room;
- mix;
- bypass separado;
- descrição de transição atual.

### 7.5 WORSHIP

- Worship Intensity hero;
- layer level;
- tone/air;
- width/modulation;
- reverb/room;
- AutoMix;
- macro target preview;
- proteção contra ganho;
- opção de ouvir layer isolada apenas fora do modo de performance.

## 8. Layout responsivo

### 8.1 Breakpoints lógicos

Não depender apenas de pixels físicos; considerar escala e densidade:

| Classe | Largura lógica | Navegação | Comportamento |
|---|---:|---|---|
| phone portrait | `< 600` | bottom nav | LIVE fixa, outras páginas podem rolar |
| phone landscape | `600–839` e baixa altura | rail | duas faixas compactas |
| tablet | `600–1023` com altura útil | rail/tabs | grade 8 colunas |
| desktop compact | `900–1279` | tabs | grade 10 colunas |
| desktop wide | `>= 1280` | tabs | grade 12 colunas |

Altura útil tem precedência: uma janela desktop baixa adota layout landscape compacto.

### 8.2 Reflow

- cards mudam de coluna, não apenas encolhem;
- labels não colidem;
- drawbars preservam área de interação;
- visualizer pode reduzir detalhes;
- controles secundários migram para Advanced;
- navegação permanece estável;
- popovers nunca saem da viewport;
- safe areas são aplicadas no Android.

### 8.3 Densidade

Modos:

- `comfortable`: desktop/touch;
- `compact`: janela reduzida;
- `stage`: valores e controles críticos maiores, detalhes reduzidos.

Stage mode não altera parâmetros de áudio, apenas apresentação.

## 9. Estados de produto

| Estado | Tratamento |
|---|---|
| Inicializando | skeleton estável, controles desabilitados, status textual |
| Preparando engine | preset anterior permanece quando possível |
| Pronto | indicador verde discreto |
| Bypass/Power off | controles preservados, painel atenuado, Power explícito |
| Preset modificado | ponto + “Editado” |
| Salvando | progresso curto, sem bloquear áudio |
| MIDI desconectado | badge com ação de diagnóstico |
| MIDI Learn | foco vermelho e instrução |
| CPU alta | warning âmbar e recomendação |
| Overload | alerta vermelho, contador e ação Quality |
| Erro recuperável | mensagem inline e Retry |
| Erro fatal do engine | fallback/bypass seguro + diagnóstico |
| Offline | presets locais continuam; nenhuma falsa falha DSP |
| Migração de estado | badge “Atualizado” e opção de salvar |
| Conteúdo incompatível | motivo e alternativa, sem sumir silenciosamente |

## 10. Acessibilidade

### 10.1 Teclado

- ordem de foco segue leitura visual;
- tabs/páginas acessíveis por teclado;
- Escape fecha popover, não desfaz som;
- Enter/Espaço ativa botões;
- setas editam controles;
- Panic tem atalho configurável e não ambíguo;
- foco nunca fica preso.

### 10.2 Leitor de tela

Cada controle fornece:

- nome;
- função;
- valor;
- unidade/faixa;
- estado;
- ação;
- grupo/manual relacionado.

Exemplo: “Rotary speed, Fast, seletor, 3 opções”.

### 10.3 Contraste e daltonismo

- AA para texto;
- foco com contraste mínimo adequado;
- âmbar/vermelho acompanhados de forma e label;
- meters usam marcações e padrões, não apenas matiz;
- screenshots de validação em grayscale e simulações comuns.

### 10.4 Toque e motor

- alvo Android mínimo 48 x 48;
- nenhuma ação crítica depende de gesto oculto;
- alternativa para drag fino;
- tolerância a movimento inicial;
- controles próximos não disparam simultaneamente;
- feedback visual aparece fora da área coberta pelo dedo quando possível.

### 10.5 Redução de movimento

- respeitar preferência do sistema;
- manter transições curtas;
- remover rotação decorativa;
- não piscar acima de limites seguros;
- estados continuam identificáveis sem animação.

## 11. Adaptação por plataforma

### Windows

- escala de 100% a 250%;
- teclado, mouse, touch e high-DPI;
- foco visível mesmo sem hover;
- janela mínima documentada;
- comportamento consistente com atalhos do ChurchDAW.

### macOS

- Retina;
- trackpad e rolagem natural;
- atalhos com Command quando apropriado;
- safe titlebar/fullscreen;
- não imitar controles nativos que não sejam realmente nativos.

### Android

- portrait, landscape, tablet e ChromeOS quando aplicável;
- safe areas/cutouts;
- lifecycle e retomada;
- font scale;
- TalkBack;
- alvos de 48 dp;
- LIVE integral sem scroll;
- consumo visual/animation limitado no perfil Eco.

## 12. Views obrigatórias para design e QA

1. LIVE desktop wide, Tonewheel pronto;
2. LIVE desktop compact com preset modificado;
3. LIVE Android portrait sem scroll;
4. LIVE Android landscape;
5. DRAWBARS com Upper/Lower;
6. CHARACTER com Advanced aberto;
7. ROTARY em transição Slow → Fast;
8. WORSHIP com intensidade alta e AutoMix;
9. MIDI Learn com conflito;
10. CPU overload com recomendação de Quality;
11. estado de engine preparando;
12. erro recuperável com fallback.

Cada view precisa de:

- estado normal;
- hover quando houver;
- pressed;
- focus;
- disabled;
- texto longo;
- escala/densidade extrema relevante.

## 13. Critérios visuais de aceite

- identidade reconhecível como ChurchDAW sem copiar outro instrumento;
- drawbars, rotary e Worship são legíveis em menos de dois segundos;
- LIVE Android portrait não rola;
- nenhuma colisão em tamanhos mínimos suportados;
- contraste e foco aprovados;
- controles críticos possuem alvo adequado;
- âmbar e vermelho mantêm papéis consistentes;
- sem excesso de gloss, neon ou decoração vintage;
- animação não consome orçamento relevante do áudio;
- todos os estados da seção 9 possuem tratamento;
- labels e valores continuam compreensíveis em PT-BR;
- arquitetura permite localização futura sem mudar IDs DSP.
