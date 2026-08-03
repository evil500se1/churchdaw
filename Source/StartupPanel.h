#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "ChurchDAWServices.h"
#include "NordLookAndFeel.h"

//==============================================================================
// Portao de inicializacao. O motor de audio ainda NAO existe enquanto esta tela
// verifica versao ou instala o pack inicial; assim o APK abre leve e nunca tenta
// escanear uma biblioteca que esta pela metade.
//==============================================================================
class StartupPanel final : public juce::Component
{
public:
    explicit StartupPanel (juce::Image logoImage)
        : logo (std::move (logoImage))
    {
        setOpaque (true);
        setSize (820, 540);

        configureButton (primaryButton, true);
        configureButton (secondaryButton, false);
        addAndMakeVisible (primaryButton);
        addAndMakeVisible (secondaryButton);

        primaryButton.setVisible (false);
        secondaryButton.setVisible (false);
    }

    ~StartupPanel() override
    {
        installer.reset();
        restoreScreenSaver();
    }

    void showChecking()
    {
        mode = Mode::checking;
        eyebrow = "CHURCHDAW  /  ANDROID";
        title = juce::String::fromUTF8 ("Preparando seu instrumento");
        detail = juce::String::fromUTF8 (
            u8"Verificando atualizações e o catálogo de bibliotecas...");
        progress = -1.0f;
        primaryButton.setVisible (false);
        secondaryButton.setVisible (false);
        repaint();
    }

    void showRequiredUpdate (ChurchDAWServices::UpdatePolicy policy)
    {
        activeUpdatePolicy = std::move (policy);
        mode = Mode::requiredUpdate;
        eyebrow = juce::String::fromUTF8 (u8"ATUALIZAÇÃO OBRIGATÓRIA");
        title = juce::String::fromUTF8 (u8"Atualize com segurança no ChurchDAW");
        const auto reason = activeUpdatePolicy.updateMessage.isNotEmpty()
                              ? activeUpdatePolicy.updateMessage
                              : juce::String::fromUTF8 (
                                  u8"Uma nova versão é necessária para continuar.");
        detail = reason + "\n"
               + juce::String::fromUTF8 (
                   u8"O APK assinado será aberto no site oficial; confirme a instalação no Android para continuar.");
        footnote = "v" + juce::String (ChurchDAWServices::currentVersionName)
                 + "  ->  v" + activeUpdatePolicy.latestVersionName;
        progress = 0.0f;

        primaryButton.setButtonText ("Atualizar no ChurchDAW");
        primaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
        {
            if (auto* self = safe.getComponent())
            {
                self->detail = juce::String::fromUTF8 (
                    u8"Abrindo o APK assinado no churchdaw.com. "
                    u8"Depois do download, confirme a instalação exibida pelo Android.");
                self->primaryButton.setButtonText (
                    juce::String::fromUTF8 (u8"Abrindo atualização..."));
                self->primaryButton.setEnabled (false);
                self->repaint();

                if (! ChurchDAWServices::startWebsiteUpdate (
                        self->activeUpdatePolicy.playStoreUrl))
                {
                    self->detail = juce::String::fromUTF8 (
                        u8"Não foi possível abrir a atualização segura agora. "
                        u8"O ChurchDAW continuará bloqueado; tente novamente mais tarde.");
                    self->primaryButton.setButtonText ("Tentar novamente");
                    self->primaryButton.setEnabled (true);
                    self->repaint();
                    return;
                }

                juce::Timer::callAfterDelay (
                    17000,
                    [safe]
                    {
                        if (auto* current = safe.getComponent())
                        {
                            current->detail = juce::String::fromUTF8 (
                                u8"A instalação ainda não foi concluída. Baixe o APK no site oficial, "
                                u8"confirme no Android e abra o ChurchDAW novamente.");
                            current->primaryButton.setButtonText ("Tentar novamente");
                            current->primaryButton.setEnabled (true);
                            current->repaint();
                        }
                    });
            }
        };

        primaryButton.setVisible (true);
        secondaryButton.setVisible (false);
        resized();
        repaint();
    }

    void showOptionalUpdate (ChurchDAWServices::UpdatePolicy policy)
    {
        activeUpdatePolicy = std::move (policy);
        mode = Mode::optionalUpdate;
        eyebrow = juce::String::fromUTF8 (u8"NOVA VERS\u00c3O DISPON\u00cdVEL");
        title = juce::String::fromUTF8 (u8"Seu ChurchDAW pode ficar ainda melhor");
        const auto reason = activeUpdatePolicy.updateMessage.isNotEmpty()
                              ? activeUpdatePolicy.updateMessage
                              : juce::String::fromUTF8 (
                                  u8"H\u00e1 uma nova vers\u00e3o dispon\u00edvel para seu aparelho.");
        detail = reason + "\n"
               + juce::String::fromUTF8 (
                   u8"Voc\u00ea pode atualizar agora pelo site oficial ou continuar normalmente.");
        footnote = "v" + juce::String (ChurchDAWServices::currentVersionName)
                 + "  ->  v" + activeUpdatePolicy.latestVersionName;
        progress = 0.0f;

        primaryButton.setButtonText ("Atualizar");
        primaryButton.setEnabled (true);
        primaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
        {
            if (auto* self = safe.getComponent())
            {
                if (! ChurchDAWServices::isSafeWebsiteUpdateUrl (
                        self->activeUpdatePolicy.playStoreUrl))
                {
                    self->detail = juce::String::fromUTF8 (
                        u8"A atualiza\u00e7\u00e3o recebida n\u00e3o passou pela valida\u00e7\u00e3o de seguran\u00e7a. "
                        u8"Voc\u00ea pode continuar usando o ChurchDAW.");
                    self->primaryButton.setEnabled (false);
                    self->repaint();
                    return;
                }

                self->detail = juce::String::fromUTF8 (
                    u8"Abrindo a atualiza\u00e7\u00e3o assinada no churchdaw.com. "
                    u8"Se preferir instalar depois, volte e toque em Depois.");
                self->primaryButton.setButtonText (
                    juce::String::fromUTF8 (u8"Abrindo atualiza\u00e7\u00e3o..."));
                self->primaryButton.setEnabled (false);
                self->repaint();

                if (! ChurchDAWServices::startWebsiteUpdate (
                        self->activeUpdatePolicy.playStoreUrl))
                {
                    self->detail = juce::String::fromUTF8 (
                        u8"N\u00e3o foi poss\u00edvel abrir a atualiza\u00e7\u00e3o agora. "
                        u8"Voc\u00ea pode tentar novamente ou continuar normalmente.");
                    self->primaryButton.setButtonText ("Tentar novamente");
                    self->primaryButton.setEnabled (true);
                    self->repaint();
                }
            }
        };
        primaryButton.setVisible (true);

        secondaryButton.setButtonText ("Depois");
        secondaryButton.setEnabled (true);
        secondaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
        {
            if (auto* self = safe.getComponent())
            {
                // Consome a continuacao antes de chama-la: dois toques rapidos
                // nunca repetem login/catalogo nem recriam esta oferta.
                auto continuation = self->onContinue;
                self->onContinue = nullptr;
                self->secondaryButton.setEnabled (false);

                if (continuation != nullptr)
                    continuation();
            }
        };
        secondaryButton.setVisible (true);

        resized();
        repaint();
    }

    void showStarterChoice (const ChurchDAWServices::LibraryPack* starter,
                            bool serverWasReached,
                            bool requireInstalledLibrary = false)
    {
        starterPack.reset();
        starterRequiredForAccess = requireInstalledLibrary;

        if (starter != nullptr)
            starterPack = std::make_unique<ChurchDAWServices::LibraryPack> (*starter);

        mode = Mode::starterChoice;
        eyebrow = juce::String::fromUTF8 (u8"PRIMEIRA CONFIGURAÇÃO");

        const bool offlineCultoStarter = starterPack != nullptr
            && (starterPack->id.equalsIgnoreCase ("pad-continuo")
                || starterPack->id.equalsIgnoreCase ("culto-offline")
                || starterPack->id.equalsIgnoreCase ("culto-offline-v1"));

        title = juce::String::fromUTF8 (
            u8"Baixe o que quiser — ou entre sem samples");
        progress = 0.0f;
        footnote = juce::String::fromUTF8 (
            offlineCultoStarter
                ? u8"Culto offline (leve) ou pack completo em Bibliotecas. "
                  u8"No mixer você também baixa timbre por timbre (ex.: Pad 2, piano)."
                : u8"Pack completo, pack leve ou timbres 1 a 1 no seletor do canal. "
                  u8"Nada é obrigatório para abrir o app.");

        // O cache pode desenhar o catalogo, mas uma primeira instalacao enorme
        // nao deve nascer de URL vencida quando nenhum servidor respondeu.
        if (serverWasReached && starterPack != nullptr && starterPack->canDownload())
        {
            const auto temporaryBytes =
                starterPack->downloadBytes + starterPack->installedBytes
                + ChurchDAWServices::installerStorageReserve;
            detail = juce::String::fromUTF8 (
                         offlineCultoStarter ? u8"CULTO OFFLINE  /  download "
                                             : u8"PACK COMPLETO  /  download ")
                   + ChurchDAWServices::humanBytes (starterPack->downloadBytes)
                   + juce::String::fromUTF8 (u8"\nPico estimado de espaço: ")
                   + ChurchDAWServices::humanBytes (temporaryBytes)
                   + juce::String::fromUTF8 (u8"\nPartes já retomadas podem reduzir esse pico.")
                   + "\n"
                   + (offlineCultoStarter
                          ? juce::String::fromUTF8 (
                                "Pads A–G para culto ao vivo sem baixar 12 GB agora.")
                          : starterPack->description)
                   + juce::String::fromUTF8 (
                         u8"\nPrefira Wi-Fi e mantenha o app aberto durante a instalação.");
            primaryButton.setButtonText (
                offlineCultoStarter ? "Baixar Culto offline"
                                    : "Baixar pack completo");
            primaryButton.setVisible (true);
            primaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
            {
                if (auto* self = safe.getComponent())
                    self->beginStarterDownload();
            };
        }
        else
        {
            detail = serverWasReached
                       ? juce::String::fromUTF8 (
                           u8"O pack inicial ainda não foi publicado no catálogo. Você pode abrir o app e instalar depois em Bibliotecas.")
                       : juce::String::fromUTF8 (
                           u8"Não foi possível falar com o servidor agora. Confira a internet do aparelho e toque em Tentar novamente.");

            // Servidor fora do ar ou celular sem rede: sem este botao a tela
            // fica sem saida nenhuma, porque o "Abrir sem baixar" tambem some
            // quando a biblioteca e obrigatoria para a demonstracao.
            if (! serverWasReached && onRetryCatalog != nullptr)
            {
                primaryButton.setButtonText ("Tentar novamente");
                primaryButton.setEnabled (true);
                primaryButton.setVisible (true);
                primaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
                {
                    if (auto* self = safe.getComponent())
                    {
                        self->primaryButton.setButtonText ("Verificando...");
                        self->primaryButton.setEnabled (false);
                        if (self->onRetryCatalog != nullptr)
                            self->onRetryCatalog();
                    }
                };
            }
            else
            {
                primaryButton.setVisible (false);
            }
        }

        // Sempre permite entrar sem baixar (demo e conta). Download fica
        // opcional: pack leve, pack completo ou timbres individuais depois.
        secondaryButton.setButtonText ("Abrir sem baixar");
        secondaryButton.setVisible (true);
        secondaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
        {
            if (auto* self = safe.getComponent())
            {
                ChurchDAWServices::rememberStarterChoiceCompleted();

                if (self->onContinue != nullptr)
                    self->onContinue();
            }
        };

        resized();
        repaint();
    }

    void showReady()
    {
        mode = Mode::ready;
        eyebrow = "PRONTO";
        title = juce::String::fromUTF8 ("Abrindo o ChurchDAW");
        detail = juce::String::fromUTF8 (u8"Iniciando áudio, MIDI e seus timbres...");
        footnote.clear();
        progress = 1.0f;
        primaryButton.setVisible (false);
        secondaryButton.setVisible (false);
        repaint();
    }

    void showLegacyAccess (std::function<void()> allow,
                           std::function<void()> skip)
    {
        mode = Mode::legacyAccess;
        eyebrow = juce::String::fromUTF8 (u8"ACERVO DE VERSÕES ANTERIORES");
        title = juce::String::fromUTF8 (u8"Você já usava o ChurchDAW neste aparelho?");
        detail = juce::String::fromUTF8 (
            u8"Se você guardou timbres em Music/ChurchDAW, podemos verificar esse acervo "
            u8"depois que você autorizar o acesso a arquivos de áudio. "
            u8"Essa etapa é opcional e pode ser ativada depois em Bibliotecas.");
        footnote = juce::String::fromUTF8 (
            u8"Nada será movido, copiado ou apagado. Novos packs ficam no armazenamento privado do app.");
        progress = 0.0f;

        primaryButton.setButtonText ("Verificar meu acervo");
        primaryButton.onClick = [callback = std::move (allow)]
        {
            if (callback != nullptr)
                callback();
        };
        primaryButton.setVisible (true);

        secondaryButton.setButtonText (juce::String::fromUTF8 (u8"Agora não"));
        secondaryButton.onClick = [callback = std::move (skip)]
        {
            if (callback != nullptr)
                callback();
        };
        secondaryButton.setVisible (true);
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const bool compactLandscape = bounds.getHeight() < 430.0f;
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff090a0d), 0.0f, 0.0f,
                                                 juce::Colour (0xff17191f), bounds.getRight(),
                                                 bounds.getBottom(), false));
        g.fillRect (bounds);

        // Luz vermelha muito sutil atras do card. Mantem identidade sem virar
        // uma tela gamer ou competir com o texto.
        juce::ColourGradient glow (juce::Colour (0x42c4112e), bounds.getCentreX(),
                                   bounds.getBottom() * 0.23f,
                                   juce::Colours::transparentBlack, bounds.getCentreX(),
                                   bounds.getBottom() * 0.82f, true);
        g.setGradientFill (glow);
        g.fillEllipse (bounds.withSizeKeepingCentre (bounds.getWidth() * 0.82f,
                                                     bounds.getHeight() * 1.15f));

        auto card = bounds.reduced (juce::jmax (16.0f, bounds.getWidth() * 0.055f),
                                    juce::jmax (14.0f, bounds.getHeight() * 0.06f));
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillRoundedRectangle (card.translated (0.0f, 8.0f), 24.0f);
        g.setColour (juce::Colour (0xff17191f).withAlpha (0.97f));
        g.fillRoundedRectangle (card, 24.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (card, 24.0f, 1.0f);

        auto content = card.reduced (juce::jmax (24.0f, card.getWidth() * 0.07f),
                                     juce::jmax (22.0f, card.getHeight() * 0.08f));

        auto logoArea = content.removeFromTop (
            compactLandscape ? juce::jmin (44.0f, content.getHeight() * 0.20f)
                             : juce::jmin (86.0f, content.getHeight() * 0.20f));

        if (logo.isValid())
        {
           #if JUCE_ANDROID
            constexpr float imageInset = 0.0f;
           #else
            constexpr float imageInset = 4.0f;
           #endif
            const auto imageBox = logoArea.removeFromLeft (logoArea.getHeight())
                                  .reduced (imageInset);
           #if JUCE_ANDROID
            g.setOpacity (1.0f);
            g.drawImageWithin (logo, (int) imageBox.getX(), (int) imageBox.getY(),
                               (int) imageBox.getWidth(), (int) imageBox.getHeight(),
                               juce::RectanglePlacement::centred);
           #else
            juce::Path circle;
            circle.addEllipse (imageBox);
            juce::Graphics::ScopedSaveState clipped (g);
            g.reduceClipRegion (circle);
            g.drawImageWithin (logo, (int) imageBox.getX(), (int) imageBox.getY(),
                               (int) imageBox.getWidth(), (int) imageBox.getHeight(),
                               juce::RectanglePlacement::centred);
           #endif
        }

        g.setColour (NordLookAndFeel::nordRedBright());
        g.setFont (juce::Font (juce::FontOptions (12.5f)).boldened()
                       .withExtraKerningFactor (0.10f));
        g.drawText (eyebrow, logoArea.reduced (14.0f, 4.0f),
                    juce::Justification::centredLeft);

        content.removeFromTop (compactLandscape
                                   ? 4.0f
                                   : juce::jmax (8.0f, content.getHeight() * 0.035f));
        auto titleArea = content.removeFromTop (
            compactLandscape ? juce::jmin (42.0f, content.getHeight() * 0.25f)
                             : juce::jmin (74.0f, content.getHeight() * 0.24f));
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (
            compactLandscape
                ? juce::jlimit (20.0f, 29.0f, titleArea.getHeight() * 0.48f)
                : juce::jlimit (23.0f, 38.0f, titleArea.getHeight() * 0.46f))).boldened());
        g.drawFittedText (title, titleArea.toNearestInt(), juce::Justification::centredLeft,
                          2, 0.86f);

        // Em celulares deitados o aviso do pack tem cinco ou mais linhas
        // (download, pico, retomada, descricao e Wi-Fi). Reserve o espaco que
        // sobra depois do progresso/rodape, em vez de corta-lo em quatro linhas.
        const float compactLowerReserve = 5.0f + 8.0f + 18.0f + 62.0f;
        const float detailHeight = compactLandscape
            ? juce::jmax (48.0f, content.getHeight() - compactLowerReserve)
            : juce::jmin (100.0f, content.getHeight() * 0.38f);
        auto detailArea = content.removeFromTop (detailHeight);
        g.setColour (juce::Colours::white.withAlpha (0.68f));
        g.setFont (juce::Font (juce::FontOptions (compactLandscape ? 13.0f : 15.0f)));
        g.drawFittedText (detail, detailArea.toNearestInt(), juce::Justification::topLeft,
                          compactLandscape ? 7 : 4, compactLandscape ? 0.78f : 0.85f);

        content.removeFromTop (compactLandscape ? 5.0f : 8.0f);
        auto progressArea = content.removeFromTop (8.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (progressArea, 4.0f);

        float shown = progress;

        if (shown < 0.0f)
        {
            const auto phase = (float) ((juce::Time::getMillisecondCounter() % 1400) / 1400.0);
            shown = 0.18f;
            const auto start = progressArea.getX()
                             + (progressArea.getWidth() - progressArea.getWidth() * shown) * phase;
            auto moving = progressArea.withX (start).withWidth (progressArea.getWidth() * shown);
            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRed(), moving.getX(), 0.0f,
                                                     NordLookAndFeel::nordRedBright(), moving.getRight(),
                                                     0.0f, false));
            g.fillRoundedRectangle (moving, 4.0f);
            repaint (progressArea.toNearestInt());
        }
        else if (shown > 0.0f)
        {
            auto done = progressArea.withWidth (progressArea.getWidth()
                                                * juce::jlimit (0.0f, 1.0f, shown));
            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRed(), done.getX(), 0.0f,
                                                     NordLookAndFeel::nordRedBright(), done.getRight(),
                                                     0.0f, false));
            g.fillRoundedRectangle (done, 4.0f);
        }

        // O rodape precisa ficar acima da faixa dos botoes. Antes ambos
        // ocupavam os ultimos pixels do card e a frase aparecia por baixo dos
        // botoes, principalmente nas telas Android em paisagem.
        content.removeFromBottom (compactLandscape ? 62.0f : 68.0f);
        auto noteArea = content.removeFromBottom (compactLandscape ? 18.0f : 26.0f);
        g.setColour (juce::Colours::white.withAlpha (0.36f));
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawFittedText (footnote, noteArea.toNearestInt(), juce::Justification::centredLeft,
                          1, 0.8f);
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (juce::jmax (16, juce::roundToInt (getWidth() * 0.055f)),
                                              juce::jmax (14, juce::roundToInt (getHeight() * 0.06f)));
        auto content = card.reduced (juce::jmax (24, juce::roundToInt (card.getWidth() * 0.07f)),
                                     juce::jmax (22, juce::roundToInt (card.getHeight() * 0.08f)));
        auto buttons = content.removeFromBottom (58);
        const int gap = 10;
        const bool two = primaryButton.isVisible() && secondaryButton.isVisible();
        const int width = two ? (buttons.getWidth() - gap) / 2
                              : juce::jmin (310, buttons.getWidth());

        if (secondaryButton.isVisible())
            secondaryButton.setBounds (two ? buttons.removeFromLeft (width)
                                           : buttons.withSizeKeepingCentre (width,
                                                                            buttons.getHeight()));
        else
            secondaryButton.setBounds ({});

        if (two)
            buttons.removeFromLeft (gap);

        if (primaryButton.isVisible())
            primaryButton.setBounds (two ? buttons.removeFromLeft (width)
                                         : buttons.removeFromRight (width));
        else
            primaryButton.setBounds ({});
    }

    std::function<void()> onContinue;

    // Refaz a checagem de catalogo. Sem isso, quando o servidor nao responde
    // na primeira configuracao a tela fica sem nenhum botao e o cliente so
    // consegue sair fechando o app.
    std::function<void()> onRetryCatalog;

private:
    enum class Mode
    {
        checking,
        requiredUpdate,
        optionalUpdate,
        legacyAccess,
        starterChoice,
        downloading,
        ready
    };

    static void configureButton (juce::TextButton& button, bool primary)
    {
        button.setColour (juce::TextButton::buttonColourId,
                          primary ? NordLookAndFeel::nordRed() : juce::Colour (0xff292c33));
        button.setColour (juce::TextButton::buttonOnColourId,
                          primary ? NordLookAndFeel::nordRedBright() : juce::Colour (0xff333740));
        button.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    void beginStarterDownload()
    {
        if (starterPack == nullptr || installer != nullptr)
            return;

        keepScreenAwake();
        cancelRequested = false;
        mode = Mode::downloading;
        eyebrow = "INSTALADOR DE BIBLIOTECAS";
        title = "Instalando " + starterPack->name;
        detail = "Preparando download...";
        footnote = juce::String::fromUTF8 (
            u8"Pode demorar em packs grandes. Não feche o app durante a instalação.");
        progress = 0.0f;
        primaryButton.setVisible (false);
        secondaryButton.setButtonText ("Cancelar");
        secondaryButton.setEnabled (true);
        secondaryButton.onClick = [safe = juce::Component::SafePointer<StartupPanel> (this)]
        {
            if (auto* self = safe.getComponent())
                self->requestStarterCancel();
        };
        secondaryButton.setVisible (true);
        resized();
        repaint();

        auto safe = juce::Component::SafePointer<StartupPanel> (this);
        installer = std::make_unique<ChurchDAWServices::LibraryInstaller> (
            *starterPack,
            [safe] (float value, const juce::String& status)
            {
                if (auto* self = safe.getComponent())
                {
                    self->progress = value;
                    self->detail = status;
                    self->repaint();
                }
            },
            [safe] (juce::Result result)
            {
                if (auto* self = safe.getComponent())
                {
                    self->installer.reset();
                    self->restoreScreenSaver();
                    self->cancelRequested = false;
                    self->secondaryButton.setEnabled (true);

                    if (result.wasOk())
                    {
                        ChurchDAWServices::rememberStarterChoiceCompleted();
                        self->showReady();
                        juce::Timer::callAfterDelay (550, [safe]
                        {
                            if (auto* panel = safe.getComponent())
                                if (panel->onContinue != nullptr)
                                    panel->onContinue();
                        });
                    }
                    else
                    {
                        self->mode = Mode::starterChoice;
                        self->eyebrow = juce::String::fromUTF8 (u8"NÃO FOI POSSÍVEL INSTALAR");
                        self->title = juce::String::fromUTF8 (u8"O download não foi concluído");
                        self->detail = result.getErrorMessage();
                        self->footnote = juce::String::fromUTF8 (
                            u8"Nada incompleto foi ativado. Você pode tentar de novo.");
                        self->progress = 0.0f;
                        self->primaryButton.setButtonText ("Tentar novamente");
                        self->primaryButton.setVisible (true);
                        if (self->starterRequiredForAccess)
                        {
                            self->footnote = juce::String::fromUTF8 (
                                u8"A demonstração ainda não foi reservada nem iniciada. "
                                u8"Libere espaço ou corrija a rede e tente novamente.");
                            self->secondaryButton.setVisible (false);
                            self->secondaryButton.onClick = nullptr;
                        }
                        else
                        {
                            self->secondaryButton.setButtonText ("Abrir sem baixar");
                            self->secondaryButton.onClick =
                                [safe = juce::Component::SafePointer<StartupPanel> (self)]
                                {
                                    if (auto* panel = safe.getComponent())
                                    {
                                        ChurchDAWServices::rememberStarterChoiceCompleted();

                                        if (panel->onContinue != nullptr)
                                            panel->onContinue();
                                    }
                                };
                            self->secondaryButton.setVisible (true);
                        }
                        self->resized();
                        self->repaint();
                    }
                }
            });
        installer->begin();
    }

    void requestStarterCancel()
    {
        if (installer == nullptr || cancelRequested)
            return;

        cancelRequested = true;
        detail = juce::String::fromUTF8 (
            u8"Cancelando com segurança... As partes válidas ficam salvas para retomar.");
        secondaryButton.setButtonText ("Cancelando...");
        secondaryButton.setEnabled (false);
        installer->cancel();
        repaint();
    }

    void keepScreenAwake()
    {
        if (! screenSaverManaged)
        {
            screenSaverWasEnabled = juce::Desktop::isScreenSaverEnabled();
            screenSaverManaged = true;

            if (screenSaverWasEnabled)
                juce::Desktop::setScreenSaverEnabled (false);
        }
    }

    void restoreScreenSaver()
    {
        if (screenSaverManaged)
        {
            if (screenSaverWasEnabled)
                juce::Desktop::setScreenSaverEnabled (true);

            screenSaverManaged = false;
        }
    }

    Mode mode { Mode::checking };
    juce::Image logo;
    juce::TextButton primaryButton, secondaryButton;
    juce::String eyebrow, title, detail, footnote;
    float progress = -1.0f;
    ChurchDAWServices::UpdatePolicy activeUpdatePolicy;
    std::unique_ptr<ChurchDAWServices::LibraryPack> starterPack;
    std::unique_ptr<ChurchDAWServices::LibraryInstaller> installer;
    bool cancelRequested = false;
    bool starterRequiredForAccess = false;
    bool screenSaverManaged = false;
    bool screenSaverWasEnabled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StartupPanel)
};
