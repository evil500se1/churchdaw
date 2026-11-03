# Original User Request

## 2026-08-14T15:39:23Z

Realizar uma auditoria técnica e arquitetural aprofundada da codebase do ChurchDAW (C++17 / JUCE 7), avaliando o motor de áudio em tempo real, subsistemas DSP (Órgão, Reverb, Sampler), segurança de threads, integridade de memória e conformidade estrita com a política de zero-allocation na thread de áudio.

Working directory: /Users/igorteixeira/ChurchDAW
Integrity mode: development

## Requirements

### R1. Auditoria do Pipeline de Áudio em Tempo Real
Inspecionar todo o caminho de áudio (`processBlock`, `renderVoices`, rotinas de DSP e streamers de samples), identificando qualquer potencial violação de hard real-time: alocações dinâmicas de memória (`new`, `delete`, `malloc`, `free`, redimensionamento de contêineres), bloqueios por sincronização (`std::mutex`, `juce::CriticalSection`), chamadas de E/S em disco ou rede, ou chamadas de log dentro da thread de áudio.

### R2. Auditoria dos Subsistemas DSP e Síntese
Avaliar a arquitetura e estabilidade matemática dos módulos DSP:
- Motor do Órgão (`ChurchOrganCore`, drawbars, Leslie rotativo, scanner vibrato)
- Worship Reverb (matriz FDN, Pitch Shimmer +12/+7 semitons, ducking dinâmico)
- Sampler / Synth Voice rendering e fade curves
- Stereo Lookahead Limiter e Worship Punch Compressor

### R3. Auditoria de Concorrência e Mecanismos Lock-Free
Inspecionar a camada de transferência de parâmetros e eventos entre a Message Thread (UI) e a Audio Thread, avaliando a corretude de `std::atomic`, buffers circulares SPSC lock-free (`ChurchOrganParameterSink`), bitmasks atômicas e prevenção de race conditions ou memory ordering bugs.

### R4. Análise de Arquitetura Cross-Platform e Build
Verificar consistência entre os sistemas de build (CMake para Desktop/macOS/Windows/iOS e Projucer para Android), dependências do JUCE 7 e backends de áudio (ASIO, CoreAudio, Oboe).

### R5. Relatório Técnico Consolidado
Gerar o documento `CHURCHDAW_AUDIT_REPORT.md` no diretório de trabalho contendo o sumário executivo da arquitetura, tabela de vulnerabilidades/riscos classificados por severidade (Crítico, Alto, Médio, Baixo) com referências exatas de arquivos e linhas, e recomendações acionáveis de melhoria.

## Acceptance Criteria

### Verificação de Tempo Real
- [ ] O relatório aponta explicitamente se existem violações de zero-allocation na thread de áudio, indicando arquivos e linhas de código.
- [ ] Todas as chamadas de bloqueio, E/S ou alocação detectadas no caminho de áudio são catalogadas com seu impacto na latência.

### Verificação de DSP e Thread Safety
- [ ] Os módulos DSP essenciais (Órgão, Reverb, Limiter, Sampler) possuem análise de integridade numérica, saturação e performance.
- [ ] A sincronização entre threads (UI ↔ Áudio) é validada com diagnóstico sobre atomicidade e consistência de memória.

### Entregável Completo
- [ ] Arquivo `CHURCHDAW_AUDIT_REPORT.md` criado em `/Users/igorteixeira/ChurchDAW` estruturado com:
  - Visão Geral da Arquitetura do ChurchDAW
  - Diagnóstico do Motor de Áudio e Zero-Allocation
  - Análise dos Subsistemas DSP e Síntese
  - Análise de Concorrência e Thread Safety
  - Tabela de Achados e Riscos por Severidade
  - Recomendações e Próximos Passos

## 2026-09-03T03:06:03Z

This is a single self-contained fix; keep it small and focused.

Redesenhar completamente o canal Master do ChurchDAW no formato de console de estúdio profissional (SSL/Neve), tornando-o mais intuitivo, ergonômico e visualmente integrado com os canais ao vivo.

Working directory: C:\ChurchDAW-rc-1.6.27
Integrity mode: development

## Requirements

### R1. Layout Master Console Integrado
Reestruturar a coluna Master em 3 blocos bem proporcionados:
- **Master FX (Topo):** Slots de insert de efeitos claros e botão de edição de Reverb com controle direto de retorno (Return Level).
- **Master Bus & Medição (Centro):** Fader Master vertical longo com acabamento idêntico aos faders de canal, acompanhado por medidor estéreo de pico (L/R) de alta resolução e medidor de redução do Limiter contra distorção do som da igreja.
- **Live Utility & Emergency (Rodapé):** Botão PANIC grande e tátil com iluminação vermelha de alerta, controle de Transpose em dial central com visor numérico nítido do semitom (-12 a +12) e anel de LEDs, e interruptores de Sustain e Inverter pedal.

### R2. Preservação Total de Áudio e DSP
- Conservar a integridade de áudio, curvas de ganho, Master Limiter com true-peak e compatibilidade MIDI de hardware.
- Todos os testes unitários de DSP e segurança MIDI devem ser executados e passar com 100% de sucesso.

### R3. Implantação e Disponibilidade no Sistema
- Compilar o binário em Release e implantá-lo nos diretórios de execução do usuário (`Desktop\ChurchDAW-NOVO.exe` e `AppData\Local\Programs\ChurchDAW\ChurchDAW.exe`), garantindo que o bundle de backup para Mac Mini também seja atualizado.

## Acceptance Criteria

### Visual & Usabilidade
- [ ] O fader Master tem curso vertical estendido de pelo menos 180px e acabamento em alumínio usinado escovado.
- [ ] O medidor de pico estéreo (L/R) e o medidor de redução do Limiter ficam perfeitamente alinhados ao lado do fader, sem sobreposição de textos.
- [ ] O botão PANIC é destacado como alvo de toque prioritário no rodapé da coluna.
- [ ] O display de Transpose mostra o número do semitom (-12 a +12) com alto contraste no centro do dial.
- [ ] O rack Master FX permite visualizar o status dos plugins e controlar o retorno do Reverb de forma limpa e direta.

### Validação Automatizada
- [ ] Compilação do target `MinhaDAW` em Release concluída com código de saída 0.
- [ ] Execução de `ChurchDAWMasterLimiterTests.exe` finalizada com status `MASTER_LIMITER_TESTS_OK`.
- [ ] Execução de `ChurchDAWMidiMappingSafetyTests.exe` finalizada com status `MIDI_MAPPING_SAFETY_OK`.
- [ ] Executável copiado e verificado com hash SHA-256 idêntico em `Desktop\ChurchDAW-NOVO.exe`.
- [ ] Bundle Git atualizado em `C:\Users\Administrator\churchdaw-latest-1.6.30.bundle`.
