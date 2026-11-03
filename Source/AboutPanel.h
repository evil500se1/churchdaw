#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "NordLookAndFeel.h"
#include "BinaryData.h"

#include <functional>

//==============================================================================
// Card "Sobre": logo redondo, versao e os dados do proprietario. Abre clicando
// na marca do header. Mesmo acabamento de vidro escuro dos outros cards da DAW.
//==============================================================================
class AboutPanel : public juce::Component
{
public:
    std::function<void()> onClose;

    AboutPanel()
    {
       #if ! JUCE_ANDROID
        logo = juce::ImageCache::getFromMemory (BinaryData::logo_round_png,
                                                BinaryData::logo_round_pngSize);
       #endif

       #if ! JUCE_ANDROID
        whatsBtn.setButtonText (juce::String::fromUTF8 ("Falar no WhatsApp"));
        whatsBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f8f4d));
        whatsBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        whatsBtn.onClick = []
        {
            juce::URL ("https://wa.me/5562981278461").launchInDefaultBrowser();
        };
        addAndMakeVisible (whatsBtn);
       #endif

        privacyBtn.setButtonText (juce::String::fromUTF8 ("Privacidade"));
        privacyBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d35));
        privacyBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        privacyBtn.onClick = []
        {
            // Dominio oficial, com DNS e certificado validos. A URL tecnica
            // antiga agora redireciona para 404 e nao pode aparecer na Play.
            juce::URL ("https://churchdaw.com/politica-de-privacidade.html")
                .launchInDefaultBrowser();
        };
        addAndMakeVisible (privacyBtn);

        closeBtn.setButtonText ("Fechar");
        closeBtn.onClick = [this] { if (onClose != nullptr) onClose(); };
        addAndMakeVisible (closeBtn);

        // Log de diagnostico: o FileLogger criado em Main.cpp roda em
        // %APPDATA%\ChurchDAW\ChurchDAW.log, mas o musico (leigo, no culto)
        // nao tem como achar nem enviar esse arquivo. O botao abre o
        // gerenciador no arquivo; se nao abrir, entrega caminho + conteudo
        // na area de transferencia.
        logBtn.setButtonText (juce::String::fromUTF8 ("Enviar log de diagn\xc3\xb3stico"));
        logBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d35));
        logBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        logBtn.onClick = []
        {
            auto* logger = juce::Logger::getCurrentLogger();
            if (auto* fl = dynamic_cast<juce::FileLogger*> (logger))
            {
                const auto f = fl->getLogFile();
                // revealToUser() devolve void: sempre deixamos caminho + conteudo
                // na area de transferencia, para o musico conseguir colar e enviar
                // mesmo quando o gerenciador de arquivos nao abre.
                if (f.existsAsFile())
                    f.revealToUser();

                juce::String clip = f.getFullPathName();
                if (f.existsAsFile())
                    clip << juce::newLine << juce::newLine << f.loadFileAsString();
                juce::SystemClipboard::copyTextToClipboard (clip);
            }
            else
            {
                juce::SystemClipboard::copyTextToClipboard (
                    juce::String::fromUTF8 ("Log de arquivo indispon\xc3\xadvel neste modo."));
            }
        };
        addAndMakeVisible (logBtn);

        setSize (392 + margin * 2, 476 + margin * 2);
    }

    void paint (juce::Graphics& g) override
    {
        const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
        auto card = getLocalBounds().toFloat().reduced ((float) margin);
        const float rad = 14.0f;

        g.setColour (desktop ? juce::Colour (0xff1b1f26) : NordLookAndFeel::bgCard());
        g.fillRoundedRectangle (card, rad);
        g.setColour (desktop ? juce::Colour (0xff3b414b) : NordLookAndFeel::borderDefault());
        g.drawRoundedRectangle (card.reduced (0.5f), rad, 1.0f);

        auto body = card.toNearestInt().reduced (24, 22);
        body.removeFromTop (4);

        // logo redondo (a arte tem fundo preto chapado -> recorta no circulo)
        auto logoSlot = body.removeFromTop (168);
        auto logoBox = desktop ? logoSlot.withTrimmedBottom (38).withSizeKeepingCentre (112, 112)
                               : logoSlot.withSizeKeepingCentre (168, 168);

       #if JUCE_ANDROID
        NordLookAndFeel::drawChurchDAWMark (g, logoBox.toFloat());
       #else
        if (logo.isValid())
        {
            juce::Path circle;
            circle.addEllipse (logoBox.toFloat());

            juce::Graphics::ScopedSaveState clipped (g);
            g.reduceClipRegion (circle);
            g.setOpacity (1.0f);   // imagem herda o alpha do ultimo setColour
            g.drawImageWithin (logo, logoBox.getX(), logoBox.getY(),
                               logoBox.getWidth(), logoBox.getHeight(),
                               juce::RectanglePlacement::centred);
        }
       #endif

        if (desktop)
        {
            g.setColour (NordLookAndFeel::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
            g.drawText ("ChurchDAW", logoSlot.removeFromBottom (32), juce::Justification::centred);
        }
        body.removeFromTop (12);

        g.setColour (desktop ? NordLookAndFeel::txtSecondary() : NordLookAndFeel::dimText().withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        auto version = juce::String ("--");
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            version = app->getApplicationVersion();

        g.drawText (juce::String::fromUTF8 ("Vers\xc3\xa3o ") + version,
                    body.removeFromTop (16), juce::Justification::centred);

        body.removeFromTop (14);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (body.removeFromTop (1));
        body.removeFromTop (16);

        g.setColour (desktop ? NordLookAndFeel::txtSecondary() : NordLookAndFeel::nordRedBright().withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (desktop ? 10.5f : 9.5f)).boldened()
                       .withExtraKerningFactor (desktop ? 0.035f : 0.18f));
        g.drawText (juce::String::fromUTF8 ("PROPRIET\xc3\x81RIO"),
                    body.removeFromTop (14), juce::Justification::centred);

        body.removeFromTop (4);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (21.0f)).boldened());
        g.drawText ("Igor Teixeira", body.removeFromTop (28), juce::Justification::centred);

       #if ! JUCE_ANDROID
        body.removeFromTop (2);
        g.setColour (NordLookAndFeel::dimText());
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (juce::String::fromUTF8 ("WhatsApp (62) 98127-8461"),
                    body.removeFromTop (20), juce::Justification::centred);
       #endif
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (margin);
        auto row = card.removeFromBottom (62).removeFromTop (38).reduced (24, 0);

        // Botao de log em faixa propria, logo acima dos tres do rodape.
        auto logRow = card.removeFromBottom (44).removeFromTop (38).reduced (24, 0);
        logBtn.setBounds (logRow);

        constexpr int gap = 8;
       #if JUCE_ANDROID
        const int buttonWidth = (row.getWidth() - gap) / 2;
        privacyBtn.setBounds (row.removeFromLeft (buttonWidth));
        row.removeFromLeft (gap);
        closeBtn.setBounds (row);
       #else
        const int buttonWidth = (row.getWidth() - gap * 2) / 3;
        privacyBtn.setBounds (row.removeFromLeft (buttonWidth));
        row.removeFromLeft (gap);
        whatsBtn.setBounds (row.removeFromLeft (buttonWidth));
        row.removeFromLeft (gap);
        closeBtn.setBounds (row);
       #endif
    }

private:
    static constexpr int margin = 18;   // respiro pra sombra caber

    juce::Image logo;
    juce::TextButton privacyBtn, whatsBtn, closeBtn, logBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutPanel)
};
