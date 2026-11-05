// Regras de seguranca da pagina CENAS (desktop), por cliques simulados.
//
// A regra de ouro do palco: UM TOQUE NUNCA GRAVA. Tocar numa musica gravada a
// carrega; tocar num slot vazio so o seleciona; gravar e botao proprio e
// substituir uma musica gravada pede um segundo toque. Um toque errado no
// escuro nao pode apagar a cena da proxima musica.

#include <juce_audio_basics/juce_audio_basics.h>   // LevelMeter (incluido por ScenesPage via skin) usa juce::Decibels

#include "ScenesPage.h"

#include <iostream>

struct ScenesPageTestAccess
{
    static int sel (const ScenesPage& p) { return p.sel; }
    static juce::TextButton& salvar (ScenesPage& p) { return p.salvarBtn; }
    static juce::TextButton& limpar (ScenesPage& p) { return p.limparBtn; }
    static juce::TextButton& carregar (ScenesPage& p) { return p.carregarBtn; }
    static juce::TextButton& anterior (ScenesPage& p) { return p.anteriorBtn; }
    static juce::TextButton& proxima (ScenesPage& p) { return p.proximaBtn; }
    static juce::TextEditor& bpm (ScenesPage& p) { return p.bpmEd; }
    static juce::TextEditor& musica (ScenesPage& p) { return p.musicaEd; }

    static juce::Point<int> centroDaLinha (ScenesPage& p, int i)
    {
        return p.calcularLayout (ChurchLiveSkin::metricsOf (p)).linhas[(size_t) i].getCentre();
    }
};

namespace
{
void clicar (ScenesPage& page, juce::Point<int> p)
{
    const auto agora = juce::Time::getCurrentTime();
    const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                              juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &page, &page,
                              agora, p.toFloat(), agora, 1, false);
    page.mouseDown (e);
}
}

int runScenesPageTests()
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* message)
    {
        if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };

    // Shell Church Live: paisagem >= 1,3:1, o que inclui os iPads 4:3 (1,33).
    for (const auto tamanho : { juce::Point<int> (1366, 1024), juce::Point<int> (1180, 820),
                                juce::Point<int> (1080, 810), juce::Point<int> (1024, 768),
                                juce::Point<int> (884, 390) })
        expect (ChurchLiveSkin::wantsShell (tamanho.x, tamanho.y), "paisagem de iPad/celular deve usar o shell");
    expect (! ChurchLiveSkin::wantsShell (820, 1180), "retrato nao usa o shell");
    expect (! ChurchLiveSkin::wantsShell (1000, 900), "quase quadrado nao usa o shell");
    expect (! ChurchLiveSkin::wantsShell (600, 400), "abaixo de 640 dp nao usa o shell");

    ChurchDAWLive::Cena cenas[ChurchDAWLive::numeroDeCenas];
    const char* musicas[] { "Aleluia", "Santo", "Grande e o Senhor" };
    for (int i = 0; i < 3; ++i)
    {
        cenas[i].preenchida = true;
        cenas[i].musica = musicas[i];
        cenas[i].bpm = 70.0 + i;
        cenas[i].canais[0].rotuloDoTimbre = "Pearl Upright";
    }

    ScenesPage page;
    page.setSize (1496, 740);

    int carregou = -1, gravou = -1, apagou = -1, detalhes = -1, nCarregou = 0, nGravou = 0;
    ChurchDAWLive::Cena ultimaMeta;
    page.onCarregar = [&] (int i) { carregou = i; ++nCarregou; return ChurchDAWLive::Resultado::ok; };
    page.onGravar   = [&] (int i) { gravou = i; ++nGravou; return ChurchDAWLive::Resultado::ok; };
    page.onApagar   = [&] (int i) { apagou = i; };
    page.onDetalhes = [&] (int i, const ChurchDAWLive::Cena& c) { detalhes = i; ultimaMeta = c; };
    page.atualizar (cenas, 0);

    // (a) toque numa musica gravada: carrega e seleciona
    clicar (page, ScenesPageTestAccess::centroDaLinha (page, 1));
    expect (carregou == 1 && nCarregou == 1, "tapping a saved song must load it");
    expect (ScenesPageTestAccess::sel (page) == 1, "tapping a saved song must select it");
    expect (nGravou == 0, "loading must never record");

    // (b) toque num slot vazio: so seleciona, NAO carrega (nao apaga o som atual)
    clicar (page, ScenesPageTestAccess::centroDaLinha (page, 5));
    expect (nCarregou == 1, "tapping an empty slot must not load anything");
    expect (ScenesPageTestAccess::sel (page) == 5, "tapping an empty slot must select it");
    expect (nGravou == 0, "tapping an empty slot must never record");

    // (c) cena vazia selecionada: gravar e imediato (nao ha o que perder)
    ScenesPageTestAccess::salvar (page).onClick();
    expect (gravou == 5 && nGravou == 1, "saving into an empty slot must not need confirmation");

    // (d) musica gravada: o primeiro toque so ARMA; o segundo grava
    clicar (page, ScenesPageTestAccess::centroDaLinha (page, 2));
    ScenesPageTestAccess::salvar (page).onClick();
    expect (nGravou == 1, "first tap on SAVE over a saved song must only arm it");
    ScenesPageTestAccess::salvar (page).onClick();
    expect (gravou == 2 && nGravou == 2, "second tap on SAVE must replace the song");

    // (e) trocar de selecao desarma: o toque seguinte volta a ser so o primeiro
    ScenesPageTestAccess::salvar (page).onClick();           // arma a cena 2
    clicar (page, ScenesPageTestAccess::centroDaLinha (page, 0));
    ScenesPageTestAccess::salvar (page).onClick();           // 1o toque na cena 0
    expect (nGravou == 2, "changing the selection must disarm the pending replace");

    // (f) limpar so age em cena gravada (e a confirmacao fica com o MainComponent)
    ScenesPageTestAccess::limpar (page).onClick();
    expect (apagou == 0, "LIMPAR must ask the host to delete the selected saved song");
    apagou = -1;
    clicar (page, ScenesPageTestAccess::centroDaLinha (page, 6));
    ScenesPageTestAccess::limpar (page).onClick();
    expect (apagou == -1, "LIMPAR on an empty slot must do nothing");

    // (g) editar detalhes: BPM e limitado, o resto da cena nao muda
    clicar (page, ScenesPageTestAccess::centroDaLinha (page, 1));
    ScenesPageTestAccess::bpm (page).setText ("999", false);
    ScenesPageTestAccess::bpm (page).onReturnKey();
    expect (detalhes == 1 && ultimaMeta.bpm == 300.0, "BPM must be clamped to 300");
    expect (ultimaMeta.musica == "Santo", "editing BPM must keep the song title");

    detalhes = -1;
    ScenesPageTestAccess::bpm (page).setText ("abc", false);
    ScenesPageTestAccess::bpm (page).onReturnKey();
    expect (detalhes == -1 || ultimaMeta.bpm == cenas[1].bpm, "garbage BPM must not corrupt the scene");

    // (h) ANTERIOR / PROXIMA percorrem so as musicas gravadas, dando a volta
    page.atualizar (cenas, 0);
    ScenesPageTestAccess::proxima (page).onClick();
    expect (carregou == 1, "NEXT from song 1 must load song 2");
    page.atualizar (cenas, 0);
    ScenesPageTestAccess::anterior (page).onClick();
    expect (carregou == 2, "PREVIOUS from the first song must wrap to the last saved one");

    // (i) cliques nao podem roubar o foco do teclado do palco: sem isto o JUCE entrega o
    // foco ao primeiro filho focavel (o campo "Musica") e as teclas do computador
    // passariam a digitar nele. Regressao achada no app real com mouse de verdade.
    expect (! page.getMouseClickGrabsKeyboardFocus(), "page clicks must not grab keyboard focus");
    for (auto* b : { &ScenesPageTestAccess::salvar (page), &ScenesPageTestAccess::limpar (page),
                     &ScenesPageTestAccess::carregar (page), &ScenesPageTestAccess::anterior (page),
                     &ScenesPageTestAccess::proxima (page) })
        expect (! b->getMouseClickGrabsKeyboardFocus(), "page buttons must not grab keyboard focus on click");

    // (j) faixa do modo Live: um toque carrega; a musica NO AR nao e recarregada; vazias nao existem
    SceneStrip strip;
    strip.setSize (700, 28);
    expect (! strip.getMouseClickGrabsKeyboardFocus(), "strip clicks must not grab keyboard focus");

    int faixaCarregou = -1, nFaixa = 0, abriu = 0;
    strip.onCarregar = [&] (int i) { faixaCarregou = i; ++nFaixa; return ChurchDAWLive::Resultado::ok; };
    strip.onAbrirCenas = [&] { ++abriu; };
    strip.atualizar (cenas, 0);

    const auto toqueNaFaixa = [&] (juce::Point<int> p)
    {
        const auto agora = juce::Time::getCurrentTime();
        const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                                  juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &strip, &strip,
                                  agora, p.toFloat(), agora, 1, false);
        strip.mouseDown (e);
    };

    // 3 musicas gravadas, cabem todas: chips lado a lado depois do icone (28 px)
    toqueNaFaixa ({ 14, 14 });
    expect (abriu == 1, "the strip icon must open the Scenes page");
    expect (nFaixa == 0, "the strip icon must not load a scene");

    // varre a faixa: so as musicas gravadas respondem e a no ar (0) nunca recarrega
    for (int x = 40; x < 700; x += 4)
        toqueNaFaixa ({ x, 14 });
    expect (nFaixa > 0 && faixaCarregou >= 1 && faixaCarregou <= 2,
            "strip must load only saved songs that are not already on air");

    if (failures == 0)
        std::cout << "SCENES_PAGE_OK\n";

    return failures == 0 ? 0 : 1;
}
