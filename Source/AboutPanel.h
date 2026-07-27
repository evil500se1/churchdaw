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
        logo = juce::ImageCache::getFromMemory (BinaryData::logo_round_png, BinaryData::logo_round_pngSize);

        whatsBtn.setButtonText (juce::String::fromUTF8 ("Falar no WhatsApp"));
        whatsBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1f8f4d));
        whatsBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        whatsBtn.onClick = []
        {
            juce::URL ("https://wa.me/5562981278461").launchInDefaultBrowser();
        };
        addAndMakeVisible (whatsBtn);

        closeBtn.setButtonText ("Fechar");
        closeBtn.onClick = [this] { if (onClose != nullptr) onClose(); };
        addAndMakeVisible (closeBtn);

        setSize (392 + margin * 2, 432 + margin * 2);
    }

    void paint (juce::Graphics& g) override
    {
        auto card = getLocalBounds().toFloat().reduced ((float) margin);
        const float rad = 14.0f;

        {
            juce::Path sp;
            sp.addRoundedRectangle (card, rad);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 28, { 0, 8 }).drawForPath (g, sp);
        }

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1c22), card.getX(), card.getY(),
                                                 juce::Colour (0xff101013), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, rad);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine (card.getX() + 14.0f, card.getY() + 1.5f, card.getRight() - 14.0f, card.getY() + 1.5f, 1.0f);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.45f));
        g.drawRoundedRectangle (card, rad, 1.2f);

        // acento vermelho no topo: mesma linguagem dos cards do mixer
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (card.reduced (16.0f, 0.0f).removeFromTop (9.0f).withTrimmedTop (6.0f), 1.5f);

        auto body = card.toNearestInt().reduced (24, 22);
        body.removeFromTop (4);

        // logo redondo (a arte tem fundo preto chapado -> recorta no circulo)
        auto logoBox = body.removeFromTop (168).withSizeKeepingCentre (168, 168);

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

        body.removeFromTop (12);

        g.setColour (NordLookAndFeel::dimText().withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (juce::String::fromUTF8 ("Vers\xc3\xa3o 1.1.0"),
                    body.removeFromTop (16), juce::Justification::centred);

        body.removeFromTop (14);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (body.removeFromTop (1));
        body.removeFromTop (16);

        g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.18f));
        g.drawText (juce::String::fromUTF8 ("PROPRIET\xc3\x81RIO"),
                    body.removeFromTop (14), juce::Justification::centred);

        body.removeFromTop (4);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (21.0f)).boldened());
        g.drawText ("Igor Teixeira", body.removeFromTop (28), juce::Justification::centred);

        body.removeFromTop (2);
        g.setColour (NordLookAndFeel::dimText());
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (juce::String::fromUTF8 ("WhatsApp (62) 98127-8461"),
                    body.removeFromTop (20), juce::Justification::centred);
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (margin);
        auto row = card.removeFromBottom (62).removeFromTop (38).reduced (24, 0);

        whatsBtn.setBounds (row.removeFromLeft (row.getWidth() / 2 - 5));
        row.removeFromLeft (10);
        closeBtn.setBounds (row);
    }

private:
    static constexpr int margin = 18;   // respiro pra sombra caber

    juce::Image logo;
    juce::TextButton whatsBtn, closeBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutPanel)
};
