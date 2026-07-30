# Church Organ Engine — Matriz de Testes

Status: plano de verificação para núcleo isolado, integração futura e release multiplataforma.

## 1. Objetivos

A matriz deve provar:

- correção musical e numérica;
- segurança de tempo real;
- estabilidade sob troca de preset/família;
- desempenho previsível em Eco, Standard e Ultra;
- comportamento consistente em Windows, macOS e Android;
- responsividade e acessibilidade da UI;
- compatibilidade de estado;
- ausência de regressão no ChurchDAW;
- origem clean-room dos recursos entregues.

## 2. Convenções

### 2.1 Prioridade

| Classe | Significado |
|---|---|
| P0 | falha bloqueia qualquer build distribuível |
| P1 | falha bloqueia release candidate |
| P2 | falha pode ser aceita somente com decisão registrada |

### 2.2 Camadas

| Camada | Escopo |
|---|---|
| Unit | módulo DSP/estado isolado |
| Offline | render determinístico sem dispositivo |
| RT harness | callback simulado com instrumentação |
| Integration | adaptador + ChurchDAW |
| Platform | dispositivo e sistema reais |
| Listening | avaliação auditiva controlada |

### 2.3 Configurações mínimas

Combinações automatizadas:

- sample rates: 44.1 kHz, 48 kHz e 96 kHz;
- block sizes: 32, 64, 128, 256, 512 e 1024;
- saída: mono defensivo e estéreo nominal;
- qualidade: Eco, Standard e Ultra;
- família: tonewheel, transistor, pipe e worshipHybrid;
- build: Debug/Assertions e Release/otimizada;
- arquitetura: x64, arm64 desktop quando aplicável e arm64 Android.

Combinações de stress adicionais:

- bloco alternando entre tamanhos permitidos quando o host suporta;
- eventos no primeiro e último sample do bloco;
- 0 eventos, capacidade exata e overflow;
- silêncio, impulso, DC defensivo, ruído, acordes e glissando;
- valores mínimos, máximos, default, NaN e infinito.

## 3. Infraestrutura

### 3.1 Harness isolado

O harness do núcleo deve:

- gerar MIDI determinístico;
- renderizar para buffers em memória;
- medir tempo, pico, RMS, DC, espectro e valores não finitos;
- registrar contagem de alocação e tentativa de lock;
- usar seed explícito;
- salvar apenas relatórios e pequenos vetores derivados;
- não depender do executável ChurchDAW;
- rodar em diretório de build exclusivo.

### 3.2 Referências

São permitidas:

- equações publicadas e conceitos genéricos;
- sinais sintéticos;
- medições produzidas pelo próprio projeto;
- golden files gerados pela implementação e aprovados internamente;
- gravações próprias com cadeia de direitos documentada.

Não são permitidos:

- samples, respostas de impulso ou presets extraídos de concorrente;
- comparação binária com software proprietário como “oracle”;
- nomes de presets de marcas;
- tabelas copiadas de implementação fechada.

### 3.3 Tolerâncias

Toda tolerância deve informar:

- unidade;
- perfil de qualidade;
- sample rate;
- janela de medição;
- motivo musical/técnico.

Mudança de tolerância exige revisão e histórico, não simples atualização do golden file.

## 4. Matriz DSP comum

| ID | Pri. | Objetivo | Estímulo | Métrica / aceite |
|---|---|---|---|---|
| DSP-COM-001 | P0 | Saída finita | ranges, NaN/Inf, MIDI inválido | zero NaN/Inf na saída; contador defensivo correto |
| DSP-COM-002 | P0 | Silêncio estável | 60 s sem notas | sem ruído não configurado; sem crescimento de estado |
| DSP-COM-003 | P0 | Reset completo | tocar, reset, render silêncio | energia residual abaixo de -120 dBFS após janela definida |
| DSP-COM-004 | P0 | Panic | sustain + polifonia máxima | vozes encerradas dentro do limite; sem clique severo |
| DSP-COM-005 | P0 | Determinismo | mesma seed/eventos/configuração | buffers idênticos ou tolerância ULP documentada |
| DSP-COM-006 | P0 | Limites de buffer | 0, 1, máximo e inválidos defensivos | sem acesso fora de faixa; retorno seguro |
| DSP-COM-007 | P1 | Smoothing | automação min↔max | sem degrau de parâmetro; zipper abaixo do limite auditivo/espectral |
| DSP-COM-008 | P1 | Headroom | acordes densos em defaults | sem clipping interno não intencional; ganho documentado |
| DSP-COM-009 | P1 | DC | notas sustentadas + drive | DC de saída abaixo de -80 dBFS em regime |
| DSP-COM-010 | P1 | Denormals | caudas muito baixas | sem pico de CPU; valores tratados |
| DSP-COM-011 | P1 | Mono defensivo | buffer de um canal | sem crash; política de downmix/mono correta |
| DSP-COM-012 | P2 | Reprodutibilidade | compiladores suportados | métricas dentro da tolerância entre plataformas |

## 5. Eventos, contatos e vozes

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| EVT-001 | P0 | Ordenação por offset | eventos fora de ordem | render igual ao vetor normalizado |
| EVT-002 | P0 | Retrigger | off/on mesma nota e offset | política documentada e determinística |
| EVT-003 | P0 | Overflow | capacidade + 1 e burst extremo | sem alocação; contador incrementado; Panic preservado |
| EVT-004 | P0 | All notes off | todos os manuais + sustain | nenhuma voz presa |
| EVT-005 | P1 | Sample accuracy | ataque em offsets variados | início exatamente no offset definido |
| KEY-001 | P1 | Contatos múltiplos | velocidades e seeds | offsets dentro da faixa e determinísticos |
| KEY-002 | P1 | Release contact | staccato/legato | transient correto, sem duplicação |
| KEY-003 | P1 | Modo Eco | comparação de contatos | caráter preservado dentro de envelope/tolerância |
| VOI-001 | P0 | Limite de pool | polifonia máxima + 1 | sem crescimento; steal contabilizado |
| VOI-002 | P1 | Política de steal | baixo sustentado + novos acordes | prioridade conforme contrato |
| VOI-003 | P1 | Separação de manuais | Upper/Lower/Pedal saturados | orçamento de um não corrompe o outro |
| VOI-004 | P0 | Lifecycle | prepare/reset/reprepare repetidos | sem leak, use-after-free ou voz fantasma |

## 6. Tonewheel

| ID | Pri. | Objetivo | Método | Aceite inicial |
|---|---|---|---|---|
| TW-001 | P0 | Afinação | FFT/zero crossing por nota | erro fundamental ≤ 0,1 cent após settling |
| TW-002 | P0 | Fase contínua | note on/off repetido | gerador compartilhado não reinicia indevidamente |
| TW-003 | P0 | Foldback seguro | varrer MIDI 0–127 e harmônicos | índices válidos; mapeamento atende tabela aprovada |
| TW-004 | P1 | Foldback musical | fronteiras graves/agudas | sem salto de energia fora da tolerância definida |
| TW-005 | P0 | Drawbar mute | todos em 0 | saída tonal abaixo de -120 dBFS, exceto ruído configurado |
| TW-006 | P1 | Ganho por passo | passos 0–8 | curva monotônica e igual à especificação |
| TW-007 | P1 | Automação de drawbar | rampas e CC rápido | sem zipper/click detectável acima do limite |
| TW-008 | P1 | Soma dos nove | todas combinações de borda + amostragem combinatória | sem overflow; headroom consistente |
| TW-009 | P1 | Perfis | Clean/Aged/Road | seeds estáveis e métricas distintas aprovadas |
| TW-010 | P1 | Alias | notas altas, drawbars altos, drive off | energia alias atende perfil: Eco ≤ -60, Standard ≤ -70, Ultra ≤ -80 dBFS relativo |
| TW-011 | P2 | Drift | nota longa em Aged | faixa limitada, sem passeio cumulativo |

Os limites de alias são metas iniciais e devem ser confirmados no M2 com o método exato registrado.

## 7. Click, leakage e percussion

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| CHR-001 | P1 | Click limitado | min/default/max | transient finito, sem exceder headroom |
| CHR-002 | P1 | Click por velocidade | curva completa | monotônica ou conforme curva documentada |
| CHR-003 | P1 | Leakage | silêncio + valores 0/default/max | zero em 0; espectro/nível dentro da configuração |
| CHR-004 | P1 | Seed persistente | salvar/reabrir | caráter reproduzível |
| PER-001 | P0 | Harmônico | segunda/terceira | frequência correta em toda faixa |
| PER-002 | P1 | Decay | short/long | constantes dentro de ±5% da especificação |
| PER-003 | P1 | Retrigger | legato e frase separada | segue regra aprovada |
| PER-004 | P1 | Interação drawbar | percussion on/off | sem salto de loudness não especificado |
| PER-005 | P0 | Nota presa | sustain + mudança de preset | nenhuma cauda infinita |

## 8. Scanner chorus/vibrato

| ID | Pri. | Objetivo | Método | Aceite |
|---|---|---|---|---|
| SCN-001 | P0 | Buffer seguro | todos os rates/blocks | sanitizers limpos |
| SCN-002 | P1 | Profundidade/rate | medir modulação | dentro de ±3% da especificação |
| SCN-003 | P1 | Mudança de modo | cada par de modos | crossfade sem descontinuidade |
| SCN-004 | P1 | Estéreo/mono | correlação e energia | política coerente, sem cancelamento destrutivo |
| SCN-005 | P1 | Eco vs Standard | null/medidas auditivas | degradação controlada, sem clique |
| SCN-006 | P1 | Silence transition | modo muda sem nota | próxima nota começa em estado válido |

## 9. Drive e caráter

| ID | Pri. | Objetivo | Método | Aceite |
|---|---|---|---|---|
| DRV-001 | P0 | Estabilidade | sweep de amplitude/frequência | saída finita em todos os perfis |
| DRV-002 | P1 | Curva | níveis -60 a 0 dBFS | monotônica e conforme curva aprovada |
| DRV-003 | P1 | Simetria/assimetria | seno bipolar | assinatura dentro da especificação do perfil |
| DRV-004 | P1 | Alias | sweep e multitone | metas Eco/Standard/Ultra atendidas |
| DRV-005 | P1 | Compensação | bypass vs amounts | diferença de loudness dentro do limite definido |
| DRV-006 | P0 | Troca de oversampling | mudança de Quality | somente swap preparado; zero crash/corrupção |
| DRV-007 | P1 | DC blocker | drive máximo | DC abaixo de -80 dBFS após settling |
| DRV-008 | P1 | Bypass | alternância rápida | sem resíduo, pop severo ou mudança de latência não declarada |

## 10. Rotary cabinet

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| ROT-001 | P0 | Estados | Slow/Stop/Fast | alvos corretos; Stop não equivale a bypass |
| ROT-002 | P1 | Inércia | Slow→Fast→Stop→Slow | tempos dentro de ±10% da especificação |
| ROT-003 | P1 | Rotores independentes | medir horn/drum | rates e sentidos conforme modelo |
| ROT-004 | P1 | Crossover | sweep | soma sem notch/boost fora da tolerância |
| ROT-005 | P1 | Doppler coerente | tom sustentado | desvio acompanha geometria e velocidade |
| ROT-006 | P1 | Mic distance | min/max | level, Doppler e room mudam de forma coordenada |
| ROT-007 | P1 | Stereo width | mono→wide | correlação não cai abaixo do limite aprovado |
| ROT-008 | P0 | Mix zero | tocar, zerar, reativar | áudio antigo não reaparece |
| ROT-009 | P1 | Movimento no silêncio | transição sem notas | próxima nota encontra velocidade física esperada |
| ROT-010 | P1 | Mudança de parâmetro | drag rápido | smoothing sem zipper |
| ROT-011 | P1 | Mono | entrada/saída mono | sem cancelamento ou acesso inválido |
| ROT-012 | P2 | Room | impulso e burst | decay/early reflections dentro da especificação |

## 11. Transistor

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| TRN-001 | P0 | Registro/divisores | notas e combinações | razões corretas, sem índice inválido |
| TRN-002 | P1 | Identidade | análise espectral | assinatura distinta do tonewheel |
| TRN-003 | P1 | Transient | staccato/legato | consistente e limitado |
| TRN-004 | P1 | Filtro brilhante | sweep | resposta conforme curvas aprovadas |
| TRN-005 | P1 | Nonlinearity | multitone | alias/headroom dentro do perfil |
| TRN-006 | P1 | Direct/rotary | alternância | transição sem clique |

## 12. Pipe

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| PIP-001 | P0 | Afinação por rank | faixa completa | ≤ 0,2 cent em regime |
| PIP-002 | P0 | Polifonia por divisão | saturar divisões | pools respeitados; sem corrupção |
| PIP-003 | P1 | Chiff/transient | velocidades/registro | curva conforme especificação |
| PIP-004 | P1 | Sustain | nota de 5 minutos | nível estável, zero drift numérico |
| PIP-005 | P1 | Wind interaction | acordes crescentes | resposta limitada e determinística |
| PIP-006 | P1 | Stereo placement | ranks/divisões | imagem conforme mapa aprovado |
| PIP-007 | P1 | Room bypass | alternar | ambiente desacoplado e sem pop |
| PIP-008 | P0 | Mudança de stops | nota sustentada | política segura, sem voz presa |

## 13. Worship Hybrid e AutoMix

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| WRP-001 | P0 | Macro bounded | 0–100 e inválidos | todos os destinos permanecem na faixa |
| WRP-002 | P1 | Monotonicidade | sweep lento | intensidade percebida/métricas não recuam anormalmente |
| WRP-003 | P1 | Compensação | acordes em 0/25/50/75/100 | loudness dentro do envelope aprovado |
| WRP-004 | P1 | Layer lifecycle | preset/family/panic | nenhuma camada presa |
| WRP-005 | P1 | Width mono | downmix | sem cancelamento destrutivo |
| WRP-006 | P1 | Reverb/air | extremos | finito, sem runaway |
| MIX-001 | P0 | Limites AutoMix | stress de vozes | ajustes nunca excedem limites |
| MIX-002 | P1 | Tempo de reação | entrada densa/esparsa | sem pumping rápido |
| MIX-003 | P1 | Bypass | on/off | transição nivelada |
| MIX-004 | P1 | Reprodutibilidade | mesma sequência | mesma automação interna |

## 14. Estado e presets

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| STA-001 | P0 | Round trip | salvar/carregar cada família | parâmetros equivalentes |
| STA-002 | P0 | Versão | v1, futura e inválida | migra/ignora/rejeita conforme contrato |
| STA-003 | P0 | Campos ausentes | remover cada campo | defaults corretos |
| STA-004 | P0 | Dados hostis | profundidade/tamanho/números extremos | limite seguro, sem crash |
| STA-005 | P1 | Campos futuros | extensões desconhecidas | leitura preserva comportamento conhecido |
| STA-006 | P0 | ID estável | catálogo completo | sem duplicata ou reutilização |
| PRE-001 | P0 | Swap estrutural | família e Quality | instância preparada, sem uso após free |
| PRE-002 | P1 | Crossfade | acordes sustentados | sem gap/click acima do limite |
| PRE-003 | P1 | Spam de preset | 100 mudanças rápidas | somente estado final/coalescido, sem leak |
| PRE-004 | P1 | Cancelamento | preparar e substituir | recursos descartados fora do áudio |
| PRE-005 | P1 | Dirty state | editar/reverter/salvar | indicador correto |
| PRE-006 | P0 | Migração legada | estados Rotary antigos | mapeamento determinístico e documentado |

## 15. MIDI e controle

| ID | Pri. | Objetivo | Cenário | Aceite |
|---|---|---|---|---|
| MID-001 | P0 | Note routing | canais/manuais/splits | destino correto |
| MID-002 | P0 | Panic reservado | durante learn | nunca capturado como mapping |
| MID-003 | P1 | Pickup | controle físico divergente | sem salto até cruzar valor |
| MID-004 | P1 | Rotary momentary/toggle | ambos os modos | estado esperado |
| MID-005 | P1 | Expression disconnect | remover pedal | sem zero abrupto |
| MID-006 | P1 | Hot plug | conectar/remover | áudio continua |
| MID-007 | P1 | Mapping conflict | mesmo CC | aviso e resolução determinística |
| MID-008 | P1 | Burst | tráfego CC intenso | fila limitada; notas/Panic prioritários |
| MID-009 | P1 | Launchkey default | mapa factory | IDs válidos e sobrescrevíveis |

## 16. Segurança de tempo real

| ID | Pri. | Verificação | Aceite |
|---|---|---|---|
| RT-001 | P0 | contador global de alocação dentro do callback | zero alocações após warm-up |
| RT-002 | P0 | instrumentação de mutex/wait | zero locks/esperas |
| RT-003 | P0 | interceptar filesystem/log/string formatting | zero chamadas |
| RT-004 | P0 | exceptions/assertions | nenhuma exceção cruza callback |
| RT-005 | P0 | AddressSanitizer | zero erro |
| RT-006 | P0 | UndefinedBehaviorSanitizer quando disponível | zero erro |
| RT-007 | P0 | ThreadSanitizer em harness compatível | zero race acionável |
| RT-008 | P0 | guard pages/canary de buffer | zero overwrite |
| RT-009 | P1 | parâmetro em alta taxa | tempo permanece limitado |
| RT-010 | P1 | command queue overflow | política segura e contador |
| RT-011 | P1 | teardown concorrente | áudio nunca acessa objeto destruído |
| RT-012 | P1 | reprepare | recursos antigos liberados fora do áudio |

## 17. Performance

### 17.1 Métrica de deadline

Para cada bloco:

```text
deadline = numSamples / sampleRate
load = processDuration / deadline
```

Gate inicial na configuração nominal da plataforma:

- média sustentada ≤ 35% do deadline;
- p99 ≤ 50%;
- máximo observado ≤ 80%;
- zero deadline miss em 30 minutos;
- zero dropout audível;
- UI ativa não piora p99 em mais de 10 pontos percentuais.

Configuração de stress:

- p99 ≤ 75%;
- nenhum crash, NaN ou corrupção;
- overload é sinalizado;
- degradação/fallback segue política, sem troca automática surpresa.

### 17.2 Cenários

| ID | Pri. | Cenário | Duração |
|---|---|---|---:|
| PRF-001 | P0 | preset nominal, 10 notas, Standard | 10 min |
| PRF-002 | P0 | polifonia nominal + sustain | 10 min |
| PRF-003 | P1 | glissando + CC + rotary transition | 10 min |
| PRF-004 | P1 | Worship 100 + room + drive | 10 min |
| PRF-005 | P1 | troca de preset a cada 2 s | 30 min |
| PRF-006 | P0 | apresentação contínua variada | 2 h |
| PRF-007 | P1 | background/foreground Android | 100 ciclos |
| PRF-008 | P1 | sample rate/device restart | 100 ciclos |

### 17.3 Memória

- tamanho residente estabiliza após warm-up;
- crescimento após 30 minutos ≤ 2% ou 8 MiB, o que for maior, com justificativa para caches;
- zero leak reportado por ferramenta;
- nenhuma alocação proporcional ao tempo de execução;
- capacidade de vozes/eventos/buffers registrada na telemetria;
- Eco precisa caber no orçamento do dispositivo Android de referência definido antes do M2.

### 17.4 Dispositivos de referência

Antes do gate M2, registrar em arquivo versionado:

- um Windows mínimo e um recomendado;
- um Mac Intel mínimo suportado;
- um Mac Apple Silicon;
- um Android arm64 de entrada;
- um Android arm64 intermediário;
- um tablet Android.

Para cada dispositivo:

- CPU/SOC;
- RAM;
- sistema;
- driver/API de áudio;
- sample rate/block reais;
- modo térmico/bateria;
- versão do build.

Nenhum release pode declarar desempenho aprovado usando apenas “PC moderno” ou emulador.

## 18. UI, responsividade e acessibilidade

| ID | Pri. | Objetivo | Plataformas / aceite |
|---|---|---|---|
| UI-001 | P0 | LIVE sem scroll | Android portrait em todos os tamanhos suportados |
| UI-002 | P0 | Sem overflow/colisão | breakpoints e font scale |
| UI-003 | P1 | Touch targets | ≥ 48 dp Android |
| UI-004 | P1 | Foco visível | Windows/macOS keyboard |
| UI-005 | P1 | Ordem de foco | leitura lógica, sem trap |
| UI-006 | P1 | Screen reader | nomes, valores, estados e ações |
| UI-007 | P1 | Contraste | WCAG AA para texto/controle aplicável |
| UI-008 | P1 | Não depender de cor | grayscale e simulação de daltonismo |
| UI-009 | P1 | Reduced motion | preferência respeitada |
| UI-010 | P0 | Panic | sempre alcançável e primeiro toque |
| UI-011 | P1 | MIDI Learn | normal, conflito, cancelamento |
| UI-012 | P1 | Textos longos | PT-BR e pseudo-localização |
| UI-013 | P1 | DPI/scale | Windows 100–250%, Retina e Android density |
| UI-014 | P1 | Orientação | Android portrait↔landscape sem perda de estado |
| UI-015 | P1 | CPU visualizer | animação não interfere no áudio |

Capturas obrigatórias correspondem às views definidas em `DESIGN_SYSTEM.md`.

## 19. Plataforma

### Windows

- WASAPI shared/exclusive conforme suporte atual;
- ASIO quando habilitado no ChurchDAW;
- troca de device;
- sleep/wake;
- DPI por monitor;
- teclado/mouse/touch;
- instalador e atualização sem alterar presets do usuário.

### macOS

- Intel e Apple Silicon;
- CoreAudio;
- sample rate/device change;
- sleep/wake;
- Retina;
- teclado/trackpad;
- bundle universal e deployment target suportado;
- estado do usuário preservado entre updates.

### Android

- AAudio/Oboe conforme produto;
- arm64;
- foreground/background;
- perda/retomada de audio focus;
- rota speaker/headphone/USB;
- Bluetooth tratado conforme política de latência;
- rotação de tela;
- thermal throttling;
- chamadas/notificações quando aplicável;
- processo recriado pelo sistema;
- TalkBack e font scale.

## 20. Regressão do ChurchDAW

Depois da integração:

| ID | Pri. | Área | Aceite |
|---|---|---|---|
| REG-001 | P0 | pianos/pads existentes | áudio e estado inalterados |
| REG-002 | P0 | plugins desktop | host continua funcional |
| REG-003 | P0 | biblioteca/download | catálogo e conteúdo local funcionam |
| REG-004 | P0 | licenciamento/login | fluxo preservado |
| REG-005 | P0 | update policy | preservada |
| REG-006 | P1 | rotary legado | estados antigos migram ou preservam comportamento |
| REG-007 | P1 | MIDI Learn global | sem colisão |
| REG-008 | P1 | master/reverb/limiter | headroom e sends corretos |
| REG-009 | P1 | projeto salvo | abre antes/depois da atualização |
| REG-010 | P1 | CPU sem órgão | custo adicional desprezível quando engine não existe/está bypass |

## 21. Listening tests

Painel mínimo de avaliação:

- organista experiente;
- tecladista worship;
- técnico de áudio;
- pessoa não envolvida na implementação.

Sessões cegas quando possível:

- drawbar registrations;
- percussão;
- scanner;
- drive;
- rotary Slow/Fast/Stop;
- Worship macro;
- pipe sustain;
- Eco vs Standard vs Ultra;
- preset transitions.

Escalas:

- ataque;
- corpo;
- estabilidade;
- movimento;
- largura;
- aspereza/alias;
- naturalidade;
- previsibilidade em mix;
- fadiga;
- adequação ao culto.

Listening test complementa, mas não substitui, gates P0 numéricos.

## 22. Clean-room e recursos

| ID | Pri. | Verificação | Aceite |
|---|---|---|---|
| CLR-001 | P0 | inventário de fontes | cada recurso tem origem/licença |
| CLR-002 | P0 | nomes de preset | originais e revisados |
| CLR-003 | P0 | código | revisão confirma ausência de cópia |
| CLR-004 | P0 | samples/IR | próprios ou licenciados com prova |
| CLR-005 | P1 | documentação | referências públicas registradas |
| CLR-006 | P1 | arte/UI | sem trade dress de concorrente |

## 23. Gates de release

### Gate Core Alpha

- todos os P0 de Unit/Offline;
- RT-001 a RT-004;
- ASan/UBSan limpos;
- performance nominal em uma máquina de desenvolvimento;
- estado v1 round-trip.

### Gate Integration Beta

- todos os P0 de Integration;
- REG-001 a REG-005;
- UI-001, UI-002 e UI-010;
- 30 minutos sem deadline miss em cada plataforma;
- migração legada validada.

### Gate Release Candidate

- todos os P0 e P1, exceto waivers assinados;
- endurance de 2 horas;
- dispositivos de referência completos;
- acessibilidade;
- clean-room;
- presets e recursos auditados;
- artefatos Windows/macOS/Android reproduzíveis.

### Gate Release

- hashes dos artefatos;
- versão/schema/revisão do engine registradas;
- rollback testado;
- suporte conhece telemetria e mensagens de erro;
- nenhum arquivo de teste/golden não autorizado no pacote;
- aprovação final de produto, áudio e plataforma.

## 24. Relatório por execução

Cada execução relevante guarda:

- commit/snapshot exato;
- sistema, CPU/SOC e RAM;
- compilador e flags;
- sample rate/block/quality;
- preset e seed;
- média, p99, máximo e deadline misses;
- vozes/steals/overflows;
- memória inicial/final;
- testes aprovados/falhos;
- links para logs e capturas;
- hash de golden/reference usada.

Uma falha nunca é resolvida apenas apagando ou regenerando o golden. A mudança precisa explicar por que o novo resultado é correto.
