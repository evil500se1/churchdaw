#include <juce_gui_extra/juce_gui_extra.h>
#include "NordLookAndFeel.h"
#include "MainComponent.h"
#include "BinaryData.h"

//==============================================================================
// Tela de abertura: icone completo, nome e barra fina vermelha
//==============================================================================
class SplashWindow : public juce::TopLevelWindow
{
public:
    SplashWindow() : TopLevelWindow ("ChurchDAW", true)
    {
        // logo redondo: ja traz o nome e o slogan, entao a splash nao repete o
        // wordmark em texto (era icone + "ChurchDAW" escrito embaixo)
        icon = juce::ImageCache::getFromMemory (BinaryData::logo_round_png, BinaryData::logo_round_pngSize);
        setOpaque (true);
        centreWithSize (460, 400);
        setAlwaysOnTop (true);
        setDropShadowEnabled (true);
        setVisible (true);
    }

    void setStatus (const juce::String& text)
    {
        if (status != text)
        {
            status = text;
            repaint();
        }
    }

    // 0..1 — progresso de verdade: a splash so sai quando chega ao fim
    void setProgress (float p)
    {
        p = juce::jlimit (0.0f, 1.0f, p);

        if (std::abs (p - progress) > 0.002f)
        {
            progress = p;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0d10));

        auto area = getLocalBounds();
        auto logoArea = area.removeFromTop (296).withSizeKeepingCentre (280, 280);

        if (icon.isValid())
        {
            // a arte tem fundo PRETO chapado (sem transparencia): recorta no
            // circulo pra nao aparecer um quadrado atras do logo
            juce::Path circle;
            circle.addEllipse (logoArea.toFloat());

            juce::Graphics::ScopedSaveState clipped (g);
            g.reduceClipRegion (circle);
            g.setOpacity (1.0f);   // imagem herda o alpha do ultimo setColour
            g.drawImageWithin (icon, logoArea.getX(), logoArea.getY(),
                               logoArea.getWidth(), logoArea.getHeight(),
                               juce::RectanglePlacement::centred);
        }

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (status, area.removeFromTop (24), juce::Justification::centred);

        // barra de progresso REAL (antes era um 72% pintado na mao)
        auto bar = area.removeFromTop (16)
                       .withSizeKeepingCentre (juce::roundToInt ((float) getWidth() * 0.62f), 4);

        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.fillRoundedRectangle (bar.toFloat(), 2.0f);

        auto done = bar.toFloat().withWidth ((float) bar.getWidth() * progress);

        if (done.getWidth() > 2.0f)
        {
            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRed(), done.getX(), 0.0f,
                                                     NordLookAndFeel::nordRedBright(), done.getRight(), 0.0f, false));
            g.fillRoundedRectangle (done, 2.0f);
        }

        // proprietario
        area.removeFromTop (12);

        g.setColour (juce::Colours::white.withAlpha (0.42f));
        g.setFont (juce::Font (juce::FontOptions (12.5f)).boldened());
        g.drawText ("Igor Teixeira", area.removeFromTop (18), juce::Justification::centred);

        g.setColour (juce::Colours::white.withAlpha (0.26f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (juce::String::fromUTF8 ("WhatsApp (62) 98127-8461"),
                    area.removeFromTop (16), juce::Justification::centred);
    }

private:
    juce::Image icon;
    juce::String status { juce::String::fromUTF8 ("Iniciando...") };
    float progress = 0.04f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplashWindow)
};

//==============================================================================
class ChurchDAWApplication : public juce::JUCEApplication
{
public:
    ChurchDAWApplication() = default;

    const juce::String getApplicationName() override    { return "ChurchDAW"; }
    const juce::String getApplicationVersion() override { return "1.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&nordLookAndFeel);

        // traduz os textos internos do JUCE (seletor de audio/MIDI) pra pt-BR
        juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (juce::String::fromUTF8 (
            "language: pt-BR\n"
            "countries: br\n"
            "\"Audio device type:\" = \"Tipo de driver:\"\n"
            "\"Device:\" = \"Dispositivo:\"\n"
            "\"Output:\" = \"Sa\xc3\xad" "da:\"\n"
            "\"Input:\" = \"Entrada:\"\n"
            "\"Active output channels:\" = \"Sa\xc3\xad" "das ativas:\"\n"
            "\"Active input channels:\" = \"Entradas ativas:\"\n"
            "\"Sample rate:\" = \"Taxa de amostragem:\"\n"
            "\"Audio buffer size:\" = \"Tamanho do buffer:\"\n"
            "\"Control Panel\" = \"Painel do driver\"\n"
            "\"Opens the device's own control panel\" = \"Abre o painel de controle da interface\"\n"
            "\"Reset Device\" = \"Resetar\"\n"
            "\"Resets the audio interface - sometimes needed after changing a device's properties in its custom control panel\" = \"Reinicia a interface de \xc3\xa1udio\"\n"
            "\"Test\" = \"Testar\"\n"
            "\"Plays a test tone\" = \"Toca um tom de teste\"\n"
            "\"Active MIDI inputs:\" = \"Entradas MIDI ativas:\"\n"
            "\"MIDI Output:\" = \"Sa\xc3\xad" "da MIDI:\"\n"
            "\"No MIDI inputs available\" = \"Nenhuma entrada MIDI\"\n"
            "\"(no audio output channels found)\" = \"(sem canais de sa\xc3\xad" "da)\"\n"
            "\"(no audio input channels found)\" = \"(sem canais de entrada)\"\n"
            "\"none\" = \"nenhum\"\n"
            "\"Show advanced settings...\" = \"Mostrar avan\xc3\xa7" "adas...\"\n"
            "\"Error when trying to open audio device!\" = \"Erro ao abrir o dispositivo de \xc3\xa1udio!\"\n"
            "\"OK\" = \"OK\"\n"
            // janela de plugins (PluginListComponent) — vinha toda em ingles
            "\"Name\" = \"Nome\"\n"
            "\"Format\" = \"Formato\"\n"
            "\"Category\" = \"Categoria\"\n"
            "\"Manufacturer\" = \"Fabricante\"\n"
            "\"Description\" = \"Descri\xc3\xa7\xc3\xa3o\"\n"
            "\"Options...\" = \"Op\xc3\xa7\xc3\xb5" "es...\"\n"
            "\"Scan\" = \"Escanear\"\n"
            "\"Cancel\" = \"Cancelar\"\n"
            "\"Clear list\" = \"Limpar lista\"\n"
            // o JUCE troca XFMTX pelo nome do formato DEPOIS de traduzir
            "\"Scan for new or updated XFMTX plug-ins\" = \"Escanear plugins XFMTX novos ou atualizados\"\n"
            "\"Remove all XFMTX plug-ins\" = \"Remover todos os plugins XFMTX\"\n"
            "\"Remove selected plug-in from list\" = \"Remover o plugin selecionado\"\n"
            "\"Remove plug-in from list\" = \"Remover o plugin da lista\"\n"
            "\"Remove any plug-ins whose files no longer exist\" = \"Remover plugins que n\xc3\xa3o existem mais\"\n"
            "\"Show folder containing selected plug-in\" = \"Abrir a pasta do plugin selecionado\"\n"
            "\"Show folder containing plug-in\" = \"Abrir a pasta do plugin\"\n"
            "\"Select folders to scan...\" = \"Escolha as pastas para escanear...\"\n"
            "\"Scanning for plug-ins...\" = \"Procurando plugins...\"\n"
            "\"Searching for all possible plug-in files...\" = \"Vasculhando os arquivos de plugin...\"\n"
            "\"Testing\" = \"Testando\"\n"
            "\"Scan complete\" = \"Escaneamento conclu\xc3\xad" "do\"\n"
            "\"Plugin Scanning\" = \"Escaneamento de plugins\"\n"
            "\"Deactivated after failing to initialise correctly\" = \"Desativado: falhou ao iniciar\"\n"
            "\"The following files encountered fatal errors during validation\" = \"Estes arquivos deram erro grave na verifica\xc3\xa7\xc3\xa3o\"\n"
            "\"The following files appeared to be plugin files, but failed to load correctly\" = \"Estes pareciam plugins, mas n\xc3\xa3o carregaram\"\n"), false));

        splash = std::make_unique<SplashWindow>();

        // deixa a splash aparecer antes do carregamento pesado dos plugins
        juce::Timer::callAfterDelay (180, [this]
        {
            mainWindow = std::make_unique<MainWindow> (getApplicationName());

            if (splash != nullptr && mainWindow->content() != nullptr)
                gate = std::make_unique<LoadGate> (*splash, *mainWindow->content(),
                                                   [this] { splash = nullptr; });
            else
                juce::Timer::callAfterDelay (900, [this] { splash = nullptr; });
        });
    }

    void shutdown() override
    {
        gate = nullptr;          // para o timer antes de soltar a janela que ele consulta
        splash = nullptr;
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              NordLookAndFeel::background(),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (960, 600, 32000, 32000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);

            // Abre ocupando a AREA UTIL da tela. setFullScreen(true) dimensionava a
            // janela pela tela INTEIRA: a barra de titulo empurrava o conteudo pra
            // baixo e a barra de tarefas comia o resto — o fim da regua master
            // (Auto EQ Master / Punch) ficava fora da tela. userArea ja desconta a
            // barra de tarefas, entao tudo cabe em qualquer escala de DPI.
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                setBounds (display->userArea);
            else
                setFullScreen (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        MainComponent* content() const
        {
            return dynamic_cast<MainComponent*> (getContentComponent());
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    //==============================================================================
    // Segura a splash ate a DAW estar REALMENTE pronta: bibliotecas escaneadas e
    // samples decodificados. Antes a splash saia por tempo fixo (900 ms) e a janela
    // aparecia com os canais ainda vazios — quem tocasse nesse intervalo nao ouvia nada.
    class LoadGate : private juce::Timer
    {
    public:
        LoadGate (SplashWindow& s, MainComponent& c, std::function<void()> whenReady)
            : splash (&s), content (c), onReady (std::move (whenReady))
        {
            startTimer (tickMs);
        }

    private:
        void timerCallback() override
        {
            auto* sp = splash.getComponent();

            if (sp == nullptr)   // splash sumiu por outro caminho
            {
                stopTimer();
                return;
            }

            elapsedMs += tickMs;

            const bool busy = content.isStillLoading();

            // a barra persegue o alvo da fase atual, entao ela sempre anda um
            // pouco em vez de congelar num numero. Ja pronto, corre mais rapido
            // pra chegar visivelmente ao fim antes da splash sair.
            progress += (content.getLoadingProgress() - progress) * (busy ? 0.08f : 0.30f);
            sp->setProgress (progress);

            // "Tudo pronto" com a barra em 85% e uma contradicao visual na
            // primeira tela do app -- o texto so muda quando a barra chegou la
            sp->setStatus (busy            ? content.getLoadingDetail()
                         : progress < 0.97f ? juce::String::fromUTF8 ("Finalizando...")
                                            : juce::String::fromUTF8 ("Tudo pronto"));

            // Exige varios ciclos parados EM SEQUENCIA: entre "biblioteca escaneada"
            // e "samples comecam a decodificar" existe um respiro em que nada parece
            // ocupado, e fechar ali entregaria a DAW pela metade.
            idleTicks = busy ? 0 : idleTicks + 1;

            if ((idleTicks >= idleTicksNeeded && progress >= 0.97f) || elapsedMs >= maxWaitMs)
            {
                stopTimer();

                if (onReady != nullptr)
                    onReady();
            }
        }

        static constexpr int tickMs = 100;
        static constexpr int idleTicksNeeded = 9;    // ~0,9 s parado sem nada pendente
        static constexpr int maxWaitMs = 90000;      // rede de seguranca: nunca trava na splash

        juce::Component::SafePointer<SplashWindow> splash;
        MainComponent& content;
        std::function<void()> onReady;
        float progress = 0.04f;
        int idleTicks = 0, elapsedMs = 0;
    };

    NordLookAndFeel nordLookAndFeel;
    std::unique_ptr<SplashWindow> splash;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<LoadGate> gate;
};

START_JUCE_APPLICATION (ChurchDAWApplication)
