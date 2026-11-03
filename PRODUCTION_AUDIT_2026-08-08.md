# Auditoria de produção — ChurchDAW

Data: 08/08/2026  
Plataformas auditadas: Windows 11/MSVC 2022 e macOS 26.5.1/Xcode, configuração Release  
Versão publicada: `1.6.8` (`version code 10615`)

## Resultado executivo

- **ChurchDAW 1.6.8 para Windows e macOS: compilado, testado e publicado.** Os builds Release e as regressões automatizadas passaram nos dois computadores.
- **Auto-update opcional comprovado para a 1.6.7.** A policy pública serial 17 foi validada pelo código nativo nos dois sistemas e decidiu `optional` para o version code 10614.
- **Disponibilidade pública do New Pad: pendente de publicação autorizada do catálogo.** O código, os catálogos locais e o ZIP estão corrigidos e validados, mas o catálogo atualmente servido pelo domínio ainda não contém `new_pad`.
- **Publicação direta concluída.** EXE e DMG foram enviados primeiro e a policy foi promovida por último; todos foram baixados novamente do domínio e conferidos byte a byte.

## Artefatos Release auditados

- Windows: `ChurchDAW-1.6.8-Setup.exe`, `14.711.808` bytes, SHA-256 `1c87bf21db3c20271b5ad463550553883f0395af522864016925bb21ec7bcdbd`.
- macOS: `ChurchDAW-1.6.8.dmg`, `17.804.377` bytes, SHA-256 `f5543813800bf57bbecce135beb8e45566f32725760510c569fc85d620d87b98`.
- Policy pública: serial `17`, SHA-256 `fc7f5e0ee439b1c51e6a62e7269564bd3813eb33514d42faee84f69b69c3727b`.
- App do DMG: `com.churchdaw.app`, versão `1.6.8`, universal `x86_64` + `arm64`, piso macOS 10.15.

## Gates automatizados

| Gate | Resultado |
|---|---:|
| CTest Release — áudio, MIDI, pads, cenas, plugins, biblioteca, master e limites | **19/19 PASS no Windows e 19/19 PASS no macOS** |
| Política assinada + manifesto real + BOM/line endings/adulteração | **45/45 PASS** |
| Validador Python de pacote de timbre | **18/18 PASS** |
| E2E New Pad em cinco tiers | **21/21 PASS** |
| Política pública de atualização desktop | **42/42 PASS no Windows e 38/38 PASS no macOS** |
| Fixture de licença RS256, claims, adulteração e vínculo de dispositivo | **PASS** |
| Android distribution probe — policy, catálogo assinado, bibliotecas e fallback | **PASS** |
| Harnesses especializados do Church Organ | **23/23 PASS** |

Também foram executados testes reais de carregamento/renderização: roteamento MASTER em oito cenários, render Astoria, 100 carregamentos sequenciais de timbre, loop de pad e round-trips VST3.

O probe do VST3 também validou o contrato observado depois da instanciação: um barramento principal estéreo ativo, dois canais de saída, 128 blocos processados com áudio, restauração e round-trip de estado, além de abertura e fechamento do editor.

## Auditoria visual e funcional

Foram abertos e inspecionados no executável Release:

- cadeia de Instrumentos;
- Live: Mixer, Performance e Sessão;
- Mapear MIDI;
- Pad Synth e painéis AMP, FILTER, MOD, LFO, MATRIX e ARP/GATE;
- Pad Contínuo em estado pronto e edição;
- Pads de Samples;
- Plugins;
- Bibliotecas em Todos, Instaladas e Disponíveis;
- Áudio/MIDI;
- EQ/ENV e editor de sample por canal;
- seletor de timbre em Todos, Favoritos, Recentes, categorias, busca e plugins;
- MASTER e controles ao vivo.

Correções visuais confirmadas no executável final:

- faders dos canais reduzidos em aproximadamente 30%, com o fader MASTER propositalmente maior;
- somente a página ativa fica destacada na navegação;
- `LIVE` fica vermelho apenas quando está ativo;
- `Instrumentos` encerra corretamente `Mapear MIDI` e retorna à cadeia;
- estados vazios de Favoritos, Recentes e busca no seletor agora explicam o próximo passo.

No smoke final de 14:09–14:15, o aplicativo reconheceu `ASIO4ALL v2`, `loopMIDI Port` e `Launchkey MK4 61 MIDI`; todas as rotas visuais acima abriram sem travamento. O processo encerrou normalmente e o trecho correspondente do log terminou com `steals=0`, `nan=0`, `xruns=0` e `clips=0`. Esse resultado comprova navegação e estabilidade em repouso nessa configuração; ele não substitui um ensaio musical prolongado com áudio e MIDI físicos.

## Matriz de garantia funcional

Uma garantia absoluta de “todas as funções em qualquer ambiente” não é tecnicamente defensável. A evidência produzida permite a seguinte classificação:

| Área | Status | Evidência e limite |
|---|---|---|
| Build e núcleo Windows Release | **COMPROVADO** | Todos os targets compilaram; CTest principal em `19/19 PASS`; execução e encerramento normais. |
| Mixer, canais, faders, MASTER, cenas, Performance e Sessão | **COMPROVADO localmente** | Testes de controles, cenas e roteamento, oito cenários de MASTER e smoke visual no executável final. |
| Timbres nativos e seletor | **COMPROVADO localmente** | Render real do Astoria, stress com 462 samples/100 trocas, seletor com 101 timbres, busca, categorias, favoritos e recentes. |
| Pad Synth, Pad Contínuo e Pads de Samples | **COMPROVADO localmente** | Testes DSP e de loop, pacote real instalado e smoke dos três painéis. |
| Church Organ | **COMPROVADO nos harnesses** | `23/23 PASS` em Core, Articulation, Character, Hybrid, Parameters, Pipe, State, Worship, Listening, UI, Composite, Integration, Motion e Transistor. |
| MIDI e mapeamento | **COMPROVADO na lógica; hardware condicional** | Segurança, roteamento e bytes MIDI testados; loopMIDI e Launchkey foram detectados. Não cobre todo teclado, pedal, cabo, driver ou reconexão possível. |
| VST3 do ChurchDAW | **COMPROVADO no host de teste** | Factory, instância, saída estéreo, áudio, estado e editor validados. |
| Plugins VST3 de terceiros | **PARCIAL** | Scanner isolado e Kontakt 8 validados. Não é possível garantir todo plugin, fabricante e versão existente. |
| Licença e atualização desktop | **COMPROVADO criptograficamente; serviço condicional** | RS256, claims, dispositivo e adulteração passaram; a policy e os instaladores públicos obtiveram `42/42` no Windows e `38/38` no Mac. Login real, indisponibilidade do servidor e contas de produção não foram exercitados. |
| Auto-update Windows/macOS | **1.6.8 DISPONÍVEL PUBLICAMENTE** | Policy serial 17 válida e assinada; EXE, DMG e policy públicos conferidos por tamanho, SHA-256 e formato. O código nativo classificou a 1.6.7 como atualização opcional nos dois sistemas. |
| Bibliotecas e New Pad | **LOCAL COMPROVADO; PÚBLICO PENDENTE** | ZIP, áudio, hashes, três catálogos locais e instalação passaram; o catálogo público de 61 entradas ainda não oferece `new_pad`. |
| Android e macOS | **macOS COMPROVADO; Android FÍSICO PENDENTE** | O Mac físico compilou o app universal, passou 19/19 testes, licença, policy e validação integral do DMG. O DMG segue sem Developer ID/notarização; não houve dispositivo Android físico. |
| Sessão ao vivo prolongada | **NÃO EXECUTADO** | Ainda falta soak test de horas, suspensão/retorno, unplug/replug, múltiplas taxas de amostragem e interfaces físicas. |

### Limite conhecido de roteamento

O MASTER FX do Church Live foi validado para os canais do mixer nos cenários previstos. Por arquitetura atual, o `Pad Contínuo` e a entrada externa de áudio conectam-se fora desse caminho e não atravessam o MASTER FX do Church Live. Alterar isso muda o comportamento de projetos salvos e exige uma decisão explícita de produto.

## New Pad e catálogo de timbres

Artefato auditado:

- Tamanho: `30.361.462` bytes
- SHA-256: `4b78ba9ff461bff56156db3fd4db901b174334f7d812385f4c73b3daa5953168`
- Conteúdo: 12 WAV + 1 SFZ + 1 JSON
- Áudio: 44,1 kHz, PCM 24-bit
- Cobertura MIDI: contínua de 0 a 127
- Artwork proibido e referências quebradas: zero

Os três catálogos locais possuem exatamente uma entrada canônica `new_pad`, instalada em `Pads/New Pad`. O catálogo rastreado em `Packaging/TimbrePacks/timbre-catalog-v1.json` contém 62 entradas e o gerador auditado produziu 96 entradas, sem IDs duplicados, preservando `new_pad` e `mellow_upright` mesmo quando ausentes da máquina geradora.

O catálogo público baixado em 08/08/2026 continha 61 entradas e não continha `new_pad`. Antes de anunciar o New Pad em produção, é necessário:

1. gerar o catálogo com `Packaging/BuildTimbreCatalog.ps1` no ambiente autorizado, sem `-PruneMissing`;
2. revisar o diff e confirmar exatamente uma entrada `new_pad` e uma `mellow_upright`;
3. publicar pelo procedimento oficial;
4. baixar novamente do domínio e repetir a validação de hash, contagem e instalação.

## Higiene final

- `git diff --check`: sem erros de whitespace.
- O build foi repetido depois das últimas correções visuais.
- A suíte CTest completa foi repetida depois do build final no Windows e no Mac.
- Os artefatos e a policy foram baixados novamente de `churchdaw.com` depois da promoção e seus hashes coincidiram com os originais.
- O executável foi fechado ao terminar a inspeção para não manter arquivos de build bloqueados.
