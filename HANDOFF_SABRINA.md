# HANDOFF — Auditoria e correções de 05/08/2026

> **CORREÇÕES JÁ APLICADAS E COMPILADAS (0 erros, 0 avisos) — exe instalado 11:36.**
> Snapshot do código antes de tudo: `D:\ChurchDAW_Source\_snapshot-Source-20260805-1130`
> Rollbacks do exe: `ChurchDAW.exe.bak-20260805-1133-pre-midifix` e `.bak-pre-sabrina` (04/08).
>
> 1. **`MainComponent.cpp:4575`** — condição invertida. O timer de 1 s desligava as portas MIDI
>    que **não** eram DAW (o teclado do usuário) em vez da porta DAW. Removido o `!`.
>    Sintoma: usuário marcava a entrada em Áudio/MIDI e ela desmarcava sozinha em <1 s.
>    Agravante confirmado: ao fechar o painel, `saveAudioStateNow()` **persistia** o estado
>    desligado, então o dano sobrevivia ao restart.
> 2. **`TimbresProcessor.h:7743`** — **LAÇO INFINITO na audio thread** (o mais grave de todos).
>    O corte de silêncio inicial rodava depois da validação do loop e podia deixar
>    `loopLen == 0` com `hasLoop` ainda `true`; a primeira amostra da nota entrava em
>    `while (pos >= loopEnd) pos -= 0;` e travava o app inteiro (UI junto, pois o callback
>    lock nunca era solto). Corrigido preservando a invariante `loopLen > 256` e zerando
>    `hasLoop`. Risco alto: a biblioteca tem 97,5% dos samples com silêncio inicial.
> 3. **`TimbresProcessor.h:1302`** — `sabrinaFx.prepare()` realocava os 8 buffers de delay do
>    reverb enquanto a audio thread os lia. Envolvido em `suspendProcessing(true)` +
>    `ScopedLock(getCallbackLock())`, mesmo padrão que o resto do arquivo já usa.
> 4. **`SabrinaProcessor.cpp:60`** — removido `|| sabrinaEngine.ensureLoaded()` do `processBlock`
>    (era o caminho pelo qual o travamento original podia voltar). Fallback do Android
>    preservado intacto.
> 5. **`SabrinaFx.h`** — `nativeReady` virou `std::atomic<bool>` com release/acquire, e
>    `prepare()` agora derruba a flag **antes** de realocar, não depois.
>
> **PENDÊNCIAS conhecidas (não corrigidas, exigem decisão):** dois motores Sabrina vivos
> compartilhando um `hostSelf` estático; reverb aplicado ao master desde a inicialização e
> duplicado se o toggle for ligado; `refreshMidiInputs` ignora a preferência salva do usuário;
> `MidiBuffer::swapWith` vazando capacidade pequena do host para os scratch (realocação na
> audio thread sob rajada); `PadProcessor` interpolando linear nos 5 tons sustenidos.

---

# Sabrina VST2 travando o ChurchDAW (diagnóstico 05/08/2026)

Sessão Claude Code. Usuário relatou: "fiz build pra ele abrir o plugin sabrina com o toggle
já integrado, porém fez foi bugar o aplicativo travando".

**Status: causa raiz PROVADA por medição. Nenhuma correção aplicada ainda.**

---

## Resumo em uma linha

São **dois** problemas independentes: (1) a `Sabrina.dll` **não carrega nesta máquina**, e
(2) o ChurchDAW **tenta recarregá-la a cada bloco de áudio dentro da thread de tempo real**,
o que trava o app. O (2) é bug nosso e é o que trava; o (1) é da DLL.

---

## Arquivos envolvidos (todos alterados em 05/08/2026 10:34–10:38)

| Arquivo | Papel |
|---|---|
| `Source/SabrinaFx.h` (651 linhas) | Host VST2 mínimo — `locateDll()`, `Instance`, `Engine` |
| `Source/Plugins/Sabrina/SabrinaProcessor.cpp` (144 linhas) | Nó `juce::AudioProcessor` do graph |
| `Source/Plugins/Sabrina/SabrinaProcessor.h` | Declaração + `bypassed` atômico |
| `Source/MainComponent.cpp` | Toggle (linhas 918-934) e montagem do graph (7169-7206) |

Build: `Staging/WindowsRelease-1.6.1-Sabrina/` — built 2026-08-05T10:54:52, version code 10601.
Instalado em `%LOCALAPPDATA%\Programs\ChurchDAW\` (backup do exe anterior:
`ChurchDAW.exe.bak-pre-sabrina`, 04/08 18:19).

---

## PROBLEMA 1 — A `Sabrina.dll` não carrega (não é culpa do ChurchDAW)

Log `%APPDATA%\ChurchDAW\ChurchDAW.log`: **114 ocorrências** de
`SABRINA: LoadLibrary falhou ... (erro 1114)` em 3047 linhas.

**Erro 1114 = `ERROR_DLL_INIT_FAILED`** — o `DllMain` da DLL retornou FALSE.

### Prova de que o problema é da DLL, não do host

Teste em processo Python limpo, sem ChurchDAW nenhum:

```
LoadLibraryEx normal                      -> NULL, erro 1114
LoadLibraryEx DONT_RESOLVE_DLL_REFERENCES -> SUCESSO (handle 0x6b740000)
LoadLibraryEx LOAD_LIBRARY_AS_DATAFILE    -> SUCESSO
```

`DONT_RESOLVE_DLL_REFERENCES` **pula o `DllMain`**. Carrega sem ele e falha com ele
⇒ o `DllMain` é o culpado. Reproduz fora do ChurchDAW.

### Dados do PE (`Sabrina.dll`, 1.683.456 bytes)

- Arquitetura **x64** (0x8664), PE32+ — arquitetura está certa, não é 32/64 trocado
- **`DllCharacteristics = 0x0000`** → sem ASLR (DYNAMICBASE), **sem DEP (NX_COMPAT)**, sem SafeSEH
- `AddressOfEntryPoint` (DllMain) = `0x13E0` — existe DllMain
- Imports: `GDI32.dll`, `KERNEL32.dll`, `msvcrt.dll`, `USER32.dll` — **todas do sistema, nada faltando**
- Exports: `VSTPluginMain` e `main` — é um VST2 válido
- **Toolchain: `GCC: (tdm64-1) 4.9.2` e `(tdm64-1) 5.1.0`** = TDM-GCC/MinGW de ~2015
- Tem seção **`.tls`** (104 bytes) e `.CRT` → TLS callbacks do runtime MinGW

### Investigação do Grok (05/08/2026 11:01–11:15) — o que ela provou e o que descartou

O Grok gerou ~20 programas em `%TEMP%\sab_*.c/.exe` e 3 DLLs patcheadas. **Não resolveu**, mas
produziu dois achados sólidos:

**✅ ACHADO 1 — A DLL é estruturalmente sã.** O mapeador PE manual (`sab_map.exe`,
log `sab_trace2.txt`) carregou a DLL à mão, sem `LoadLibrary`:
```
map base=000000006B740000 pref=000000006B740000 size=1A5000
sections copied
imports ok
vst=000000006B7414B0
```
Conseguiu a **ImageBase preferida**, copiou seções, **resolveu todos os imports** e achou o
`VSTPluginMain`. Como o mapeador manual **não executa o DllMain**, isso isola o defeito:
o problema está **exclusivamente na inicialização (DllMain)**, não na estrutura, nos imports
nem em relocação de base.

**❌ DESCARTADO — não é TLS.** O Grok patcheou 3 variantes; testei as quatro com `LoadLibraryEx`:
```
ORIGINAL            -> FALHOU erro=1114
Sabrina_notls.dll   -> FALHOU erro=1114   (TLS directory zerado: rva=0, size=0)
Sabrina_nocb.dll    -> FALHOU erro=1114
Sabrina_patched.dll -> FALHOU erro=1114
```
Zerar o TLS directory **não muda nada**. A hipótese de TLS callbacks está morta.

**⚠️ A tentativa de força bruta falhou.** `sab_load_real.c` instala um
`AddVectoredExceptionHandler` que intercepta access violations, decodifica o tamanho da
instrução com um disassembler caseiro, **pula a instrução** (`Rip += len`), **zera RAX/RSI/RDI/RDX**
e continua — aceitando até 8000 exceções. O log `sab_ok.txt` **termina abruptamente na AV nº 43**,
sem nunca chegar a registrar o resultado do `LoadLibrary`. Ou seja, o processo morreu durante o
`LoadLibraryW`. 20 endereços distintos de AV, todos entre `0x6B7A2667` e `0x6B7A4068`
(RVA `0x62667`–`0x64068`, dentro do `.text`), com repetição em laço.

**🚫 NÃO USAR ESSA TÉCNICA EM PRODUÇÃO.** Mesmo que funcionasse: pular instruções e zerar
registradores faz o DSP rodar com estado corrompido (saída seria lixo), e o `insn_len` é
comprovadamente incorreto — assume `imm64` para todo `B8..BF` (é `imm32` sem REX.W), não trata
SSE/AVX (que um plugin de áudio usa o tempo todo) e cai num `return 3` chutado para opcode
desconhecido, o que dessincroniza o fluxo. Num app de áudio ao vivo isso é inaceitável.

### Hipótese principal atual (forte, ainda não confirmada)

Strings extraídas da DLL revelam o runtime MinGW-w64:
```
Mingw-w64 runtime failure:
  VirtualQuery failed for %d bytes at address %p
  VirtualProtect failed with code 0x%x
```
Essas são exatamente as mensagens de `pseudo-reloc.c` do MinGW-w64. O
**`_pei386_runtime_relocator()`** roda na inicialização e, para cada pseudo-relocation, chama
`VirtualProtect` para tornar a página gravável e corrigir o valor. Se isso falha, o runtime
aborta ⇒ `DllMain` retorna FALSE ⇒ **erro 1114**. Isso explica as AVs em massa numa região
contígua e o fato de o mapeamento manual (que pula o DllMain) funcionar.

Toolchain confirmado nas strings: TDM-GCC (`GCC: (tdm64-1) 4.9.2` e `5.1.0`), libstdc++ do GNU
(`__gnu_cxx::__concurrence_*`, `St13runtime_error` etc.), resquício de `libgcj-16.dll`.

### Próximos testes (em ordem de custo/benefício)

1. **Testar a DLL num host VST2 de referência** (Reaper tem trial livre). Se falhar lá também,
   o problema é a DLL/máquina e não vale mais tempo do nosso lado. **Fazer isso ANTES de qualquer
   outra coisa** — é o teste mais barato e o que mais decide.
2. Recompilar a AV com log de `ExceptionInformation[1]` (o **endereço acessado**, que o VEH do
   Grok não registrou — ele só logou o RIP). Isso diria se está escrevendo em `.text` read-only.
3. Conferir se o `.reloc` e a lista `__RUNTIME_PSEUDO_RELOC_LIST__` estão consistentes.

Original em `%USERPROFILE%\Downloads\Sabrina(64)\Sabrina.dll` — veio **sozinha**, sem dependências.
Artefatos do Grok preservados em `%TEMP%\sab_*` (36 arquivos) e `%TEMP%\Sabrina_*.dll` (3).

Original em `%USERPROFILE%\Downloads\Sabrina(64)\Sabrina.dll` — veio **sozinha**, sem dependências.

---

## PROBLEMA 2 — O travamento (BUG NOSSO, é o que precisa ser corrigido)

### O bug

`Source/Plugins/Sabrina/SabrinaProcessor.cpp`, linha 60, dentro de `processBlock`:

```cpp
if (sabrinaEngine.isLoaded() || sabrinaEngine.ensureLoaded())   // <-- AUDIO THREAD
{
    sabrinaEngine.processIfEnabled (buffer);
    return;
}
```

`processBlock` é **thread de áudio em tempo real**. `ensureLoaded()` faz:

1. `locateDll()` — **~19 acessos a disco** (`existsAsFile`), incluindo um loop `walk` de 8 níveis
   de diretório procurando `Packaging/BundledPlugins/...` e `Assets/Plugins/...`
2. `LoadLibraryW` — pega o **loader lock do Windows**, faz I/O, mapeia a DLL, roda o `DllMain`
3. `juce::Logger::writeToLog(...)` — **I/O de disco com lock**
4. `instance.prepare()` → `scratchIn.assign(...)` — **alocação de heap**

O próprio `SabrinaFx.h:556` documenta a intenção correta:
`// Message thread: tenta achar e carregar a DLL.` — mas está sendo chamado da audio thread.

**E não há cache da falha** — como a DLL sempre falha, ele repete tudo isso a cada bloco.

### Por que isso trava com certeza matemática

Log linha 2836:
`AUDIO_CHANGE type="ASIO" output="ASIO4ALL v2" input="ASIO4ALL v2" inChannels=2 buffer=128 rate=44100`

**128 samples @ 44100 Hz = 2,9 ms de orçamento por bloco.** Um `LoadLibraryW` que falha com
1114 (mapear a DLL, resolver imports, rodar e desfazer o DllMain) leva dezenas de ms.
O callback estoura o prazo em toda chamada ⇒ xrun contínuo ⇒ áudio e app travam.

Sintoma no log: `ChurchDAW 1.6.0 iniciado` **3× em 44 segundos** (10:51:10, 10:51:23, 10:51:54)
= reinícios seguidos. (Nota: log diz 1.6.0 mas o build é 1.6.1 — string de versão pode não
ter sido atualizada, checar.)

### Correção mínima

- Nunca chamar `ensureLoaded()` de `processBlock`. Carregar só em `prepareToPlay`
  (message thread) ou numa thread de background.
- Guardar `std::atomic<bool> loadFailed` — **uma** tentativa; se falhar, nunca mais tentar
  e cair direto no fallback `tanh`.
- Tirar `Logger::writeToLog` de qualquer caminho de audio thread.
- Enquanto não carregado, `processBlock` deve ser passthrough puro.

---

## Outros bugs encontrados (não são a causa do travamento, mas são reais)

**A) `MainComponent.cpp:7171-7173` — `shared_ptr` aliasing perigoso**
```cpp
auto proc = std::make_unique<SabrinaProcessor>();
sabrinaProcessor = std::shared_ptr<SabrinaProcessor> (proc.get(), [] (SabrinaProcessor*) {});
sabrinaNode = graph.addNode (std::move (proc));
```
`shared_ptr` com deleter no-op apontando para objeto cujo dono é o graph. Se o graph destruir
o nó, `sabrinaProcessor` vira dangling e o `onClick` do botão chama
`sabrinaProcessor->setSabrinaBypassed()` ⇒ **use-after-free**.

**B) `SabrinaFx.h:490` — `static inline Instance* hostSelf`**
Ponteiro global único (o callback VST2 C não carrega user-data). Se o graph recriar o nó,
duas instâncias coexistem: a nova sobrescreve `hostSelf`, e o `unload()` da **antiga** seta
`hostSelf = nullptr`, cegando a nova (passa a reportar 44100/512 fixos).

**C) Sabrina liga por padrão**
`SabrinaProcessor.h:67` tem `bypassed{false}` e `MainComponent.cpp:920` faz
`setToggleState(true)` (= não-bypassed). Resultado: **trava assim que o app abre**, sem o
usuário tocar em nada. Deveria nascer em bypass até a DLL confirmar carregamento.

**D) `getTailLengthSeconds()` retorna `0.0`** para um plugin descrito como delay/diffusion —
o graph corta a cauda do efeito.

**E) Pendência de leitura:** a string `SABRINA: desligada (bypass)` aparece no log mas **não**
está em `SabrinaFx.h` nem em `SabrinaProcessor.cpp`. Deve estar em `TimbresProcessor.h` ou
`MainComponent.cpp` — localizar, pode haver um segundo caminho de código para o Sabrina.

---

## Como voltar ao estado bom agora (rollback rápido)

```
# fechar o ChurchDAW antes
Copy-Item "$env:LOCALAPPDATA\Programs\ChurchDAW\ChurchDAW.exe.bak-pre-sabrina" `
          "$env:LOCALAPPDATA\Programs\ChurchDAW\ChurchDAW.exe" -Force
```
O backup é o exe de 04/08 18:19, anterior ao Sabrina.

---

## Decisão pendente do usuário

1. **Rollback** para o exe anterior e tratar Sabrina depois; ou
2. **Corrigir o host** (tirar o load da audio thread + cache de falha + nascer em bypass) —
   resolve o travamento, mas o Sabrina seguirá em fallback `tanh` enquanto a DLL não carregar; ou
3. **Atacar a DLL** (por que o DllMain falha) — sem isso o Sabrina real nunca vai tocar.

O caminho 2 é obrigatório de qualquer forma: mesmo com a DLL consertada, carregar VST na
audio thread é bug.
