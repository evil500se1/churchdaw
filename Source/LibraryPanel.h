#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <thread>
#include "ChurchDAWServices.h"
#include "AndroidCultoConnectivity.h"
#include "NordLookAndFeel.h"
#include "LibraryCatalogStatus.h"

class LibraryCatalogPanel;

//==============================================================================
class LibraryPackCard final : public juce::Component
{
public:
    explicit LibraryPackCard (ChurchDAWServices::LibraryPack packToShow,
                              bool catalogActionsAreAllowed,
                              juce::String unavailableButtonText)
        : pack (std::move (packToShow)),
          catalogActionsAllowed (catalogActionsAreAllowed),
          catalogUnavailableText (std::move (unavailableButtonText))
    {
        addAndMakeVisible (action);
        action.onClick = [this]
        {
            if (onAction != nullptr)
                onAction (pack);
        };
        refreshState();
    }

    void setCatalogActionsAllowed (bool allowed, juce::String unavailableButtonText)
    {
        catalogActionsAllowed = allowed;
        catalogUnavailableText = std::move (unavailableButtonText);
        refreshState();
    }

    void refreshState()
    {
        // refreshState() tambem e chamado ao terminar uma instalacao. O estado
        // visual antigo precisa morrer aqui; antes o botao virava "Instalada",
        // mas a barra e a mensagem "Instalando..." continuavam no card.
        installing = false;
        progress = 0.0f;
        installText.clear();
        lastError.clear();

        const auto managedFolder = pack.detectedInstalledFolder();
        const bool managedPathExists = managedFolder.exists();
        const bool managedFolderExists = managedFolder.isDirectory();
        const int installedVersion = pack.installedVersion();
        const bool managedContentIsUsable =
            managedFolderExists && containsUsableSample (managedFolder);
        const bool managedInstallIsInconsistent =
            managedPathExists
            && (! managedFolderExists
                || ! managedFolder.getChildFile (".churchdaw-library.json").existsAsFile()
                || installedVersion <= 0
                || ! managedContentIsUsable);
        const bool packIsInstalled = pack.isInstalled() && ! managedInstallIsInconsistent;
        installed = packIsInstalled;
        const bool updateAvailable = installedVersion > 0
                                  && installedVersion < pack.version
                                  && (! managedPathExists || managedContentIsUsable);

        if (packIsInstalled)
        {
            status = "INSTALADA";
            statusColour = juce::Colour (0xff63d28b);
            action.setButtonText ("Instalada");
            action.setEnabled (false);
        }
        else if (! pack.included)
        {
            status = pack.priceLabel.isNotEmpty() ? pack.priceLabel.toUpperCase()
                                                  : "VENDIDA SEPARADAMENTE";
            statusColour = juce::Colour (0xffe2ad54);
            const bool comingSoon =
                ! ChurchDAWServices::paidLibraryPurchasesEnabled
                || pack.priceLabel.containsIgnoreCase ("em breve");
            action.setButtonText (comingSoon ? "Em breve" : "Ver detalhes");
            action.setEnabled (! comingSoon && pack.purchaseUrl.isNotEmpty());
        }
        else if (pack.canDownload())
        {
            if (managedInstallIsInconsistent)
            {
                status = "PRECISA DE REPARO";
                statusColour = juce::Colour (0xffee6b6b);
                action.setButtonText ("Reparar");
            }
            else if (updateAvailable)
            {
                status = "ATUALIZACAO DISPONIVEL";
                statusColour = juce::Colour (0xffe2ad54);
                action.setButtonText ("Atualizar");
            }
            else
            {
                status = "INCLUIDA";
                statusColour = NordLookAndFeel::nordRedBright();
                action.setButtonText ("Baixar");
            }

            action.setEnabled (true);
        }
        else
        {
            status = "EM BREVE";
            statusColour = juce::Colours::white.withAlpha (0.42f);
            action.setButtonText ("Indisponivel");
            action.setEnabled (false);
        }

        if (! packIsInstalled && ! catalogActionsAllowed)
        {
            action.setButtonText (catalogUnavailableText.isNotEmpty()
                                      ? catalogUnavailableText
                                      : juce::String ("Sem conexao"));
            action.setEnabled (false);
        }

        repaint();
    }

    void setInstalling (float value, const juce::String& text)
    {
        installing = true;
        progress = value;
        installText = text;
        lastError.clear();
        action.setButtonText ("Instalando...");
        action.setEnabled (false);
        repaint();
    }

    void setInstallFailed (const juce::String& text)
    {
        installing = false;
        installText = text;
        lastError = text;
        status = "TENTE NOVAMENTE";
        statusColour = juce::Colour (0xffee6b6b);
        action.setButtonText ("Tentar novamente");
        action.setEnabled (catalogActionsAllowed && pack.canDownload());
        repaint();
    }

    const juce::String& id() const noexcept { return pack.id; }
    bool isInstalled() const noexcept { return installed; }

    void paint (juce::Graphics& g) override
    {
        const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
        const bool dense = getHeight() <= 100;
        const bool compact = dense || getHeight() <= 118;
        auto card = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (desktop ? juce::Colour (0xff22262d) : NordLookAndFeel::bgCardHi());
        g.fillRoundedRectangle (card, dense ? 8.0f : 10.0f);
        g.setColour (desktop ? juce::Colours::white.withAlpha (0.055f) : NordLookAndFeel::borderSoft());
        g.drawRoundedRectangle (card.reduced (0.5f), dense ? 8.0f : 10.0f, 1.0f);

        const int marginX = dense ? 10 : compact ? 14 : 18;
        const int marginY = dense ? 8 : compact ? 10 : 13;
        const int buttonWidth = dense ? 112 : compact ? 150 : 136;
        auto area = getLocalBounds().reduced (marginX, marginY);
        const auto buttonSpace = area.removeFromRight (buttonWidth);
        area.removeFromRight (dense ? 8 : compact ? 12 : 18);

        auto statusArea = area.removeFromTop (dense ? 14 : compact ? 17 : 20);
        g.setColour (statusColour);
        g.setFont (juce::Font (juce::FontOptions (dense ? 9.5f : compact ? 11.5f : 11.0f)).boldened()
                       .withExtraKerningFactor (desktop ? 0.025f : 0.08f));
        g.drawText (status, statusArea, juce::Justification::centredLeft);

        auto titleArea = area.removeFromTop (dense ? 22 : compact ? 27 : 35);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (dense ? 15.0f : compact ? 17.0f : 19.0f)).boldened());
        g.drawFittedText (
            ChurchDAWAndroidCulto::displayNameForPack (pack.id, pack.name),
            titleArea, juce::Justification::centredLeft, 1, 0.8f);

        juce::String meta;

        if (pack.versionName.isNotEmpty())
            meta = "v" + pack.versionName;

        if (pack.downloadBytes > 0)
            meta += (meta.isNotEmpty() ? "  /  " : "")
                  + ChurchDAWServices::humanBytes (pack.downloadBytes);

        if (meta.isNotEmpty())
        {
            g.setColour (desktop ? NordLookAndFeel::txtSecondary() : juce::Colours::white.withAlpha (0.42f));
            g.setFont (juce::Font (juce::FontOptions (dense ? 9.5f : 11.5f)));
            g.drawText (meta, area.removeFromTop (dense ? 13 : compact ? 16 : 18),
                        juce::Justification::centredLeft);
        }

        g.setColour (desktop ? NordLookAndFeel::txtSecondary() : juce::Colours::white.withAlpha (0.62f));
        g.setFont (juce::Font (juce::FontOptions (dense ? 10.5f : compact ? 12.0f : 12.5f)));
        const auto bodyText = installing ? installText
                            : lastError.isNotEmpty() ? lastError
                            : ChurchDAWAndroidCulto::descriptionHintForPack (
                                  pack.id, pack.description);
        g.drawFittedText (bodyText, area, juce::Justification::topLeft,
                          compact ? 2 : 3, 0.8f);

        if (installing)
        {
            auto bar = getLocalBounds().toFloat().reduced (dense ? 10.0f : 18.0f, 0.0f)
                          .removeFromBottom (dense ? 4.0f : 6.0f);
            bar.setRight ((float) buttonSpace.getX() - (dense ? 10.0f : 18.0f));
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (NordLookAndFeel::nordRedBright());
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth()
                                                    * juce::jlimit (0.0f, 1.0f, progress)),
                                    3.0f);
        }
    }

    void resized() override
    {
        const bool dense = getHeight() <= 100;
        const bool compact = dense || getHeight() <= 118;
        const int buttonWidth = dense ? 112 : compact ? 150 : 136;
        const int buttonHeight = dense ? 36 : compact ? 48 : 42;
        action.setBounds (getLocalBounds().reduced (dense ? 10 : compact ? 14 : 18,
                                                    dense ? 7 : compact ? 8 : 16)
                           .removeFromRight (buttonWidth)
                           .withSizeKeepingCentre (buttonWidth, buttonHeight));
    }

    std::function<void (const ChurchDAWServices::LibraryPack&)> onAction;

private:
    bool containsUsableSample (const juce::File& folder) const
    {
        if (! folder.isDirectory())
            return false;

        // O Pad Continuo e um conjunto indivisivel de sete notas naturais.
        // Um marker valido mais apenas um WAV fazia o card dizer INSTALADA e
        // desabilitar o botao, enquanto o painel do Pad continuava indisponivel.
        // Nesse caso o catalogo deve oferecer Reparar.
        if (ChurchDAWServices::isPadContinuoPackId (pack.id))
        {
            auto complete = [] (const juce::File& candidate)
            {
                if (! candidate.isDirectory())
                    return false;

                for (const char note : { 'A', 'B', 'C', 'D', 'E', 'F', 'G' })
                    if (! candidate.getChildFile (
                              "pad_" + juce::String::charToString (note) + ".wav")
                             .existsAsFile())
                        return false;

                return true;
            };

            if (complete (folder))
                return true;

            for (const auto& entry : juce::RangedDirectoryIterator (
                     folder, true, "*", juce::File::findDirectories))
                if (complete (entry.getFile()))
                    return true;

            return false;
        }

        // Iterador lazy: para no primeiro sample, sem materializar os milhares
        // de arquivos do Church Live apenas para decidir o estado do botao.
        for (const auto& entry : juce::RangedDirectoryIterator (
                 folder, true, "*", juce::File::findFiles))
        {
            const auto extension = entry.getFile().getFileExtension();

            if (extension.equalsIgnoreCase (".flac")
                || extension.equalsIgnoreCase (".wav")
                || extension.equalsIgnoreCase (".aif")
                || extension.equalsIgnoreCase (".aiff"))
                return true;
        }

        return false;
    }

    ChurchDAWServices::LibraryPack pack;
    juce::TextButton action;
    juce::String status, installText, lastError;
    bool catalogActionsAllowed = false;
    juce::String catalogUnavailableText;
    juce::Colour statusColour { juce::Colours::white };
    bool installed = false;
    bool installing = false;
    float progress = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryPackCard)
};

//==============================================================================
class LibraryCatalogPanel final : public juce::Component,
                                  private juce::Thread
{
public:
    LibraryCatalogPanel()
        : juce::Thread ("ChurchDAW catalog refresh")
    {
        setOpaque (false);
        setInterceptsMouseClicks (true, true);
        setWantsKeyboardFocus (true);
        setSize (900, 590);

        closeButton.setButtonText ("Fechar");
        refreshButton.setButtonText ("Atualizar catalogo");
        legacyAccessButton.setButtonText ("Liberar acervo antigo");
        legacyAccessButton.setColour (juce::TextButton::buttonColourId,
                                      juce::Colour (0xff312125));
        legacyAccessButton.setColour (juce::TextButton::textColourOffId,
                                      juce::Colours::white);
        addAndMakeVisible (closeButton);
        addAndMakeVisible (refreshButton);
        for (auto* filter : { &allFilter, &installedFilter, &availableFilter })
        {
            filter->setClickingTogglesState (true);
            filter->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff24272e));
            filter->setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
            filter->setColour (juce::TextButton::textColourOffId,
                               NordLookAndFeel::txtSecondary());
            filter->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            addAndMakeVisible (*filter);
        }
        allFilter.setButtonText ("Todas");
        installedFilter.setButtonText ("Instaladas");
        availableFilter.setButtonText ("Disponiveis");
        allFilter.setToggleState (true, juce::dontSendNotification);
        allFilter.onClick = [this] { setLibraryFilter (0); };
        installedFilter.onClick = [this] { setLibraryFilter (1); };
        availableFilter.onClick = [this] { setLibraryFilter (2); };
        addAndMakeVisible (legacyAccessButton);
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
        content.addAndMakeVisible (emptyState);
        emptyState.setJustificationType (juce::Justification::centred);
        emptyState.setColour (juce::Label::textColourId,
                              juce::Colours::white.withAlpha (0.52f));
        emptyState.setFont (juce::Font (juce::FontOptions (15.0f)));
        emptyState.setInterceptsMouseClicks (false, false);

        closeButton.onClick = [this]
        {
            if (installer != nullptr)
            {
                if (! cancelRequested)
                {
                    cancelRequested = true;
                    closeButton.setButtonText ("Cancelando...");
                    closeButton.setEnabled (false);
                    statusText = "Cancelando com seguranca. Partes validas serao preservadas...";
                    installer->cancel();
                    repaint();
                }

                return;
            }

            if (onClose != nullptr)
                onClose();
        };

        refreshButton.onClick = [this]
        {
            if (! isThreadRunning() && installer == nullptr)
            {
                refreshButton.setEnabled (false);
                catalogActionsAllowed = false;
                catalogUnavailableText = "Verificando...";
                statusText = "Buscando o catalogo seguro...";
                updateCardsCatalogAvailability();
                repaint();
                startThread();
            }
        };

        legacyAccessButton.onClick = [this]
        {
            requestLegacyLibraryAccess();
        };

        refreshLegacyAccessState();
        manifest = ChurchDAWServices::loadCachedManifest();
        catalogActionsAllowed = false;
        catalogUnavailableText = "Verificando...";
        statusText = manifest.valid
            ? (ChurchDAWServices::cacheIsFresh()
                  ? juce::String ("Catalogo salvo. Validando a conexao...")
                  : juce::String ("Catalogo salvo expirado. Conecte para atualizar."))
            : juce::String ("Buscando o catalogo...");
        rebuildCards();

        // A abertura do painel tambem tenta atualizar. O cache faz a tela nascer
        // instantanea, mas fica SOMENTE PARA LEITURA ate uma resposta viva. Isso
        // impede download/compra por URL antiga quando o cache ja venceu.
        startThread();
    }

    ~LibraryCatalogPanel() override
    {
        signalThreadShouldExit();
        if (installer != nullptr)
        {
            installer->cancel();
            if (installer->isThreadRunning() && ! installer->waitForThreadToExit (60))
                std::thread ([dead = std::move (installer)] () mutable { dead.reset(); }).detach();
            else
                installer.reset();
        }
       #if JUCE_ANDROID || JUCE_IOS
        stopThread (1200);
       #else
        stopThread (16000);
       #endif
        restoreScreenSaver();
        viewport.setViewedComponent (nullptr, false);
    }

    void run() override
    {
        auto check = ChurchDAWServices::checkRemoteManifest();
        // A recuperacao de transacoes interrompidas e a leitura dos markers
        // locais podem percorrer/remover diretorios grandes. Fazemos tudo no
        // worker do painel; rebuildCards() fica estritamente visual.
        auto installed = ChurchDAWServices::loadInstalledLibraryPacks();

        if (threadShouldExit())
            return;

        auto safe = juce::Component::SafePointer<LibraryCatalogPanel> (this);
        juce::MessageManager::callAsync ([safe, check = std::move (check),
                                          installed = std::move (installed)] () mutable
        {
            if (auto* self = safe.getComponent())
            {
                self->installedPacks = std::move (installed);
                self->refreshButton.setEnabled (true);
                const auto failure = check.catalogFailure != ChurchDAWDistribution::Failure::none
                    ? check.catalogFailure : check.updateFailure;

                if (check.reachedServer && check.manifest.valid)
                {
                    self->manifest = std::move (check.manifest);
                    self->catalogActionsAllowed = true;
                    self->catalogUnavailableText.clear();
                    self->statusText = "Catalogo atualizado e pronto";
                    self->catalogEmptyText = "Nenhum pack publicado.\nAs bibliotecas instaladas continuam disponiveis.";
                }
                else
                {
                    // Um cache recente ainda e util para mostrar o que ja existe,
                    // mas nao autoriza uma acao nova depois de a rede falhar.
                    if (check.manifest.valid)
                        self->manifest = std::move (check.manifest);

                    self->catalogActionsAllowed = false;
                    const auto status = ChurchDAWLibraryStatus::unavailable (failure, false);
                    self->catalogUnavailableText = status.button;
                    self->statusText = status.message;
                    self->catalogEmptyText = status.empty;
                    juce::Logger::writeToLog (
                        "Library catalog unavailable: update=" + check.updateDiagnostic
                        + " catalog=" + check.diagnostic);
                }

                self->rebuildCards();

                if (! self->catalogActionsAllowed)
                {
                    const auto status = ChurchDAWLibraryStatus::unavailable (
                        failure, self->showingInstalledLibraries);
                    self->statusText = status.message;
                }

                self->repaint();
            }
        });
    }

    void paint (juce::Graphics& g) override
    {
        // O componente ocupa o pai inteiro. Alem do efeito visual, esta pelicula
        // intercepta o toque fora do card e impede mudar timbre/abrir outro
        // painel por baixo durante uma instalacao.
        const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
        g.setColour (juce::Colours::black.withAlpha (desktop ? 0.58f : 0.72f));
        g.fillRect (getLocalBounds());

        const bool dense = isShortLandscape();
        const bool compact = dense || isCompact();
        auto bounds = surfaceBounds().toFloat();
        const float corner = desktop ? 14.0f : dense ? 12.0f : compact ? 14.0f : 20.0f;
        g.setColour (desktop ? juce::Colour (0xff1b1f26) : NordLookAndFeel::bgCard());
        g.fillRoundedRectangle (bounds, corner);
        g.setColour (desktop ? juce::Colour (0xff3b414b) : NordLookAndFeel::borderDefault());
        g.drawRoundedRectangle (bounds.reduced (0.5f), corner, 1.0f);

        const int margin = dense ? 8 : compact ? 12 : 26;
        const int headerHeight = dense ? 44 : compact ? 64 : 82;
        auto area = surfaceBounds().reduced (margin);
        auto header = area.removeFromTop (headerHeight);
        auto textHeader = header;
        textHeader.removeFromRight (dense ? 220 : compact ? 296 : 320);
        g.setColour (desktop ? NordLookAndFeel::txtSecondary() : NordLookAndFeel::nordRedBright());
        g.setFont (juce::Font (juce::FontOptions (dense ? 9.0f : compact ? 10.5f : 11.5f)).boldened()
                       .withExtraKerningFactor (desktop ? 0.035f : 0.10f));
        g.drawText ("CHURCHDAW STORE", textHeader.removeFromTop (dense ? 12 : compact ? 15 : 20),
                    juce::Justification::centredLeft);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (dense ? 18.0f : compact ? 22.0f : 27.0f)).boldened());
        g.drawText ("Bibliotecas", textHeader.removeFromTop (dense ? 21 : compact ? 28 : 38),
                    juce::Justification::centredLeft);
        g.setColour (desktop ? NordLookAndFeel::txtSecondary() : juce::Colours::white.withAlpha (0.46f));
        g.setFont (juce::Font (juce::FontOptions (dense ? 9.5f : 11.5f)));
        g.drawFittedText (statusText, textHeader, juce::Justification::centredLeft,
                          1, 0.72f);

        area.removeFromTop (dense ? 4 : compact ? 6 : 10);
        area.removeFromTop (dense ? 30 : compact ? 36 : 40);
        area.removeFromTop (dense ? 4 : compact ? 6 : 8);

        if (legacyAccessButton.isVisible())
        {
            auto legacyCard = area.removeFromTop (dense ? 48 : compact ? 64 : 72).toFloat();
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.10f));
            g.fillRoundedRectangle (legacyCard, dense ? 8.0f : compact ? 10.0f : 12.0f);
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.34f));
            g.drawRoundedRectangle (legacyCard.reduced (0.5f),
                                    dense ? 8.0f : compact ? 10.0f : 12.0f, 1.0f);

            auto copy = legacyCard.reduced (dense ? 10.0f : compact ? 12.0f : 15.0f,
                                            dense ? 5.0f : compact ? 7.0f : 9.0f);
            copy.removeFromRight ((float) (dense ? 160 : compact ? 202 : 218));
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (dense ? 10.5f : compact ? 12.0f : 13.5f)).boldened());
            g.drawText ("Ja tinha timbres neste aparelho?",
                        copy.removeFromTop (dense ? 16.0f : compact ? 20.0f : 23.0f),
                        juce::Justification::centredLeft);
            g.setColour (juce::Colours::white.withAlpha (0.58f));
            g.setFont (juce::Font (juce::FontOptions (dense ? 8.5f : compact ? 10.0f : 11.0f)));
            g.drawFittedText (
                "Autorize o Android para verificar Music/ChurchDAW. Opcional; nada sera movido ou apagado.",
                copy.toNearestInt(), juce::Justification::centredLeft, 2, 0.78f);
        }
    }

    void resized() override
    {
        const bool dense = isShortLandscape();
        const bool compact = dense || isCompact();
        const int margin = dense ? 8 : compact ? 12 : 26;
        const int headerHeight = dense ? 44 : compact ? 64 : 82;
        const int closeWidth = dense ? 82 : compact ? 106 : 110;
        const int refreshWidth = dense ? 122 : compact ? 174 : 176;
        const int buttonHeight = dense ? 34 : compact ? 48 : 38;
        const int cardHeight = dense ? 96 : compact ? 112 : 130;
        const int cardStep = dense ? 104 : compact ? 120 : 142;

        auto area = surfaceBounds().reduced (margin);
        auto header = area.removeFromTop (headerHeight);
        const int actionGap = dense ? 8 : 10;
        auto actions = header.removeFromRight (closeWidth + refreshWidth + actionGap);
        closeButton.setBounds (actions.removeFromRight (closeWidth)
                                   .withSizeKeepingCentre (closeWidth, buttonHeight));
        actions.removeFromRight (actionGap);
        refreshButton.setBounds (actions.removeFromRight (refreshWidth)
                                     .withSizeKeepingCentre (refreshWidth, buttonHeight));
        area.removeFromTop (dense ? 4 : compact ? 6 : 10);

        auto filters = area.removeFromTop (dense ? 30 : compact ? 36 : 40);
        const int filterGap = dense ? 5 : 7;
        const int filterWidth = juce::jmax (1,
            (filters.getWidth() - filterGap * 2) / 3);
        allFilter.setBounds (filters.removeFromLeft (filterWidth));
        filters.removeFromLeft (filterGap);
        installedFilter.setBounds (filters.removeFromLeft (filterWidth));
        filters.removeFromLeft (filterGap);
        availableFilter.setBounds (filters);
        area.removeFromTop (dense ? 4 : compact ? 6 : 8);

        if (legacyAccessButton.isVisible())
        {
            auto legacyCard = area.removeFromTop (dense ? 48 : compact ? 64 : 72);
            const int legacyButtonWidth = dense ? 148 : compact ? 184 : 198;
            const int legacyButtonHeight = dense ? 34 : compact ? 42 : 40;
            legacyAccessButton.setBounds (
                legacyCard.removeFromRight (legacyButtonWidth + (dense ? 8 : compact ? 10 : 14))
                    .removeFromRight (legacyButtonWidth)
                    .withSizeKeepingCentre (legacyButtonWidth, legacyButtonHeight));
            area.removeFromTop (dense ? 4 : compact ? 6 : 10);
        }

        viewport.setBounds (area);
        const int contentWidth =
            juce::jmax (1, viewport.getWidth() - viewport.getScrollBarThickness());
        const int columnGap = dense ? 8 : 0;
        const int columns = dense && contentWidth >= 680 ? 2 : 1;
        int visibleCards = 0;
        for (auto* card : cards)
            visibleCards += card->isVisible() ? 1 : 0;

        const int rows = (visibleCards + columns - 1) / columns;
        content.setSize (contentWidth, juce::jmax (viewport.getHeight(), rows * cardStep));
        const int cardWidth =
            juce::jmax (1, (content.getWidth() - columnGap * (columns - 1)) / columns);

        int visibleIndex = 0;
        for (int i = 0; i < cards.size(); ++i)
        {
            if (! cards[i]->isVisible())
                continue;

            const int column = visibleIndex % columns;
            const int row = visibleIndex / columns;
            const int x = column * (cardWidth + columnGap);
            const int width = column == columns - 1 ? content.getWidth() - x : cardWidth;
            cards[i]->setBounds (x, row * cardStep, width, cardHeight);
            ++visibleIndex;
        }

        emptyState.setBounds (content.getLocalBounds().reduced (24));
    }

    void setLibraryFilter (int filter)
    {
        libraryFilter = juce::jlimit (0, 2, filter);
        allFilter.setToggleState (libraryFilter == 0, juce::dontSendNotification);
        installedFilter.setToggleState (libraryFilter == 1, juce::dontSendNotification);
        availableFilter.setToggleState (libraryFilter == 2, juce::dontSendNotification);
        applyLibraryFilter();
        resized();
        repaint();
    }

    void applyLibraryFilter()
    {
        int visibleCount = 0;

        for (auto* card : cards)
        {
            const bool visible = libraryFilter == 0
                              || (libraryFilter == 1 && card->isInstalled())
                              || (libraryFilter == 2 && ! card->isInstalled());
            card->setVisible (visible);
            visibleCount += visible ? 1 : 0;
        }

        emptyState.setVisible (visibleCount == 0);

        if (visibleCount == 0)
        {
            // All restores the current catalog result after leaving a filter;
            // Installed/Available keep their own inventory-specific prompts.
            emptyState.setText (libraryFilter == 1
                                  ? juce::String ("Nenhuma biblioteca instalada.")
                                  : libraryFilter == 2
                                      ? juce::String::fromUTF8 ("Nenhuma biblioteca dispon\xc3\xadvel.")
                                      : catalogEmptyText,
                                juce::dontSendNotification);
        }
    }

    void parentSizeChanged() override
    {
        if (auto* parent = getParentComponent())
            setBounds (parent->getLocalBounds());
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey && installer == nullptr)
        {
            if (onClose != nullptr)
                onClose();

            return true;
        }

        return false;
    }

    std::function<void()> onClose;
    std::function<void()> onLibrariesChanged;

private:
    bool isCompact() const noexcept
    {
        return surfaceBounds().getHeight() < 500;
    }

    bool isShortLandscape() const noexcept
    {
       #if JUCE_ANDROID || JUCE_IOS
        const auto surface = surfaceBounds();

        if (surface.getWidth() < 700
            || surface.getHeight() > 400
            || surface.getWidth() < surface.getHeight() * 2)
            return false;

        // Nao ativa a grade densa em janela livre/split-screen: ela e um perfil
        // para o telefone deitado ocupando o display. O desktop e o portrait
        // continuam exatamente nos ramos existentes.
        auto* topLevel = getTopLevelComponent();

        if (topLevel == nullptr)
            return false;

        const auto windowOnScreen = topLevel->getScreenBounds();

        if (const auto* display =
                juce::Desktop::getInstance().getDisplays().getDisplayForRect (windowOnScreen))
        {
            const auto userBounds = display->userBounds.toNearestInt();

            // Em landscape com camera/notch o Android pode entregar a janela ja
            // aparada na lateral (TANK3: cerca de 37 dp). Isso ainda e fullscreen,
            // nao uma janela livre. Altura quase total + largura a ate 64 dp da
            // user area distingue esse caso de split-screen.
            return windowOnScreen.getWidth() >= userBounds.getWidth() - 64
                && windowOnScreen.getHeight() >= userBounds.getHeight() - 8;
        }
       #endif

        return false;
    }

    juce::Rectangle<int> surfaceBounds() const
    {
        auto available = getLocalBounds().reduced (getHeight() < 500 ? 4 : 12);
        const int width = juce::jmin (900, available.getWidth());
        const int height = juce::jmin (590, available.getHeight());
        return available.withSizeKeepingCentre (width, height);
    }

    void updateCardsCatalogAvailability()
    {
        for (auto* card : cards)
            card->setCatalogActionsAllowed (catalogActionsAllowed,
                                            catalogUnavailableText);
    }

    void refreshLegacyAccessState()
    {
       #if JUCE_ANDROID
        const bool granted = juce::RuntimePermissions::isGranted (
            juce::RuntimePermissions::readMediaAudio);
        legacyAccessButton.setVisible (! granted);
        legacyAccessButton.setEnabled (! granted && installer == nullptr
                                       && ! legacyScanInProgress);
       #else
        legacyAccessButton.setVisible (false);
       #endif

        resized();
        repaint();
    }

    void requestLegacyLibraryAccess()
    {
       #if JUCE_ANDROID
        if (installer != nullptr || legacyScanInProgress)
            return;

        if (juce::RuntimePermissions::isGranted (
                juce::RuntimePermissions::readMediaAudio))
        {
            refreshLegacyAccessState();
            return;
        }

        statusText = "O Android vai pedir acesso somente aos arquivos de audio do acervo antigo.";
        legacyAccessButton.setEnabled (false);
        repaint();

        auto safe = juce::Component::SafePointer<LibraryCatalogPanel> (this);
        juce::RuntimePermissions::request (
            juce::RuntimePermissions::readMediaAudio,
            [safe] (bool granted)
            {
                ChurchDAWServices::rememberLegacyLibraryPromptHandled();

                auto* self = safe.getComponent();

                if (self == nullptr)
                    return;

                if (! granted)
                {
                    self->statusText =
                        "Acesso nao liberado. Voce pode tentar novamente aqui ou permitir Audio nas configuracoes do Android.";
                    self->legacyAccessButton.setButtonText ("Tentar acesso novamente");
                    self->refreshLegacyAccessState();
                    return;
                }

                self->legacyScanInProgress = true;
                self->statusText = "Acesso liberado. Verificando o acervo antigo sem mover arquivos...";
                self->refreshLegacyAccessState();

                if (! juce::Thread::launch ([safe]
                    {
                        const bool found =
                            ChurchDAWServices::hasLegacyInstalledLibrary();

                        juce::MessageManager::callAsync ([safe, found]
                        {
                            if (auto* panel = safe.getComponent())
                            {
                                panel->legacyScanInProgress = false;
                                panel->statusText = found
                                    ? juce::String (
                                        "Acervo antigo reconhecido e pronto. Nenhum sample foi movido.")
                                    : juce::String (
                                        "Acesso liberado. Nenhum sample compativel foi encontrado em Music/ChurchDAW.");

                                if (panel->onLibrariesChanged != nullptr)
                                    panel->onLibrariesChanged();

                                panel->refreshLegacyAccessState();
                            }
                        });
                    }))
                {
                    self->legacyScanInProgress = false;
                    self->statusText =
                        "Acesso liberado. Abra Bibliotecas novamente para verificar o acervo.";

                    if (self->onLibrariesChanged != nullptr)
                        self->onLibrariesChanged();

                    self->refreshLegacyAccessState();
                }
            });
       #endif
    }

    void rebuildCards()
    {
        cards.clear (true);
        showingInstalledLibraries = false;
        juce::StringArray displayedIds;

        if (manifest.valid)
            for (const auto& pack : manifest.libraries)
            {
                auto* card = cards.add (new LibraryPackCard (
                    pack, catalogActionsAllowed, catalogUnavailableText));
                card->onAction = [safe = juce::Component::SafePointer<LibraryCatalogPanel> (this)]
                                 (const ChurchDAWServices::LibraryPack& selected)
                {
                    if (auto* self = safe.getComponent())
                        self->handleAction (selected);
                };
                content.addAndMakeVisible (card);
                displayedIds.addIfNotAlreadyThere (pack.id);
            }

        // O catálogo é necessário para baixar/atualizar, mas não para provar que
        // um pack já instalado continua no disco. Mescla os markers locais
        // válidos para que uma falha de rede nunca produza uma tela vazia.
        for (const auto& pack : installedPacks)
        {
            showingInstalledLibraries = true;

            if (displayedIds.contains (pack.id))
                continue;

            auto* card = cards.add (new LibraryPackCard (
                pack, false, "Sem conexao"));
            card->onAction =
                [safe = juce::Component::SafePointer<LibraryCatalogPanel> (this)]
                (const ChurchDAWServices::LibraryPack& selected)
                {
                    if (auto* self = safe.getComponent())
                        self->handleAction (selected);
                };
            content.addAndMakeVisible (card);
            displayedIds.add (pack.id);
        }

        if (cards.isEmpty())
        {
            if (manifest.valid && catalogActionsAllowed)
                statusText = "Nenhum pack publicado. As bibliotecas instaladas continuam disponiveis.";
        }

        applyLibraryFilter();
        resized();
        repaint();
    }

    LibraryPackCard* findCard (const juce::String& id) const
    {
        for (auto* card : cards)
            if (card->id() == id)
                return card;

        return nullptr;
    }

    void handleAction (const ChurchDAWServices::LibraryPack& pack)
    {
        if (installer != nullptr || ! catalogActionsAllowed)
            return;

        if (! pack.included)
        {
            if (ChurchDAWServices::paidLibraryPurchasesEnabled
                 && ChurchDAWServices::isHttpsUrl (pack.purchaseUrl))
                juce::URL (pack.purchaseUrl).launchInDefaultBrowser();

            return;
        }

        if (! pack.canDownload())
            return;

        const auto temporaryBytes =
            ChurchDAWServices::estimatedStreamingInstallPeakBytes (pack);

        if (pack.downloadBytes >= (juce::int64) 100 * 1024 * 1024)
        {
            const auto message =
                "Este e um pack completo, baixado como um unico conjunto.\n\n"
                "Download: " + ChurchDAWServices::humanBytes (pack.downloadBytes)
                + "\nPico conservador de espaco: "
                + ChurchDAWServices::humanBytes (temporaryBytes)
                + "\nCada parte e validada, extraida e liberada antes da proxima."
                + "\nPartes ja instaladas no staging reduzem ainda mais o restante."
                + "\n\nPrefira Wi-Fi, mantenha o app aberto e ligado durante a instalacao.";
            auto safe = juce::Component::SafePointer<LibraryCatalogPanel> (this);

            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::QuestionIcon,
                "Baixar " + pack.name + "?",
                message,
                "Baixar agora",
                "Cancelar",
                this,
                juce::ModalCallbackFunction::create (
                    [safe, pack] (int result)
                    {
                        if (result != 0)
                            if (auto* self = safe.getComponent())
                                self->beginInstall (pack);
                    }));
            return;
        }

        beginInstall (pack);
    }

    void beginInstall (const ChurchDAWServices::LibraryPack& pack)
    {
        if (installer != nullptr || ! catalogActionsAllowed || ! pack.canDownload())
            return;

        keepScreenAwake();
        cancelRequested = false;
        activePackId = pack.id;
        closeButton.setButtonText ("Cancelar");
        closeButton.setEnabled (true);
        refreshButton.setEnabled (false);
        legacyAccessButton.setEnabled (false);
        statusText = "Download seguro em andamento...";
        repaint();

        auto safe = juce::Component::SafePointer<LibraryCatalogPanel> (this);
        installer = std::make_unique<ChurchDAWServices::LibraryInstaller> (
            pack,
            [safe, id = pack.id] (float value, const juce::String& text)
            {
                if (auto* self = safe.getComponent())
                    if (auto* card = self->findCard (id))
                        card->setInstalling (value, text);
            },
            [safe, id = pack.id] (juce::Result result)
            {
                if (auto* self = safe.getComponent())
                {
                    self->installer.reset();
                    self->restoreScreenSaver();
                    self->cancelRequested = false;
                    self->closeButton.setButtonText ("Fechar");
                    self->closeButton.setEnabled (true);
                    self->refreshButton.setEnabled (true);

                    if (auto* card = self->findCard (id))
                    {
                        if (result.wasOk())
                        {
                            card->refreshState();
                            self->statusText = "Biblioteca instalada e pronta para uso.";

                            if (self->onLibrariesChanged != nullptr)
                                self->onLibrariesChanged();
                        }
                        else
                        {
                            card->setInstallFailed (result.getErrorMessage());
                            self->statusText = result.getErrorMessage();
                        }
                    }

                    self->repaint();
                    self->refreshLegacyAccessState();
                }
            });
        installer->begin();
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

    ChurchDAWServices::RemoteManifest manifest;
    juce::Array<ChurchDAWServices::LibraryPack> installedPacks;
    juce::TextButton closeButton, refreshButton, legacyAccessButton;
    juce::TextButton allFilter, installedFilter, availableFilter;
    juce::Viewport viewport;
    juce::Component content;
    juce::Label emptyState;
    juce::OwnedArray<LibraryPackCard> cards;
    int libraryFilter = 0;
    juce::String statusText { "Buscando o catalogo..." };
    juce::String catalogEmptyText { "Buscando o catalogo..." };
    bool catalogActionsAllowed = false;
    juce::String catalogUnavailableText { "Verificando..." };
    juce::String activePackId;
    std::unique_ptr<ChurchDAWServices::LibraryInstaller> installer;
    bool cancelRequested = false;
    bool legacyScanInProgress = false;
    bool showingInstalledLibraries = false;
    bool screenSaverManaged = false;
    bool screenSaverWasEnabled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryCatalogPanel)
};
