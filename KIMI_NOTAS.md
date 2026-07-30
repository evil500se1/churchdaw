# KIMI_NOTAS — diagnóstico paralelo (Kimi, 30/07/2026 ~03:30 UTC)

Notas de uma segunda análise independente (agente Kimi), feita em modo leitura
enquanto o Claude Code trabalha na árvore. Nada aqui foi editado por mim.
Apague este arquivo quando quiser.

Contato indireto: o usuário relê estas notas e pode colar trechos na sessão
do Claude Code. Eu monitoro a sessão e valido builds/testes de forma
independente (ADB: Redmi 24115RA8EG Android 16 + emulador x86_64 disponíveis).

---

## 1. Verificado por mim: gate CATALOG_LEGACY_ORGAN está VERDE

O binário rebuildado às 02:31:35 UTC
(`build-organ-integration/ChurchDAWTimbreLoadStressTests.exe`) passou 4/4
vezes com `--controls-only` fora da árvore:

```
CATALOG_LEGACY_ORGAN_OK families=4 cached=7 original_rms == restored_rms
CHANNEL_CONTROL_STATE_OK
```

A falha das 02:30 (`restart-restore` com timbre="") foi corrigida pelo Codex.
Mecânica do bug (para referência): o restore só podia morrer em silêncio em
dois pontos — descarte por geração (`finishLayerLoad`, ~linha 6270) ou
`zones` vazias por cancelamento (~linha 6280).

## 2. Mapa do restore (o "defeito 1" que você está olhando)

- `setStateInformation` (~4629): se o `root` salvo difere de `rootFolder`,
  faz `entriesScanned = false` + `startEntriesScanAsync(true)`.
- `requestLayerLoad` (~6705): com `entriesScanned == false`, o rótulo vai para
  `pendingLayerLabels[l]` e só carrega em `drainPendingLayerLoads()` quando o
  scan termina (`finishEntriesScan` / `finishEntriesScanFailure`).
- Risco residual: se o scan assíncrono nunca completar (pasta raiz ausente,
  abort), os rótulos ficam pendentes — `finishEntriesScanFailure` cobre falha
  antes do worker iniciar, mas vale um gate com raiz inexistente.
- No restore, `cancelLayerLoad(l)` + `++layerLoadGeneration[l]` rodam para
  TODAS as camadas antes dos `requestLayerLoad` individuais — ordem correta,
  sem risco de abortar o próprio pedido.

## 3. Rotary: existem DOIS caminhos distintos (suspeito do "som some")

- **Sampled/Transistor/Pipe** → `ChannelRotarySpeaker::process`
  (TimbresProcessor.h ~8540): `amount = *lay.rotaryAmount`,
  `mode = rotaryMode->getIndex()` (0=Slow, 1=Stop, 2=Fast). Revisado: sem
  caminho de cancelamento — Amount=0 reconstrói o input (delay zero),
  sem comb filter (corrigido conforme comentário em ChannelRotarySpeaker.h
  linhas 172-182).
- **Tonewheel/Worship (motor nativo)** → `RotaryCabinet` DENTRO do motor
  (`Source/ChurchOrgan/RotaryCabinet.cpp`), dirigido por
  `setChurchOrganRotaryMode` (~2602) e pelos `RotaryParameters`.

**Hipótese principal para "quanto mais rotary, mais some o som":**
a UI tem UM knob Rotary (`rotaryAmount` 0..1) + seletor Slow/Stop/Fast.
No motor nativo, verificar:
1. O `rotaryAmount` da UI chega ao `RotaryCabinet`? Em `RotaryParameters`,
   `mix` default é 1.0 e `output` 0.90. Se a UI sobe `amount` mas o motor
   só recebe `speedMode`, o knob pode estar mudando outro parâmetro (ou
   nada) — e o som "some" por outra causa (ex.: `wetCalibrationGain`,
   perspectiva `vintageMono` com `monoBlend`, ou `output` baixo).
2. `layerUsesChannelRotary(l)` decide qual caminho processa — conferir se
   para Tonewheel/Worship o canal NÃO processa o ChannelRotarySpeaker
   (correto) e se o modo chega ao motor via `setChurchOrganRotaryMode`
   em toda troca de UI/restore (e não só na seleção do timbre).
3. No `RotaryCabinet::process`, procurar soma dry+wet coerente com delay
   pequeno (mesmo defeito histórico do ChannelRotarySpeaker, já corrigido
   lá): `hornLine_`/`drumLine_`/`sideLine_` + `wetMix_` — se wet é somado
   ao dry em vez de substituí-lo, delay fixo do cabinet vira pente.
4. `RotarySpeedMode::brake` existe no motor mas NÃO existe no
   ChannelRotarySpeaker nem no clamp do restore
   (`jlimit(slowMode, fastMode, ...)`) — mapeamento de índices UI→motor
   pode estar desalinhado por 1 posição (Slow/Stop/Fast/Brake = 4 estados
   na UI? ver `ChurchOrganControls`).

### Atualização 03:40 UTC — o miolo do RotaryCabinet (lido por completo)

A cadeia UI→motor está íntegra: `setChurchOrganRotaryMix` escreve
`rotaryAmount` + `OrganParameterId::rotaryMix`; `setChurchOrganRotaryMode`
mapeia stop/slow/fast/brake corretamente (brake→STOP no ChannelRotarySpeaker
é deliberado, comentário em ~2639). NÃO há desalinhamento de índice.

O estágio de saída do RotaryCabinet é equal-power (sem dip de -6 dB) com
duas compensações adaptativas — e é nelas que moram os suspeitos reais:

1. **`wetCalibration_` (linhas ~577-598):** a cada 64 amostras, alvo =
   `sqrt(dryPower/wetPower)` clamp [0,68; 2,25], detector 1,5 s + smoother
   3,5 s. O update é PULADO abaixo de 1e-9/1e-10 — mas logo ACIMA desses
   limiares (cauda morrendo, pad quieto) a razão dry/wet é lixo numérico e
   pode arrastar a calibragem. No próximo ataque, o smoother de 3,5 s leva
   segundos pra voltar → "o som some e depois retorna". Sugestão: exigir
   dryPowerState_ > ~1e-6 (sinal musical de verdade) antes de atualizar,
   ou congelar a calibragem quando wet (mix) está baixo.
2. **`wetLeft = leftAirState_` (~550):** o wet é SUBSTITUÍDO pelo low-pass
   de absorção de ar. Com distance/absorption altos o órgão escurece
   muito — percebido como "sumindo", principalmente em fone/desktop.
3. **`correlationCompensation` floor 0,78 em mix intermediário:** com
   wet atrasado ~2,35 ms + filtrado, dry+wet correlacionados negativamente
   perdem até ~-2 dB mesmo com o clamp (0,42 → comp 1,32 máx). Audível
   ao girar o knob no meio do curso.
4. O comentário em ~524-528 ("cabinet floor ... prevents angular nulls
   from making a fully-wet organ disappear") indica que o autor já viu o
   sintoma de sumir — o `cabinetFloor` (0,14+) é a band-aid; conferir se
   o piso sobrevive ao low-pass de ar em todos os presets/perspectivas
   (`vintageMono` tem monoBlend que pode colapsar a imagem).

## 4. Transpose "automático" no Mac (canais 1 e 2)

Tudo que era causa conhecida JÁ tem guarda no código atual:
- Knob 8 do Launchkey não é mais transpose global (MainComponent.cpp ~2206).
- Mapas MIDI legados ambíguos são descartados no restore
  (`isSafeStoredTarget`, MainComponent.cpp ~3219) — inclusive os que
  apontariam para Transpose.
- `transposeSchema >= 1` zera transpose global de saves antigos
  (TimbresProcessor.h ~4686).
- Ledger `noteTranspose` por camada com fail-closed (~7389) e PANIC global.

Suspeitos residuais que valem instrumentação:
1. `pluginPitchCentresPending[l]` é setado `true` nos caminhos de hot-load
   (2064, 2196, 2824, 2920) e consumido em ~7658. Se ALGUM caminho de
   criação de instância AU/VST no Mac não seta o flag, o plugin nasce sem
   os 16 centros — e se o estado salvo do AU tinha pitch ≠ centro, as notas
   "andam". Procurar: todo `loadInstrumentPlugin`/`loadInsert` seta o flag?
2. No Mac, `restoreMidiMap` roda antes do primeiro callback? Um mapa
   restaurado tarde pode aplicar CC de transpose vindo do controlador.
3. Sugestão de teste: `CHURCHDAW_SELFTEST=external` no Mac + log por bloco
   de `*lay.transp + *globalTransp` nos canais 1/2 — se o valor muda sem
   gesto do usuário, o log mostra a origem (UI, CC, restore).

## 5. Estado da infra (para não duplicar trabalho)

- Codex foi parado pelo usuário ~03:10 UTC; Claude Code é o dono da árvore.
- `build` antigo já foi limpo; ~8 GB livres em C:.
- APK 1.5.4 (versionCode 9) instalado no Redmi; emulador tem variantes
  qa/qarelease/x86test/miditest.
- Pendências conhecidas (não são bugs): certificado de código Windows,
  notarização macOS, publicar update-policy.json + API na HostGator,
  chave privada `churchdaw-update-2026-01` não localizada.
