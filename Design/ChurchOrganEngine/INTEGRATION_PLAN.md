# Church Organ Engine — Plano de Integração

Status: plano pós-freeze. Nenhuma etapa deste documento autoriza modificar os arquivos ativos do ChurchDAW durante o trabalho paralelo atual.

## 1. Objetivo

Integrar o `ChurchOrganCore` ao ChurchDAW sem:

- sobrescrever trabalho de outra sessão;
- quebrar pianos, pads, plugins ou bibliotecas;
- introduzir alocação/lock no callback;
- invalidar estados antigos;
- misturar UI com DSP;
- criar divergência entre CMake e Projucer;
- alterar o instalador ou a publicação antes de aprovação.

## 2. Snapshot da auditoria

Auditoria realizada em 29/07/2026 por volta de 18:45, somente leitura:

- working tree principal: `C:\Users\Administrator\MinhaDAW`;
- branch: `main`;
- HEAD observado: `4fa4969ce846dc9316163d10ca01f82f9709ba55`;
- 17 arquivos rastreados modificados;
- dezenas de arquivos não rastreados;
- nenhuma alteração staged naquele momento;
- diff rastreado aproximado: `+9681 / -749`;
- `CMakeLists.txt` já continha aproximadamente 500 linhas adicionadas;
- `ChurchDAW.jucer` era não rastreado;
- arquivos DSP/testes tiveram gravações entre 18:19 e 18:44;
- duas amostras de hash por cerca de 20 segundos ficaram estáveis, o que não constitui freeze;
- havia processo MSBuild residente, possivelmente node reuse.

Conclusão: risco alto de colisão silenciosa. Arquivo “não rastreado” não significa arquivo livre; vários arquivos centrais novos ainda não estavam no Git.

## 3. Estado atual relacionado a órgão

Já existe funcionalidade parcial no produto:

- `TimbresProcessor.h`
  - detecta categoria/nome de órgão;
  - mantém `layerIsOrgan`;
  - expõe `rotModeN` e `rotAmtN`;
  - salva XML `rotMode` e `rotAmt`;
  - processa/avança o rotary por canal;
  - contém também grande parte do editor de timbres;
- `ChannelRotarySpeaker.h`
  - rotary speaker de horn/drum;
  - Slow/Stop/Fast;
  - smoothing, Doppler e caminho silencioso;
- `ChannelRotarySpeakerTests.cpp`
  - regressões do rotary atual;
- `NordLookAndFeel.h`
  - cor de categoria de órgão e desenho de controles;
- `OledDisplay.h`
  - ícone de órgão;
- `Assets/glyph_organ.png`
  - glyph da categoria;
- `MainComponent.cpp/.h`
  - shell e apresentação do instrumento/timbres;
- `LaunchkeyControl.h`, `MidiLearnPanel.h` e `PerformanceMidiSafety.h`
  - controle e proteções MIDI existentes.

Não foi encontrado um motor completo de tonewheel, drawbars, scanner, percussion, transistor ou pipe organ.

## 4. Arquivos proibidos durante o trabalho isolado

Não modificar antes de freeze e handoff explícito:

```text
CMakeLists.txt
ChurchDAW.jucer
.github/workflows/build-mac.yml
JUCE/modules/juce_audio_devices/native/juce_Oboe_android.cpp

Source/Main.cpp
Source/MainComponent.cpp
Source/MainComponent.h
Source/TimbresProcessor.h
Source/MasterProcessor.h
Source/WorshipReverb.h
Source/ChannelRotarySpeaker.h
Source/ChannelToneShaper.h
Source/TimbreToneMemory.h
Source/NordLookAndFeel.h
Source/OledDisplay.h
Source/TimbrePicker.h
Source/LaunchkeyControl.h
Source/MidiLearnPanel.h
Source/PerformanceMidiSafety.h

Tests/Audio/ChannelRotarySpeakerTests.cpp
Tests/Audio/ChannelToneShaperTests.cpp
Tests/Audio/TimbreLoadStressTests.cpp
```

Também não modificar nesta frente:

- licenciamento;
- catálogo/biblioteca;
- updater;
- instaladores;
- Android Java/manifest;
- workflows;
- site e artefatos publicados.

## 5. Faixa segura pré-freeze

Ownership exclusivo recomendado:

```text
Source/OrganEngineCore/
Tests/OrganEngineCore/
Design/ChurchOrganEngine/
```

Regras:

- somente arquivos novos;
- namespace novo e estável;
- nenhuma inclusão a partir dos arquivos atuais;
- nenhum registro no `CMakeLists.txt` raiz;
- nenhum item no `ChurchDAW.jucer`;
- nenhum asset novo no `BinaryData`;
- harness com CMake isolado dentro de `Tests/OrganEngineCore`;
- build em diretório único, por exemplo `build-organ-engine-core-<owner>`;
- não reutilizar `build`, `build-ui-tone` ou `build-licensing`;
- hashes dos arquivos de origem registrados no handoff.

Se outra sessão já tiver escolhido os mesmos diretórios, parar e renomear/redistribuir ownership antes de qualquer edição.

## 6. Condições para declarar freeze

O owner atual do produto precisa fornecer:

1. lista exata de arquivos congelados;
2. hash SHA-256 de cada arquivo compartilhado;
3. `git status --short`;
4. commit ou snapshot identificável;
5. confirmação de que builds/testes em andamento terminaram;
6. resultado mínimo dos testes atuais;
7. diretórios que permanecem sob trabalho de outra frente;
8. janela de integração e owner único.

Um timestamp sem hash não é freeze.

## 7. Ownership

| Área | Owner durante desenvolvimento | Owner durante integração |
|---|---|---|
| Núcleo DSP | DSP owner | DSP owner |
| Harness/testes isolados | DSP test owner | DSP owner |
| Host adapter | Integrator | Integrator |
| Estado/migração | Integrator + DSP owner | Integrator |
| UI/ViewModel | UI owner | UI owner |
| `TimbresProcessor` | owner atual antes do freeze | Integrator único |
| `MainComponent` | owner atual antes do freeze | Integrator único |
| MIDI/Launchkey | owner MIDI | Integrator único |
| CMake/Jucer | build owner | Build owner único |
| Android | Android owner | Android owner |
| Windows/macOS | desktop owner | Desktop/build owner |
| Release/site | release owner | release owner |

Princípio: um arquivo compartilhado tem um editor por janela de integração.

## 8. Contrato de handoff do núcleo

O DSP owner entrega:

- source tree isolada;
- API pública;
- catálogo de parâmetros;
- schema de estado;
- presets de teste originais;
- harness;
- relatório de alocação/locks;
- métricas de performance;
- limitações conhecidas;
- hashes;
- licença/origem de qualquer recurso;
- instruções de build isolado.

O integrator não recebe um patch que já modifique o host.

## 9. Pontos de hook pós-freeze

Os pontos abaixo são semânticos. Números de linha não devem ser usados porque os arquivos estão ativos.

### 9.1 Build

Arquivo: `CMakeLists.txt`

Após freeze:

- adicionar fontes `.cpp` do núcleo ao target apropriado;
- adicionar adapter/UI ao aplicativo;
- criar target de testes dedicado;
- vincular somente módulos JUCE necessários;
- manter `CHURCHDAW_BUILD_AUDIO_TESTS` ou criar opção específica documentada;
- aplicar `/utf-8` no MSVC;
- validar x64/arm64/macOS universal/Android;
- evitar glob de fontes para manter build reproduzível.

Arquivo: `ChurchDAW.jucer`

- registrar os mesmos arquivos;
- marcar headers como `compile=0`;
- marcar `.cpp/.mm` conforme plataforma;
- garantir que regenerar Projucer não apague flags de CMake/Android;
- comparar o inventário CMake x Jucer automaticamente ou por checklist.

O build owner faz essas duas alterações na mesma janela.

### 9.2 Processador/host

Arquivo: `Source/TimbresProcessor.h`

Hooks semânticos:

1. construção/registro de parâmetros;
2. `prepareToPlay`;
3. `releaseResources`/reset;
4. recepção e roteamento MIDI;
5. render por bloco;
6. cadeia por layer e sends;
7. serialização;
8. restauração/migração;
9. telemetria;
10. editor existente.

Estratégia:

- `TimbresProcessor` contém apenas `ChurchOrganHostAdapter`, não os módulos DSP;
- adapter possui lifecycle explícito;
- processador entrega evento/buffer/snapshot;
- núcleo renderiza para buffer dedicado pré-alocado;
- adapter decide mix, bypass e send;
- nenhum estado de UI é lido pelo callback;
- nenhuma string/ValueTree é manipulada no render;
- criação/preparo de nova engine ocorre fora do áudio;
- troca estrutural usa swap/crossfade.

Evitar:

- inserir centenas de linhas do órgão diretamente em `TimbresProcessor.h`;
- reutilizar as vozes do sampler sem contrato claro;
- fazer `looksLikeOrgan` selecionar implicitamente o novo engine;
- transformar todo pack rotulado “organ” em engine sintetizada;
- executar dois rotary speakers sem decisão.

### 9.3 UI

Arquivos:

- `Source/MainComponent.cpp`
- `Source/MainComponent.h`
- editor atualmente contido em `TimbresProcessor.h`
- `Source/NordLookAndFeel.h`
- `Source/OledDisplay.h`

Estratégia:

- criar `OrganPanel` e `OrganViewModel` separados;
- o shell abre/fecha a página;
- ViewModel publica comandos;
- telemetria chega por timer/snapshot em taxa limitada;
- layout usa breakpoints e safe area;
- Android LIVE não rola;
- UI não possui ponteiro cru para objetos destruídos no swap;
- LookAndFeel recebe somente extensões mínimas e reutilizáveis;
- assets entram apenas depois de revisão clean-room.

### 9.4 MIDI

Arquivos:

- `LaunchkeyControl.h`
- `MidiLearnPanel.h`
- `PerformanceMidiSafety.h`
- roteamento dentro do processador/host.

Estratégia:

- registrar parâmetros por ID estável;
- manter Panic, notas e controles reservados fora de learn;
- suportar pickup;
- separar MIDI bruto de eventos normalizados do núcleo;
- preservar mappings existentes;
- versionar mappings do órgão;
- tratar hot plug fora do áudio.

### 9.5 Master e ambiente

Arquivos:

- `MasterProcessor.h`
- `WorshipReverb.h`
- cadeia/send do `TimbresProcessor`.

Decisão obrigatória:

- definir se o órgão envia ao ambiente global, usa ambiente próprio ou combina ambos;
- impedir dupla reverberação por default;
- manter headroom;
- medir latência/cauda;
- nenhuma mudança no reverb existente até owner e testes estarem definidos.

## 10. Integração do rotary existente

Existem duas opções válidas; escolher uma em ADR antes do merge.

### Opção A — Adaptar o rotary legado

O novo core fornece sinal pré-cabinet e o host usa `ChannelRotarySpeaker`.

Vantagens:

- menor mudança inicial;
- preserva testes e comportamento de sessões atuais.

Riscos:

- contrato mais limitado que a especificação de mic/room/geometria;
- núcleo deixa de ser totalmente reutilizável;
- qualidade depende do host.

### Opção B — Rotary completo no núcleo

O Church Organ Engine usa `RotaryCabinet` próprio. O rotary legado continua para timbres/sample packs antigos.

Vantagens:

- arquitetura autocontida;
- coerência física e perfis de qualidade;
- reutilização futura.

Riscos:

- dois motores coexistem;
- estado/mix precisam distinguir origem;
- risco de duplo rotary.

Recomendação: Opção B para o novo instrumento, mantendo o legado somente nos canais antigos. O adaptador garante exclusão mútua.

## 11. Migração de estado

### 11.1 Estado legado observado

Parâmetros por layer:

```text
rotMode0..rotMode4
rotAmt0..rotAmt4
```

XML por layer:

```text
rotMode = 0 Slow, 1 Stop, 2 Fast
rotAmt  = 0.0 .. 1.0
```

Default atual observado:

```text
rotMode = Stop
rotAmt  = 0
```

### 11.2 Regras

- estado sem envelope do órgão continua sendo estado legado;
- não converter automaticamente qualquer timbre com nome “organ”;
- somente instância explicitamente criada como Church Organ Engine recebe schema novo;
- timbres/sample packs existentes preservam `rotMode/rotAmt`;
- na primeira criação do novo engine, valores legados podem inicializar:
  - `organ.rotary.mode`;
  - `organ.rotary.mix`;
- o estado original continua disponível para rollback;
- migração é idempotente;
- schema e migration version são salvos;
- números não finitos/fora da faixa recebem clamp/default;
- preset user não é sobrescrito silenciosamente.

### 11.3 Projeto/session

Envelope sugerido:

```text
ORGAN_ENGINE
  schemaVersion
  engineRevision
  instanceId
  family
  presetId
  parameters
  midiMappings
```

O projeto deve abrir mesmo quando a engine não puder ser preparada:

- manter o estado;
- exibir erro recuperável;
- não substituir por defaults;
- permitir retry;
- permitir bypass;
- salvar sem perder payload desconhecido.

## 12. Migração de preset

- factory preset usa ID namespaced e somente leitura;
- user preset recebe UUID;
- nomes de preset são originais;
- preset antigo não é “atualizado” sobrescrevendo o arquivo;
- migração cria cópia ou marca estado para salvar;
- recursos são resolvidos por ID, não por caminho arbitrário;
- preset incompatível informa motivo;
- unknown fields são preservados quando possível.

## 13. Sequência de integração

### Fase 0 — Freeze e snapshot

- receber hashes;
- criar snapshot/worktree de integração;
- confirmar status;
- congelar owners;
- rodar baseline de testes;
- arquivar relatório.

Saída: baseline reproduzível.

### Fase 1 — Núcleo no build, ainda desconectado

- adicionar fontes;
- compilar testes isolados e target do produto;
- nenhum objeto criado em runtime;
- validar Windows/macOS/Android toolchains.

Saída: zero mudança funcional.

### Fase 2 — Adapter em modo oculto

- criar adapter;
- prepare/reset em teste interno;
- não expor UI;
- processar somente em harness/instrumentação;
- medir custo quando inativo.

Saída: lifecycle validado.

### Fase 3 — Instância explícita

- adicionar um caminho interno para criar Church Organ Engine;
- não usar heurística por nome;
- render em layer/bus dedicado;
- bypass e Panic;
- state round-trip.

Saída: engine tocável sem UI final.

### Fase 4 — MIDI e parâmetros

- registrar IDs;
- snapshots/smoothing;
- mappings;
- expression e rotary;
- testes de burst/hot plug.

Saída: controle completo por harness/MIDI.

### Fase 5 — UI

- integrar shell/painel/ViewModel;
- LIVE;
- páginas avançadas;
- responsividade;
- acessibilidade;
- telemetria.

Saída: feature beta.

### Fase 6 — Migração e coexistência

- estado legado;
- rotary legado x novo;
- projetos antigos;
- presets;
- ausência/falha de engine;
- rollback.

Saída: compatibilidade aprovada.

### Fase 7 — Plataformas

- Windows;
- macOS universal;
- Android;
- dispositivos mínimos;
- performance/thermal/lifecycle;
- UI em orientations/DPI.

Saída: release candidate.

### Fase 8 — Release

- matriz completa;
- clean-room;
- installers/packages;
- hashes;
- update;
- publicação sob owner de release;
- suporte/telemetria.

Saída: artefatos distribuíveis.

## 14. Estratégia de branches/worktrees

Devido ao working tree principal muito sujo:

- não usar `git reset`, checkout destrutivo ou clean;
- não mover/apagar arquivos de outra frente;
- preferir worktree/branch criado a partir de snapshot confirmado;
- se o snapshot contém arquivos não rastreados necessários, copiá-los por processo controlado e comparar hashes;
- cada owner usa build dir próprio;
- commits separam:
  1. núcleo;
  2. testes;
  3. adapter;
  4. UI;
  5. build;
  6. migração;
- evitar commit “mega merge” impossível de revisar.

## 15. Protocolo contra colisão

Antes de editar arquivo compartilhado:

1. anunciar arquivo e janela;
2. confirmar owner;
3. capturar hash;
4. receber confirmação de freeze;
5. editar;
6. rodar testes;
7. publicar novo hash;
8. liberar ownership.

Se o hash mudar inesperadamente:

- parar;
- não salvar por cima;
- preservar ambos os snapshots;
- gerar diff;
- decidir merge com o owner.

## 16. Rollback

O release integrado deve permitir:

- abrir projeto com engine em bypass;
- ignorar UI/engine nova sem apagar estado;
- manter rotary legado para layers antigas;
- desligar feature por flag de build/runtime aprovada;
- reverter adapter sem alterar schema salvo;
- restaurar artefato anterior pelo processo de update existente.

Feature flag não pode:

- desativar licenciamento/update;
- mudar comportamento de bibliotecas;
- deixar parâmetros órfãos que quebrem carregamento;
- decidir dentro da thread de áudio por configuração mutável.

## 17. Verificação por fase

| Fase | Teste mínimo |
|---|---|
| 0 | suite atual antes da integração |
| 1 | build de todas as toolchains + testes core |
| 2 | prepare/reset/reprepare + zero custo inativo relevante |
| 3 | render, Panic, bypass, state round-trip |
| 4 | MIDI, smoothing, learn e overflow |
| 5 | layouts, foco, TalkBack/screen reader e LIVE sem scroll |
| 6 | projetos/presets antigos e rotary legado |
| 7 | performance/endurance por dispositivo |
| 8 | matriz completa, instalação/update e rollback |

## 18. Checklist do integrator

- [ ] Freeze documentado com hashes.
- [ ] Owner único dos arquivos compartilhados.
- [ ] Baseline de testes salvo.
- [ ] Núcleo não depende da UI.
- [ ] Nenhuma alocação/lock no callback.
- [ ] IDs de parâmetro revisados.
- [ ] Schema e migrações revisados.
- [ ] Rotary novo/legado mutuamente exclusivos.
- [ ] Ambiente/reverb sem duplicação.
- [ ] CMake e Jucer equivalentes.
- [ ] Windows/macOS/Android compilam.
- [ ] Projetos antigos abrem.
- [ ] Pianos/pads/plugins não regrediram.
- [ ] LIVE Android não rola.
- [ ] Acessibilidade aprovada.
- [ ] Clean-room e assets auditados.
- [ ] Rollback aprovado.
- [ ] Publicação feita somente pelo owner de release.

## 19. Critério de conclusão

A integração termina somente quando:

- o núcleo passa os gates de `TEST_MATRIX.md`;
- o produto atende `DESIGN_SYSTEM.md`;
- a arquitetura permanece conforme `ARCHITECTURE.md`;
- regressões existentes passam;
- migração é idempotente e reversível;
- todas as plataformas suportadas possuem evidência;
- hashes e ownership do release estão registrados;
- nenhuma alteração paralela foi perdida.
