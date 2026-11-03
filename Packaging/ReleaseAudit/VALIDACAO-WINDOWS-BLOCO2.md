# Validação Windows MSVC — Freeze Bloco 2 (2026-08-17)

Mesmo fluxo das rodadas anteriores (harness MSVC 19.44 em cópia isolada, sem tocar no checkout).

## Hashes congelados (SHA-256)

Copie exatamente estes arquivos e confira o hash antes de compilar:

```
c5d2a07848d499b8cbd987040a0f387862f90373bd597d10acefff40e01b1e2d  Source/MidiMappingSafety.h
9bac6c1197b767ca3d932e4508a5d62d9e9b9d192dad9fbac9a763f08a169165  Source/MainComponent.h
30d7a1d4d0067c468500c34e3cf0298b931deaff3905a358f2150a83ccf61456  Source/MainComponent.cpp
de7bc281c950fbdd9d1bdd83d117e71d200831aed9b4adb336d1ea5887316452  Source/MidiMappingPanel.h
a5a7ee545fcdca3dee2f9ece638ae7b74b86d3f197af2364f67abc6ca0e5cf7a  Source/PadProcessor.h
1ac4e883401197ad1fd5e0a4115589b3aa2de7b933cd010e4c9a6b71335cce19  Source/PerformanceMidiSafety.h
09612ae3c7ca6ea77c3763a4419d82f3178b70fddcbb9fe67f08ab2df3e72a78  Tests/Audio/MidiMappingSafetyTests.cpp
```

## O que rodar

1. **Teste focado Release**: compilar `MidiMappingSafetyTests.cpp` no harness e executar.
   Esperado: saída exata `MIDI_MAPPING_SAFETY_OK`, exit 0.
   (Os testes novos do Bloco 2 usam `std::thread`/`std::vector`/`std::is_sorted` — padrão C++17, sem dependência nova.)
2. **`/Zs` do `MainComponent.cpp`** com este `Source/` (valida sintaxe MSVC sem link).

## O que mudou desde o freeze anterior (contexto)

- `FixedControllerRouteDispatcher`: republicação não zera mais a tabela inteira
  (membership sobrevive à janela de rebuild), `finishRouteUpdate` aposenta os
  slots excedentes, spin bounded (128) no produtor para latest-wins,
  `matchesActiveRoute` novo, `resetAfterProducersStopped` (API morta) removida.
- `processControllerCC`: retorno = "pertence ao mapa do usuário"
  (membership || publicado || aceito pelo Learn); desmapear por gesto funciona
  em overflow >128; bloco morto `#if 0` de 261 linhas removido.
- Painel: `Command::rowDisambiguator` (linhas placeholder do Church Live),
  botão Armar não sobrescreve mais o status de falha.
- EOL normalizado para LF em todos os arquivos tocados (o CRLF misto era o que
  quebrava `apply_patch`/`git diff --check`).

Validação Mac já concluída neste freeze: focada Release OK, hosted OK,
ASan/UBSan universal OK, CTest 25/25, app Release universal x86_64+arm64 OK,
`git diff --check` limpo.
