# PESQUISA — MOTOR DE PIANO POR MODELAGEM FÍSICA PARA O CHURCHDAW

Documento técnico de referência. Fase de pesquisa — nenhuma linha de código de produção foi escrita.
Data: 2026-08-05.

**Convenção de confiança usada no documento inteiro:**

| Marca | Significado |
|---|---|
| `[FATO]` | Consta em paper, patente ou documentação técnica que alguém abriu e leu. |
| `[MARKETING]` | Declaração de fabricante sem detalhe técnico verificável. |
| `[INFERÊNCIA]` | Raciocínio/derivação feita durante a pesquisa. Pode estar errado. |
| `[NÃO CONFIRMADO]` | Citado por terceiros, não verificado na fonte primária. |

Onde há número de arquivo e linha (`TimbresProcessor.h:9484`), isso vem de leitura direta do código do ChurchDAW.

---

## 0. TABELA DE SÍMBOLOS — leia antes de copiar qualquer fórmula

**As fontes usam a MESMA letra para coisas diferentes.** Três colisões neste documento já causariam erro de muitas ordens de grandeza. A partir daqui o documento usa os sufixos abaixo; onde uma equação vem de um paper, o símbolo original do paper está indicado.

| Símbolo aqui | No paper | Significado | Unidade | Faixa típica |
|---|---|---|---|---|
| `κ_B` | `κ` (Bensa 2003) | coeficiente de rigidez `√(EI/μ_ρ)` | **m²/s** | 0,58 a 1,25 |
| `κ_r` | `κ` (Bank 2010) | **raio de giração** `= d/4` | **m** | 0,2 a 0,3 mm |
| `μ_ρ` | `μ` (Bank), `ρA` (Bensa) | densidade linear da corda | kg/m | 6e−3 a 0,2 |
| `μ_s` | `μ` (Bensa, §3.3) | número de estabilidade `κ_B·Δt/X²` | **adimensional** | < 0,5 |
| `T₀` | `T` (Bensa/Young) | tensão da corda | N | 590 a 1780 |
| `Δt` | `T` (Bensa, §3.3) | passo de tempo | s | 1/fs |
| `b₂` | `b₂` (Bensa Eq. 37b) | perda dep. de freq., **casa com `(kπ/L)²`** | m²/s | 1e−5 a 2e−3 |
| `b₃` | `b₃` (Bank Eq. 7) | perda dep. de freq., **casa com `(2πf_k)²`** | **s** | — |

**As três armadilhas, explicitamente:**

1. **`κ_B` ≠ `κ_r`.** São grandezas físicas diferentes com unidades diferentes. `κ_B = 1,25 m²/s` (Tabela I do Bensa, C4) jogado dentro de `B = κ_r²·(E·S/T₀)·(π/L)²` erra por ~12 ordens de grandeza. **Nunca substitua um pelo outro.**
2. **`b₂` ≠ `b₃`.** A relação é `b₃ = b₂/c²` (verificada dimensionalmente: `(m²/s)/(m²/s²) = s`). O par `b₁/b₂` das Eqs. 37a/37b do Bensa **só vale com `(kπ/L)²`**, nunca com `(2πf_k)²`.
3. **`μ` e `T` colidem** entre a seção da corda (§2.6, §3.2) e a seção de FDTD (§3.3). Sempre confira qual é qual pelo contexto e pela unidade.

**A ponte entre as duas notações de inarmonicidade** `[FATO]`, verificada numericamente contra a Tabela I do Bensa:
```
B = ( κ_B · π / (c · L) )²
```
Reproduz exatamente os valores da §2.6: C2 → 8,48e−5; C4 → 3,58e−4; C7 → 8,66e−3. **Esta é a fórmula a usar quando os seus dados vierem do Bensa.**

---

## 1. RESUMO EM 10 LINHAS

1. Piano por modelagem física em tempo real **é viável em CPU** — o laço de áudio custa menos que o `PipeOrganEngine` que já roda ao vivo hoje.
2. O gargalo **não é DSP, é calibração**: obter `(f_k, τ_k, A_k)` das 88 notas que soem como piano é onde a Modartt gastou ~20 anos com 6+ PhDs.
3. Das três famílias (waveguide / modal / FDTD), a escolha é **MODAL**. É a única com prova pública de piano completo em tempo real e a única que permite não-linearidade com anti-aliasing controlado.
4. O Pianoteq **não é waveguide nem FDTD** — é pré-síntese offline (FEM) + síntese aditiva de senoides amortecidas em tempo real. Isso é fato de patente (US7915515B2), não especulação.
5. Reproduzir o pipeline do Pianoteq (FEM + autovalores não-lineares + Padé multivariado) é **inviável para dev solo** — é trabalho de doutorado em análise numérica antes do primeiro som.
6. Existe um atalho que muda tudo: extrair `(f_k, τ_k, A_k)` por **análise das gravações que você já tem** em vez de FEM. Pula o pipeline físico inteiro e ainda sai fora da claim 1 da patente da Modartt.
7. **Se a decisão for CONSTRUIR, o caminho HÍBRIDO vale muito mais a pena que o modelo puro** — as quatro coisas que sampler faz mal (ressonância simpática, abafador/meio-pedal, degrau entre camadas de velocity, re-ataque) são as quatro que modelagem faz bem e barato. **Mas "construir" não é a única opção, e a §9.0 põe as quatro lado a lado**: hospedar um piano pronto (o VST hosting já existe) e não fazer nada provavelmente vencem, e o argumento real a favor de construir é comercial (não depender de licença de terceiro), não sonoro.
8. `[INFERÊNCIA]` Primeiro retorno **audível** do híbrido: **~2 meses de trabalho parcial** (Etapa 0 + Etapa 1), e só se a extração funcionar e o material de entrada for seco. Modelo puro: **12–24 meses**, e provavelmente soa pior que o sampler atual. **Antes de decidir, leia a §9.0 — construir não é a única opção, e provavelmente não é a melhor.**
9. Comece pela **Etapa 0**: um `.cpp` standalone, fora do JUCE, que ressintetiza notas do seu sampler a partir de uma tabela modal e escreve WAVs lado a lado. Risco zero para a DAW, e responde a única pergunta que importa. **Custo real: 2–3 semanas, não 3–5 dias** — ver §ETAPA 0.
10. Riscos jurídicos: US7915515B2 ativa nos EUA até 20/10/2027 (evitável por desenho); Yamaha US8115092/8895831/8729376 ativas até 2030–2032. O risco mais provável de todos **não é patente, é marca** — nunca nomeie um preset de "Steinway".

---

## 2. COMO UM PIANO DE VERDADE FUNCIONA (a física que importa para o modelo)

Só o que tem consequência direta no código. Cada parâmetro aqui vira um número numa struct.

### 2.1 O martelo — a parte barata e bem documentada

`[FATO]` Chaigne & Askenfelt, JASA 95(2):1112-1118, 1994, Eqs. (6) e (7) — lido integralmente:

```
F_H(t) = K · |η(t) − y(x₀,t)|^p        (força de contato, só se a compressão > 0)
M_H · η''(t) = −F_H(t)                 (2ª lei de Newton no martelo)
```

| Símbolo | Significado | Unidade |
|---|---|---|
| `η(t)` | deslocamento da cabeça do martelo | m |
| `y(x₀,t)` | deslocamento da corda no ponto de batida | m |
| `u = η − y` | **compressão do feltro** (só vale para u > 0) | m |
| `K` | rigidez generalizada do feltro | N/m^p (a unidade **depende de p**) |
| `p` | expoente de não-linearidade | adimensional |
| `M_H` | massa do martelo | kg |

**Valores medidos** `[FATO]` — Bank et al., EURASIP JASP 2003:10, Tabela 1, lida verbatim (origem: Chaigne & Askenfelt Parte II, que **não** foi aberta):

| nota | p | K | massa |
|---|---|---|---|
| C2 | 2,3 | 4,0 × 10⁸ N/m^2,3 | 4,9 g |
| C4 | 2,5 | 4,5 × 10⁹ N/m^2,5 | 2,97 g |
| C6 (ou C7 — as fontes divergem) | 3,0 | 1,0 × 10¹² N/m³ | 2,2 g |

**Sanidade** `[INFERÊNCIA]`, aritmética verificável: C4 com u = 1 mm → F = 4,5e9 × (1e−3)^2,5 ≈ **142 N**. Faixa medida de pico: 2–300 N (Hall & Askenfelt) e 5–310 N (Russell & Rossing, medido direto). Se sua implementação sair fora disso, o bug é de unidade — você passou `u` em milímetros.

**Contra-intuitivo e importante**: martelo **DURO tem p MENOR**. Duas evidências, com pesos diferentes — não as misture:

- `[FATO]` **Evidência própria de Russell & Rossing** (Acustica 84:967-975, 1998, lido integralmente) — é a mais forte e é a que sustenta a tese: a Fig. 5 compara **13 martelos entoados contra 6 martelos duros**, e os autores concluem que *"most of the hard hammers have lower exponent values than the voiced hammers"*. A frase que fecha o argumento é deles: *"the p exponent is not a measure of the hammer felt stiffness, but rather a measure of how much the stiffness changes with force"*.
- `[NÃO CONFIRMADO]` **Números de terceiros repassados por eles** (Russell & Rossing, Seção 2, que é REVISÃO de medições anteriores de Hall & Askenfelt — **não** resultado próprio): um conjunto de 4 martelos casados rendeu p = 2,3 no mais duro, 2,8 no macio e 3,3 num tão macio que estava arruinado. Trate como ordem de grandeza; a fonte primária (Hall & Askenfelt) não foi aberta.

Ou seja: **K é dureza absoluta, p é o RANGE DINÂMICO de timbre.**

### 2.2 Por que velocity alta = mais brilho (e por que NÃO é filtro)

Este é o ponto que separa modelo físico de sampler, e tem prova experimental limpa.

**Mecanismo** `[FATO]`: a corda recebe um pulso de força de duração τ. O espectro do pulso cai acima de ~1/τ. Modos com período menor que τ são mal excitados. Bater mais forte → contato mais curto → joelho sobe → mais parciais agudos.

**Derivação fechada** `[INFERÊNCIA]` (álgebra padrão de impacto, mas confere com a medição abaixo). Conservação de energia, massa M contra mola `F = K·u^p`:

```
½ M v² = K · u_max^(p+1) / (p+1)
  ⇒ u_max ∝ v^(2/(p+1))
  ⇒ τ     ∝ v^((1−p)/(1+p))
  ⇒ f_knee ∝ v^((p−1)/(p+1))
  ⇒ F_max ∝ v^(2p/(p+1))
```

Para p = 2,5: τ ∝ v^(−0,43), f_knee ∝ v^(+0,43).
Para **p = 1 (mola linear)**: τ ∝ v⁰ = constante → **brilho não muda com velocity**.

**Medição que confirma** `[FATO]` (Russell & Rossing, Tabela I — 13 martelos entoados de um Steinway D). Ajustaram `f_max = a · v^b`:

| tecla | a (Hz) | b | p implicado (`p = (1+b)/(1−b)`) |
|---|---|---|---|
| 1 (A0) | 763,2 | 0,41 | 2,39 |
| 18 (D2) | 746,6 | 0,36 | 2,12 |
| 37 (A3) | 755,6 | 0,46 | 2,70 |
| 49 (A4) | 827,1 | 0,46 | 2,70 |
| 73 (A6) | 1147,0 | 0,50 | 3,00 |
| 81 (F7) | 1306,9 | 0,62 | 4,26 |

**O experimento de controle (a prova definitiva)** `[FATO]`: eles testaram um martelo LINEAR de poliuretano. "the pulse duration remained essentially constant over the velocity range of 1-5 m/s" e `f_max` **não mudou nada**. A não-linearidade *é* o mecanismo.

**Interpolação para as 88 notas** `[INFERÊNCIA]` — ajuste linear sobre os 13 pontos de Russell & Rossing:
```
p(tecla) = 0,02251 · tecla + 1,9171        (tecla 1 = A0, tecla 88 = C8)
```
Dá p(C2)=2,28, p(C4)=2,82, p(C6)=3,36, p(C8)=3,90.

**Cross-validação — e o resultado é PIOR do que gostaríamos** `[INFERÊNCIA]`, aritmética refeita e conferível. Convertendo os âncoras do Bank pela relação `b = (p−1)/(p+1)` e comparando com a Tabela I do Russell & Rossing:

| nota | p (Bank, Tab. 1) | b implicado | b medido (R&R) | erro |
|---|---|---|---|---|
| C2 | 2,3 | 0,394 | 0,36 (tecla 18) | **+9,4%** |
| C4 | 2,5 | 0,429 | 0,46 (tecla 49) | **−6,8%** |
| C6 | 3,0 | 0,500 | 0,50 (tecla 73) | 0% |

E a própria reta `p(tecla)` diverge dos âncoras do Bank no meio do teclado: p(C4)=2,82 contra 2,5 medidos (**+12,7%**), p(C6)=3,36 contra 3,0 (**+11,9%**).

**Conclusão honesta**: as duas fontes concordam em **ORDEM DE GRANDEZA** e na **TENDÊNCIA** (p cresce do grave ao agudo, e o expoente do agudo é ~1,5× o do grave). Elas **NÃO** se validam numericamente — divergem de −7% a +13% em `b`, e a reta diverge até 13% no meio do teclado. **Não trate a concordância como validação.**

**Qual usar como default** `[INFERÊNCIA]`: use os **3 âncoras K/p/m do Bank** e interpole entre eles, **não** a reta do Russell. Motivo: no Bank, `K`, `p` e `m` vêm **calibrados juntos** para a mesma nota. Misturar `p` do Russell com `K` do Bank desloca o ponto de operação da não-linearidade `F = K·u^p` — e como o expoente entra num produto com `K`, um erro de 13% em `p` vira um erro grande em força de pico. A reta `p(tecla)` serve para **extrapolar a tendência** onde não há âncora, não para substituir os âncoras.

**Recomendação prática**: trate **K como o knob de voicing** que você ajusta de ouvido por região, porque é ele que posiciona `f_max` em absoluto e é ele que difere entre pianos.

**Tempos de contato absolutos** `[FATO]` (Askenfelt & Jansson, KTH, "Five Lectures on the Acoustics of the Piano"): ~4 ms no grave → menos de 1 ms no agudo extremo. Variação pp→ff: cerca de ±20% em relação a mezzo forte. Em fração do período fundamental o padrão **inverte**: ~10% do período no grave, mas **vários períodos** no agudo. É isso, sozinho, que explica por que o timbre muda tanto ao longo do teclado.

Velocidade do martelo medida: **0,6 a 6,2 m/s** (Russell & Rossing). Use para mapear MIDI velocity 1–127.

### 2.3 Histerese do feltro (Stulov) — NÃO implemente na v1

`[FATO]` Equação de Stulov, obtida verbatim de duas fontes abertas co-assinadas pelo próprio Stulov (o JASA 97(4):2577, 1995 original **não abriu** — 403):

```
R(t) = (ε/τ₀) · e^(−t/τ₀),   0 ≤ ε ≤ 1
F(t) = F₀ · [ u^p(t) − (ε/τ₀) ∫₀ᵗ u^p(ξ) · e^(−(t−ξ)/τ₀) dξ ]
```

**Atenção**: a memória age sobre `u^p`, **não** sobre `u`. Bank et al. renderizam como `k·[(1−h_r)∗y]^p` (convolução ANTES da potência) — isso diverge da formulação original.

Valores confirmados: feltro de fabricação ε = 0,96, τ₀ = 10 µs; martelos de grave τ₀ = 14 µs, ε ≈ 0,99.

**Veredito: não implemente.** Três razões, duas com fonte:
1. `[INFERÊNCIA]` τ₀ é 2–14 µs contra 1000–4000 µs de contato. A memória se extingue em ~0,5% do fenômeno.
2. `[FATO]` Bank et al. 2003, verbatim: *"informal listening tests indicate that taking into account the hysteresis in the hammer model does not improve the sound quality significantly."*
3. Quebra as provas de conservação de energia dos esquemas modernos.

Se um dia quiser, use o termo de 1 parâmetro de Chabassier, não a integral hereditária `[FATO]` (Chabassier/Chaigne/Joly JASA 134(1), 2013, Eqs. 12-13):
```
Φ(δ) = [ (ξ̄ − δ)₊ ]^p
F(t) = K^H · Φ(⟨u_s⟩ − ξ)  +  R^H · d/dt[ Φ(⟨u_s⟩ − ξ) ]
```
`R^H` é coeficiente **independente**, não um múltiplo de K. Tem prova de decaimento monotônico de energia.

### 2.4 A largura do martelo — e o bug clássico

`[FATO]` Chaigne & Askenfelt separam a força em tempo × espaço (Eq. 4): `f(x,x₀,t) = f_H(t) · g(x,x₀)`, onde `g` é uma janela espacial adimensional. Motivo declarado no paper: *"the use of such a smoothing window eliminates the artifacts that occur in the solution (in the form of strong discontinuities) when the excitation is concentrated in a single point."*

`[FATO]` Chabassier et al. afirmam explicitamente que `∫₀^L δ_H(x) dx = 1` e que *"depending on the note, the width of the contact can be adjusted from a few mm to nearly 2 cm"*.

`[INFERÊNCIA]` derivada durante a pesquisa: no esquema discreto de Chaigne o termo fonte é `Δt²·N·F_H·g(i,i₀)/M_s`. Como `N/M_s = 1/(ρ·Δx)`, a força total aplicada é `F_H · Σᵢ gᵢ`. Logo **`Σᵢ gᵢ = 1` é OBRIGATÓRIO**, senão você escala a força do martelo por um fator arbitrário — e como `F = K·u^p` é não-linear, isso desloca o ponto de operação inteiro e portanto o brilho.

O **OpenPiano erra exatamente isso** (`string_hammer.h`, linhas ~235-239): monta `hammer_win = hanning(g)` e escreve direto em `hammer_mask` sem normalizar. Uma janela de Hann de comprimento g soma ~(g−1)/2.

### 2.5 A corda: uma equação, três discretizações

`[FATO]` A PDE é a mesma nas três famílias. Forma de Bensa/Bilbao/Kronland-Martinet/Smith (JASA 114(2):1095-1107, 2003, lido integralmente, Eq. 6) — esta é a versão **bem-posta**:

```
∂²y/∂t² = c²·∂²y/∂x² − κ²·∂⁴y/∂x⁴ − 2b₁·∂y/∂t + 2b₂·∂³y/(∂x²∂t)
```

| Símbolo | Significado | Unidade |
|---|---|---|
| `c = √(T/ρ)` | velocidade de onda transversal | m/s |
| `κ_B = √(EI/μ_ρ)` | coeficiente de rigidez (gera inarmonicidade). **NÃO é o `κ_r` da §2.6/§3.2** — ver §0 | m²/s |
| `b₁` | perda independente de frequência | 1/s |
| `b₂` | perda dependente de frequência | m²/s |

**AVISO CRÍTICO** `[FATO]`: Bensa et al. provam no apêndice que *"the PDE first proposed by Ruiz, and later popularized by Chaigne and Askenfelt is in fact not well-posed, and possesses a spurious unstable solution"*. O problema é o termo de derivada **terceira no tempo**. Se você copiar a EDP do Chaigne 1994, está construindo em cima de uma equação matematicamente mal-posta que só é estabilizada pela discretização. **Use a Eq. (6) do Bensa.**

**Valores medidos** `[FATO]` (Bensa et al., Tabela I — Yamaha Disklavier C6 em câmara anecoica, acelerômetro na ponte):

| | L (m) | c (m/s) | κ (m²/s) | b₁ (1/s) | b₂ (m²/s) |
|---|---|---|---|---|---|
| C2 | 1,23 | 160,9 | 0,58 | 0,25 | 7,5e−5 |
| C4 | 0,63 | 329,6 | 1,25 | 1,10 | 2,7e−4 |
| C7 | 0,10 | 418,6 | 1,24 | 9,17 | 2,1e−3 |

**O achado mais útil de toda a pesquisa** `[FATO]` — Bensa et al. publicam duas RETAS válidas para o teclado inteiro (Eqs. 37a/37b):
```
b₁ = 4,4e−3 · f₀ − 4e−2      [1/s]
b₂ = 1,0e−6 · f₀ + 1e−5      [m²/s]
```
Verificado numericamente: reproduzem a Tabela I nas três notas com erro < 2%. **Isso te dá `τ_k` para as 88 notas inteiras com duas linhas de código**, calibrado em piano de cauda real. Note que b₁ cresce ~37× de C2 a C7: agudo morre rápido, grave sustenta. Isso não é opcional.

### 2.6 Inarmonicidade — o parâmetro nº 1

Sem isso, o modelo soa como harpa sintética.

`[FATO]` Fórmula das parciais (forma padrão; a fonte primária é Young, JASA 24(3):267-273, 1952, cujo **texto completo não foi aberto** — paywall AIP; título/autor/ano/DOI confirmados):
```
f_n = n · f₀ · √(1 + B·n²)
B = π³ · E · d⁴ / (64 · L² · T)
```
| Símbolo | Valor típico |
|---|---|
| E (Young do aço) | 2,0 × 10¹¹ Pa |
| d (diâmetro) | 0,80 a 1,10 mm |
| L (comprimento falante) | ~2,0 m (A0) a ~0,10 m (C7) |
| T (tensão) | 590 a 1780 N |

**ATENÇÃO — colisão de símbolos.** Existem DUAS formas equivalentes desta fórmula, e elas usam a letra `κ` para grandezas **diferentes**:

```
(a) notação de Bank:   B = κ_r² · (E·S/T₀) · (π/L)²      κ_r = d/4   → RAIO DE GIRAÇÃO, em METROS
(b) notação de Bensa:  B = ( κ_B · π / (c·L) )²          κ_B         → coef. de rigidez, em m²/s
```

A forma (a) é idêntica à `B = π³·E·d⁴/(64·L²·T₀)` substituindo `S = πd²/4` e `κ_r = d/4` (verificado algebricamente).
A forma (b) é a ponte para os dados do Bensa, e sai de `κ_B²/c² = EI/T₀` (verificado algebricamente e numericamente).

**Se você pegar `κ_B = 1,25` da Tabela I do Bensa e jogar na forma (a), o resultado erra por ~12 ordens de grandeza.** Use a forma (b) com dados do Bensa e a forma (a) com diâmetro de corda.

**Valores de B calculados a partir da Tabela I do Bensa pela forma (b)** `[FATO]` (aritmética verificável): C2 → **8,48e−5**; C4 → **3,58e−4**; C7 → **8,66e−3**.

**Curva medida** `[FATO]` (Rauhala & Välimäki, DAFx-06, Fig. 1, Steinway de cauda): B de ~1e−5 nos graves até ~1e−2 no agudo extremo, com um mínimo na região médio-grave. Os autores afirmam que **todos** os valores medidos ficam acima do limiar de audibilidade — inarmonicidade é audível em toda a extensão, não só no grave.

**Consequência de projeto**: se você modelar B corretamente, o *stretch tuning* (curva de Railsback) **emerge do modelo** em vez de ser um hack. Num contexto de igreja onde o piano toca junto com órgão de tubos afinado de outro jeito, isso é argumento real.

### 2.7 Posição de batida

`[FATO]` Corda ideal: batida em `α = x₀/L` excita o modo n com amplitude `∝ sin(n·π·α)`. Modos múltiplos de 1/α ficam em ZERO.
- α = 1/7 → mata 7º, 14º, 21º
- α = 1/8 → mata 8º, 16º, 24º

Os construtores escolheram 1/7 a 1/9 porque mata o 7º parcial, o mais dissonante da série baixa. É decisão de luthier, não física obrigatória.

Valor concreto: **α = 0,12** para C4 (usado por Bilbao/Torin/Chatziioannou; também é o valor que o OpenPiano usa). Faixa segura: 0,11 a 0,14.

**Por que o nulo nunca é limpo num piano real** — duas razões confirmadas, duas inferidas:
1. `[FATO]` O martelo tem LARGURA. Bilbao et al. simularam janelas de 3, 10 e 20 mm e mostram que o histórico de força muda com a largura.
2. `[FATO]` O contato dura 1–4 ms; durante esse tempo a onda já refletiu no agrafe e voltou — Bilbao mostra "secondary humps in the force history".
3. `[INFERÊNCIA]` Inarmonicidade desloca os nós.
4. `[INFERÊNCIA]` No agudo, o 8º parcial já está abaixo do joelho de 1/τ.

**Se implementar a janela `g` corretamente, o entalhe sai de graça e sai REALISTA — raso e borrado.** Como delta de Dirac sai profundo e sintético.

### 2.8 Coro de cordas, duas polarizações, batimento e decaimento duplo

Este é o ponto que a maioria das implementações amadoras erra.

**Erro comum**: achar que duas cordas desafinadas produzem decaimento duplo. **Não produzem** — produzem batimento apenas.

`[FATO]` Bank (tese 2000, Sec. 2.2.1) é explícito: *"If we assume that the strings are not tuned to the same frequency but neglect the effect of coupling, the result will be beating between the partials. In reality, these strings are not independent, since they are coupled to the bridge, giving rise to new modal frequencies."*

**Mecanismo 1 — duas polarizações de UMA corda** `[FATO]` (Weinreich, palestra KTH; o paper original JASA 62(6):1474-1484, 1977 **não foi aberto** — paywall; citação título/volume/páginas/DOI 10.1121/1.381677 confirmada):
- O martelo excita quase toda a energia na polarização VERTICAL.
- A ponte cede **muito mais facilmente** na vertical que na horizontal.
- Vertical transfere energia rápido → **soa alto e morre rápido** (som "prompt", ~8 dB/s).
- Horizontal transfere devagar → **soa baixo e sustenta** (aftersound, < 2 dB/s).
- A soma das duas exponenciais **é** o decaimento duplo.

**Mecanismo 2 — coro de 2-3 cordas** `[FATO]` (Weinreich, KTH):
- Modo simétrico (em fase): as forças na ponte SOMAM → *"the decay rate will be doubled by this coupling"* → rápido.
- Modo antissimétrico (fora de fase): *"the forces exerted by the two strings on the bridge will cancel, so the bridge will not move at all, just as though it were infinitely rigid. Hence, the decay rate of the vibration will be very slow."*

**O resultado teórico que você precisa entender antes de escrever qualquer linha** `[FATO]` (análise de Weinreich resumida em Bank, tese 2000):

- **Admitância de ponte puramente REATIVA**: os dois modos normais têm frequências SEMPRE diferentes, **mesmo com uníssono perfeito**. Afinar perfeitamente não elimina o batimento.
- **Admitância puramente RESISTIVA**, dois regimes:
  - diferença de afinação **acima** de um limiar → mesmo decaimento, frequências diferentes → **batimento puro**.
  - diferença **abaixo** do limiar → mesma frequência, decaimentos diferentes → **decaimento duplo sem batimento**.

**Batimento e decaimento duplo são dois regimes do MESMO fenômeno**, e o afinador de piano navega entre eles ajustando o uníssono. É por isso que "afinar o uníssono" é arte e não medição.

**Desafinação de uníssono — valores** `[NÃO CONFIRMADO]`: não foi encontrada fonte primária revisada por pares com distribuição medida em cents. O que existe é material de afinadores (exame PTG RPT: uníssono abaixo de ~0,9 cent pontua 100%). **Ordem de grandeza útil: 0,1 a 1,0 cent**, crescendo levemente nos agudos. Meça um piano real antes de fixar.

### 2.9 Cavalete e tampo — o truque que torna tudo tratável

**A decisão arquitetural que define o projeto inteiro** `[FATO]` — Bank/Zambon/Fontana, TASLP 2010, Sec. II, verbatim:

> *"since the impedance of the soundboard is around a thousand times larger than that of the string, the only significant effect of the string-soundboard coupling is a change in the modal frequencies and decay times of the string. Therefore, it is customary to use a simplified modeling where the impedance effects of the coupling ... are taken into account by changing the parameters of the string block, and only the radiation properties are modeled in the radiator block."*

Consequência prática, e é enorme:
- O tampo vira **filtro FEEDFORWARD (LTI)** na saída. Sem loop. Sem problema de estabilidade. Sem delay-free loop.
- Tudo que o tampo "faz" na corda (frequências deslocadas, decaimento dependente de frequência) você **programa nos `τ_k` dos ressoadores**.

Isso é o oposto do que a intuição física sugere, e é o motivo de o modelo caber em tempo real.

**Impedância do cavalete — números reais** `[FATO]` (Ege & Boutillon, ICA 2010 / arXiv:1210.5688, compilando Wogram 1980, Conklin 1996, Giordano 1998):
- Abaixo do degrau (~700 Hz a 1 kHz): **|Z| ~ 10³ kg/s**.
- Acima: cai para **200–300 kg/s** (impedância característica da placa infinita para flexão).
- A queda é um **DEGRAU**, não rampa.
- No cavalete, |Z| é 2–3× a do tampo nu.

`[FATO]` Contra-intuitivo (Conklin 1996): no agudo, a mobilidade **longitudinal** no cavalete pode ser 10–20 dB MAIOR que a normal ao tampo. O caminho dominante de energia no agudo pode não ser o que você está modelando.

**Fechamento numérico do "truque da impedância"** `[INFERÊNCIA]` — derivação feita durante a pesquisa, mas com números de fontes publicadas, e o resultado é forte:

```
α_acoplamento [1/s] ≈ 2·f₀·Z_corda / |Z_ponte(f)|
```
Corda C4 de aço d=1,0 mm: μ = 6,17 g/m, c = 329,6 m/s → Z_corda = μ·c = **2,03 kg/s**.

| |Z_ponte| | α resultante | T60 |
|---|---|---|
| 500 kg/s | 2,13 1/s | 3,2 s |
| **1000 kg/s** | **1,06 1/s** | 6,5 s |
| 2000 kg/s | 0,53 1/s | 13,0 s |

O `b₁` **medido** de C4 (Bensa 2003) é **1,1 1/s**. Com |Z_b| = 1000 kg/s dá 1,06. **Bate praticamente exato.**

Isso significa que você pode gerar `τ_k` para as 88 notas escolhendo uma curva |Z_b(f)| plausível, e **um único parâmetro global** (o nível de |Z_b|) vira o knob "quanto o piano sustenta".

**Física medida do tampo, para parametrizar sem ter um piano** `[FATO]` (Boutillon & Ege, arXiv:1305.3057 / JSV, piano vertical em condição de tocar):
- painel de abeto: espessura 8 ± 2 mm; espaçamento entre costelas 10 a 18 cm (irregular de propósito); ângulo de ortotropia −32,5°.
- **densidade modal ~0,02 modos/Hz → ~200 modos em 0–10 kHz.**
- **fator de perda modal η ≈ 2% ± 1%**, aproximadamente constante ao longo de vários kHz.
- **f_lim ≈ 1,1 kHz**: abaixo, os modos se estendem pelo tampo todo; acima, ficam **localizados** entre costelas (localização de Anderson por irregularidade do espaçamento).
- Fórmula de Skudrzyk (média, forma fechada): `Y_c = n(f)/(4·M)`. No cavalete as **impedâncias** de viga e placa SOMAM.

Conversão para o seu ressoador: `α = π·f·η` ⇒ `τ = 1/(π·f·η)`. Com η=0,023 e f=500 Hz: τ = 27,7 ms, T60 = 191 ms, Q ≈ 43.

**Cross-check de sanidade**: 200 modos medidos vs 100 seções de 2ª ordem (ordem 200) que o Bank usa no filtro paralelo. **Mesma ordem de grandeza.** As duas abordagens convergem.

Detalhe importante `[FATO]`: o espaçamento dos modos NÃO é regular. Use `f_{ν+1} = f_ν + r/n(f_ν)` com r ~ Rayleigh de média 1. Um banco de polos perfeitamente log soa "regular demais".

### 2.10 Abafadores e pedais

`[FATO]` Timing mecânico (Askenfelt & Jansson, KTH):
- Damper levantado **~15 ms ANTES** do martelo bater.
- Damper cai sobre a corda **~80 ms depois** do contato (nota curta), e *"brings the string to rest after some bouncing"* — **o damper QUICA**.
- Velocidade da tecla: 0,3–0,5 m/s em mezzo forte; raramente > 1 m/s.
- Velocidade do martelo: ~5× a da tecla.
- Escape (let-off) 1–3 mm antes da corda.
- Ruídos parasitas reais do ataque: "backwash" ~50 Hz (toque suave), "ripple" ~400 Hz (toque vigoroso).

Um damper implementado como decaimento instantâneo **soa errado**.

`[FATO]` As notas mais agudas do piano **não têm damper nenhum** — estão sempre vibrando por simpatia.

**Achado contra-intuitivo do pedal de sustain** `[FATO]` (Lehtonen, Penttinen, Rauhala & Välimäki, JASA 122(3):1787-1797, 2007 — citação e achado principal confirmados via resumo; **texto completo não aberto**): com o pedal levantado, as **taxas de decaimento dos parciais AUMENTAM** (o som decai mais rápido), especialmente na região MÉDIA. No grave e no agudo o efeito não aparece. Também mudam as características de batimento e aumenta a energia do sinal residual.

**Por quê**: a corda tocada ganha caminhos extras de fuga de energia — ela bombeia energia para as ~230 cordas livres via cavalete. O que o ouvinte percebe como "mais rico e mais longo" é: **parcial individual decaindo mais rápido + uma nuvem de ressonância simpática por cima**. Se você só aumentar o sustain da nota tocada, soa errado.

`[FATO]` **Meio-pedal é problema EM ABERTO na academia.** Zambon et al. (DAFx-08) e Bank et al. (TASLP 2010) listam explicitamente como trabalho futuro. Se você resolver bem, está à frente do que está publicado.

### 2.11 Duplex scale

`[FATO]` (Bank & Chabassier via citação; e Zambon et al. EUSIPCO 2015): cada corda tem três segmentos. O falante (agrafe → cavalete) é o que você toca. Os outros dois (front duplex e rear duplex) são curtos, portanto ressoam alto. Quando o fabricante afina esses segmentos em razões harmônicas, chama-se aliquot.

Na prática industrial, o Viscount Physis tem um bloco **explícito e separado** de "duplex resonators" — mais um banco de 2ª ordem em paralelo, alimentado pela força de cavalete, sem feedback.

`[INFERÊNCIA]` **Duplex é o item de melhor relação custo/benefício da lista toda.** Poucos ressoadores de alta frequência por nota, sem acoplamento, e produzem exatamente aquele brilho metálico que separa piano modelado bom de ruim no agudo.

### 2.12 Vibração longitudinal e phantom partials

`[FATO]` (Bank et al. TASLP 2010, Sec. V): em fortissimo a tensão deixa de ser constante e a vibração transversal excita modos longitudinais.
```
f_ξ,k = k · f_ξ,0 = (k/2L)·√(E·S/μ)
```
`[FATO]` Bank et al. dão duas afirmações que **não fecham entre si**, e o documento reproduz as duas de propósito:
- a razão `f_ξ,0 / f₀` é *"around 16 to 20 times higher"* (regra de ordem de grandeza, atribuída a Conklin 1996);
- mas no exemplo simulado de **G1** deles (~49 Hz) a 1ª longitudinal cai em **~690 Hz** — o que é **~14×**, fora da faixa 16–20.

**Não dimensione o banco longitudinal pelo multiplicador.** Se você aplicar 16–20× ao G1 não reproduz os 690 Hz do próprio exemplo. Calibre **por nota**, a partir de `f_ξ,0 = (1/2L)·√(E·S/μ_ρ)`, com os `L` e `T₀` reais da sua tabela. Use 16–20× só como sanidade grosseira. É o "metálico" característico do grave.

**Regra de ouro do acoplamento**: um modo longitudinal k só é excitado por PARES de modos transversais m,n com `m+n = k` ou `|m−n| = k`. Modos longitudinais ímpares são excitados por espectro ímpar; pares por espectro par.

**Anti-aliasing é trivial e só existe no modal** `[FATO]`: como você CONHECE `f_n` de cada modo, basta não computar nenhum produto cuja frequência-soma passe de fs/2. Simplificação aceitável do paper: limitar aos modos transversais com `f_N < fs/4`. **Num FDTD você não tem essa saída.** Este é o argumento técnico mais forte a favor do modal.

---

## 3. AS TRÊS FAMÍLIAS DE ALGORITMO

As três resolvem a MESMA PDE. A escolha é só COMO discretizar.

### 3.1 WAVEGUIDE — discretiza a solução de onda viajante

`[FATO]` Estrutura (Smith, CMJ 16(4), 1992): duas linhas de atraso num laço de realimentação, com três filtros concentrados:
- `H_loss(z)`: 1-polo, perda dependente de frequência
- `H_disp(z)`: cascata de allpass, dispersão/rigidez
- `H_frac(z)`: allpass de 1ª ordem ou Thiran, afinação fracionária

**Projeto do filtro de dispersão — forma fechada usável direto** `[FATO]` (**ref. 11a** — Rauhala & Välimäki, *"Dispersion Modeling in Waveguide Piano Synthesis Using Tunable Allpass Filters"*, Proc. DAFx-06, Montréal, pp. 71-76, Eqs. 3-8).

> **CUIDADO AO PROCURAR A FONTE**: existem **dois** papers de 2006 dos mesmos autores com títulos quase iguais. As constantes abaixo são do paper do **DAFx-06** (ref. 11a), que usa cascata de allpass de **1ª ordem**. O outro — *"Tunable dispersion filter design for piano synthesis"*, **IEEE Signal Processing Letters 13(5):253-256, 2006** (ref. 11b) — é o método original e usa cascata de **2ª ordem**, com uma tabela de constantes **diferente**. Se você abrir o SPL e comparar, vai achar que as constantes deste documento estão erradas. **Não estão — vêm do outro paper.**
```
I_key(f₀) = 12 · log₂(f₀ / 27.5)                       [A0 = tecla 0]
k_d(B)    = exp( k1·(ln B)² + k2·ln B + k3 )
C_d(B,M)  = exp( (m1·ln M + m2)·ln B + m3·ln M + m4 )
D         = exp( C_d − I_key · k_d )                   [atraso alvo em DC]
a1        = (1 − D) / (D + 1)                          [coef. do allpass de 1ª ordem]
H_ap(z)   = (a1 + z⁻¹) / (1 + a1·z⁻¹)
```
Constantes publicadas (Tabela 2, cascata de 1ª ordem):
```
k1 = −0,00179   k2 = −0,0233   k3 = −2,93
m1 =  0,0126    m2 =  0,0606   m3 = −0,00825   m4 = 1,97
```
(O alinhamento da tabela no PDF está deslocado uma linha; foi reconstruído e **validado numericamente** contra a Fig. 2 do próprio paper.)

Quantas seções: M abaixo de 20 por razões computacionais; o exemplo de piano do paper usa **M = 8**. Erro de frequência: M=1 passa de −25 cents; M=8-10 fica em −5 a −10 cents.

**Armadilha**: o filtro de dispersão ADICIONA atraso ao laço. Você tem que subtrair do delay line. E o paper avisa que `D·M` não é estimativa suficiente para M grande — **meça o group delay de verdade**.

**Custo** `[INFERÊNCIA]`, contando ops: ~25 ops/amostra/corda com M=4, ~41 com M=8. Piano inteiro (88 × 3 cordas × 2 polarizações = 528 modelos, M=8): **~0,8 GFLOP/s**. É de 10× a 50× mais barato que modal ou FDTD. Custo **independente da nota**.

**O DEFEITO QUE MATA** `[FATO]` (Bank/Zambon/Fontana, TASLP 2010, Sec. III, verbatim):

> *"precise modeling is not possible with digital waveguides in their efficient form, because the modal shapes of the string model become non-orthogonal due to the lumped dispersion filter."*

Ou você distribui o filtro de dispersão (e perdeu a vantagem de custo), ou usa truques não-físicos.

**Tradução prática**: waveguide te dá um piano bonito em pianissimo e mezzo-forte, e um piano que **não "abre" no fortissimo**. Foi exatamente por isso que Bank abandonou waveguide.

`[FATO]` Segunda armadilha: allpass de ordem alta é numericamente frágil. Abel & Smith (DAFx-06) relatam que métodos clássicos falham acima de ordem ~10-20 por round-off.

### 3.2 MODAL — discretiza a solução de modos ← **RECOMENDADO**

`[FATO]` Bank/Zambon/Fontana, TASLP 18(4):809-821, 2010, Secs. III-A e III-B (lido integralmente). Esta é a derivação mais limpa e diretamente implementável que existe.

**Do contínuo ao biquad, completo:**

PDE (Eq. 1):
```
μ·∂²y/∂t² = T₀·∂²y/∂x² − E·S·κ²·∂⁴y/∂x⁴ − 2R·μ·∂y/∂t + d_y(x,t)
```

Expansão modal (Eq. 2), corda simplesmente apoiada: `y(x,t) = Σ_k y_k(t)·sin(kπx/L)`

EDO por modo (Eqs. 3-4):
```
ÿ_k + a1_k·ẏ_k + a0_k·y_k = b0_k · F_y,k(t)
a1_k = 2·R_k
a0_k = (T₀/μ)·(kπ/L)² + (E·S·κ²/μ)·(kπ/L)⁴
b0_k = 2/(L·μ)
```

Resposta impulsiva (Eq. 5) — **senoide amortecida**:
```
θ_k(t) = A_k · e^(−t/τ_k) · sin(2π·f_k·t)
A_k   = 1/(π·L·μ·f_k)
τ_k   = 1/R_k                    [decaimento de AMPLITUDE; T60 = 6,91·τ_k]
f_k   ≈ f₀·k·√(1 + B·k²)
B     = π²·E·S·κ²/(T₀·L²)
```

Perdas (Eq. 7):
```
1/τ_k = R_k = b₁ + b₃·(2π·f_k)²
```
`[NOTA DE HONESTIDADE]` A Eq. (7) sai **ambígua** na extração de texto do PDF — não foi possível resolver se há um expoente 2 perdido. A forma **quadrática** acima é a correta, por três razões: (i) o texto chama de "the second-order function"; (ii) o texto diz que implementa as mesmas perdas do FD do Chaigne, e a Eq. (3) do Chaigne 1994 (aberta e verificada) é `σ = b₁ + b₃·ω²`; (iii) Bensa Eq. (39a) dá a mesma forma. Confira na versão IEEE antes de implementar.

**A DISCRETIZAÇÃO — e esta é a decisão mais importante do paper inteiro** (Eqs. 10-11). Use **INVARIÂNCIA AO IMPULSO**, não bilinear:
```
p_k   = exp(−1/(τ_k·fs)) · exp(j·2π·f_k/fs)
a1_k  = −2·Re{p_k}
a2_k  = |p_k|² = exp(−2/(τ_k·fs))
b_k   = (A_k/fs)·Im{p_k}

H_res,k(z) = b_k·z⁻¹ / (1 + a1_k·z⁻¹ + a2_k·z⁻²)
```
Laço de áudio:
```c
y = b*x1 - a1*y1 - a2*y2;  y2 = y1; y1 = y; x1 = x;
```
Exatamente **3 multiplicações** quando x ≠ 0, **2** quando x = 0 (corda livre).

**POR QUE INVARIÂNCIA AO IMPULSO E NÃO BILINEAR** `[FATO]`: a invariância ao impulso amostra `h(t) = A·e^(−t/τ)·sin(2πft)`, que vale **0 em t=0**. Esse zero inicial é o `z⁻¹` no numerador, e **é ele que elimina o delay-free loop com o martelo**. Com bilinear você perde esse zero e é obrigado a usar `y_s[n−1]`, o que o próprio paper diz que causa *"numerical instabilities at high impact velocities"*. Escreva isso num comentário no código — é a diferença entre um motor que aguenta um fortissimo ao vivo e um que explode.

O fator `1/fs` existe porque o impulso unitário discreto tem área 1/fs e o Dirac tem área 1.

**Pesos** (Eqs. 14-16):
```
w_in,k  = sin(k·π·x_h/L)      (força do martelo → modo k)
w_out,k = sin(k·π·x_h/L)      (modos → deslocamento no ponto do martelo — idêntico)
w_b,k   = k·π·T₀/L            (modos → força na ponte)
F_b(t) = (π·T₀/L)·Σ_k k·y_k(t)
```

**OTIMIZAÇÃO OBRIGATÓRIA** `[FATO]`: por linearidade, junte `w_in`, `w_out`, `w_b` e `b_k` de forma que **`w_b,k = 1`**. Assim a força na ponte vira **soma pura, zero multiplicações** — e é o caminho quente do laço. Os outros dois coeficientes só rodam durante o contato do martelo (poucos ms). Os `z⁻¹` também se fundem: **UM elemento de atraso por corda**, não por modo.

**Martelo** (Eqs. 12-13):
```
F_h = K_h · (δy)^P_h   se δy > 0, senão 0;    δy = y_h − y_s
y_h(z) = −F_h(z)·(1/m_h)·(1/fs · 1/(1−z⁻¹))²·z⁻¹
```
Em código: `a_h = −F_h/m_h;  v_h += a_h/fs;  y_h += v_h/fs`. **Nada de Newton-Raphson.**

**Custo** `[FATO]` (Tabela I do paper): 3 MPOS por ressoador com entrada, 2 em evolução livre.

**Contagem de modos por nota** `[INFERÊNCIA]` (contando parciais abaixo de Nyquist com `f_k = k·f₀·√(1+B·k²)`, fs = 44,1 kHz).

**Esta tabela é reprodutível — o B usado em cada linha está publicado junto.** (A versão anterior deste documento imprimia números que não saíam de nenhum B declarado; foram refeitos.)

| nota | f₀ | B usado | origem do B | modos |
|---|---|---|---|---|
| A0 | 27,5 Hz | *não há B publicado para A0* | — | **não auditável — ver nota** |
| C2 | 65,4 Hz | 8,48e−5 | Bensa Tab. I, forma (b) da §2.6 | **176** |
| C4 | 261,6 Hz | 3,58e−4 | Bensa Tab. I, forma (b) | **57** |
| C6 | 1046 Hz | ~2e−3 | interpolado C4↔C7 | ~16 |
| C7 | 2093 Hz | 8,66e−3 | Bensa Tab. I, forma (b) | **8** |
| C8 | 4186 Hz | ~2e−2 | extrapolado | ~4 |

`[NÃO CONFIRMADO]` **A linha do A0 é o furo desta tabela, e é justamente a nota que dimensiona o orçamento inteiro.** Não existe `B` medido para A0 em nenhuma fonte aberta desta pesquisa — a Tabela I do Bensa começa em C2. Uma versão anterior deste documento afirmava **339 modos** para A0; esse número **não pode ser conferido** e não deve ser usado para dimensionar nada.

**Como fechar isso quando precisar** `[INFERÊNCIA]`: gere a tabela inteira a partir de `B = (κ_B·π/(c·L))²` com `c(f₀) = 2·L·f₀`, ancorando `L(nota)` e `κ_B(nota)` nos 3 pontos do Bensa. Aí a tabela vira reprodutível e o A0 fica auditável.

**Mas na prática isso quase não importa**, porque o teto de modos manda (ver logo abaixo): `[FATO]` Bank limita o grave a **~130-140 modos** (Fig. 7, eixo até 140 — confirmado no PDF do TASLP). **É esse teto, não a contagem teórica, que entra no orçamento de CPU.**

Mas `[FATO]` Bank **limita** o número de parciais (Fig. 7): *"The number of resonators is derived by imposing an upper limit on their frequency and an additional absolute limit on the number of resonators."* Teto em ~130-140 no grave. **Faça o mesmo** — economiza 5,7× na nota mais cara.

**Estabilidade**: `|polo| = exp(−1/(τ_k·fs)) < 1` sempre. Verificação de custo zero, feita uma vez por nota no design de parâmetro. Sem CFL, sem dispersão numérica, sem passo de tempo a escolher. **Isso sozinho já é um argumento forte a favor de modal para um dev solo.**

### 3.3 FDTD — discretiza a PDE direto

`[FATO]` Esquema explícito de Bensa et al. (Eqs. 17-18), com `λ = c·T/X` e `μ = κ·T/X²`:
```
y[m][n+1] = a10·y[m][n] + a11·(y[m+1][n] + y[m−1][n]) + a12·(y[m+2][n] + y[m−2][n])
          + a20·y[m][n−1] + a21·(y[m+1][n−1] + y[m−1][n−1])

a10 = (2 − 2λ² − 6μ² − 4b₂μ/κ)/(1 + b₁T)
a11 = (λ² + 4μ² + 2b₂μ/κ)/(1 + b₁T)
a12 = (−μ²)/(1 + b₁T)
a20 = (−1 + 4b₂μ/κ + b₁T)/(1 + b₁T)
a21 = (−2b₂μ/κ)/(1 + b₁T)
```

**Estabilidade (Von Neumann, necessária E suficiente, exata)**:
```
λ² + 4μ² + 4·b₂·μ/κ ≤ 1
```

**Pontos de grade** `[INFERÊNCIA]`, calculados resolvendo a condição acima com os parâmetros medidos do Bensa:

| | fs = 44,1 kHz | fs = 96 kHz |
|---|---|---|
| C2 | X=5,81 mm, **N=212** | N=334 |
| C4 | X=9,55 mm, **N=66** | N=110 |
| C7 | X=10,8 mm, **N=9** | N=16 |

**É viável em tempo real?** Sim para corda linear, não para não-linear iterativa.

`[FATO]` Bilbao, Ducceschi & Webb, DAFx-19, Tabelas 1-3 (Xeon E5-1650 v2, 1 thread, double, sem multithread):

| caso | tempo de CPU por 1 s de áudio |
|---|---|
| 25 cordas rígidas LINEARES (2.550 pontos), −O0 | 0,59 s |
| idem, −O3 com SSE | 0,18 s |
| idem, **intrinsics AVX manual** | **0,13 s** ← 13% de um core |
| 25 cordas + 24 conexões NÃO-LINEARES, Newton-Raphson (10 iter) | **1,84 s** ← não cabe |
| idem, solver não-iterativo novo | **0,28 s** ← speedup 6,6× |

Diagnóstico dos próprios autores: a diferença vem quase toda de chamadas a `std::pow()` com expoente não-inteiro — 22 chamadas no iterativo contra 2 no não-iterativo.

`[INFERÊNCIA]` Extrapolação para piano completo: 88 notas × 1 corda ≈ 9.300 pontos ≈ 3,7× os 2.550 medidos → **~48% de um core**. Com 3 cordas × 2 polarizações → ~3 cores dedicados no pior caso. Caro mas não impossível num desktop. **Em Android/iOS, esqueça.**

**Realidade comercial** `[FATO]`: a Physical Audio (Craig Webb, co-autor do NESS) vende plugins FDTD em tempo real. O Preparation 2 — cordas não-lineares com rattle — declara polifonia de **7 VOZES**. Esse é o teto prático honesto de FDTD não-linear em 2026.

**O solver não-iterativo por quadratização de energia** `[FATO]` (Ducceschi & Bilbao, DAFx-19 companion; Ducceschi/Bilbao/Willemsen/Serafin, JASA 149(5):3502, 2021). Vale conhecer mesmo indo de modal:

```
Φ(η) = K/(α+1)·[η]₊^(α+1)          (potencial de colisão, sempre ≥ 0)
ψ = √(2Φ)                           (quadratização)
g = dψ/dη = Φ'(η)/√(2Φ(η))
```
Para o caso potência, resolvido em forma fechada durante a pesquisa `[INFERÊNCIA]`:
```
ψ(η) = √(2K/(α+1)) · [η]₊^((α+1)/2)
g(η) = √(K(α+1)/2) · [η]₊^((α−1)/2)
```
Com α ≈ 2,3 a 3,0 (piano), o expoente fica entre 0,65 e 1,0 → g → 0 suavemente na entrada do contato, **sem divisão por zero**.

Atualização por **uma divisão**, sem iteração:
```
A = M/k² + (g^n)²/4
v = M(2u^n − u^{n−1})/k² − M·ω₀²·u^n + ((g^n)²/4)·u^{n−1} − ψ^{n−1/2}·g^n
u^{n+1} = v/A
```

Custo **FIXO** por amostra. Citação literal do paper: *"while the computational cost of N-IT remains fixed. This is an important aspect in view of any real-time implementation requiring a precise allocation of computing resources."*

**Dois safeguards obrigatórios que o paper exige**:
- A forma ingênua do gradiente produz *"spurious oscillations"*. Use a razão incremental (Eq. 27).
- Cheque `g^n · ψ^{n−1} < 4Φ^{n−1/2}`. Se violar, **ZERE g^n**. Sem isso o martelo pode PUXAR a corda — feltro não puxa.

**Caveat honesto que o paper faz** `[FATO]`: *"for values of K such that √(K/M)·k > 1, the schemes become first-order accurate"*. Com K = 1e12 (C6) e M = 2,2 g a 48 kHz, √(K/M)·k = **437 >> 1**. Na faixa de parâmetros do piano real **você está em primeira ordem**. O esquema não explode — ele **mente**: com barreira dura, *"the excess kinetic energy is converted to potential energy ... reflected in lower-than-expected collision output velocities"*. O martelo sai da corda mais devagar do que deveria.

**Se você for de MODAL, não precisa disso.** O martelo de Bank já é explícito porque a não-linearidade é estática e o `z⁻¹` do ressoador quebra o laço.

### 3.4 TABELA DE DECISÃO

|  | WAVEGUIDE | **MODAL** | FDTD |
|---|---|---|---|
| Custo/unidade | ~25-41 ops/amostra/corda (constante) | 3 mult/modo (2 livre) | ~16 flops/ponto |
| Piano completo | ~0,8 GFLOP/s | ~2,4-7 GFLOP/s | ~7 a 40 GFLOP/s |
| Benchmark real | nenhum moderno encontrado | **10.000 ressoadores + 4 conv 20k taps = 30% de um Core 2 Duo 2,4 GHz (2010)** | 25 cordas lineares = 0,13 s/s; não-linear = 7 vozes comerciais |
| Parâmetros/nota | f₀, B, filtro de perda, M allpass, delay | **tabela (f_k, τ_k, A_k) — extraível de gravações** | 7 físicos {ρ,E,r,T,σ₀,σ₁,L} |
| Não-linearidade | **impossível na forma eficiente** | trivial, anti-aliasing por índice | possível, aliasing sem solução trivial |
| SIMD | ruim (delays de tamanhos diferentes) | **trivial (SoA, sem gather)** | médio (stencil) |
| Estabilidade | allpass frágil em ordem alta | `|polo| < 1`, trivial | CFL + dispersão, delicado |
| Dificuldade | MÉDIA | MÉDIA-ALTA | ALTA |

**Escolha: MODAL.** É o único ponto do espaço com custo aceitável **E** controle de parâmetro por parcial **E** não-linearidade com anti-aliasing **E** o único com benchmark público de piano completo em tempo real.

---

## 4. O PIANOTEQ POR DENTRO — fato vs. marketing

### 4.1 O que se SABE (e a fonte de cada coisa)

Três fontes duras: a **patente US7915515B2** (Modartt/Philippe Guillaume, prioridade 28/07/2006), o **manual do produto**, e uma **entrevista de 2010** (cdm.link, 31/05/2010) onde Guillaume descreve o método.

`[FATO]` **A arquitetura NÃO é waveguide nem diferenças finitas.** É pré-síntese offline + síntese aditiva em tempo real. A claim 1 define DOIS módulos:

**(1) PRESYNTHESIS MODULE (não-tempo-real).** Verbatim da patente:
> *"The timbre coefficients are determined by the presynthesis module by a process that is not a real-time process and that does not therefore affect the effectiveness and the performance of the real-time module for production of sounds."*

**(2) REAL-TIME PRODUCTION MODULE.** Verbatim:
> *"This real-time process for production of the sounds ... generates the audio signals by additive synthesis according to the aforementioned formulae (1), (2) and (3), i.e. by cumulating the partials."*

### 4.2 As 6 fórmulas da patente — inventário completo

`[FATO]` A patente tem **somente** as fórmulas (1) a (6). Confirmado no texto integral (FreePatentsOnline); não existem (7)-(10).

```
(1)  s(p,t) = Σ_n a_n(p)·exp(−d_n(p)·t)·sin(2π·f_n(p)·t + θ_n(p)) + b(p,t)
(2)  s(p,t) = real( Σ_n α_n(p)·exp(2πi·f_n(p)·t − d_n(p)·t) ) + b(p,t)
(3)  α_n(p) = −i·a_n(p)·exp(i·θ_n(p))
(4)  T2 = ε_p·T1
(5)  T3 = (2 − ε_p)·T1
(6)  u(x,t) = Re( Σ_n α_n·exp(2πi(f_n + i·d_n)·t) )
```

| Símbolo | Significado |
|---|---|
| `p` | sinal de disparo (pitch, e possivelmente velocity e duração) |
| `f_n` | frequência do parcial [Hz] — "timbre coefficient", vem da pré-síntese |
| `d_n` | amortecimento [1/s] — "timbre coefficient", vem da pré-síntese |
| `a_n` | amplitude inicial "directly after the impact of the hammer" — "excitation parameter" |
| `θ_n` | fase inicial [rad] — "excitation parameter" |
| `b(p,t)` | *"the percussive part of the sound and any other component of the sound that cannot be modelled ... by a decomposition into a sum of sines"* |
| `ε_p` | tuning factor, *"a positive real number less than unity"* |

**ARMADILHA DE UNIDADES** `[INFERÊNCIA]` — isto vai te queimar se transcrever direto: a fórmula (1) diz `exp(−d_n·t)`. A fórmula (6) expandida dá `exp(−2π·d_n·t)` — decaimento 2π vezes mais rápido. MAS o texto corrido escreve o autovalor como `(f_np + i·d_np/2π)`, que expandido dá `exp(−d_np·t)`, consistente com (1).

**A fórmula (6) como impressa OMITE o /2π.** Convenção correta e autoconsistente:
```
λ_n = f_n + i·d_n/(2π)   [Hz]  ⇒  d_n = 2π·Im(λ_n) [1/s]  ⇒  envelope = exp(−d_n·t)
```
Conversões que você vai usar:
```
τ_n   = 1/d_n
T60_n = 6,9078/d_n
B_n   = d_n/π            (largura de banda −3 dB, Hz)
R_n   = exp(−d_n/fs)     (raio do polo por amostra)
```
A relação `R = exp(−π·B/fs)` confirma `d_n = π·B_n` — bate com Julius Smith, *Physical Audio Signal Processing*, seção Modal Expansion.

### 4.3 O laço de áudio real — por que a patente escreve a fórmula (2)

`[INFERÊNCIA]` A fórmula (2) não é enfeite matemático. Ela existe porque `exp(2πi f t − d t)` amostrado vira **progressão geométrica complexa** = UMA multiplicação complexa por amostra.

Pré-cálculo por parcial (no note-on):
```
r_n = exp(−d_n/fs) · [cos(2π f_n/fs) + i·sin(2π f_n/fs)]     // |r_n| < 1
z_n = α_n = −i·a_n·exp(i·θ_n)                                 // fórmula (3)
```
Laço por amostra:
```
acc = 0
for n in 0..N-1:
    z_n = z_n * r_n        // 4 mult reais + 2 somas
    acc += z_n.re          // 1 soma
out[k] = acc + b_sample[k]
```
**~7 flops por parcial por amostra.** Nenhuma chamada a sin/cos, nenhuma wavetable, nenhum acesso a disco.

### 4.3.1 REQUISITO DE PRECISÃO — não é curiosidade, é requisito de implementação

`[INFERÊNCIA]` Para decaimentos longos, `d_n/fs` fica minúsculo. τ = 20 s a 48 kHz ⇒ `|r| = 0,99999896`. Em float32 (eps ≈ 1,2e−7) você está a **menos de uma ordem de grandeza do epsilon da máquina**. O erro de arredondamento em `|r|` vira erro direto em τ, e o erro em `θ` vira **desafinação**.

**Isto vale para o SEU motor, não só para o Pianoteq.** É por isso que está marcado como requisito e repetido na Etapa 1, na Etapa 4 e no cartão do apêndice:

> **REGRA**: todo ressoador com **τ acima de ~2 s** — ou seja, o grave inteiro e todo o banco simpático com pedal embaixo — usa **coeficientes E estado em `double`**, ou renormalização periódica de `|z|`. Em float32 só o agudo de decaimento curto é seguro.

**Por que isso é traiçoeiro**: o erro **não aparece no teste de bancada**. Um impulso de 1 segundo passa. O sintoma aparece depois de **minutos de sustentação ao vivo**, na forma de afinação e decaimento errados no grave — exatamente o cenário "pedal embaixo durante o louvor inteiro".

**Teste de bancada obrigatório**: 60 segundos de sustentação contínua de uma nota grave, medindo **desvio de frequência** e **desvio de τ** entre o início e o fim. Se a frequência andar, você está em precisão insuficiente.

### 4.4 O achado mais desconfortável: o martelo é MEDIDO, não simulado

`[FATO]` Citação verbatim da patente: os sinais de excitação *"can be measured directly on a piano ... by using an automatic ... mechanical device"* e são *"recorded in backup files"*.

O fluxo: para cada nota p e cada intensidade I, grava-se `E_pI(t)` num piano real com dispositivo mecânico automático. A pré-síntese projeta esse sinal sobre os modos e extrai `α_n = a_n·e^(iθ_n)` por parcial. Resultado: **TABELAS indexadas por (nota, intensidade, parcial)**.

Em tempo real o módulo apenas *"selects excitation parameters closest to received velocity"* — **vizinho mais próximo, sem interpolação mencionada**.

`[FATO]` E o `b(p,t)` é **SAMPLE**. Verbatim:
> *"A percussive sound b(p,t) is added to the sum of the partials. The same prerecorded sound can be combined with each of the summed signals ... Preferably a plurality of noises b(p,t) of percussion are recorded for various notes p."*

**Isso explica por que o Pianoteq tem footprint de ~50 MB e não de 50 GB sem ser "100% síntese": os samples são só os transientes percussivos, não a cauda tonal.**

**Para você isso é boa notícia arquitetural**: você JÁ tem um motor de samples maduro. O `b(p,t)` reaproveita infraestrutura existente. São samples de dezenas de ms — cabe tudo em RAM, nem precisa de streaming.

### 4.5 A pré-síntese: problema de autovalores NÃO-LINEAR

`[FATO]` A patente cita explicitamente UMA referência matemática, e é do próprio inventor:
> PH. GUILLAUME, *"Nonlinear eigenproblems"*, SIAM J. Matrix Anal. Appl., Vol. 20, No. 3 (1999), pp. 575-595.

(A referência foi **confirmada** na página de publicações de Guillaume em modartt.com. O paper em si **não foi lido** — paywall SIAM.)

**Por que NÃO-LINEAR**: porque o amortecimento depende da frequência. Quando as matrizes dependem do próprio autovalor λ, você cai num NEP. É exatamente a especialidade de pesquisa do Guillaume — **e é por isso que ele, e não um acústico, é o inventor.** A escolha de modal em vez de waveguide não foi acidente estético: foi um matemático aplicando a ferramenta que ele dominava.

Modelo mecânico da patente:
- cada corda é uma **VIGA ELÁSTICA** com rigidez à flexão (daí a inarmonicidade)
- cordas engastadas nos pontos de fixação e no capotasto
- cordas ligadas rigidamente ao tampo nos pontos do cavalete
- tampo em elementos finitos de **CASCA (shell)**, isotrópico ou anisotrópico

**ARMADILHA DE TRADUÇÃO** `[INFERÊNCIA]`: a patente diz *"wippen"* onde deveria dizer **CAVALETE (bridge)**. O texto é tradução de um depósito francês; o original é "chevalet". Se ler literalmente, você vai tentar acoplar a corda à ação do teclado, o que é fisicamente absurdo.

### 4.6 Como os knobs "físicos" funcionam sem rodar FEM

`[FATO]` Este é o insight de engenharia mais valioso da patente:
1. Escolhe-se um vetor de parâmetros físicos (`Z_np`, tensor de Hooke, espessura, umidade, `Δ_p`, tensão...).
2. Amostra-se o espaço com *"Latin hypercube sampling"*, *"space-filling techniques"* ou *"sparse grid"*, formando uma *"constellation of points"*.
3. Para cada ponto roda-se o FEM e guarda-se `(f_n, d_n)`.
4. Constrói-se um interpolador. Métodos citados: *"polynomial functions with multiple variables"*, *"radial basis functions"*, kriging, redes neurais, *"support vector machines"*, e derivadas via *"Taylor polynomial or Padé approximant"*.
5. Em runtime, mexer o knob = avaliar o interpolador (barato) e reescrever os coeficientes, num thread de background.

`[INFERÊNCIA]` Qual método é o real: a lista de 8 métodos é redação defensiva de patente. Mas o CV de Guillaume (lido em modartt.com) tem **cinco papers em Padé multivariado** e um em derivadas de alta ordem em projeto de forma. Aposta forte: **Taylor/Padé de alta ordem em torno de poucos pontos-base**, usando derivadas de autovalores obtidas analiticamente do FEM. Isso reduz drasticamente o número de rodadas de FEM, porque cada rodada entrega valor + derivadas.

**Função de ponderação da impedância** `[FATO]`, verbatim e implementável direto:
```
σ_p(h) = C                       se h ≤ h_c
σ_p(h) = C − Q·(h − h_c)         se h > h_c
```
onde h = ordem do harmônico, C = "weighting constant", h_c = "cut-off index", Q = "quality factor" (a inclinação). Três knobs de GUI modificam C, h_c e Q.

**Correspondência com o produto de 2026** `[FATO]`: o manual do Pianoteq 9 expõe exatamente "Soundboard mechanical Impedance", "Soundboard Impedance Cutoff" e "Soundboard Impedance Slope". C↔Impedance, h_c↔Cutoff, Q↔Slope. **Evidência forte de que o produto atual ainda usa essa estrutura de 2006.**

### 4.7 Uníssono: mapeando ε_p para cents

`[INFERÊNCIA]` As fórmulas (4)/(5) são fato de patente; a conversão é derivação. Como `f ∝ √T`:
```
Δcents(T2) = 600·log₂(ε_p)
Δcents(T3) = 600·log₂(2 − ε_p)
```
| ε_p | desvio |
|---|---|
| 0,9999 | ∓0,087 cents (estéril) |
| 0,999 | ∓0,866 cents (**faixa musical típica**) |
| 0,995 | ∓4,34 cents (honky-tonk) |
| 0,99 | ∓8,7 cents (fora da realidade) |

Faixa útil para um knob "Unison Width": **ε_p ∈ [0,995 ; 0,9999]**, mapeado logaritmicamente.

`[INFERÊNCIA]` **Incoerência no texto da patente**: ela descreve T2 como a corda de frequência MAIOR, mas com ε_p < 1, T2 = ε_p·T1 < T1 dá frequência MENOR. **Os rótulos estão trocados.** Não muda nada na prática (o par é simétrico), mas se copiar ao pé da letra você inverte os papéis.

### 4.8 Ressonância simpática vem DE GRAÇA

`[FATO]` Verbatim: *"The model makes it possible to take account of the mutual influence of the strings of the piano, notably the phenomenon of resonance by virtue of sympathy between the notes."*

Como o autoproblema é resolvido no sistema **acoplado** (todas as cordas + tampo juntas), os modos resultantes já são modos globais do instrumento inteiro. Batimento e decaimento duplo também: com K cordas, existem K parciais por harmônico com amortecimentos diferentes. **Zero código extra.**

`[INFERÊNCIA]` É por isso que o modal ganha do waveguide para piano: no waveguide você tem que ACOPLAR explicitamente e brigar com estabilidade; no modal o acoplamento já foi resolvido offline pelo solver.

### 4.9 O que a patente NÃO cobre — e é justamente o que faz soar como piano

`[FATO]` Varredura dirigida no texto integral:

| | |
|---|---|
| **AUSENTE** | pedal de sustain, dampers, note-off, tratamento de release |
| **AUSENTE** | re-strike de corda ainda vibrando (notas repetidas) — a síntese é one-shot |
| **AUSENTE** | parciais variantes no tempo (f_n e d_n são CONSTANTES) |
| **AUSENTE** | equação de martelo (nenhuma; só "a non-linear signal model ... according to a subtractive synthesis") |
| **PRESENTE** | ressonância simpática (via acoplamento nos modos) |

`[INFERÊNCIA]` Essas ausências são exatamente os problemas difíceis de um piano vivo, e são todos problemas de **ESTADO**, não de física:
- **Note-off / damper**: aumentar `d_n` dinamicamente. Troque `r_n` por `r_n' = exp(−d_n'/fs)·exp(i·2πf_n/fs)`. O damper real é dependente da frequência: `d_n' = d_n + δ(f_n)`.
- **Re-strike**: SOME a nova excitação ao estado existente — `z_n += α_n^novo`. **Se você resetar, o piano soa cortado nas repetições — erro clássico.**
- **Pedal**: manter vozes vivas e/ou ter dois conjuntos de `d_n` (damped/undamped) por nota.

**O Pianoteq de 2026 obviamente faz tudo isso. Nada disso está publicado. A patente te dá o esqueleto, não o produto.**

### 4.10 O que é MARKETING e não deve ser tratado como arquitetura

`[MARKETING]`:
1. *"Pianoteq is the very first piano where the sound is computed via a detailed physical model involving the hammers, the strings and the soundboard."* — reivindicação de primazia comercial. E como a seção 4.4 mostra, a patente descreve algo **mais fraco**: o martelo é dado empírico amostrado.
2. *"Every note is generated as you play... the entire piano mechanism is simulated."* — hipérbole. A patente não descreve simulação da ação (escapement, wippen como cinemática).
3. *"A new audio engine"* (v9), *"an improved model of the soundboard vibration"* — zero detalhe técnico publicado. Impossível avaliar.
4. *"Dozens of physical parameters"*, *"Thousands of editable overtones"* — verdadeiro como contagem de GUI; não é evidência de resolução de PDE.

**A única mudança de versão com conteúdo físico identificável** `[FATO]` é a **dupla polarização da v7** (SOS review, abr/2021): *"Double polarisation takes account of string vibrations that can emanate in any direction into the soundboard ... supports acoustic piano bass notes that can sustain as long as a couple of minutes."* Isso é fisicamente real e conhecido na literatura. `[INFERÊNCIA]` Num modelo modal, implementar significa **dobrar o número de modos por harmônico** (2 polarizações × K cordas).

**REGRA PRÁTICA**: só considere fato sobre o Pianoteq o que estiver (a) na patente US7915515B2, (b) no manual como definição de parâmetro, ou (c) em fala atribuída e datada de Guillaume/Pommier. Todo o resto é ruído.

### 4.11 Confirmação de primeira mão de Guillaume

`[FATO]` Entrevista cdm.link, 31/05/2010, citação literal:
> *"The modeling technique is based on various standard techniques issued from mechanics theory including modal analysis (calculation of vibration frequencies and the corresponding modes) and parametrization techniques that we developed at the university."*

Julien Pommier na mesma entrevista confirma que veio do **GetFEM** (biblioteca FEM open-source, GPL/LGPL) e que assumiu *"the development of the real-time engine"*. Único número de performance dado: em netbooks da época rodava *"with very high CPU load (80% or more)"*.

`[FATO]` A frase mais reveladora do manual: *"Be aware that bass notes require more computations than treble, because they contain many more overtones."*

`[INFERÊNCIA]` Isso **prova** aditivo/modal e não waveguide. Num waveguide o custo por voz é praticamente independente do pitch (a delay line é mais longa em memória, mas o loop por amostra é idêntico). Num banco modal o número de parciais cresce como `fs/(2·f₀)`: A0 admite ~800 harmônicos, C7 admite ~10.

### 4.12 A equipe — por que é caro reproduzir

`[FATO]` Da página modartt.com/modartt:
- **P. Guillaume** — concepção do modelo. Ex-afinador/restaurador de pianos (1976-1991), depois Agrégation, doutorado em matemática aplicada, diretor do Departamento de Matemática do INSA-Toulouse.
- **J. Pommier** — implementou a simulação em tempo real; vinha do GetFEM.
- **J. Chabassier** — tese *"Modélisation et simulation numérique d'un piano par modèles physiques"* (École Polytechnique/INRIA, 2012).
- **R. Auvray** — aeroacústica de tubos de órgão (Organteq). **R. Tournemenne, G. Castera** — modelo do piano. **X. Morel** — sopros. **A. Thibault** — sound design.

`[INFERÊNCIA]` **A presença da Chabassier NÃO significa que o Pianoteq usa o esquema FDTD conservativo da tese dela.** A tese é um simulador offline pesadíssimo (24 h em 300 CPUs por 1 s de som). É ferramenta de projeto/validação, não motor de tempo real. Vários textos na internet confundem os dois.

`[FATO]` Observação importante: o CV acadêmico de Guillaume (lido) tem publicações de otimização de forma, análise numérica, eletromagnetismo e mecânica — **NÃO há artigo revisado por pares dele sobre síntese de piano**. Ele registra apenas *"New research orientation in music instruments synthesis since 2003"*. **O conhecimento do Pianoteq nunca foi publicado academicamente; foi para a patente e para o produto.**

---

## 5. CUSTO DE CPU E POLIFONIA REALISTA

### 5.1 O número âncora

`[FATO]` Bank, Zambon e Fontana, TASLP 2010, Sec. VIII, verbatim:
> *"Our software is able to run at full polyphony, with a total number of 10000 second-order resonators and four 20000 tap convolutions, at an approximate load of 30% on a Intel's Core 2 Duo@2.4 GHz laptop."*

O que esse modelo inclui: vibração transversal modal (centenas de modos no grave), vibração **longitudinal**, martelo não-linear com massa, ressoadores secundários (batimento + decaimento duplo + simpatia), e tampo por convolução particionada de 20.000 taps.

**Distribuição do custo** `[FATO]` (Tabela I, com todas as notas soando):

| bloco | MPOS por nota | % do total |
|---|---|---|
| Vibração transversal | 2×N (livre), 2×N+3 (com martelo) | **40%** |
| Ressoadores secundários | 3×N2 | **30%** |
| Vibração longitudinal | 3×Nexc + 3×Nl | **25%** |
| Update do martelo | 2×N + 3 | **5%** |

Regra base: **3 MPOS por ressoador com entrada não-nula, 2 em evolução livre.**

### 5.2 Extrapolação para hardware moderno

`[INFERÊNCIA]`, aritmética verificável:
```
biquad y = b·x1 − a1·y1 − a2·y2  ⇒  3 mult + 2 add = 5 flops
10.000 ressoadores × 5 flops × 44.100 = 2,2 GFLOP/s
+ tampo: 4 × 633 flops/amostra × 44.100 = 112 MFLOP/s
```
(Contagem correta: são **3 multiplicações e 2 adições**, não 3 e 3. Uma versão anterior deste documento dizia "1,32 G mult/s + igual número de adições ≈ 2,2 GFLOP/s" — a prosa contradizia a conta, que daria 2,65. O valor 2,2 está certo; a justificativa é que o biquad tem 5 flops, não 6.)

Core 2 Duo 2,4 GHz com SSE: teto teórico ~38,4 GFLOP/s (2 núcleos). 2,2/38,4 = 5,7% do pico teórico, **medido a 30% de carga real** ⇒ eficiência efetiva ~19%. Plausível e saudável para código com estado por ressoador.

Um núcleo Zen4/Golden Cove a ~4,5 GHz com AVX2 e 2 FMA de 8 lanes = **~144 GFLOP/s** — **~3,8× um Core 2 Duo inteiro no pico teórico** (144/38,4 = 3,75).

**Conclusão**: modelo classe-Bank completo ⇒ **~5-15% de UM núcleo moderno.**

**Cross-check independente** `[FATO]`: Pianoteq no iPad M1 dá **14%**. As duas estimativas independentes batem.

### 5.3 Pianoteq — números medidos reais

`[FATO]` Sound On Sound, review do Pianoteq 8 (Robin Bigwood), medições diretas:
- **iPad Air 5ª geração (M1): "never saw more than about 14 percent processor usage"**, buffer 128 amostras, 2,7 ms de latência.
- **iPhone 12 (A14): "25 percent at most"**.
- Ambos *"even when deliberately sustaining many low piano notes, which is computationally intensive"* — ou seja, o pior caso.
- Instalação: **~50 MB** contra *"approx 1GB installation sizes"* dos concorrentes por sample.

**Número muito revelador** `[FATO]` do mesmo review (velocidade de render offline = headroom):
- Pianoteq 7: *"would often render at 12 times faster than real time"*
- Pianoteq 8: *"would drop to nearer four times real time"*

`[INFERÊNCIA]` **A Modartt GASTOU o ganho de hardware em qualidade de modelo.** Headroom de 4× significa que o modelo em qualidade máxima consome ~25% do orçamento de tempo. A qualidade do modelo é um **dial**, não uma constante.

`[NÃO CONFIRMADO]` Relatos de fórum (sem controle de método): Raspberry Pi 4B com Pianoteq 6.6 rodou 98 vozes sem dropouts, com sample rate INTERNO de 24 kHz e buffer 256. Pi 5 com Pianoteq 8 a 48 kHz / 192 amostras: 68% máx.

### 5.4 O outro extremo: física completa está fora de cogitação

`[FATO]` Chabassier, Chaigne e Joly, JASA 134(1), 2013, citação literal:
> *"In average, computing 1 s of sound for the complete piano model (with frequency content up to 10 kHz) takes 24 h on a 300 CPU cluster."*

`[INFERÊNCIA]` 24 h × 300 CPUs = 7.200 CPU-horas por segundo de áudio = **~26 milhões de vezes mais lento que tempo real** num núcleo. Mesmo com 1000× de ganho de hardware, faltam 4 ordens de grandeza.

Por quê: passo de tempo Δt = 1e−6 s (**1 MHz** efetivo), ~3.000 DOF por corda, 420.000 DOF no tampo, >9e7 DOF no domínio de ar, e Newton-Raphson iterativo por passo.

`[NÃO CONFIRMADO]` E a avaliação dos próprios autores é modesta: Bank & Chabassier (IEEE Signal Processing Magazine, 2019) avaliam que o som obtido pelo modelo físico completo ainda decepciona, principalmente porque o ouvido é muito sensível a taxas de decaimento e os fenômenos dissipativos do tampo e das cordas ainda não são bem modelados. **Paráfrase, não citação** — este paper está na lista da §10.2 (não aberto: HAL bloqueado por anti-bot, IEEE pago). A formulação pública que se consegue confirmar em fontes abertas é mais fraca: os sons sintetizados *podem ser percebidos como não-realistas, principalmente pela falta de modelagem fina e precisa dos fenômenos dissipativos no tampo e nas cordas*. **Se for citar isso em algum lugar, obtenha o PDF antes.**

**O modelo físico completo do tampo, hoje, soa PIOR que o filtro caixa-preta calibrado por medição. Não invista aí.**

### 5.5 Produto real com números publicados

`[FATO]` Gabrielli, Zambon & Fontana, *"Parallel Digital Signal Processing for Efficient Piano Synthesis"*, EUSIPCO 2015 (Viscount Physis Piano):
- **~15.000 ressoadores** no total, dos quais ~10.000 em evolução livre a maior parte do tempo.
- **~3 GFLOPS**.
- Máximo de ressoadores computáveis numa janela de 1,4 ms (64 amostras @44,1 kHz), por core:

| CPU | ressoadores/core | TDP/core |
|---|---|---|
| x86 Core i5 @ 2,5 GHz | **17.300** | 8,7 W |
| ARM Cortex-A7 @ 1 GHz | 3.920 | 1,4 W |
| ARM Cortex-A9 @ 1 GHz | 3.800 | 0,6 W |
| TI C674x DSP @ 456 MHz | 5.200 | 0,8 W |

*(Nota: numa primeira extração os rótulos de linha desta tabela saíram embaralhados. Foram reconferidos contra o PDF livre em `new.eurasip.org/Proceedings/Eusipco/Eusipco2015/papers/1570104271.pdf` — a leitura acima é a correta.)*

- Filtro de tampo com **polos comuns** entre 4 regiões do cavalete: **speed-up ~50×** vs convolução FFT direta com buffer de 64.

### 5.6 O que REALMENTE limita, em ordem

`[INFERÊNCIA]`, com base nos benchmarks acima:

1. **Nº de modos por nota no GRAVE.** A0 com 140 modos custa ~14× uma nota aguda com 10. A curva de LOD é a alavanca principal.
2. **O piso dos ressoadores simpáticos**, que não desliga (30% do total).
3. **Capacidade de L2.** Por ressoador você toca `y1, y2, a1, a2, b` = **20 bytes**. 10.000 ressoadores = **200 KB** — cabe folgado em L2 (1-2 MB), **não** cabe em L1 (32-48 KB). A 30.000 ressoadores você chega a **~586 KB** (30.000 × 20 = 600.000 bytes), o que **já estoura um L2 de 512 KB**; a ~42.000 ressoadores você chega a 840 KB e estoura também os L2 de 768 KB, caindo em L3 — **a performance despenca de forma não-linear.**
   **Regra derivada: dimensione para que (total de ressoadores) × 20 bytes fique abaixo de ~60% do L2 por núcleo.**
4. **O modelo de tampo.** Convolução de 20.000 taps × 4 canais ≈ 25% do custo das cordas.
5. **A parte longitudinal**, O(K·N), 25% do total. **Primeiro candidato a virar dial de qualidade.**

### 5.7 SIMD: é DESIGN, não otimização

`[FATO]` Bank et al., Sec. VIII: *"The parallel bank of filters of Figure 3 can be easily implemented in a SIMD fashion. We used Intel's SSE instructions, which can produce a speed-up factor of almost four times with single precision floating point data."*

`[INFERÊNCIA]` **O ponto crítico**: um ressoador de 2ª ordem É uma recorrência serial no tempo. Você **NÃO** vetoriza no tempo — vetoriza **ATRAVÉS de ressoadores**, que são mutuamente independentes.

Isso **obriga** structure-of-arrays:
```c
// CERTO
float y1[N], y2[N], a1[N], a2[N], b[N];

// ERRADO — isto MATA o SIMD
struct Mode { float y1, y2, a1, a2, b; };  Mode modes[N];
```
Com AoS você paga shuffle/gather por lane. **Essa decisão precisa estar tomada no dia 1 — refatorar depois significa reescrever tudo.**

Larguras: SSE=4, AVX2=8, NEON=4 floats. Alinhe em 32 bytes e faça o número de modos por corda múltiplo da largura do vetor, com padding de ganho zero.

`[INFERÊNCIA]` **Por que modal ganha de waveguide aqui**: no waveguide os delay lines têm comprimentos DIFERENTES por corda → processar 8 cordas em paralelo vira gather/scatter. No modal todos os ressoadores têm a mesma estrutura → **SIMD puro, sem gather**.

**O ponto chato**: a força na ponte precisa de uma **redução horizontal por amostra por corda**. Acumule num registrador vetorial e faça UMA redução por amostra. Isso é latência pura e não vetoriza.

### 5.8 Denormais — a armadilha que vai te morder

`[FATO]` Wittmann, Zeiser, Hager e Wellein, arXiv:1506.03997, 2015, medições em SandyBridge/IvyBridge/Haswell/Interlagos:

| operação | ciclos |
|---|---|
| add/mul normalizado | 3 a 5 |
| **denormal** | *"about two magnitudes larger"* |
| adição com underflow SEM FTZ/DAZ | 38,20 (SNB) / 31,90 (HSW) |
| divisão com underflow SEM FTZ/DAZ | 71 (SNB) / 57 (HSW) |
| **COM FTZ e DAZ ligados** | *"no negative performance impact"* |

`[INFERÊNCIA]` **Por que isso é pior aqui do que no motor de samples**: um banco de 10.000 ressoadores IIR decaindo exponencialmente é a máquina de gerar denormals mais eficiente que existe. Cada modo, depois do release, decai até ~1e−38 e fica ali produzindo denormals 48.000 vezes por segundo, em 10.000 filtros. Sem FTZ/DAZ, com ~32-38 ciclos de penalidade, você multiplica o custo do banco por ~10× — **e isso acontece DEPOIS que o músico solta as teclas**. É o clássico "a DAW travou no fim do louvor".

**BOA NOTÍCIA: no ChurchDAW isso já está resolvido.** `juce::ScopedNoDenormals` já cobre o callback inteiro em **`TimbresProcessor.h:5060`**.

Defesa complementar: desligue ressoadores cuja amplitude caia abaixo de um limiar audível — resolve o custo E o denormal de uma vez. `[FATO]` O próprio Bank sugere isso: *"selectively deactivating the partials of a single note when they are not audible anymore"*.

### 5.9 Polifonia: orçamento por CUSTO, não por contagem

`[FATO]` Bank et al., Sec. VIII, verbatim:
> *"By knowing the approximate cost of each note, we have implemented a polyphony queue where the limit is imposed on the total computational cost and not on the number of the notes."*

**Isso é diferente do seu motor de samples.** Numa DAW de samples, uma voz é uma voz. Aqui, uma nota grave custa 10-20× uma aguda. **Um budget de "616 vozes" não significa nada aqui.**

`[FATO]` **E há uma limitação declarada que é decisão de arquitetura, não de debug:**
> *"The only limit coming from the architecture of the model is that the secondary bank of resonators cannot be deactivated, since they are also used for simulating sympathetic resonances."*

Os ressoadores secundários são **30% do custo total** e **RODAM SEMPRE**, mesmo com o instrumento em silêncio.

`[INFERÊNCIA]` **Este é o custo de piso — a diferença fundamental contra samples.** Sample em silêncio custa zero; modelagem física custa o piso. Numa DAW rodando órgão de tubos + tonewheel + piano simultaneamente, **esses pisos SOMAM**. Meça o consumo do piano com NENHUMA tecla pressionada — esse é o número que entra no seu orçamento fixo.

### 5.10 Latência

`[FATO]` O modelo de CORDA tem latência ZERO — é recorrência amostra-a-amostra. A latência vem de duas fontes, ambas no tampo, ambas evitáveis:

1. **Convolução particionada**: com N = tamanho do buffer não adiciona latência, mas o custo por amostra é `O(log N + M/N)` — e com N pequeno o termo M/N DOMINA. **Latência baixa e convolução longa são inimigos diretos.**
2. **Thread separada para o tampo**: Bank faz isso e admite — *"The drawback is that some additional latency is introduced."*

`[FATO]` **A recomendação, com as palavras do próprio paper**: use banco de filtros paralelos de 2ª ordem:
> *"the parallel filter requires significantly smaller amount of memory, has zero latency and results in a simpler code"*

Trade-off honestamente declarado: *"the computational complexity seems to be larger compared to the partitioned convolution for the same sound quality"*.

`[INFERÊNCIA]` **Para uma DAW ao vivo esse trade é claramente a favor do filtro paralelo**: latência zero, código mais simples, uma thread a menos, e **a MESMA estrutura de dados da corda** (é tudo ressoador de 2ª ordem — mesmo kernel SIMD, reaproveitamento total).

### 5.11 O banco paralelo de tampo — projeto completo

`[FATO]` Bank, ICMC 2007, pp. 458-465; refinado em IEEE SPL 18(2):138-141, 2011.
```
H(z⁻¹) = Σ_{k=1..K} (d_k0 + d_k1·z⁻¹)/(1 + a_k1·z⁻¹ + a_k2·z⁻²)  +  Σ_{m=0..M} b_m·z⁻ᵐ
```

**PASSO 1 — polos FIXOS em escala logarítmica:**
```
θ_k = 2π·f_k/fs         (f_k em série logarítmica)
p_k = R^(θ_k/π) · e^(±j·θ_k)
a_k1 = −2·Re{p_k};  a_k2 = |p_k|²
```
R = raio do polo em fs/2. Exemplo do paper: **100 polos log, R = 0,98, ordem 200**.

**PASSO 2 — zeros por mínimos quadrados.** Com os polos FIXOS, H fica **LINEAR** nos parâmetros ⇒ solução em **FORMA FECHADA**:
```
p_opt = (Mᴴ M)⁻¹ Mᴴ h_t
```
onde as colunas de M são as respostas ao impulso das seções de 2ª ordem e suas versões atrasadas de 1 amostra. Sem otimização não-linear, sem mínimo local, sem risco de polos instáveis. Bank reporta projeto robusto **até ordem 1000**.

`[FATO]` Versão em FREQUÊNCIA (SPL-2011): para coeficientes reais com especificação unilateral,
```
p_opt = (Re{Mᴴ M})⁻¹ · Re{Mᴴ h_t}
```
**Ganho prático enorme**: precisou de **80× menos pontos** de especificação e ficou ~2 ordens de grandeza mais rápida de projetar. Se você quiser recalcular o tampo em runtime, é por aqui.

**Ordens necessárias** `[FATO]` (ICMC-07, Sec. 4):
- IIR tradicional (Prony): ordem 200 = aceitável; ordem 800 = indistinguível.
- **Filtro paralelo: ~4× menos ordem** que IIR tradicional para a mesma qualidade.
- FIR direto: 1000-2000 taps para boa tonalidade; **~10.000 taps para reproduzir o "knock"** característico das médias e agudas.

`[FATO]` **A frase que economiza um mês de debug**: ~80% do custo do filtro de tampo existe SÓ para o thump/knock do ataque nas notas agudas. **Se você truncar a IR em 2000 taps, o piano fica com ataque morto no agudo e você vai culpar o martelo por engano.**

**Extras que só o banco paralelo dá** `[FATO]` (ICMC-07 Sec. 5.1), todos em runtime, sem filtro adicional:
- **Tamanho do corpo**: multiplicador global nos ângulos θ_k.
- **Decaimento do corpo**: expoente global em |p_k|.
- **EQ / brilho**: escalar apenas d_k0 e d_k1.
- **Morphing** entre corpos: interpolar os d's com os mesmos polos.
- **Estéreo barato**: MESMOS polos, só duplicar os coeficientes de saída (2 MAC extra por seção em vez de 4).
- **Escalabilidade de sample rate**: projete na maior taxa; ao rodar em fs menor, **descarte os modos acima de fs/2** e reescale. Isso resolve 44,1/48/96 kHz com um único design e dá caminho de degradação natural para Android/iOS.

**Código de referência** `[FATO]`: `https://home.mit.bme.hu/~bank/parfilt/` — MATLAB/Octave. Funções `freqpoles.m`, `wppoles.m`, `parfiltdes.m`, `parfiltdesfr.m`, `parfilt.m`. O `parfiltdes.m` literalmente monta M com `filter(1, poly(par_de_polos), delta)`, resolve `A = M'*M; b = M'*y; par = A\b`. São ~50 linhas. **LICENÇA: NÃO-COMERCIAL — ver seção 6.**

---

## 6. O QUE EXISTE PRONTO — com licença e se serve para produto comercial

### 6.1 Tabela resumo

| Projeto | Método | Licença | Serve para produto fechado? |
|---|---|---|---|
| **STK** | waveguide, blocos genéricos | **MIT** (verificado no LICENSE) | ✅ SIM — mas **não tem piano** |
| **NESS (Edinburgh)** | FDTD/modal | **MIT** ⚠️ | ✅ provavelmente — mas é **OFFLINE** e **não tem piano** |
| **Faust** | compilador + libs | compilador LGPL, **código gerado LIVRE** | ✅ SIM (checar cada `.lib`) — **não tem piano** |
| **OpenPiano** | diferenças finitas, JUCE, C++17 | **AGPL-3.0** | ❌ **VENENO ABSOLUTO** |
| **Resonarium** | waveguide/cordas acopladas, JUCE | GPL-3.0 **com porta aberta para licença comercial** | ⚠️ só via e-mail ao autor |
| **parfilt (Bank)** | banco paralelo de 2ª ordem | *"Non-commercial use permitted"* | ❌ código não; **método sim** |
| **Csound `prepiano`** | FDTD (Bilbao) | LGPL-2.1 | ⚠️ só link dinâmico; **fonte não localizado** |
| **mda Piano** | **sample playback** — NÃO é modelagem | MIT | ✅ mas valor **ZERO** aqui |
| **Salamander Grand Piano** | dataset | **CC BY 3.0** | ✅ SIM |
| MAESTRO / MAPS / BiVib | datasets | CC BY-NC-SA 4.0 | ❌ **NÃO-COMERCIAL** |

### 6.2 Dois mitos que precisam morrer

`[FATO]` **MITO 1 — mda Piano NÃO é modelagem física.** O fonte foi lido (`elk-audio/mda-vst2`, `plugins/mdaPiano.cpp`). Define 15 keygroups apontando para offsets num array de amostras:
```c
kgrp[0].root = 36;  kgrp[0].pos = 0;      kgrp[0].end = 36275;
kgrp[1].root = 40;  kgrp[1].pos = 36278;  kgrp[1].end = 83135;
```
E o laço de síntese é interpolação linear pura sobre a tabela:
```c
i = waves[V->pos] + ((V->frac * (waves[V->pos+1] - waves[V->pos])) >> 16);
```
Não há waveguide, não há ressoador, não há corda. **Riscar da lista.**

`[FATO]` **MITO 2 — o Faust `physmodels.lib` NÃO tem piano.** O arquivo foi baixado cru do `grame-cncm/faustlibraries`. O próprio TODO dentro do arquivo diz: *"Currently still missing keyboard instruments"*.

O que existe e é aproveitável como bloco: `openString`, `nylonString`, `steelString`, `idealString`, `ks`, `stringSegment`, `bridgeFilter` (FIR de dois zeros), **`modeFilter`** (passa-banda ressonante — este é o tijolo do modal), `modularInterpBody`.

### 6.3 OpenPiano — dissecação (útil como aviso, proibido como base)

`[FATO]` Licença confirmada **lendo o cabeçalho dos dois arquivos** (`string_hammer.h`, `piano.h`): *"GNU Affero General Public License ... either version 3 of the License, or (at your option) any later version"*. Copyright 2021-2022 Michele Perrone.

**AGPL é o pior cenário possível para uma DAW comercial fechada** — pior que GPL, porque estende a obrigação até para uso em rede. **Recomendação forte: não leia o código com intenção de aproveitar**, para não contaminar a autoria.

`[FATO]` O que o código **NÃO tem** (verificado lendo, não inferido):
- **52 notas.** `FIRST_NOTE=A0`, `LAST_NOTE=C5`. De C#5 a C8 está **comentado no fonte**.
- **Uma corda por nota.** Sem uníssono, sem batimento, sem decaimento duplo.
- **NENHUM tampo.** O bloco de contorno com impedância (`zeta_b=1e3`) está escrito mas **comentado**. O que roda é reflexão rígida perfeita.
- Mesmos parâmetros de martelo para as **52 notas**: `Mh=4.9e-3, p=2.3, K=4e8` — é o martelo de **C2** copiado para tudo.
- `E = 9e7 Pa` em todas as notas. **Aço é 2,02e11 Pa** — 2000× menor.

`[INFERÊNCIA]` Como `Te = ρ·L²·4·f₀²`, foi calculado: A0 dá T = 203 N (plausível) mas **A7 dá T = 8,3e5 N = 831 kN**. Uma corda de piano rompe perto de 1-2 kN. **As cordas agudas dele estão 500× acima da carga de ruptura.**

**Bugs encontrados lendo** `[INFERÊNCIA]`:
1. `compute_FD_coefficients()` lê `this->r` na linha ~448 (`r_sqr = r*r`) e só ATRIBUI `this->r` na linha ~451. Na chamada do construtor, `r` é lixo não inicializado.
2. `D = 1 + b1*Xs + 2*b2/Ts` usa `Xs` (passo **ESPACIAL**, em metros) onde a linha seguinte usa `b1*Ts`. O denominador publicado é `(1 + b1·T)`. A identidade `a1+a2+2a3+2a4+3a5 = 1` quebra ⇒ ganho DC espúrio.
3. Janela de Hann **não normalizada** (ver seção 2.4).
4. `memcpy(..., g*sizeof(double*))` em vez de `sizeof(double)`.
5. `powf()` em variáveis double no caminho quente; um `pow(x, 2.3)` por corda por amostra.
6. **Threading não é real-time-safe**: busy-wait `while(n_running_threads != 0) {}` sem yield, threads detached, `malloc` de `std::atomic`. **Inaceitável numa DAW ao vivo.**

`[INFERÊNCIA]` **Custo calculado rodando as próprias fórmulas do código a 48 kHz**: 8.627 pontos de grade para as 52 notas → **~7,45 GFLOP/s**. O modal do Bank faz **88 notas COM uníssono, simpatia, longitudinais e tampo em ~2,4 GFLOP/s.** O OpenPiano gasta 3× mais para fazer bem menos.

**Veredito: útil como material didático. Inútil como base técnica. Proibido como base legal.**

### 6.4 STK e a patente que caiu

`[FATO]` Licença MIT verificada (`github.com/thestk/stk`, ©1995-2023). Permite explicitamente *"use, copy, modify, merge, publish, distribute, sublicense, and/or sell"*, com um pedido **NÃO-vinculante** de enviar modificações de volta.

O README tem um alerta na seção "Legal and Ethical": *"Some of the concepts are covered by various patents ... Many of the ones known to us are administered by the Stanford Office of Technology and Licensing."*

`[FATO]` **MAS a patente-mãe expirou.** US 4,984,276 — *"Digital signal processing using waveguide networks"*, Julius O. Smith, Leland Stanford Junior University. Prioridade 02/05/1986, concessão 08/01/1991, **expiração 08/01/2008. Status: "Expired - Lifetime"**.

**O aviso do README é conservador/histórico.** O núcleo do waveguide digital é domínio público há mais de 18 anos.

`[FATO]` **STK não tem modelo de piano.**

### 6.5 Datasets — a tabela que decide

`[FATO]` **SALAMANDER GRAND PIANO v3** (`github.com/sfzinstruments/SalamanderGrandPiano`):
- **Licença: CC BY 3.0 Unported.** Permite uso comercial e modificação, exigindo apenas crédito.
- 48 kHz / 24 bit, **16 camadas de velocity**, amostrado em terças menores.
- Dois AKG C414 em AB, ~12 cm acima das cordas.
- Inclui: 440 arquivos principais (A0v1 a C8v16), 69 de ressonância de release, **88 de ruído de martelo (cromático)**, 4 de ruído de pedal.
- Formatos SFZ e SF2.

**Este é o seu dataset de calibração seguro.** Com 16 velocities você extrai a curva de dureza do martelo; com as notas isoladas você extrai f_n e d_n por parcial; com o ruído de martelo cromático você calibra o transiente. **É exatamente o que a Modartt faz na pré-síntese.**

`[FATO]` **NÃO USÁVEIS COMERCIALMENTE:**
- **BiVib** (Papetti, Avanzini et al., Zenodo 1213211 / 3982955) — tecnicamente o MELHOR: WAV 24-bit @ 96 kHz, binaural + vibração do teclado, dois Yamaha Disklavier, **todas as 88 teclas em 10 velocities**, acionadas por computador, com as especificações nominais de toda a cadeia documentadas (permite calcular Pa e m/s² reais). **Licença: CC BY-NC-SA 4.0.**
- **MAESTRO** (Google/Magenta) — 200+ h, alinhamento ~3 ms. **CC BY-NC-SA 4.0.** Além disso é performance, não notas isoladas.
- **MAPS** (Telecom ParisTech) — dataset de transcrição, não de acústica. Licença de pesquisa.

`[INFERÊNCIA]` **Zona cinzenta a decidir com cabeça fria**: usar um dataset NC para DERIVAR parâmetros que vão para um produto pago. Não é distribuir o dataset, mas "NonCommercial" na CC 4.0 fala em uso *"primarily intended for commercial advantage"*. **Não arriscaria. Use BiVib para VALIDAR e ENTENDER, e Salamander (CC BY) para o que alimenta o produto.**

### 6.6 A armadilha GPL que ninguém avisa

`[INFERÊNCIA]` Se você **LER** código GPL/AGPL e depois escrever o seu, existe risco de "derivação" por cópia de estrutura/expressão — que é **direito autoral**, não patente.

**Se for olhar código GPL, olhe só para entender a MATEMÁTICA (que não é protegida por copyright) e implemente do zero a partir do PAPER, não do código.**

**Manter um registro escrito** (caderno de engenharia versionado, com data + commit) de que a implementação veio do paper X e não do repo Y **é barato e vale muito** se um dia for questionado.

---

## 7. SITUAÇÃO DE PATENTES

> **Não sou advogado. Abaixo são FATOS (números, datas, textos de reivindicação), não parecer jurídico.**

### 7.1 O que está VERDE — waveguide inteiro caiu

`[FATO]` Tabela verificada uma a uma no Google Patents:

| Nº | Título | Titular | Expiração | Status |
|---|---|---|---|---|
| US4984276A | Digital signal processing using waveguide networks | Stanford | **2008-01-08** | Expired - Lifetime |
| US5212334A | ... closed waveguide networks | **Yamaha** | 2010-05-18 | Expired - Lifetime |
| US5448010A | ... closed waveguide networks | Stanford | 2012-09-05 | Expired - Lifetime |
| US5614686A | **Multidimensional digital waveguide (MESH 2D/3D)** | Stanford (Van Duyne + Smith) | 2013-05-04 | Expired |
| US5777255A | **Efficient synthesis ... nonlinear excitations (MARTELO DE PIANO)** | Stanford | **2015-05-10** | Expired |
| US5229536 | Musical tone synthesizing (feltro elástico + **viscoso**) | Yamaha | 2005-07-20 | Expired |
| US5241127 | Musical tone synthesizing (**key scaling** do martelo) | Yamaha | expirada | Expired |
| US9293126B2 | System to reproduce sound of stringed instrument (**modal, Bank/Zambon/Fontana**) | Viscount | — | **Expired - Fee Related** |

**Consequência**: waveguide de corda, commuted synthesis, **mesh 2D de tampo**, **excitação não-linear de martelo por pulso filtrado**, **modelo de feltro com histerese**, e **key-scaling de parâmetros de martelo** — tudo domínio público.

`[FATO]` A US5614686A é a patente de mesh 2D/3D e a descrição cita literalmente *"a Sound Board connected to Waveguide String Models"* para piano e harpa. Expirou em 2013.

`[FATO]` **Sobre a Viscount US9293126B2**: a Viscount **PAROU de pagar anuidades**. Expiração natural seria 2033; caducou muito antes. E o artigo acadêmico correspondente (Bank/Zambon/Fontana, TASLP 2010) foi publicado **DOIS ANOS ANTES** da data de prioridade da patente (2012-03-13) — sob 35 U.S.C. 102(a)(1), publicação impressa anterior é arte anterior.
`[NÃO CONFIRMADO]` Ressalva: patente caducada por falta de taxa pode, em janelas limitadas, ser reativada por petição. E o status do Google Patents é derivado automaticamente, não é certidão oficial.

`[FATO]` **Roland**: a virtual patent marking em `roland.com/us/patent` foi consultada e as três patentes mais repetidas foram abertas. US8445765 é **mecânica de teclado** (rampa e ressalto para dar "click"). US9646578 é **construção da tecla** (base de resina, madeira nas laterais, ≥2 mm). US10474590 é **driver USB de áudio**. `[INFERÊNCIA]` **Nenhuma patente de SÍNTESE marcada pela Roland nos pianos atuais.**

### 7.2 VERMELHO #1 — Modartt US7915515B2, ativa nos EUA até 2027-10-20

`[FATO]` Titular Modartt, inventor Philippe Guillaume, prioridade 2006-07-28, concessão 2011-03-29. Google Patents: *"Active, expires 2027-10-20"*.

**A família inteira MORREU exceto os EUA:**
| membro | status |
|---|---|
| FR2904462B1 | Expired, fee related |
| EP2047455B1 | **CEASED** |
| JP5261385B2 | Expired, fee related |
| CN101473368B | Expired, fee related |
| CA2651981A1 | Abandoned |
| WO2008012412 | Ceased |
| **US7915515B2** | **ACTIVE** |

**Nenhum membro brasileiro (BR) aparece na família.**

**Decomposição da claim 1 em elementos** (para infração literal, TODOS precisam estar presentes — regra do "all elements") `[FATO]`:
- **E1** — módulo de produção de som em tempo real acionado por triggering signal.
- **E2** — *"said device comprises at least one PRESYNTHESIS MODULE adapted to produce timbre coefficients ... representative of the DAMPING ... and ... the FREQUENCY of each partial"*.
- **E3** — esses coeficientes vêm de *"at least one SOUNDING BOARD parameter ... representative of a MEASURABLE PHYSICAL PROPERTY"* **E** *"said at least one STRING parameter"*.
- **E4** — o som é *"composed at least of a plurality of PARTIALS with said timbre coefficients produced by said at least one presynthesis module"*.

**DESIGN-AROUNDS** `[INFERÊNCIA]` (isto é engenharia, não parecer):

| # | Manobra | Por que funciona |
|---|---|---|
| **D1** | **NÃO EMBARCAR MÓDULO DE PRÉ-SÍNTESE.** Rodar a extração de coeficientes na SUA máquina, distribuir só as tabelas num binário read-only. | E2 exige o módulo **DENTRO do dispositivo**. Sem ele, nem a claim 1 nem a claim 24 (a espelhada de "recording medium") leem. **É o design-around mais barato que existe, e é o mais adequado ao ChurchDAW — ninguém vai editar tensor de Hooke durante um culto.** |
| **D2** | Coeficientes vindos de **ANÁLISE DE GRAVAÇÕES**, não de parâmetro físico mensurável do tampo. | E3 fica sem o elemento "sounding board parameter representative of a measurable physical property". |
| **D3** | Não sintetizar por parciais (waveguide, FDTD). | E4 não lê. E toda a base de waveguide já expirou. |
| **D4** | **Territorial.** | EP, FR, JP, CN e CA caducaram. A restrição é **só nos EUA**, e cai em **out/2027** — 14 meses. |

**A rota recomendada (seção 9) cai fora da claim 1 por DOIS elementos independentes (D1 e D2).**

### 7.3 VERMELHO #2 — Yamaha, ativas até 2030-2032 (o pior da lista)

`[FATO]` **US 8,115,092 B2** (inventor Eiji Tominaga, prioridade 2009-06-03, **expira 2030-06-06, ATIVA**) — é o VRM do Clavinova.

Claim 1 é um **MÉTODO de 8 passos (a até h), TODOS necessários**:
1. (a-b) adquirir força do martelo e deslocamento nos apoios (capotasto e cavalete);
2. (c) resolver a equação da corda em **COORDENADAS MODAIS**;
3. calcular forças de reação nos apoios via cossenos diretores;
4. (d) alimentar um **MODELO DE CORPO** sob aproximação de amortecimento viscoso proporcional, também em coordenadas modais;
5. (e-f) **realimentar** o deslocamento dos apoios a partir do movimento do corpo;
6. (g) **MODELO DE RADIAÇÃO NO AR**: convolução entre a velocidade do corpo **EM COORDENADA MODAL** e uma resposta impulsiva.

`[FATO]` **O truque central da corda** (Eq. 15): os modos de Fourier `sin(mπx/l)` só valem para extremidades FIXAS, mas num piano real as extremidades SE MOVEM. A Yamaha escreve:
```
u_k(x,t) = Σ_m A_k^(m)(t)·sin(mπx/l) + [reta ligando os dois apoios móveis]
```
E admite honestamente: *"Although the sinusoidal function of equation 15 does not express the natural vibration mode in the strict sense of words, the sinusoidal function is assumed to express the natural vibration mode for convenience's sake."*

`[FATO]` **Ordem de operações declarada**: *"the calculations through equations 16 and 18 are calculated PRIOR TO the calculation through equation 17 at each time step, and the non-linear term ... is handled AS IF IT IS A TERM EXPRESSING AN EXTERNAL FORCE."* Primeiro as duas flexões, depois a longitudinal, com o acoplamento não-linear como força externa explícita.

**DESIGN-AROUNDS** `[INFERÊNCIA]`:
- **D5** — o passo (g) exige convolução **em coordenada modal**. Se você convoluir a **FORÇA NO CAVALETE (coordenada física)** com uma IR medida do tampo, o passo (g) não está presente.
- **D6** — o passo (f) exige realimentar o deslocamento do corpo na equação da corda. Um modelo com terminação por **FILTRO DE ADMITÂNCIA** (impedância de ponte como filtro, sem estado global do corpo realimentado) não tem (e)+(f).
- **D7** — o passo (d) exige o corpo em coordenada modal com amortecimento viscoso proporcional. Um corpo como IR/FIR medida não tem (d).

**Resumo**: a rota *"corda modal → força no cavalete → IR/filtro de tampo em coordenada física → saída"* evita simultaneamente (d), (f) e (g). Perde-se a realimentação corpo→corda; ganha-se custo e liberdade.

#### US 8,895,831 B2 — a divisional do abafador (expira 2030-10-07, ATIVA)

`[FATO]` **Titular Yamaha, inventor Eiji Tominaga, prioridade 2009-06-03.** Título: *"Method for synthesizing tone signal and tone signal generating system"*. **18 claims no total; os claims 1, 2, 3 e 4 são INDEPENDENTES** — os demais são dependentes deles. (Verificado no texto integral em FreePatentsOnline; esta ressalva estava marcada `[NÃO CONFIRMADO]` em versão anterior e agora está **confirmada**.)

> **ARMADILHA DE LEITURA — perdi tempo com ela, você não precisa perder.**
> A US8895831 é uma **DIVISIONAL** do pedido Ser. No. 12/788,897 (27/05/2010), que é o pedido que virou a **US8115092**. Por isso a **DESCRIÇÃO** das duas é praticamente idêntica: a descrição da divisional recita o método (a)–(h) de corda modal + corpo + convolução da patente-mãe.
> **Se você abrir a US8895831 no Google Patents e ler a descrição, vai concluir que o claim 1 dela é o mesmo método modal da US8115092. NÃO É.** Divisional existe justamente porque o examinador exigiu separar invenções: os claims da divisional cobrem o que **não** foi reivindicado na mãe. Leia a seção **CLAIMS**, não a descrição.

`[FATO]` **Claim 1 verbatim** (conferido no texto integral) — é sobre o **ABAFADOR**, e é literalmente a definição de half-pedal:

> *"A method of simulating an acoustic tone produced through an acoustic piano ... said acoustic piano including at least one key ... at least one damper linked with said at least one key so as to be spaced from and brought into contact with said at least one vibratory wire depending upon a position of said at least one key, a damper pedal linked with said at least one damper ... said method comprising the steps of a) acquiring a first piece of data expressing **a stroke of a key** ... and a second piece of data expressing **a stroke of a pedal** ..., b) determining a third piece of data expressing **resistance** against said at least one wire by said at least one damper by **varying a value of viscous coefficient** of said at least one damper on the basis of said first and second pieces of data **in a time dependent manner**, and c) determining said tone signal in consideration of said third piece of data."*

`[FATO]` **Mapa dos quatro claims independentes** (todos lidos verbatim):

| claim | tipo | cobre |
|---|---|---|
| **1** | método | **half-pedal**: coeficiente **viscoso** do abafador variando com curso de TECLA **e** curso de PEDAL, no tempo |
| **2** | sistema | o **mesmo** que o claim 1, redigido como aparelho (módulo de cálculo do abafador com dois sub-módulos) |
| **3** | método | **una corda / soft pedal**: variar o **módulo de elasticidade do MARTELO** conforme o curso do pedal esquerdo |
| **4** | sistema | o **mesmo** que o claim 3, redigido como aparelho |

**Avaliação corrigida** `[INFERÊNCIA]`:

- **A patente É estreita** — mas estreita **exatamente onde dói para a Etapa 2**. Ela não alcança o motor de corda (isso é a US8115092); alcança o **abafador com meio-pedal** e o **pedal una corda**.
- **O que está fora dos claims 1/2**: os claims exigem (i) `stroke de tecla` **e** `stroke de pedal` como entradas, **e** (ii) que a modelagem seja por **variação de um coeficiente VISCOSO** do abafador, **e** (iii) que essa variação seja **dependente do tempo**. Faltando qualquer um desses, o claim não lê literalmente.
- **Design-around plausível** `[INFERÊNCIA]`: modelar o abafador como **envelope de amortecimento aplicado ao decaimento modal** (`d_n' = d_n + δ(f_n, pedal)`), ou como perda dependente de **estado discreto**, em vez de como coeficiente viscoso de um modelo mecânico de abafador. Não é o mesmo mecanismo. **Mas isto é engenharia, não parecer jurídico, e a doutrina dos equivalentes existe** — ver §7.7.
- **Os claims 3/4 (una corda) não afetam a Etapa 2**, mas afetam qualquer implementação futura de pedal esquerdo que mexa na dureza do martelo. Anote e siga.

`[FATO]` E os **MESMOS design-arounds D5/D6/D7** listados acima para a US8115092 continuam valendo para o motor de corda, porque a família é a mesma e a mãe é que carrega o método modal.

`[FATO]` **US 8,729,376 B2** — **VERMELHO NOVO, cobre justamente o SEU atalho.** Yamaha, prioridade 2011-03-24, **expira 2032-07-10, ATIVA (US e JP)**.

Você já tem um motor de samples maduro. O atalho óbvio é **usar o sample como sinal de excitação dentro de um loop de waveguide**. Esta patente é exatamente sobre isso. Claim 1 (do PDF oficial):
> *"a LOOP PART including at least a DELAY ELEMENT ... configured to receive an EXCITATION SIGNAL ...; a WAVEFORM MEMORY that stores first waveform data representing sound which is generated by a NATURAL MUSICAL INSTRUMENT and which CONTAINS RESONANCE ...; and a WAVEFORM PROCESSING UNIT that REMOVES ... one or more frequency component having a level that DOES NOT REACH A PREDETERMINED LEVEL ... a SEPARATOR that separates the first waveform data into ... a plurality of frequency bands; a plurality of LEVEL CONTROLLERS ... DAMPING the waveform data when the detected level does not reach the predetermined level"*

`[INFERÊNCIA]` **O elemento que estreita**: o claim exige um **gate espectral por bandas** para extrair a excitação de um sample real que CONTÉM ressonância. Design-arounds: (a) não derivar a excitação por gating espectral; (b) usar excitação sintética de um modelo de martelo; (c) usar um sample de excitação já limpo por outro meio.

### 7.4 "Publicado" vs "patenteado" — a diferença que todo mundo confunde

`[FATO]` São eixos **DIFERENTES**:

1. **Publicação num paper** é sobre DIREITO AUTORAL e ARTE ANTERIOR. **Não é licença de patente.**
   - 35 U.S.C. 102(a)(1), verbatim: *"A person shall be entitled to a patent unless — (1) the claimed invention was patented, described in a printed publication, or in public use, on sale, or otherwise available to the public before the effective filing date."*
2. **MAS o autor do paper pode ter patenteado a mesma coisa.** Existe período de graça de 1 ano nos EUA — 35 U.S.C. 102(b)(1). **Na Europa não existe.**
3. **Consequência em uma frase**: implementar um paper NÃO te dá imunidade de patente automaticamente; mas se o paper é **anterior** à prioridade da patente, ele é sua munição de invalidade.
4. **LER uma patente é 100% legal e explicitamente encorajado** — a barganha da patente é divulgação em troca de monopólio temporário. O que não pode é praticar **TODOS** os elementos de uma reivindicação viva, na jurisdição dela, durante a vigência.

`[FATO]` **Territorialidade** — 35 U.S.C. 271(a) verbatim: *"Whoever without authority makes, uses, offers to sell, or sells any patented invention, WITHIN THE UNITED STATES or imports into the United States ... infringes."*

**Patente é direito NACIONAL. Não existe "patente mundial"** — o PCT (WO) é só procedimento de depósito.

Se o ChurchDAW for vendido **só no Brasil**, as três patentes vermelhas não alcançam a venda. Venda para cliente nos EUA, ou publicação na App Store / Google Play com disponibilidade nos EUA, **aciona a jurisdição americana**.
`[NÃO CONFIRMADO]` O INPI brasileiro **não foi verificado diretamente**. Ausência de BR nas famílias do Google Patents é forte indício, não certidão.

### 7.5 O risco maior e mais imediato provavelmente NÃO é patente — é MARCA

`[FATO]` A Modartt não só modelou pianos — ela **LICENCIOU os nomes**. O site diz que os sons são *"authorized by Steinway & Sons, Bechstein, Petrof, Blüthner, Steingraeber, Grotrian, Hohner, among others"*, e vendem packs chamados literalmente "Steinway Model B".

**O que isso implica:**
- **MODELAR** fisicamente um Steinway D não é, por si, violação de marca.
- **NOMEAR** seu preset "Steinway D" (ou "Yamaha C7", "Bösendorfer Imperial") em produto comercial **é uso de marca alheia** e é onde as fabricantes efetivamente agem — é rápido, barato e elas fazem.
- "Pianoteq", "V-Piano", "SuperNATURAL", "COSM", "Physis", "VRM" são marcas registradas.
- Nomes neutros ("Concert Grand", "Studio Upright", "German D-274") **evitam o problema inteiro sem custo técnico nenhum**.

**Diferença de gravidade**: patente expira e é territorial. **Marca é renovável indefinidamente e é ativamente fiscalizada.** Uma notificação de marca chega por e-mail de escritório de advocacia semanas depois do lançamento; uma ação de patente é rara contra dev solo e normalmente só vem depois que você tem faturamento visível.

### 7.6 Risco no produto que JÁ EXISTE

`[NÃO CONFIRMADO]` **VISCOUNT / ÓRGÃO DE TUBOS.** Existe a **US7442869B2** — *"Method and electronic device used to synthesise the sound of church organ flue pipes by taking advantage of the physical modeling technique of acoustic instruments"*. **O ChurchDAW já tem engine nativa de órgão de tubos.**

O documento **não foi aberto** — só o número foi localizado por busca. Status legal, escopo das claims e jurisdição **não verificados**. Mas dado que a Viscount afirma publicamente que a tecnologia Physis é "patenteada na Itália e em outros países", **isso merece verificação antes do próximo release do módulo de órgão**. Existe uma análise pública dessa patente por Colin Pykett (colinpykett.org.uk), também não aberta.

### 7.7 Onde PRECISA de advogado, e onde NÃO precisa

**PRECISA:**
1. **FTO (Freedom To Operate) formal** — busca profissional em bases pagas (Derwent, Orbit, PatSnap), não Google Patents. Custo típico: alguns milhares de dólares. Decisão de negócio, não técnica.
2. **Claim chart**: sua arquitetura ao lado de cada elemento da claim 1 de US7915515, US8115092, US8895831 e US8729376.
3. **Doutrina dos equivalentes** — mesmo faltando um elemento literal, pode haver infração se o substituto faz "substancialmente a mesma função, do mesmo modo, com o mesmo resultado". É onde a análise leiga sempre erra.
4. **Elegibilidade sob Alice/101** — reivindicações de DSP puramente matemáticas ficaram frágeis nos EUA depois de *Alice Corp. v. CLS Bank* (2014). É argumento de invalidade, usado só depois de processado.
5. **Confirmação oficial de status** — a certidão real vem do USPTO Patent Center, do Registro Europeu da EPO, e do INPI.

**NÃO PRECISA** (já fechado pelos fatos acima): usar waveguide/mesh 2D/commuted/feltro não-linear; usar STK; usar Faust gerando C++; **ler qualquer patente**.

`[INFERÊNCIA]` **Estratégia pragmática**: em fase de pesquisa, gaste **zero** com advogado. Só chame quando (a) decidir vender nos EUA e (b) o motor estiver arquiteturalmente estável — antes disso o claim chart fica obsoleto a cada refactor. **E considere apenas esperar: US7915515 cai em 2027-10-20, ou seja, daqui a ~14 meses.**

---

## 8. ENCAIXE NO CHURCHDAW

Esta seção veio de leitura direta do código. Os números de linha são reais.

### 8.1 A descoberta que define a arquitetura

**Uma camada é OU sampler OU motor nativo, nunca os dois.**

Em `renderVoices`, **`TimbresProcessor.h:9484`** entra no ramo Church Organ e termina em `continue` na linha **9906** — o laço de vozes sampled (**`TimbresProcessor.h:10316`**) **nunca vê aquela camada**.

**Portanto "sampler + modelo na mesma camada" NÃO pode ser uma `ChurchOrganFamily`.** Há duas portas de entrada distintas, e elas não são intercambiáveis.

### 8.2 PORTA A — Piano modelado puro = 5ª família Church Organ

Caminho de menor atrito para um motor autônomo. Todos os pontos de toque, com padrão pronto para copiar:

| Arquivo:linha | O que fazer |
|---|---|
| `ChurchOrganLayerProcessor.h:20-26` | enum `ChurchOrganFamily` — adicionar `Piano` |
| `ChurchOrganLayerProcessor.h:82` | `familyCount = 4` → `5` |
| `ChurchOrganLayerProcessor.h:424-427` | membros `tonewheel_/transistor_/pipe_/hybrid_` — adicionar `piano_` |
| `ChurchOrganLayerProcessor.cpp:1973-2024` | `renderActiveFamily` — mais um `case`, exatamente como `pipe_` na linha 1997 |
| `ChurchOrganLayerProcessor.cpp:805-808` | `validFamily` fecha sozinho (compara com `familyCount`) |
| `ChurchOrganLayerProcessor.h:88` | `stateSchemaVersion = 2` → `3` |
| `TimbresProcessor.h:3455` | `layerCategory[layerIndex] = "ORGAOS"` — precisa virar condicional, senão o piano aparece no menu de órgãos |
| `TimbresProcessor.h:3248-3271` | mapeamento de qualidade e rotary por família — o piano precisa cair no ramo "sem rotary" |

**Compatibilidade de preset — a metade boa e a metade perigosa.** `[FATO]` Leitura de `ChurchOrganLayerProcessor.cpp:2250-2263`: a condição é `schemaProperty > stateSchemaVersion` seguida de `return false`.

- **Para trás (bom)**: blobs **v2** de presets antigos continuam restaurando num build v3. Sem problema.
- **Para frente (perigoso)**: qualquer preset salvo **depois** do bump vira v3, e um build revertido para v2 **rejeita o blob**. E o `return false` **aborta o restore da CAMADA INTEIRA**, não só da parte nova — a camada volta ao default, **sem erro visível**.

> **O bump é compatível apenas PARA FRENTE. Presets salvos após a Etapa 4 NÃO restauram em build revertido.**

**Por que isso importa mais aqui do que num projeto normal**: este projeto **não é repo git e não tem backup**, e o dono precisa de um build funcionando no domingo. **"Reverter o build" é o plano de emergência principal** — e o bump de schema o sabota silenciosamente. Isto não é uma linha tranquila de checklist.

**Pré-requisitos da Etapa 4, antes de bumpar:**
1. Guardar o **BINÁRIO** conhecido-bom, **datado** — não só a cópia da árvore de fontes. Reverter fonte exige recompilar, e no sábado à noite não dá tempo.
2. **Exportar os presets de culto em v2** antes do bump, e guardá-los separados.
3. **Só bumpar o schema quando o motor estiver estável.** Enquanto o piano estiver em desenvolvimento, mantenha a persistência dele **fora** do blob da família (arquivo/propriedade separada), para que o schema da família não precise mudar.

**O contrato do engine já está definido** pelo `PipeOrganEngine`: `prepare(spec)` / `reset()` / `render(view)` sem alocação, eventos com `sampleOffset` em fila fixa (`PipeOrganEngine.h:19-34`, fila de 1024 em `PipeOrganTypes.h:11`), `renderUsesFixedStorage = true` (`PipeOrganEngine.h:86`), diagnostics por bloco. Um `PianoEngine` que respeite a mesma assinatura pluga com poucas dezenas de linhas.

### 8.3 PORTA B — Híbrido = pós-processador POR CAMADA no chain de canal

**Esta é a porta que interessa primeiro.**

O laço de FX de canal fica em **`TimbresProcessor.h:10847`**, iterando `layerBuffers[l]` **depois** que a fonte (sampler ou órgão) já preencheu. Ali já existem exatamente os vizinhos certos, todos arrays indexados por camada:

| vizinho | linha |
|---|---|
| `manualEqs[l]` | `TimbresProcessor.h:10890` |
| `rotarySpeakers[l]` | `:10885` |
| `delayBufs[l]` | `:10913` |
| `reverbSendSmooth[l]` | `:10851` |

Um **`pianoResonators[l]`** entra nesse mesmo padrão, lendo `layerBuffers[l]` e somando a ressonância de volta.

**O estado musical que você precisa já está no escopo**: `struct Voice` (**`TimbresProcessor.h:7433-7464`**, campos `note` / `soundingNote` / `heldByPedal` / `released`), `pedalDownAtBlockStart` e `*globalSustain` (usados em `:9501-9526`). **Ou seja: dá para saber quais cordas estariam livres para ressoar sem inventar nada.**

### 8.4 O orçamento de vozes NÃO reusa — e é bom que não reuse

`maxVoices = 616` (**`TimbresProcessor.h:231`**, com `numLayers = 14` em `:209` e `voicesForLayer` 96/40 em `:226-229`) e `grabVoice` (**`:9089-9169`**) roubam por `stealScore` em unidade de **VOZ**.

**Um motor modal não tem essa unidade.** Uma nota A0 com 140 modos custa ~15× uma C7 com 10. **Se plugar o piano no orçamento de vozes existente, o voice stealing vai matar a nota errada.**

**O orçamento correto é em RESSOADORES ATIVOS, e ele fica DENTRO do `PianoEngine`** — igual ao `PipeOrganEngine`, que mantém sua própria `activeVoiceIds_` / `activeVoiceCount_` (**`PipeOrganEngine.h:233-237`**) e não conhece as 616 vozes do sampler. **O padrão já está estabelecido; basta não quebrá-lo.**

### 8.5 Duas boas notícias — riscos que a pesquisa previa e o código já resolve

1. **Denormais.** `juce::ScopedNoDenormals` já cobre o callback inteiro em **`TimbresProcessor.h:5060`**. Resolvido.
2. **Custo de piso.** Um banco simpático nunca desliga, o que anularia o atalho de canal ocioso em `:10870-10877`. Só que **camadas Church Organ JÁ pagam o chain todo sempre**: em `:9905` `layerHasAudio[l] = rendered`, e `rendered` é verdadeiro sempre que o adapter está preparado. **O precedente existe e o custo já foi aceito.**

### 8.6 Infraestrutura reutilizável que você já escreveu

| Em `OrganDspUtilities.h` | Linha | Serve para |
|---|---|---|
| `SmoothedValue` | :82 | rampas de parâmetro |
| one-pole por cutoff | :56 | filtro de perda, damper dependente de freq. |
| **`FixedFractionalDelay`** com Catmull-Rom de 4 pontos, capacidade potência de dois | :127-189 | **é literalmente uma linha de atraso de waveguide pronta** |
| `softPeakProtect` | :191 | proteção de saída |

O **guard de NaN por amostra com contador de diagnóstico** já tem forma canônica em **`PipeOrganEngine.cpp:364-370`** — copie exatamente.

E o padrão de **degradação por qualidade** já existe: `PipeOrganQuality::eco` (**`PipeOrganTypes.h:36-40`**), com o efeito em `renderPipe` (**`PipeOrganEngine.cpp:1415-1419`**) desligando a corda secundária. **Esse é o molde para o preset móvel do piano.**

### 8.7 O ativo que você tem e a Modartt não tinha

**`Zone`** (**`TimbresProcessor.h:6933-6968`**) guarda o áudio real **por nota e por faixa de velocity**, indexado por nota em **`zonesByNote`** (`:6983`), com `velLo`/`velHi` (`:6939`) e **`rmsDb` já medido por zona** (`:6950`).

**A rota B da literatura — extrair `(f_k, τ_k, A_k)` por ANÁLISE DE GRAVAÇÕES em vez de FEM — pede exatamente isso como entrada, e você já tem no disco, catalogado.**

Isso pula o pipeline de elementos finitos inteiro, que é a parte de doutorado. **É a diferença entre "impossível" e "difícil".**

### 8.8 Comparação de custo com o que já roda ao vivo

`[INFERÊNCIA]`, com números do próprio código:

> **AVISO SOBRE A VERSÃO ANTERIOR DESTA SEÇÃO.** Ela comparava o **PIOR** caso do órgão contra um caso quase **MELHOR** possível do piano, e concluía "CPU e RAM não são o obstáculo". A comparação era assimétrica e a conclusão estava inflada. Refeita abaixo.

**Lado do órgão — pior caso** `[INFERÊNCIA]`: `PipeOrganEngine` em registro cheio com coupler e 10 notas presas roda **3 manuais × 10 notas × 11 ranks = 330 tubos por amostra** (`renderVoices`, `PipeOrganEngine.cpp:1523-1606`), cada um com 2-4 lookups de wavetable + ruído + 3 one-poles → ordem de **500-600 MFLOP/s** a 48 kHz.

**Lado do piano — o pior caso de verdade.** O número honesto tem cinco multiplicadores que a versão anterior omitia. Pior caso de um culto = **pedal embaixo, duas mãos, 15-20 notas com baixo**:

| fator | multiplicador | fonte no documento |
|---|---|---|
| notas soando | 15-20 (não 10) | pior caso de culto |
| região | inclui o **grave** (teto de 130-140 modos, não 60) | §3.2 |
| uníssono × polarização | **× 2 a 6** por nota | §2.8 |
| banco simpático (piso) | **+30% do total, NÃO desligável** | §5.9 `[FATO]` Bank |
| tampo | **~100 seções (ordem 200), sempre ligado** | §5.11 |
| **camadas ativas** | **× nº de camadas de piano/Rhodes em split** | `numLayers = 14`, `TimbresProcessor.h:209` |

**O multiplicador que ninguém aplica**: o banco da Etapa 1 é `pianoResonators[l]`, indexado **POR CAMADA**. Duas ou três camadas de piano/Rhodes num split **multiplicam o piso** — e o piso, por definição, **não desliga**.

**Conclusão corrigida:**

> **CPU não é o obstáculo na Etapa 4 mono-camada, com corda única e teto de modos** — nesse regime o piano realmente sai mais barato que o órgão em registro cheio.
> **CPU passa a ser o obstáculo PRINCIPAL a partir do momento em que o banco simpático e o tampo ficam sempre ligados em mais de uma camada** — porque aí o custo é piso fixo, some com o dos outros motores, e não responde a voice stealing.

**PRÉ-REQUISITO DE RELEASE, não sugestão**: medir o consumo com **ZERO teclas pressionadas** (o piso), **na máquina usada no culto**, antes de a Etapa 1 sair da flag. Repetir a medição com o número real de camadas do preset de domingo. Se o piso sozinho já for desconfortável, o desenho está errado e nenhuma otimização de laço salva.

**Memória** `[INFERÊNCIA]`: aqui a folga é real e não depende de caso. As wavetables do órgão já ocupam **~1,08 MB** residentes (`PipeOrganEngine.h:215-218`). Uma tabela modal de 88 notas × 140 modos × (f, τ, A) são **~148 KB**. **RAM não é o obstáculo em nenhum cenário.**

---

## 9. CAMINHO RECOMENDADO

### 9.0 A DECISÃO REAL — e ela tem QUATRO opções, não duas

> **Versões anteriores deste documento comparavam só "híbrido vs. modelo puro" e recomendavam o híbrido "sem hesitar". Isso estava vendendo o projeto, não informando a decisão.** As duas opções omitidas provavelmente vencem as duas apresentadas. Estão abaixo.

`[INFERÊNCIA]` Custos em **homem-hora** (ver §9.0.1 sobre a unidade). "Retorno audível" = o que a congregação percebe no domingo.

| # | Opção | Custo | Retorno audível | Risco para o domingo |
|---|---|---|---|---|
| **A** | **Hospedar um piano modelado pronto** (Pianoteq via o VST hosting que a DAW **já tem** — §9.2, risco 2) | **~€200 + 2-6 h** de integração/teste | **Imediato e máximo.** Resolve os 4 problemas da §9.1(3) **hoje** | **Quase zero** |
| **B** | **NÃO FAZER NADA de piano** e gastar as horas no que já está aberto | 0 h de piano | Zero em piano; **alto em estabilidade** | **Negativo (reduz risco)** |
| **C** | **Construir o híbrido** (Etapas 0-3) | **~500-1.000 h** | Primeiro som audível em ~2 meses de trabalho parcial (Etapas 0+1 = 140-280 h) | Médio, gerenciável com flag |
| **D** | **Construir o modelo puro** (Etapas 4-5, **por cima da C**) | **~1.000-2.400 h** | Provavelmente **pior** que o sampler atual até o fim da Etapa 5 | Alto |

**Some as duas e encare o número**: C + D = **1.500 a 3.400 homem-hora**. A 10 h/semana, isso são **3 a 6,5 anos de calendário**; a 20 h/semana, **1,5 a 3 anos**. **A §9.2 diz "12-24 meses" — isso só fecha se o escopo parar na Etapa 4 e o ritmo for ~20 h/semana sustentado.** O roteiro completo 0-5 **não** cabe em 12-24 meses de trabalho parcial. Se esse número assusta, é porque deveria: é a informação mais importante desta seção.

**O que a opção B compraria com as mesmas 2-4 semanas da Etapa 1** — tudo já aberto e tudo mais caro que piano:
1. **O projeto não é repo git e não tem backup.** O próprio documento chama isso de *"o maior risco operacional do projeto"* (§9.2, risco 3) e diz que resolve em 10 minutos. **Enquanto isso for verdade, qualquer hora gasta em piano é hora gasta em cima de uma base que pode evaporar.**
2. **Ruído metálico em aberto no Prophetic Ambience.**
3. **Erro 1114 de DLL em investigação.**

**A pergunta honesta antes de escrever a primeira linha:**

> **A razão para construir o próprio motor provavelmente NÃO é "vai soar melhor".** Quase certamente não vai — não contra 20 anos da Modartt. A razão real é **"não quero depender de licença de terceiro dentro do meu produto"**: você não pode embutir Pianoteq no ChurchDAW e vender, e depender de o usuário comprar e instalar um VST de terceiro é um produto pior.
>
> **Isso é um argumento comercial legítimo e suficiente.** Mas precisa estar escrito como tal, e não disfarçado de argumento técnico. Se o objetivo é você mesmo tocar melhor no domingo, **a opção A vence com folga.** Se o objetivo é o ChurchDAW ter um piano próprio que se possa vender, então C — e só C, com D como talvez-um-dia.

**Recomendação, com a ordem em que eu faria:**
1. **`git init` e backup do binário HOJE** (opção B, custo ~1 h) — pré-requisito de qualquer outra coisa.
2. **Comprar e hospedar o Pianoteq** (opção A) — resolve o domingo imediatamente e te dá a **referência auditiva** contra a qual medir tudo que você construir depois.
3. **Só então**, e se o argumento comercial se sustentar, a Etapa 0 da opção C.

**Nunca a D antes de a C ter passado em prova de campo.**

#### 9.0.1 A unidade das estimativas — leia antes de acreditar em qualquer número deste documento

`[INFERÊNCIA]` **Todas as estimativas de esforço deste documento são INFERÊNCIA, e são a parte menos verificável dele.** Numa versão anterior elas eram as **únicas** afirmações sem marca de confiança — justamente as que menos mereciam essa isenção. Corrigido.

**A unidade é HOMEM-HORA, não semana de calendário.** A diferença é de 3-5×: "2 a 4 semanas" a 8-12 h/semana (dev solo, à noite e no sábado) são **~20-50 h**, não 80-160 h. Onde este documento diz "semanas", leia a coluna de horas.

> **CALIBRE COM O SEU PRÓPRIO HISTÓRICO ANTES DE PLANEJAR.** A pergunta que fecha isso: **quanto tempo REAL levou o `PipeOrganEngine`, do primeiro arquivo até o primeiro domingo em que rodou ao vivo?** Esse número é o seu multiplicador. **Estimativa sem baseline medido do próprio histórico é chute com aparência de plano** — inclusive as deste documento.

**Onde as estimativas antigas eram mais implausíveis**: a Etapa 4 ("2 a 3 meses") pedia banco modal com SoA+SIMD desde a primeira linha, martelo com duplo integrador, tabela calibrada de 88 notas, contrato de engine, 5ª família com estado/preset/GUI/menu, voice stealing por **CUSTO** (estrutura que não existe no código hoje) e tier degradado para móvel. A 8-12 h/semana isso daria 100-150 h — **implausível por um fator grande**. As estimativas abaixo foram refeitas em horas.

---

### 9.1 Se você for construir: por que o HÍBRIDO e não o modelo puro

**Assumindo que a decisão da §9.0 foi "construir" (opção C), então o caminho HÍBRIDO vale muito mais a pena que o modelo puro.**

Três razões, e a terceira é a que decide:

**(1) O gargalo do Pianoteq não é DSP — é calibração.** O laço modal é ~7 flops por parcial; você escreve DSP mais difícil que isso hoje (`renderPipe` em `PipeOrganEngine.cpp:1325` faz interpolação de wavetable, envelope de fala, chiff filtrado, LFO de vento e tremulante **por tubo, por amostra**). O que a Modartt tem e você não tem são ~20 anos de ajuste de `(f_n, d_n)` por nota, feito por um matemático que **também era afinador de piano profissional**, com pianos de cauda emprestados e engenheiros de gravação. **Nenhuma quantidade de talento em C++ substitui isso.** Todo projeto solo de piano modelado verificável morre exatamente aí — o OpenPiano, que é o análogo aberto mais próximo, usa os MESMOS parâmetros de martelo nas 52 notas que instancia, e o próprio autor escreve que soa *"um piano estranhamente desafinado"*.

**(2) O modelo puro entrega valor só no FIM.** 12-24 meses de trabalho de fim de semana, durante os quais o resto da DAW não evolui e os bugs conhecidos continuam abertos. E depois de 6 meses o resultado vai soar como "sintetizador de piano" e o motor de samples vai continuar soando melhor no domingo. **Isso não é derrota — é o estado da arte para um dev solo.**

**(3) As quatro coisas que sampler faz MAL são exatamente as quatro que modelagem faz BEM e BARATO:**
- ressonância simpática com pedal
- comportamento do abafador no note-off (e meio-pedal)
- degrau de timbre entre camadas de velocity
- re-ataque de nota repetida com corda em movimento

E existe **respaldo publicado direto** `[FATO]`: Zambon, Lehtonen e Bank (DAFx-08) afirmam explicitamente que o banco de ressonância do pedal é aplicável **como pós-processamento sobre síntese por AMOSTRAS** — filtrar o sinal seco por `(H_ped + 1)`. **Não é gambiarra; é o método publicado.**

**O que NÃO é verdade** é a parte do "10% do esforço": ressonância simpática decente é **2-4 semanas**, não 2-4 dias. Mas 2-4 semanas por um efeito que o ouvinte percebe imediatamente é um retorno que a rota pura nunca oferece.

**A ordem abaixo foi escolhida para que as etapas caras estejam no FIM e as de maior benefício por hora estejam no começo.** Se parar na Etapa 2, você fica com um piano sampled com ressonância simpática e abafador modelado no banco simpático — **um recurso que o motor de samples do ChurchDAW não tem hoje**. A rota pura não tem essa propriedade de parada.

*(Uma versão anterior dizia que isso seria "melhor que 90% dos samplers". Foi removido: era afirmação sem fonte e provavelmente falsa — ressonância simpática e amostras de release/ressonância são recurso comum em bibliotecas modernas como VSL Synchron, Garritan CFX e Ravenscroft. **A comparação relevante é contra o próprio produto, não contra o mercado.**)*

### 9.1.1 REGRA DE PARADA — obrigatória, e escrita ANTES de começar

> **"Cada etapa é abandonável sem prejuízo" é uma propriedade que precisa ser OPERACIONALIZADA, senão é só uma frase.** Numa versão anterior, só a Etapa 0 tinha critério de parada. Sem critério explícito, o dono cai no viés de **custo afundado** e continua ajustando por meses uma etapa que já fracassou.

**Toda etapa daqui para baixo tem obrigatoriamente três coisas, definidas ANTES da primeira linha de código:**

| | O quê | Por quê |
|---|---|---|
| **(a)** | **Critério de aceitação verificável** — A/B **cego**, no **PA da igreja**, **não no fone** | O modo de falha mais provável não é NaN nem estouro de ressoadores. É **soar impressionante no fone por cinco minutos e virar zumbido/lama no PA**, e ser desligado depois de dois domingos. Fone e PA discordam exatamente na região grave, que é onde tudo isto acontece. |
| **(b)** | **Teto de tempo em HORAS**, declarado antes | Passou do teto sem atingir (a), **reverte**. Não "só mais um fim de semana". |
| **(c)** | **Prova de campo: dois domingos com a flag ligada.** Se for desligado ao vivo **uma vez**, a etapa está **REPROVADA** | É o único teste que conta. Tudo antes disso é bancada. |

**Escreva (a), (b) e (c) num arquivo, com data, antes de começar cada etapa. Quando o custo afundado chegar, esse arquivo é a única defesa.**

---

### ETAPA 0 — Banco de provas OFFLINE (`[INFERÊNCIA]` **60 a 120 h**, risco ZERO para a DAW)

> **REESTIMATIVA.** A versão anterior dizia "3 a 5 dias". **Isso é o custo de escrever o código, não o de obter uma extração que não minta.** Escrever o extrator ingênuo é mesmo 3-5 dias; fazer com que ele produza `τ_k` confiável no grave é **2-3 semanas**. O número honesto é 60-120 h.

**Fora do JUCE e fora da DAW.** Um `.cpp` standalone (ou script Python) que lê samples da sua biblioteca, extrai uma tabela modal, ressintetiza com um banco de biquads e escreve WAVs lado a lado:
```
p_k  = exp(−1/(τ_k·fs)) · exp(j·2π·f_k/fs)
a1_k = −2·Re{p_k};  a2_k = |p_k|²;  b_k = (A_k/fs)·Im{p_k}
y[n] = b_k·x[n−1] − a1_k·y[n−1] − a2_k·y[n−2]
```

#### O erro de projeto que a versão anterior desta etapa continha

> A versão anterior mandava extrair *"os primeiros ~60 parciais por pico-picking em FFT da cauda + inclinação do log da envoltória"*, de **UMA** nota, e decidir em 30 segundos de escuta. **Esse método é contradito pela §2.8 deste mesmo documento**, e o portão de decisão que ele alimenta dá resposta errada nos dois sentidos.

`[FATO]` (§2.8) **Um parcial de piano real NÃO é uma senoide amortecida.** É a soma de **2 a 6 modos acoplados** (2 polarizações × 2-3 cordas), o que produz **batimento** e **decaimento duplo**. Ajustar **uma reta** ao log da envoltória de um parcial que bate e tem decaimento duplo dá um `τ` **sem significado físico** — e isso acontece exatamente no **grave e no médio**, que é a região que decide o resultado.

**Os dois modos de falha do portão antigo** `[INFERÊNCIA]`:
- **FALSO POSITIVO no agudo**: uma C6 tem ~16 modos, sem decaimento duplo relevante. Soa ótimo com 8 parciais e você conclui que o método serve.
- **FALSO NEGATIVO no grave**: uma A0 precisa de centenas de parciais e é toda decaimento duplo. Soa opaca e batendo errado, e você conclui que **modelagem modal não serve** — quando o que não serve é o seu extrator.

**E o documento antigo mandava PARAR O PROJETO INTEIRO com base nisso** (*"se soar mal aqui, você economizou um ano"*), sem dar nenhuma forma de distinguir **"o método não serve"** de **"meu extrator está ruim"** — que é exatamente a distinção que vale o projeto.

#### Especificação corrigida da Etapa 0

1. **Conjunto de notas de teste obrigatório: no mínimo A1, C3, C5, C7.** **Nunca uma nota só.** Sem cobrir grave e agudo o teste não informa nada.
2. **Número de parciais proporcional a `fs/(2·f₀)`**, não fixo em 60. Sessenta parciais cobrem uma C6 com folga e não chegam perto de uma A1.
3. **Modele cada parcial com 2-3 polos desde a PRIMEIRA versão** — ou, equivalentemente, ajuste **dupla exponencial + envoltória de batimento**. Um polo por parcial é o erro que gera o falso negativo no grave. Isto não é refinamento posterior; é requisito de dia 1.
4. **Precisão**: `double` para coeficientes e estado (§4.3.1).

#### Critério de aceitação — DIAGNÓSTICO, não binário

Troque *"soou bem / soou mal"* por:

| resultado | leitura |
|---|---|
| **C3 com 2 polos/parcial NÃO converge** | **o problema é o EXTRATOR, não o método.** Conserte o extrator; **não** cancele o projeto |
| C3 converge, C5/C7 convergem, mas o som ainda não convence | aí sim é evidência contra o **método** |
| tudo converge e soa | siga |

**Métrica objetiva obrigatória** (o que a Etapa 0 realmente precisa e não tinha):
- **SNR de reconstrução por parcial** (original vs. ressíntese);
- **diferença de T60 por parcial** entre original e ressíntese;
- **limiar declarado por escrito antes de rodar** — senão o critério vira "achei bonito".

**RISCO**: zero para a DAW. Nada é compilado dentro do ChurchDAW, nada roda no domingo. **É o passo com melhor relação informação/risco de todo o projeto e por isso tem que ser o primeiro.**

*(A tabela de regressão do martelo — u_max/F_max/τ por nota e velocidade — foi **movida para a Etapa 4**, que é a única etapa que constrói um simulador de martelo. A Etapa 0 ressintetiza a partir de gravações e não tem K, nem p, nem massa, nem contato: não há nada aqui contra o que rodar aquela tabela.)*

---

### ETAPA 1 — Ressonância simpática e pedal como PÓS-PROCESSADOR de canal (`[INFERÊNCIA]` **80 a 160 h**, risco MÉDIO)

#### TAREFA 1 — antes de QUALQUER linha de DSP: auditar o material de entrada

> **Esta é a primeira tarefa e é um portão. Se pular, você gasta as 80-160 h brigando com um sintoma cuja causa não está no seu código.**

`[FATO]` Zambon, Lehtonen & Bank (DAFx-08) afirmam que o banco de ressonância do pedal se aplica como pós-processamento sobre síntese por amostras. **Mas o método publicado tem uma PRÉ-CONDIÇÃO que a versão anterior deste documento não mencionava: ele pressupõe material SECO** — gravado **com os abafadores agindo** e **sem pedal**.

`[FATO]` **As bibliotecas reais não são secas.** A Salamander, que a §6.5 recomenda, inclui **69 arquivos de "ressonância de release"** — ou seja, já traz ressonância gravada. E como qualquer gravação com microfone a ~12 cm das cordas, já contém **o tampo, a sala e, em vários casos, a ressonância simpática do piano original**.

`[INFERÊNCIA]` **Consequência**: alimentar `layerBuffers[l]` num banco simpático nesse caso **não adiciona o efeito ausente — soma um segundo piano por cima do que já está gravado.** O resultado prático é **embolamento no grave e cauda metálica**. **É exatamente o modo de falha que já está aberto em outro módulo desta mesma DAW (ruído metálico do Prophetic Ambience).**

**Checklist do portão:**
1. A biblioteca alvo tem camadas gravadas **com abafador** (pedal em cima)?
2. As amostras de **release/ressonância** podem ser **desativadas**?

| resposta | rota | custo |
|---|---|---|
| **Sim aos dois** | Rota A — o método publicado se aplica direto | as 80-160 h abaixo |
| **Não** | Rota B — a etapa **muda de natureza**: vira *"atenuar a ressonância JÁ GRAVADA e substituir pela modelada"* | **bem mais cara e sem receita publicada — reorce antes de começar** |

**Documente qual rota você está seguindo antes de escrever código.**

#### TAREFA 2 — o banco

Novo arquivo `Source/Piano/PianoResonatorBank.h`, instanciado como `pianoResonators[l]` no padrão de `manualEqs[l]` / `rotarySpeakers[l]`, chamado dentro do laço de FX em **`TimbresProcessor.h:10847`**, lendo `layerBuffers[l]`.

> **REQUISITO DE PRECISÃO (§4.3.1), e aqui ele é crítico**: a razão de existir deste banco é **o grave com τ longo e pedal embaixo** — precisamente o caso que quebra em float32. **Coeficientes e estado em `double` para todo ressoador com τ acima de ~2 s.** Inclua o teste de 60 segundos de sustentação medindo desvio de frequência e de τ.

- Banco de **300-800 biquads** (4 a 32 ressoadores por tecla, só do grave ao médio). `[FATO]` Bank estima 700-1200 ressoadores até 10 kHz para a calibração "fisicamente informada" — e o efeito do pedal só é relevante nas faixas grave e média.
- **Acoplamento UNIDIRECIONAL**: a soma da camada alimenta os ressoadores, nunca o contrário. `[FATO]` Bank: *"structurally stable ... the coupled model is stable for ANY choice of the coupling parameters."* **Sem realimentação, portanto sem risco de explodir.**
- Ganho do banco controlado por `pedalDownAtBlockStart` / `*globalSustain` (já no escopo em `:9501-9526`) e pelas notas soltas.
- Matriz de acoplamento por região: **R = 8 regiões ⇒ 64 mult + 64 add por amostra.** Desprezível.
- **Atrás de flag por camada, OFF por padrão.**

**ENTREGA**: o piano sampled passa a **FLORESCER** quando o pedal desce, e as notas soltas passam a ressoar por simpatia com as tocadas. É o efeito mais audível e mais imediatamente "caro" que existe em piano, e é exatamente o que nenhum sampler entrega. **Vale para os pianos, para os Rhodes e para o órgão de tubos também.**

**Lembre da física da seção 2.10**: com o pedal levantado o decaimento de cada parcial **AUMENTA** (fica mais curto) na região média. Se você só aumentar o sustain, soa errado.

**MITIGAÇÕES**: off por padrão; guard de NaN por bloco no estilo `PipeOrganEngine.cpp:364-370`; reset do banco em all-notes-off; teto duro de ressoadores.

**CRITÉRIOS DA §9.1.1 PARA ESTA ETAPA:**
- **(a) Aceitação**: A/B **cego no PA da igreja** (não no fone), com pedal embaixo e acorde grave sustentado. Critério: **não embolar o grave**. Medir também o **piso de CPU com zero teclas**, na máquina de culto, com o número real de camadas do preset de domingo (§8.8).
- **(b) Teto**: **160 h.** Passou disso sem (a), reverte e fica só com a Rota A documentada.
- **(c) Prova de campo**: dois domingos com a flag ligada. **Desligou ao vivo uma vez → etapa reprovada.**

---

### ETAPA 2 — Abafador e meio-pedal modelados (`[INFERÊNCIA]` **80 a 160 h**, risco **MÉDIO**)

Estender o banco da Etapa 1:
- **decaimento dependente de frequência no note-off** (abafador real abafa mais os agudos): `d_n' = d_n + δ(f_n)`;
- **damper caindo com QUIQUE** — ~80 ms depois do ataque, com bounce (`[FATO]` Askenfelt & Jansson), em vez de corte instantâneo;
- **meio-pedal como interpolação contínua** do coeficiente de amortecimento;
- notas mais agudas **sem damper nenhum** (`[FATO]`).

Usa os campos `released` / `heldByPedal` já existentes em `struct Voice` (**`TimbresProcessor.h:7445-7446`**).

**ENTREGA — e ela é MENOR do que a versão anterior deste documento vendia.** `[INFERÊNCIA]` São **dois caminhos com naturezas diferentes**, e misturá-los foi o erro:

| caminho | o que dá para fazer | é abafador modelado? |
|---|---|---|
| **Banco simpático** (cordas que **NÃO** estão sendo tocadas) | abafador modelado de verdade: `d_n' = d_n + δ(f_n)`, quique, meio-pedal contínuo | **SIM — real** |
| **Nota TOCADA** (que é um sample) | passa-baixa variante no tempo + envelope no release | **NÃO — aproximação** |

> **Não é possível alterar as taxas de decaimento modal de um sample já gravado.** A nota que você toca continua sendo um sample com fade filtrado. **Meio-pedal com efeito correto sobre a nota TOCADA só existe a partir da Etapa 4**, quando a nota tocada também for modal.

Redação honesta da entrega: **"abafador modelado no banco simpático (real) + release filtrado dependente de frequência no sample (aproximação)"**. Ainda assim é ganho real — para acompanhamento de culto, que é 90% pedal e nota sustentada, muda a sensação de tocar.

**RISCO: MÉDIO** (reclassificado — a versão anterior dizia BAIXO, e isso contradizia o próprio documento em dois pontos):

1. **Risco de não convergir.** `[FATO]` A §2.10 afirma que **meio-pedal é problema EM ABERTO na academia** (Zambon DAFx-08 e Bank TASLP 2010 listam como trabalho futuro), e esta etapa usa isso como argumento de venda (*"você estaria à frente do que está publicado"*). **Não se pode simultaneamente reivindicar que se vai resolver um problema aberto e estimar 2-4 semanas com risco baixo.**
   > **Critério de parada específico**: se depois de **3 fins de semana** o meio-pedal não estiver musicalmente convincente, **entregue só o abafador dependente de frequência e adie o meio-pedal**. Isso é sucesso parcial, não fracasso.
2. **Risco JURÍDICO — que a versão anterior omitia ao dizer "o risco é musical, não numérico".** `[FATO]` O **claim 1 da Yamaha US8895831 é literalmente a definição de meio-pedal** (coeficiente viscoso do abafador variando com curso de tecla **E** curso de pedal, de forma dependente do tempo), **ATIVA até 2030-10-07** — ver §7.3, onde os quatro claims independentes estão agora lidos verbatim.
   > **Condição para a Etapa 2**: ou você adota o design-around da §7.3 (envelope de amortecimento sobre o decaimento modal, **não** coeficiente viscoso de um modelo mecânico de abafador), **ou** o meio-pedal sai **OFF por padrão** enquanto não houver decisão sobre venda nos EUA. Fora dos EUA a restrição não alcança (§7.4).
3. **Risco musical** (afinar o tempo de queda de ouvido) — este continua existindo, só não é o único.

**CRITÉRIOS DA §9.1.1 PARA ESTA ETAPA:**
- **(a) Aceitação**: A/B cego no PA; o release tem que soar como abafador, não como fade.
- **(b) Teto**: **160 h**, com o corte parcial dos 3 fins de semana para o meio-pedal.
- **(c) Prova de campo**: dois domingos. Desligou ao vivo uma vez → reprovada.

---

### ETAPA 3 — Variação CONTÍNUA de timbre por velocity

> **ESTA ETAPA FOI QUEBRADA EM DUAS.** A versão anterior a orçava em "4 a 8 semanas" precificando o **DSP** e ignorando o custo real, que é o **pipeline de análise**. A §9.2 do próprio documento reconhece que *"nenhum dos papers de referência descreve o pipeline de extração"* — ou seja, admite que a parte mais cara **não tem receita publicada** — e mesmo assim orçava a etapa como se fosse trabalho de DSP.

#### ETAPA 3a — Pipeline de extração em lote (`[INFERÊNCIA]` **200 a 400 h**, risco ALTO)

**Esta é a etapa mais cara e menos audível de todo o roteiro, e precisa aparecer como tal.**

"Todas as camadas de velocity de cada nota" são **~1.400 extrações** na Salamander (88 notas × 16 camadas), cada uma sujeita a toda a fragilidade descrita na Etapa 0, e **nenhuma delas pode ser inspecionada à mão**. O que isso exige, e nada disso é DSP:

1. **Extrator batch não supervisionado** (a Etapa 0 é supervisionada e roda em 4 notas).
2. **Detector automático de extração ruim** — sem isso você não sabe quais das 1.400 estão mentindo.
3. **Ferramenta de inspeção e reparo manual** dos casos que o detector pegar.
4. **Suavização de `τ_k`** em **duas** direções: entre **notas vizinhas** *e* entre **camadas de velocity** da mesma nota. Descontinuidade em qualquer uma das duas é audível como degrau.

**Entrega da 3a**: uma tabela `(f_k, τ_k, A_k)` por nota **e por velocity**, auditada. **Zero mudança audível no produto.** É infraestrutura.

#### ETAPA 3b — Camada modal aditiva e lei de morphing (`[INFERÊNCIA]` **80 a 160 h**, risco MÉDIO)

Com a tabela da 3a pronta: obter como o espectro (`a_k` por parcial) varia com a velocity, ajustar uma lei simples por nota, e usar essa lei para **MORFAR** entre as camadas de sample em vez de crossfade cru.

Complementarmente, o brilho do ataque segue a física do feltro (§2.2):
```
f_knee ∝ v^((p−1)/(p+1))
```
**Use os 3 âncoras p do Bank interpolados, não a reta `p(tecla)`** — e leia a §2.2 antes: as duas fontes **divergem de −7% a +13%**, e a reta diverge dos âncoras em até 13% no meio do teclado. *(Uma versão anterior desta etapa afirmava validação "dentro de 4% em todo o teclado". Esse número não se sustenta com os dados do próprio documento.)*

**ENTREGA**: **some o degrau de timbre entre camadas de velocity**, que é a assinatura sonora nº 1 de "isto é um sampler". Você tem 14 camadas hoje; um piano real tem infinitas. **Este é o item onde o híbrido chega mais perto do Pianoteq de verdade**, porque a resposta contínua ao toque é literalmente o que a modelagem física entrega e o sampling não.

**RISCO MÉDIO**: toca o caminho de velocity do sampler (`velLut` em **`TimbresProcessor.h:6988`**, `buildVelocityLut`, `findZone`), que é código maduro e calibrado. **Fazer como estágio ADITIVO** (uma camada modal fina somada por cima) em vez de alterar a seleção de zona **reduz muito o risco de regressão**.

---

### ETAPA 4 — `PianoEngine` modal autônomo (`[INFERÊNCIA]` **400 a 800 h**, risco MÉDIO-ALTO)

> **REESTIMATIVA.** A versão anterior dizia "2 a 3 meses", o que a 8-12 h/semana daria 100-150 h. Esta etapa pede banco modal com SoA+SIMD desde a primeira linha, martelo com duplo integrador, tabela calibrada de 88 notas, contrato de engine, 5ª família completa com estado/preset/GUI/menu, **voice stealing por CUSTO (estrutura que não existe no código hoje)** e tier degradado para móvel. **100-150 h é implausível por um fator grande.**

5ª família Church Organ. Corda única, sem tampo, sem não-linearidade.

- Banco de biquads por **invariância ao impulso** (obrigatoriamente, não bilinear — ver §3.2).
- Martelo físico `F = K·u^p` com duplo integrador.
- Coeficientes vindos da tabela extraída na Etapa 0/3a.
- **Layout SoA e SIMD desde a primeira linha.**
- Contrato idêntico ao `PipeOrganEngine` (`PipeOrganEngine.h:19-34`).
- **Re-strike**: `z_n += α_n^novo` — **NÃO resete o estado.**
- **REQUISITO DE PRECISÃO (§4.3.1)**: `double` em coeficientes e estado para todo ressoador com τ acima de ~2 s — o grave inteiro. Inclua o teste de 60 s de sustentação.

**Teste de regressão do martelo** `[INFERÊNCIA]` — *(movido da Etapa 0, que não constrói simulador de martelo nenhum e portanto não tinha contra o que rodar esta tabela).* Calculada por integração exata do contato contra **corda rígida** com os 3 âncoras de martelo. **Esta é a etapa que constrói o martelo; se seu simulador sair fora, tem bug:**

| nota | v (m/s) | u_max (mm) | F_max (N) | τ (ms) | f_max ≈ 0,84/τ (Hz) |
|---|---|---|---|---|---|
| C2 | 1,0 | 0,574 | 14,1 | 1,589 | 529 |
| C2 | 4,0 | 1,331 | 97,2 | 0,920 | 913 |
| C4 | 1,0 | 0,388 | 13,4 | 1,059 | 793 |
| **C4** | **4,0** | **0,858** | **97,0** | **0,585** | **1436** |
| C6 | 4,0 | 0,515 | 136,7 | 0,341 | 2465 |
| C6 | 6,0 | 0,631 | 251,1 | 0,278 | 3019 |

(A constante 0,8375 foi derivada maximizando numericamente o espectro de choque residual de um pulso sen², e validada contra os dados do próprio Russell & Rossing. Caveat: é o limite de **corda rígida**; numa corda real o contato dura mais, sobretudo no grave.)

**ENTREGA**: piano 100% sintetizado, zero streaming de disco, carregamento instantâneo, e **re-ataque de nota repetida CORRETO** (o martelo bate numa corda que já está vibrando — o efeito que mais denuncia sampler em trinado e nota repetida rápida). `[FATO]` Chaigne & Askenfelt, verbatim: *"repeated notes are obtained by re-initializing the hammer position to zero before striking the moving string. This feature is not available in today's commercial synthesizers."*

Provavelmente ainda inferior ao sampler no timbre geral, mas superior em resposta ao toque e latência de patch.

**RISCO MÉDIO-ALTO**: é onde o diff em `TimbresProcessor.h` aparece. Ver §8.2 para a lista exata — **incluindo o bump de schema, que é compatível apenas PARA FRENTE e sabota a reversão de build**. Fazer com **preset de domingo congelado**, **binário conhecido-bom datado guardado** e **presets de culto exportados em v2 antes do bump**.

---

### ETAPA 5 — Tampo, cordas do uníssono e longitudinais (`[INFERÊNCIA]` **600 a 1.600 h**, risco de ABANDONO **+ risco de REGRESSÃO**)

- **Tampo**: banco paralelo de 2ª ordem de polos fixos em escala logarítmica (~100 seções, ordem 200, projeto por mínimos quadrados em forma fechada) — **NÃO** convolução particionada. Latência zero, sem thread extra, e **reaproveita o mesmo kernel SIMD dos ressoadores de corda**.
- **Ressoadores secundários** para batimento e decaimento duplo. `[FATO]` Comece com a versão perceptual de **5-10 ressoadores** que Bank cita, não com o banco completo — o banco completo custa 30% da CPU total.
- **Longitudinais** por último: ~25% do custo e só importam no grave forte.

**ENTREGA**: o piano modelado passa de "reconhecível" para "convincente". É aqui que você teria um instrumento próprio de verdade.

**RISCO ALTO — e são DOIS riscos, não um.** A versão anterior dizia *"o risco é de ABANDONO, não de quebrar nada"*. Isso valia para as Etapas 0-3, que eram **aditivas atrás de flag**. **Não vale para a Etapa 5:**

1. **Risco de abandono** — o de sempre nesta etapa.
2. **Risco de REGRESSÃO** `[INFERÊNCIA]` — a Etapa 5 **modifica um `PianoEngine` que a Etapa 4 já colocou em produção e em preset de usuário**. Mexer nele passa a quebrar algo **que já está sendo usado no domingo**. É uma diferença de natureza em relação a todas as etapas anteriores.

**Consequências obrigatórias**: a Etapa 5 também entra **atrás de flag própria**, e o motor da Etapa 4 fica **congelado como fallback** — selecionável em runtime, sem recompilar.

Se parar na Etapa 2 ou 3, você já ganhou tudo que interessa para o domingo e não perdeu nada.

---

### 9.2 Obstáculos que podem MATAR o projeto

**1. CALIBRAÇÃO DAS 88 NOTAS.** Obter `(f_k, τ_k, A_k)` por nota e por velocity que soem como piano, sem degrau timbral entre notas vizinhas. **Não é DSP, é análise de sinal + ouvido + iteração.** É onde o Pianoteq gastou 20 anos e onde o OpenPiano desistiu. **Nenhum dos papers de referência descreve o pipeline de extração; Bank apenas diz que é possível.**

> **NÃO** tente derivar de parâmetros físicos (FEM, tensão, Young). **Extraia por análise das gravações que você JÁ tem.** Ferramenta OFFLINE separada, **nunca dentro do plugin**. Suavize `τ_k` entre notas vizinhas **e entre camadas de velocity** com uma lei 1/f (é o que Bank faz na poda: `T_min/f_k ≤ τ_k ≤ T_max/f_k`).
>
> **E aceite parar — mas com o critério DIAGNÓSTICO da Etapa 0, não com "não gostei".** Se a C3 com 2-3 polos por parcial não converge, o problema é o **extrator**, não o método, e parar aí seria jogar fora o projeto pelo motivo errado. Só é evidência contra o **método** se C3, C5 e C7 convergirem numericamente e mesmo assim não convencerem. **A informação custa 60-120 h e vale o projeto inteiro.**

**2. TEMPO DO DONO.** Dev solo mantendo uma DAW inteira (14 camadas, streaming, 4 famílias de órgão, licenciamento, updater, Android/iOS, VST hosting) e tocando em culto todo domingo. Um motor completo é 12-24 meses em tempo parcial, durante os quais o resto não evolui.

> É exatamente por isso que o caminho tem que ser **incremental com valor entregue em cada etapa**.

**3. RISCO PARA O DOMINGO.** Não é repo git e não tem backup. Um motor modal é **a primeira coisa no seu código que pode divergir numericamente** — realimentação, ressoadores com polo perto de |1|, e o banco simpático que nunca zera. Diferente do sampler, que no pior caso lê a amostra errada, **um ressoador instável cresce exponencialmente até o limitador**.

> Três defesas, todas com precedente no próprio código: (a) guard de NaN por amostra com contador, como `PipeOrganEngine.cpp:364-370`; (b) `ScopedNoDenormals` já ativo em `TimbresProcessor.h:5060`; (c) reset explícito do banco a cada note-off completo.
> **REGRAS DE PROCESSO — e as duas primeiras são mais fracas do que parecem.**
>
> As defesas que a versão anterior listava eram: (i) preset de domingo congelado, (ii) cópia datada da árvore. Ambas ajudam, **nenhuma das duas resolve o cenário que importa**:
> - **Um preset congelado não protege contra bug em código COMPILADO no mesmo binário** e rodando no chain de FX por camada, nem contra regressão vinda de outra parte do build da semana. Protege só o **caminho de sinal do piano**.
> - **A cópia datada da árvore protege o FONTE, mas voltar atrás exige RECOMPILAR** — que é exatamente o que não dá tempo de fazer no sábado à noite.
>
> **As regras completas, com a que faltava em primeiro lugar:**
> 1. **Manter o último BINÁRIO conhecido-bom, datado, NA MÁQUINA usada no culto, com atalho próprio.** Esta é a defesa que de fato resolve, e era a que não estava na lista. Reverter tem que ser um duplo-clique, não um build.
> 2. **`git init` ANTES da Etapa 0** — não como observação de rodapé. **Este é o maior risco operacional do projeto e resolve-se em 10 minutos.** Enquanto não for feito, nada mais nesta lista importa muito.
> 3. **Regra de calendário: não instalar build novo na mesma semana de culto.**
> 4. NUNCA como patch default; sempre atrás de flag.
> 5. Preset de domingo congelado que não passa pelo código novo.
> 6. Cópia datada de `D:\ChurchDAW_Source` antes de tocar em qualquer arquivo.
> 7. Antes do bump de schema da Etapa 4: **exportar os presets de culto em v2** (§8.2).

**4. OUTRAS IAs EDITAM OS MESMOS ARQUIVOS.** `TimbresProcessor.h` tem 18.531 linhas e é o arquivo mais quente da base. Um motor de piano tocaria `renderVoices` (`:9439`), o chain de canal (`:10847`), o carregamento de camada (`:3326`) e o enum de famílias — todos pontos de colisão.

> Manter **TODO** o código novo em arquivos NOVOS (`Source/Piano/PianoEngine.h/.cpp`, `Source/Piano/PianoResonatorBank.h`). O toque em `TimbresProcessor.h` deve caber em **menos de 30 linhas somadas**. **Se o diff em `TimbresProcessor.h` passar de ~50 linhas, o desenho está errado.**

**5. O ATAQUE.** É onde samplers ganham e modelos perdem, e é a região que o ouvido mais julga. `[FATO]` O componente `b(p,t)` da própria patente do Pianoteq é **SAMPLE pré-gravado**.

> Isto é argumento **a favor** do híbrido, não contra o projeto: manter o ataque em samples (onde seu motor é maduro) e modelar a cauda e a ressonância. **É literalmente a arquitetura que a patente descreve, só que com o sample fazendo mais trabalho.**

**6. ANDROID/iOS.** Um banco de 10.000 ressoadores num Cortex-A é 3-4 núcleos dedicados, e o orçamento **térmico** de 20 minutos de culto é o problema real, não o pico de FLOPS.

> Tratar móvel como **preset degradado desde o dia 1**, exatamente como você já faz com `PipeOrganQuality::eco` (`PipeOrganTypes.h:36-40`). Cortar teto de modos, desligar longitudinais, reduzir o banco simpático. **Não prometa paridade móvel.**

---

## 10. BIBLIOGRAFIA

**Apenas fontes que alguém confirmou ter aberto durante esta pesquisa.** Fontes citadas mas não abertas estão numa lista separada ao fim, marcadas como tal.

### 10.1 Lidas integralmente ou em seções substanciais

**Modal / tempo real (o caminho recomendado)**
1. **Bank, B., Zambon, S. & Fontana, F.** — *"A Modal-Based Real-Time Piano Synthesizer"*. **IEEE Trans. Audio, Speech and Language Processing 18(4):809-821, maio 2010.** DOI 10.1109/TASL.2010.2040524. PDF livre em `home.mit.bme.hu/~bank/publist/` (arquivo `taslp10.pdf`). — **LEIA ESTE PRIMEIRO. É o manual de implementação.**
2. **Bank, B.** — *"Direct Design of Parallel Second-order Filters for Instrument Body Modeling"*. **Proc. ICMC 2007, pp. 458-465.** — projeto do banco paralelo de tampo.
3. **Bank, B.** — **IEEE Signal Processing Letters 18(2):138-141, 2011** — versão em frequência do projeto por mínimos quadrados.
4. **Bank, B. & Ramos, G.** — **IEEE Signal Processing Letters 18(5):299-302, 2011** — posicionamento de polos por warping multibanda.
5. **Zambon, S., Lehtonen, H.-M. & Bank, B.** — *"Simulation of Piano Sustain-Pedal Effect by Parallel Second-Order Filters"*. **Proc. DAFx-08, pp. 199-204.** — FZ-ARMA, calibração do pedal, e a afirmação de que se aplica a síntese por samples.
6. **Gabrielli, L., Zambon, S. & Fontana, F.** — *"Parallel Digital Signal Processing for Efficient Piano Synthesis"*. **Proc. 23rd EUSIPCO, Nice, 2015.** PDF livre em `new.eurasip.org/Proceedings/Eusipco/Eusipco2015/papers/1570104271.pdf`. — implementação do Viscount Physis Piano, ~15.000 ressoadores, ~3 GFLOPS, tabela de ressoadores/core por CPU. *(Uma versão anterior deste documento citava esta entrada como "Zambon, S. et al." e sem título, o que a tornava praticamente irrastreável — **o primeiro autor é Gabrielli**.)*
7. **Bank, B., Avanzini, F., Borin, G., De Poli, G., Fontana, F. & Rocchesso, D.** — *"Physically Informed Signal Processing Methods for Piano Sound Synthesis: A Research Overview"*. **EURASIP J. Applied Signal Processing 2003:10, pp. 941-952.** — Tabela 1 com K/p/massa do martelo; a prova gráfica da instabilidade do `F(n−1)`; K-method e martelo multitaxa.
8. **Código MATLAB `parfilt`** — `home.mit.bme.hu/~bank/parfilt/` (`freqpoles.m`, `parfiltdes.m`, `parfiltdesfr.m`, `parfilt.m`). **LICENÇA NÃO-COMERCIAL.**

**Corda: física e discretização**
9. **Bensa, J., Bilbao, S., Kronland-Martinet, R. & Smith, J. O.** — *"The simulation of piano string vibration: From physical models to finite difference schemes and digital waveguides"*. **JASA 114(2):1095-1107, 2003.** DOI 10.1121/1.1587146. Preprint CC BY 4.0 no HAL (`hal-00088329`). — **PDE bem-posta, Tabela I com parâmetros medidos, e as leis lineares `b₁(f₀)`/`b₂(f₀)` para o teclado inteiro.**
10. **Chaigne, A. & Askenfelt, A.** — *"Numerical simulations of piano strings. I. A physical model for a struck string using finite difference methods"*. **JASA 95(2):1112-1118, 1994.** DOI 10.1121/1.408459. — modelo canônico de martelo/corda. **AVISO: a PDE dele é mal-posta (ver ref. 9).**
**11a. Rauhala, J. & Välimäki, V.** — *"Dispersion Modeling in Waveguide Piano Synthesis Using Tunable Allpass Filters"*. **Proc. DAFx-06, Montréal, pp. 71-76, set/2006.** PDF livre em `dafx.de/paper-archive/2006/papers/p_071.pdf`. — **É ESTA a fonte de tudo que a §3.1 usa**: `I_key`, `k_d(B)`, `C_d(B,M)` (Eqs. 3-8), a **Tabela 2** com `k1..k3`/`m1..m4`, a **Fig. 1** com os B medidos do Steinway, e o exemplo com **M = 8**. Cascata de allpass de **1ª ordem**.

**11b. Rauhala, J. & Välimäki, V.** — *"Tunable dispersion filter design for piano synthesis"*. **IEEE Signal Processing Letters 13(5):253-256, 2006.** DOI 10.1109/LSP.2006.870376. — **método ORIGINAL**, cascata de allpass de **2ª ordem**, com uma tabela de constantes **DIFERENTE**. É a ref. [7] do paper 11a.

> **Uma versão anterior deste documento fundia as duas entradas**, atribuindo o título do 11b ao venue do 11a. O dano era concreto: quem fosse buscar o SPL 13(5) encontraria a tabela do **outro** desenho e concluiria que as constantes da §3.1 estão erradas — **elas estão certas, só vêm do 11a.**

**Martelo**
12. **Russell, D. & Rossing, T.** — *"Testing the Nonlinearity of Piano Hammers using Residual Shock Spectra"*. **Acustica / Acta Acustica 84 (1998), pp. 967-975.** — Tabela I com `a` e `b` de 13 martelos de Steinway D; o experimento de controle com martelo linear de poliuretano.
13. **Stulov, A. & Kartofelev, D.** — *"Wave propagation and dispersion in microstructured wool felt"* (Tallinn Univ. of Technology). — equação de Stulov verbatim e valores de ε e τ₀.
14. **Stulov, A.** — apresentação **ViennaTalk 2015** — valores de martelos de grave.

**FDTD / esquemas conservativos**
15. **Bilbao, S., Ducceschi, M. & Webb, C.** — *"Large-scale real-time modular physical modeling sound synthesis"*. **Proc. DAFx-19, paper 22.** — condição de estabilidade, Tabelas 1-3 com tempos medidos.
16. **Ducceschi, M. & Bilbao, S.** — paper companheiro **DAFx-19** — solver não-iterativo por quadratização de energia.
17. **Ducceschi, M., Bilbao, S., Willemsen, S. & Serafin, S.** — **JASA 149(5):3502, 2021** — esquema linearmente-implícito, condição de estabilidade independente da não-linearidade, e o caveat de primeira ordem para K alto.
18. **Ducceschi, M. & Bilbao, S.** — *"A physical model for the prepared piano"*. **Proc. ICSV26, 2019.** — sistema transversal+longitudinal completo; achado de que Euler-Bernoulli basta.
19. **Chabassier, J., Chaigne, A. & Joly, P.** — *"Modeling and simulation of a grand piano"*. **JASA 134(1):648-665, 2013.** DOI 10.1121/1.4809649. — Tabela III (parâmetros de martelo e corda de um Steinway D), termo dissipativo de 1 parâmetro, e o número das 24 h em 300 CPUs. *(Nota: a extração das tabelas numéricas falhou em uma das tentativas; os valores da Tabela III foram lidos com sucesso numa segunda passagem, mas confira no original.)*

**Tampo**
20. **Boutillon, X. & Ege, K.** — **arXiv:1305.3057** (publicado em Journal of Sound and Vibration) — densidade modal, fator de perda, f_lim, propriedades de material medidas num piano vertical em condição de tocar.
21. **Ege, K. & Boutillon, X.** — **Proc. ICA 2010 / arXiv:1210.5688** — impedância de cavalete compilada de Wogram, Conklin, Giordano, Nakamura.

**Piano acústico — divulgação técnica com autoridade**
22. **Askenfelt, A. & Jansson, E.** — *"Five Lectures on the Acoustics of the Piano"*, KTH (online). — timings mecânicos, velocidades de tecla e martelo, ruídos parasitas.
23. **Weinreich, G.** — palestra online no KTH sobre cordas acopladas. — mecanismo das duas polarizações e do coro; números de 8 dB/s e <2 dB/s.
24. **Woodhouse, J.** — *Euphonics: the science of musical instruments*, seção 12.2 (online). — confirmação independente dos valores K/p/massa; frequência de turnover.

**Pianoteq**
25. **Patente US 7,915,515 B2** — *"Device for producing signals representative of sounds of a keyboard and stringed instrument"*, Philippe Guillaume / Modartt. Texto integral livre no Google Patents e FreePatentsOnline. — **É o único documento que descreve a arquitetura do Pianoteq com autoridade.**
26. **Manual do Pianoteq** (v4 EN e versão atual, modartt.com). — definições de parâmetro, "Internal Sample Rate", polifonia, a frase sobre notas graves custarem mais CPU.
27. **Entrevista com Philippe Guillaume e Julien Pommier**, cdm.link, **31/05/2010**. — a citação de "modal analysis" e a origem no GetFEM.
28. **Sound On Sound** — reviews do Pianoteq 1 (jan/2007), Pianoteq 7.0.3 (abr/2021, dupla polarização) e Pianoteq 8 (Robin Bigwood, com as medições de 14% no iPad M1 e 25% no iPhone 12).
29. **CV acadêmico de Philippe Guillaume** — `modartt.com/guillaume/cv_en.htm`. — a lista de publicações em Padé multivariado e a ausência de qualquer paper sobre piano.

**Patentes (todas lidas ou com status verificado no Google Patents)**
30. **US 4,984,276** (Smith/Stanford, waveguide — **expirada 2008**)
31. **US 5,212,334**, **US 5,448,010**, **US 5,614,686** (mesh 2D/3D), **US 5,777,255** (martelo de piano) — todas expiradas entre 2010 e 2015
32. **US 5,229,536**, **US 5,241,127** (Yamaha, martelo/corda anos 90) — expiradas
33. **US 8,115,092 B2** (Yamaha VRM, Eiji Tominaga, expira 2030-06-06) — **ATIVA**. Claim 1 = método (a)-(h) de corda modal + corpo + convolução em coordenada modal.
33b. **US 8,895,831 B2** (Yamaha, Eiji Tominaga, expira 2030-10-07) — **ATIVA**. **DIVISIONAL** do pedido Ser. No. 12/788,897 (o mesmo que virou a US8115092). **18 claims; os independentes são 1, 2, 3 e 4** — todos os quatro lidos verbatim (texto integral em FreePatentsOnline). Claims 1/2 = **half-pedal** (coeficiente viscoso do abafador vs. curso de tecla e de pedal); claims 3/4 = **una corda** (módulo de elasticidade do martelo vs. curso do pedal esquerdo). **Leia a seção CLAIMS, não a descrição** — a descrição da divisional recita o método da patente-mãe e induz a erro. Ver §7.3.
34. **US 8,729,376 B2** (Yamaha, sample como excitação em loop) — **ATIVA até 2032**
35. **US 9,293,126 B2** (Viscount, modal — **Expired, Fee Related**)
36. **US 8,445,765**, **US 9,646,578**, **US 10,474,590** (Roland — mecânica de teclado e driver, **não síntese**)

**Código e licenças (verificados no fonte)**
37. **STK** — `github.com/thestk/stk` — LICENSE MIT lido verbatim. **Sem modelo de piano.**
38. **Faust** — `faustdoc.grame.fr/manual/faq/` e `physmodels.lib` do `grame-cncm/faustlibraries` — o TODO *"Currently still missing keyboard instruments"* lido no arquivo.
39. **OpenPiano** — `github.com/michele-perrone/OpenPiano`, `OpenPianoCore/Source/string_hammer.h` e `piano.h` lidos integralmente. **AGPL-3.0** confirmada no cabeçalho dos arquivos.
40. **mda Piano** — `elk-audio/mda-vst2`, `plugins/mdaPiano.cpp` lido. **É sample playback.**
41. **Csound** — `COPYING` (LGPL 2.1) e cabeçalho de `Opcodes/bilbar.c` (LGPL 2.1+, Bilbao & ffitch) lidos. **O fonte de `prepiano.c` NÃO foi localizado** em nenhuma branch/tag verificada.
42. **Salamander Grand Piano v3** — `github.com/sfzinstruments/SalamanderGrandPiano` — **CC BY 3.0** verificada.

**Performance**
43. **Wittmann, M., Zeiser, T., Hager, G. & Wellein, G.** — *"Short Note on Costs of Floating Point Operations on current x86-64 Architectures"*. **arXiv:1506.03997, 2015.** — ciclos medidos com e sem FTZ/DAZ.

### 10.2 Citadas mas NÃO abertas — trate como pista, não como fonte lida

| Referência | Motivo |
|---|---|
| **Bank, B. & Chabassier, J.**, *"Model-Based Digital Pianos: From Physics to Sound Synthesis"*, **IEEE Signal Processing Magazine 36(1):103-114, jan/2019**, DOI 10.1109/MSP.2018.2872349 | HAL bloqueado por anti-bot; IEEE pago. Metadados/abstract confirmados. **Pelos autores e pelo título, é provavelmente o survey de entrada ideal — vale a tentativa.** ⚠️ **A §5.4 continha uma citação VERBATIM atribuída a este paper (Sec. 3.6), com aspas e número de seção — de uma fonte que nunca foi aberta.** A frase não foi localizada em nenhuma fonte aberta e foi rebaixada a paráfrase `[NÃO CONFIRMADO]`. **Regra reforçada: nada nesta tabela pode aparecer entre aspas em lugar nenhum do documento.** |
| **Weinreich, G.**, *"Coupled piano strings"*, **JASA 62(6):1474-1484, 1977**, DOI 10.1121/1.381677 | Paywall. Citação (título/volume/páginas/DOI) confirmada. |
| **Chaigne & Askenfelt, Parte II**, **JASA 95(3):1631-1640, 1994**, DOI 10.1121/1.408549 | Paywall. **É a fonte primária real dos valores K/p/massa** que aparecem na ref. 7. |
| **Stulov, A.**, *"Hysteretic model of the grand piano hammer felt"*, **JASA 97(4):2577, 1995** | HTTP 403. Equação obtida de duas fontes abertas co-assinadas pelo autor. |
| **Young, R. W.**, *"Inharmonicity of Plain Wire Piano Strings"*, **JASA 24(3):267-273, 1952**, DOI 10.1121/1.1906888 | Paywall AIP. Título/autores/ano/volume/páginas/DOI e o resultado dos 1,2 cents confirmados via metadados. |
| **Guillaume, Ph.**, *"Nonlinear eigenproblems"*, **SIAM J. Matrix Anal. Appl. 20(3):575-595, 1999** | Paywall SIAM. Referência confirmada na capa da US7915515 e na página de publicações do autor. **É a única referência matemática que a patente cita.** |
| **Lehtonen, Penttinen, Rauhala & Välimäki**, **JASA 122(3):1787-1797, 2007** (pedal de sustain) | Citação e achado principal confirmados via resumo; texto completo não aberto. |
| **Chabassier, J.**, tese *"Modélisation et simulation numérique d'un piano par modèles physiques"*, École Polytechnique/INRIA, 2012, HAL `pastel-00690351` | Não aberta integralmente. Para **entender** a física, não para implementar. |
| **Chabassier, Chaigne & Joly**, *"Time domain simulation of a piano. Part 1"*, **ESAIM M2AN 48(5):1241-1278, 2014**, DOI 10.1051/m2an/2013136 | Só o abstract confirmado. |
| **Fletcher, Blackham & Stratton**, **JASA 34(6), 1962** (fórmula de B) | Citada via Rauhala & Välimäki; original não aberto. |
| **NESS** — `github.com/Edinburgh-Acoustics-and-Audio-Group/ness` | **O arquivo LICENSE não foi confirmado diretamente.** Reportado como MIT. **VERIFIQUE antes de tocar no código.** |
| **US 7,442,869 B2** (Viscount, tubos de órgão) | Só o número foi localizado por busca. **Status, escopo e jurisdição NÃO verificados.** Relevante para o módulo de órgão que já existe. |
| **Rocchesso & Scalcon**, NAM'96, pp. 407-414 | Citado por Abel & Smith (DAFx-06); original não aberto. |

---

## APÊNDICE — Cartão de referência rápida

```c
// ============ RESSOADOR MODAL — o átomo de tudo ============
// (Bank/Zambon/Fontana 2010, Eqs. 10-11. Invariância ao impulso, NÃO bilinear.)
// Entrada: f_k [Hz], tau_k [s] (decaimento de AMPLITUDE), A_k, fs [Hz]

// >>> PRECISAO: use DOUBLE em r, a1, a2 e no estado (y1,y2) para todo
//     ressoador com tau > ~2 s -- ou seja, o grave inteiro e todo o
//     banco simpatico com pedal embaixo. Em float32, tau=20s da
//     |r| = 0.99999896, a menos de 1 ordem de grandeza do eps da
//     maquina (1.2e-7): a afinacao ANDA e o decaimento erra.
//     O bug NAO aparece em teste de bancada curto -- aparece depois
//     de minutos de sustentacao AO VIVO. Ver secao 4.3.1.
//     Teste obrigatorio: 60 s sustentando, medir desvio de f e de tau.

r     = exp(-1.0 / (tau_k * fs));
theta = 2.0 * M_PI * f_k / fs;
a1    = -2.0 * r * cos(theta);
a2    =  r * r;
b     = (A_k / fs) * r * sin(theta);

// laço (3 mult com entrada, 2 sem):
y = b*x1 - a1*y1 - a2*y2;   y2 = y1; y1 = y; x1 = x;

// O z^-1 (o uso de x1, não x) é OBRIGATÓRIO: é ele que elimina o
// delay-free loop do martelo. Com bilinear você perde e o motor
// fica instável em fortissimo.

// ============ CONVERSÕES ============
// T60 = 6.9078 * tau        largura -3dB: BW = 1/(pi*tau)
//
// >>> CUIDADO: b2 e b3 NAO sao a mesma constante. b3 = b2/c^2.
//     Cada um so casa com a SUA forma. Ver secao 0.
//
// (Bensa)  1/tau_k = b1 + b2*(k*pi/L)^2       <-- b2 SO com (k*pi/L)^2
//          b1 = 4.4e-3*f0 - 4e-2   [1/s]      (Bensa 2003, Eq. 37a)
//          b2 = 1.0e-6*f0 + 1e-5   [m^2/s]    (Bensa 2003, Eq. 37b)
//
// (Bank)   1/tau_k = b1 + b3*(2*pi*f_k)^2     <-- b3 [s], NAO b2
//
// Trocar b2 por b3 (ou a forma por a outra) erra por c^2 ~ 1e5.

// ============ INARMONICIDADE ============
// f_k = k * f0 * sqrt(1 + B*k*k)
// B: ~1e-5 (grave) .. ~1e-2 (agudo extremo). Audível em TODA a extensão.
//
// >>> DUAS formas, e a letra kappa significa coisas DIFERENTES:
//   (a) Bank:   B = kappa_r^2 * (E*S/T0) * (pi/L)^2   kappa_r = d/4  [m]
//   (b) Bensa:  B = ( kappa_B * pi / (c*L) )^2        kappa_B        [m^2/s]
// Use (b) com dados da Tabela I do Bensa; use (a) com diametro de corda.
// Trocar um kappa pelo outro erra por ~12 ordens de grandeza.

// ============ MARTELO ============
// F_h = K_h * pow(dy, P_h)   se dy > 0, senão 0;   dy = y_h - y_s
// a_h = -F_h/m_h;  v_h += a_h/fs;  y_h += v_h/fs;   // duplo integrador
// p(tecla) = 0.02251*tecla + 1.9171     (tecla 1 = A0)
// K: knob de VOICING, ajustado de ouvido por região.
// f_knee ∝ v^((p-1)/(p+1))   ← o brilho por velocity sai DAQUI, não de filtro.

// ============ PESOS ============
// w_in,k = w_out,k = sin(k*pi*x_h/L)      // x_h/L ~= 0.11 a 0.14
// w_b,k  = k*pi*T0/L
// OTIMIZAÇÃO: normalize para w_b,k = 1 => força na ponte vira SOMA PURA.

// ============ UNÍSSONO ============
// T2 = eps*T1;  T3 = (2-eps)*T1;   Δcents ≈ ±866*(1-eps)
// Faixa útil: eps ∈ [0.995, 0.9999] => ±0.09 a ±4.3 cents

// ============ RE-STRIKE (não erre isto) ============
// z_n += alpha_novo;   // SOME ao estado. NÃO resete.

// ============ ORÇAMENTO ============
// 3 MPOS/ressoador com entrada, 2 em evolução livre.
// 20 bytes/ressoador. Mantenha (N_total * 20) < 60% do L2 por núcleo.
// Teto de modos: ~130-140 no grave, caindo no agudo.
// Roube por CUSTO, não por contagem de vozes.
// SoA obrigatório. ScopedNoDenormals obrigatório (já ativo em :5060).
```

---

*Fim do documento.*
