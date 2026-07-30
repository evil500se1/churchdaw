# Acesso fora dos limites no WorshipReverb — correção e limites da evidência

**Achado e corrigido em 28/07/2026.** A correção final está em
`Source/WorshipReverb.h`: o wrap fracionário foi substituído por decomposição
inteira (`wholeDelay + fraction`) e índices inteiros sempre contidos no vetor.

---

## O local exato

```
WorshipReverb.h:505   data[(size_t) first]        <- índice >= data.size()
  chamado de
WorshipReverb.h:290   predelay[0].read (currentPredelay)
  dentro de
WorshipReverb::process()
```

Confirmado por **duas ferramentas independentes**:

1. **STL Debug do MSVC** — `vector subscript out of range`, `<vector>` linha 1941.
   É a mesma caixa que apareceu na tela da outra sessão.
2. **AddressSanitizer** (RelWithDebInfo):
   ```
   ERROR: AddressSanitizer: container-overflow
   READ of size 4 at 0x11fd48faf140
     #0 WorshipReverb::FractionalDelay::read  WorshipReverb.h:505
     #1 WorshipReverb::process                WorshipReverb.h:290
   0x11fd48faf140 is located 59712 bytes inside of 80235-byte region
   ```

`container-overflow` é a informação mais importante: o endereço está **dentro da
capacidade alocada, mas além de `size()`**. Ou seja, `first` chegou a valer
`size() + 8`. Não é ponteiro solto — é índice maior que o tamanho lógico.

## Como reproduzir

```powershell
cmake -S . -B build -DCHURCHDAW_BUILD_AUDIO_TESTS=ON
cmake --build build --config Debug --target ChurchDAWReverbBoundsProbe
.\build\ChurchDAWReverbBoundsProbe_artefacts\Debug\ChurchDAWReverbBoundsProbe.exe
```

A sonda (`Tests/Audio/ReverbBoundsProbe.cpp`) imita o app: 44,1 kHz / bloco 128,
barramento de 8192, as 12 máquinas, troca de preset e mudança de taxa. Ela
imprime a **pilha** no momento do assert — que é justamente o que a caixa
"vector subscript out of range" não mostra.

Dispara ao **re-preparar a mesma instância com outra taxa** (44100 → 48000). Em
uma execução em Debug disparou já numa instância nova a 48 kHz. Ou seja: é
intermitente também na sonda, o que bate com todo o resto.

## O que o achado explica — e o que ele não prova sozinho

| Sintoma | Explicação |
|---|---|
| **Falha potencial no Windows** | Em Release o `std::vector` não confere índice. A leitura inválida podia corromper estado e era compatível com um fail-fast, mas os quatro eventos `0xC0000409` não tinham dump/PDB e não permitem atribuir causalidade exclusiva ao reverb. A auditoria encontrou também um P0 independente no carregador: pianos inteiros podiam ultrapassar 7,6 GiB durante a troca e exceções escapavam de workers. Os dois defeitos foram corrigidos e testados separadamente. |
| **Suíte de reverb dá resultado diferente a cada execução** | Ler memória alheia devolve valores diferentes a cada vez. Medi: T60 em 125 Hz deu 63,96 s / 10,11 s / 10,11 s em três execuções do mesmo binário. |
| **Assert em Debug** | O STL confere e dispara. |

## Causa raiz e correção

O cálculo antigo fazia o wrap em `float`:

```cpp
float position = (float) writeIndex - delay;
while (position < 0.0f)
    position += (float) data.size();
```

Quando `position` era negativo, mas menor que um ULP no tamanho daquele buffer,
a soma podia arredondar para **exatamente `data.size()`**. Converter esse valor
para inteiro produzia um índice uma posição além do vetor. A capacidade
geométrica maior do MSVC tornava a leitura aparentemente válida em Release,
por isso o valor aleatório entrava no feedback em vez de falhar imediatamente.

`read()` e `makeCubicTap()` agora separam a parte inteira e fracionária do delay,
fazem o wrap somente em inteiros e constroem todos os vizinhos a partir de
índices comprovadamente em `[0, count - 1]`. Não há `fmod`, alocação ou branch
excepcional caro no callback.

## Regressão

`Tests/Audio/ReverbBoundsProbe.cpp` fica como teste permanente. Vale rodá-lo em
Debug antes de cada versão: ele pega esta classe de erro de forma determinística,
enquanto a suíte em Release apenas devolvia números diferentes sem acusar nada.

No snapshot corrigido, a sonda passou três vezes em Debug e três vezes em
RelWithDebInfo + AddressSanitizer, cobrindo 44,1/48/88,2/96 kHz, blocos de
32 a 8192, todos os 12 presets, troca no meio do bloco e reprepare da mesma
instância. A suíte principal também inclui a regressão
`Fractional-delay bounds/reprepare`.
