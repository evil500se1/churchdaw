#pragma once

#if JUCE_ANDROID

#include <juce_gui_extra/juce_gui_extra.h>

#include "AndroidLicenseClient.h"
#include "ChurchDAWServices.h"
#include "NordLookAndFeel.h"

class AndroidLoginPanel final : public juce::Component,
                                private juce::Timer
{
public:
    enum class WindowClass
    {
        compact,
        medium,
        expanded
    };

    enum class LayoutMode
    {
        compactPortrait,
        compactLandscape,
        medium,
        expanded,
        reducedHeight
    };

    enum class LibraryState
    {
        notStarted,
        downloading,
        processing,
        ready,
        error
    };

    explicit AndroidLoginPanel (juce::Image logoImage)
        : logo (std::move (logoImage)),
          fontScale (readAndroidFontScale())
    {
        setOpaque (true);
        setWantsKeyboardFocus (false);
        imeVisible = readAndroidImeVisible();
        setFocusContainerType (
            juce::Component::FocusContainerType::
                keyboardFocusContainer);
        setTitle ("Acesso ao ChurchDAW");
        setDescription (
            "Entre com sua licenca ou experimente o instrumento por 20 minutos.");

        configureEditor (emailEditor, "E-mail", "voce@exemplo.com");
        configureEditor (passwordEditor, "Senha", "Sua senha");
        emailEditor.setInputRestrictions (254);
        emailEditor.setKeyboardType (
            juce::TextInputTarget::emailAddressKeyboard);
        passwordEditor.setInputRestrictions (256);
        passwordEditor.setPasswordCharacter (0x2022);
        emailEditor.setExplicitFocusOrder (1);
        passwordEditor.setExplicitFocusOrder (2);
        showPasswordButton.setExplicitFocusOrder (3);
        forgotButton.setExplicitFocusOrder (4);
        loginButton.setExplicitFocusOrder (5);
        trialButton.setExplicitFocusOrder (6);

        configureLabel (emailLabel, "E-MAIL", false);
        configureLabel (passwordLabel, "SENHA", false);
        configureLabel (emailErrorLabel, "", true);
        configureLabel (passwordErrorLabel, "", true);
        configureLabel (libraryStatusLabel, "", false);
        configureLabel (statusLabel, "", false);

        configureButton (loginButton, true);
        configureButton (trialButton, false);
        configureButton (manageButton, false);
        configureLink (forgotButton);

        showPasswordButton.setTitle ("Mostrar senha");
        showPasswordButton.setDescription (
            "Mostra ou oculta o conteudo do campo senha");

        for (auto* component : {
                 static_cast<juce::Component*> (&emailLabel),
                 static_cast<juce::Component*> (&emailEditor),
                 static_cast<juce::Component*> (&emailErrorLabel),
                 static_cast<juce::Component*> (&passwordLabel),
                 static_cast<juce::Component*> (&passwordEditor),
                 static_cast<juce::Component*> (&passwordErrorLabel),
                 static_cast<juce::Component*> (&showPasswordButton),
                 static_cast<juce::Component*> (&forgotButton),
                 static_cast<juce::Component*> (&loginButton),
                 static_cast<juce::Component*> (&libraryStatusLabel),
                 static_cast<juce::Component*> (&trialButton),
                 static_cast<juce::Component*> (&statusLabel),
                 static_cast<juce::Component*> (&manageButton) })
            addAndMakeVisible (*component);

        showPasswordButton.onClick = [this]
        {
            passwordVisible = ! passwordVisible;
            passwordEditor.setPasswordCharacter (
                passwordVisible ? 0 : juce::juce_wchar (0x2022));
            showPasswordButton.setPasswordVisible (passwordVisible);
            showPasswordButton.setTitle (
                passwordVisible ? "Ocultar senha" : "Mostrar senha");
        };
        loginButton.onClick = [this] { submit(); };
        passwordEditor.onReturnKey = [this] { submit(); };
        emailEditor.onReturnKey = [this]
        {
            passwordEditor.grabKeyboardFocus();
        };
        emailEditor.onTextChange = [this]
        {
            validateEmail (false);
            updateControls();
        };
        passwordEditor.onTextChange = [this]
        {
            validatePassword (false);
            updateControls();
        };
        forgotButton.onClick = [this]
        {
            if (onForgotPassword != nullptr)
                onForgotPassword (emailEditor.getText().trim());
        };
        trialButton.onClick = [this]
        {
            if (onTrial != nullptr)
                onTrial();
        };
        manageButton.onClick = [this]
        {
            if (onManageActivation != nullptr)
                onManageActivation();
        };

        // O estado real chega do preflight em segundo plano de Main.cpp.
        // hasAnyInstalledLibrary() recupera transacoes e consulta o filesystem;
        // executar isso no construtor travava a primeira pintura do login.
        setLibraryState (LibraryState::notStarted);
        // O timer existe para a pequena animacao de login e para acompanhar o
        // teclado virtual. 20 Hz continua fluido e evita pressionar o renderer
        // Android durante a primeira pintura, especialmente em aparelhos de
        // entrada e em emuladores que traduzem arm64.
        startTimerHz (20);
        updateControls();
    }

    ~AndroidLoginPanel() override
    {
        emailEditor.setLookAndFeel (nullptr);
        passwordEditor.setLookAndFeel (nullptr);
    }

    std::function<void (
        juce::String email, juce::String password)> onLogin;
    std::function<void()> onTrial;
    std::function<void (juce::String email)> onForgotPassword;
    std::function<void()> onManageActivation;

    WindowClass getWindowClassForTesting() const noexcept
    {
        return windowClass;
    }

    LayoutMode getLayoutModeForTesting() const noexcept
    {
        return layoutMode;
    }

    juce::Rectangle<int> getBrandAreaForTesting() const noexcept
    {
        return brandArea;
    }

    juce::Rectangle<int> getCardAreaForTesting() const noexcept
    {
        return cardArea;
    }

    bool isLibraryReadyForTesting() const noexcept
    {
        return libraryState == LibraryState::ready;
    }

    bool essentialBoundsValidForTesting() const noexcept
    {
        return essentialBoundsValid;
    }

    void setLibraryState (
        LibraryState next,
        double progress = 0.0,
        juce::String detail = {})
    {
        libraryState = next;
        libraryProgress = juce::jlimit (0.0, 1.0, progress);
        libraryDetail = std::move (detail);

        switch (libraryState)
        {
            case LibraryState::notStarted:
                libraryStatusLabel.setText (
                    juce::String::fromUTF8 (
                        "\xe2\x97\x8f Sem samples no aparelho\n"
                        "Voc\xc3\xaa pode entrar e baixar s\xc3\xb3 o que quiser "
                        "(timbres 1 a 1 ou pack completo)."),
                    juce::dontSendNotification);
                break;
            case LibraryState::downloading:
                libraryStatusLabel.setText (
                    "Baixando "
                        + juce::String (juce::roundToInt (
                            libraryProgress * 100.0))
                        + "%  " + libraryDetail,
                    juce::dontSendNotification);
                break;
            case LibraryState::processing:
                libraryStatusLabel.setText (
                    "Preparando timbres nativos...",
                    juce::dontSendNotification);
                break;
            case LibraryState::ready:
                libraryStatusLabel.setText (
                    juce::String::fromUTF8 (
                        "\xe2\x9c\x93 Biblioteca pronta"),
                    juce::dontSendNotification);
                break;
            case LibraryState::error:
                libraryStatusLabel.setText (
                    juce::String::fromUTF8 (
                        "N\xc3\xa3o foi poss\xc3\xadvel preparar a biblioteca. ")
                        + libraryDetail,
                    juce::dontSendNotification);
                break;
        }

        libraryStatusLabel.setColour (
            juce::Label::textColourId,
            libraryState == LibraryState::ready
                ? successColour()
                : (libraryState == LibraryState::error
                       ? errorColour()
                       : secondaryTextColour()));
        libraryStatusLabel.setTitle ("Estado da biblioteca");
        updateControls();
        resized();
        repaint();
    }

    void applySnapshot (
        const ChurchDAWAndroidLicensing::Snapshot& next)
    {
        state = next.state;
        busy = next.busy;
        if (next.email.isNotEmpty()
            && emailEditor.getText().isEmpty())
            emailEditor.setText (next.email, false);

        statusLabel.setText (
            next.message, juce::dontSendNotification);
        const bool error =
            next.state
                == ChurchDAWAndroidLicensing::State::activationConflict
            || next.state
                == ChurchDAWAndroidLicensing::State::revoked
            || next.state
                == ChurchDAWAndroidLicensing::State::trialExpired;
        statusLabel.setColour (
            juce::Label::textColourId,
            error ? errorColour()
                  : (next.state
                        == ChurchDAWAndroidLicensing::State::active
                       ? successColour()
                       : secondaryTextColour()));
        statusLabel.setTitle (
            error ? "Erro de autenticacao"
                  : "Status da autenticacao");
        manageButton.setVisible (
            next.state
                == ChurchDAWAndroidLicensing::State::activationConflict);
        updateControls();
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (backgroundColour());
        const auto full = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (
            brandColour(), full.getX(), full.getY(),
            backgroundColour(), full.getRight(),
            full.getBottom(), false));
        g.fillRect (full);

        drawBrand (g);
        drawCard (g);

        if (libraryState == LibraryState::downloading)
        {
            auto bar = libraryStatusLabel.getBounds().toFloat()
                           .removeFromBottom (4.0f)
                           .reduced (4.0f, 0.0f);
            g.setColour (borderColour());
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (redColour());
            g.fillRoundedRectangle (
                bar.withWidth (
                    bar.getWidth()
                    * static_cast<float> (libraryProgress)),
                2.0f);
        }

        if (busy)
        {
            auto spinner = loginButton.getBounds().toFloat()
                               .removeFromRight (48.0f)
                               .withSizeKeepingCentre (18.0f, 18.0f);
            juce::Path arc;
            arc.addCentredArc (
                spinner.getCentreX(), spinner.getCentreY(),
                8.0f, 8.0f, 0.0f, spinnerPhase,
                spinnerPhase
                    + juce::MathConstants<float>::pi * 1.35f,
                true);
            g.setColour (juce::Colours::white);
            g.strokePath (
                arc, juce::PathStrokeType (
                    2.0f, juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));
        }
    }

    void resized() override
    {
        calculateMode();
        layoutAreas();
        layoutForm();
        validateEssentialBounds();
        repaint();
    }

private:
    class EyeButton final : public juce::TextButton
    {
    public:
        void setPasswordVisible (bool visible)
        {
            passwordVisible = visible;
            repaint();
        }

        void paintButton (
            juce::Graphics& g, bool over, bool down) override
        {
            const auto bounds = getLocalBounds().toFloat()
                                    .withSizeKeepingCentre (
                                        25.0f, 25.0f);
            const auto colour = secondaryTextColour()
                                    .withAlpha (
                                        down ? 1.0f
                                             : (over ? 0.92f : 0.78f));
            juce::Path eye;
            eye.startNewSubPath (
                bounds.getX() + 2.0f, bounds.getCentreY());
            eye.cubicTo (
                bounds.getX() + 7.0f, bounds.getY() + 4.0f,
                bounds.getRight() - 7.0f, bounds.getY() + 4.0f,
                bounds.getRight() - 2.0f, bounds.getCentreY());
            eye.cubicTo (
                bounds.getRight() - 7.0f,
                bounds.getBottom() - 4.0f,
                bounds.getX() + 7.0f,
                bounds.getBottom() - 4.0f,
                bounds.getX() + 2.0f, bounds.getCentreY());
            g.setColour (colour);
            g.strokePath (eye, juce::PathStrokeType (1.8f));
            g.fillEllipse (
                bounds.withSizeKeepingCentre (5.5f, 5.5f));
            if (! passwordVisible)
                g.drawLine (
                    bounds.getX() + 3.0f,
                    bounds.getBottom() - 3.0f,
                    bounds.getRight() - 3.0f,
                    bounds.getY() + 3.0f, 2.0f);
        }

    private:
        bool passwordVisible = false;
    };

    class LinkButton final : public juce::TextButton
    {
    public:
        explicit LinkButton (const juce::String& text)
            : juce::TextButton (text)
        {
        }

        void paintButton (
            juce::Graphics& g, bool over, bool down) override
        {
            if (over || down)
            {
                g.setColour (
                    juce::Colours::white.withAlpha (
                        down ? 0.10f : 0.055f));
                g.fillRoundedRectangle (
                    getLocalBounds().toFloat().reduced (1.0f),
                    8.0f);
            }
            const auto textColour = highContrast
                                        ? primaryTextColour().withAlpha (0.90f)
                                        : secondaryTextColour();
            g.setColour (textColour.withAlpha (
                isEnabled() ? textColour.getFloatAlpha() : 0.45f));
            g.setFont (getFont());
            g.drawFittedText (
                getButtonText(), getLocalBounds().reduced (5, 0),
                juce::Justification::centred, 1, 0.72f);
        }

        void setFont (juce::Font next) { font = std::move (next); }
        juce::Font getFont() const { return font; }
        void setHighContrast (bool shouldUseHighContrast)
        {
            highContrast = shouldUseHighContrast;
            repaint();
        }

    private:
        juce::Font font {
            juce::FontOptions (14.0f)
        };
        bool highContrast = false;
    };

    class LibraryActionButton final : public juce::TextButton
    {
    public:
        explicit LibraryActionButton (const juce::String& text)
            : juce::TextButton (text)
        {
        }

        void setDownloadIconVisible (bool shouldShow)
        {
            if (showDownloadIcon == shouldShow)
                return;
            showDownloadIcon = shouldShow;
            repaint();
        }

        void paintButton (
            juce::Graphics& g, bool over, bool down) override
        {
            const auto bounds =
                getLocalBounds().toFloat().reduced (0.75f);
            if (over || down)
            {
                g.setColour (
                    juce::Colours::white.withAlpha (
                        down ? 0.075f : 0.045f));
                g.fillRoundedRectangle (bounds, 11.0f);
            }
            g.setColour (
                borderColour().withAlpha (
                    isEnabled() ? 1.0f : 0.52f));
            g.drawRoundedRectangle (bounds, 11.0f, 1.0f);

            const auto label = getButtonText();
            const auto textFont =
                juce::Font (juce::FontOptions (14.0f));
            const float iconWidth =
                showDownloadIcon ? 16.0f : 0.0f;
            const float iconGap =
                showDownloadIcon ? 8.0f : 0.0f;
            const float textWidth =
                juce::GlyphArrangement::getStringWidth (
                    textFont, label);
            const float groupWidth = juce::jmin (
                bounds.getWidth() - 20.0f,
                iconWidth + iconGap + textWidth);
            float cursor =
                bounds.getCentreX() - groupWidth * 0.5f;
            const auto foreground =
                secondaryTextColour().withAlpha (
                    isEnabled() ? 1.0f : 0.48f);

            if (showDownloadIcon)
            {
                const auto icon = juce::Rectangle<float> (
                    cursor, bounds.getCentreY() - 8.0f,
                    iconWidth, 16.0f);
                juce::Path arrow;
                arrow.startNewSubPath (
                    icon.getCentreX(), icon.getY() + 1.0f);
                arrow.lineTo (
                    icon.getCentreX(),
                    icon.getBottom() - 5.0f);
                arrow.startNewSubPath (
                    icon.getCentreX() - 4.0f,
                    icon.getBottom() - 9.0f);
                arrow.lineTo (
                    icon.getCentreX(),
                    icon.getBottom() - 5.0f);
                arrow.lineTo (
                    icon.getCentreX() + 4.0f,
                    icon.getBottom() - 9.0f);
                arrow.startNewSubPath (
                    icon.getX() + 2.0f,
                    icon.getBottom() - 1.0f);
                arrow.lineTo (
                    icon.getRight() - 2.0f,
                    icon.getBottom() - 1.0f);
                g.setColour (foreground);
                g.strokePath (
                    arrow, juce::PathStrokeType (
                        1.7f,
                        juce::PathStrokeType::curved,
                        juce::PathStrokeType::rounded));
                cursor += iconWidth + iconGap;
            }

            g.setColour (foreground);
            g.setFont (textFont);
            g.drawFittedText (
                label,
                juce::Rectangle<int> (
                    juce::roundToInt (cursor), 0,
                    juce::jmax (
                        1, juce::roundToInt (
                               bounds.getRight() - cursor)),
                    getHeight()),
                juce::Justification::centredLeft, 1, 0.76f);
        }

    private:
        bool showDownloadIcon = true;
    };

    class InputLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        void fillTextEditorBackground (
            juce::Graphics& g, int width, int height,
            juce::TextEditor& editor) override
        {
            g.setColour (editor.findColour (
                juce::TextEditor::backgroundColourId));
            g.fillRoundedRectangle (
                juce::Rectangle<float> (
                    0.0f, 0.0f,
                    static_cast<float> (width),
                    static_cast<float> (height)),
                11.0f);
        }

        void drawTextEditorOutline (
            juce::Graphics& g, int width, int height,
            juce::TextEditor& editor) override
        {
            if (! editor.isEnabled())
                return;
            const bool focused =
                editor.hasKeyboardFocus (true);
            const float thickness = focused ? 1.8f : 1.0f;
            g.setColour (editor.findColour (
                focused
                    ? juce::TextEditor::focusedOutlineColourId
                    : juce::TextEditor::outlineColourId));
            g.drawRoundedRectangle (
                juce::Rectangle<float> (
                    thickness * 0.5f,
                    thickness * 0.5f,
                    static_cast<float> (width) - thickness,
                    static_cast<float> (height) - thickness),
                11.0f, thickness);
        }
    };

    class LoginTextEditor final : public juce::TextEditor
    {
    public:
        void mouseDown (const juce::MouseEvent& event) override
        {
            grabKeyboardFocus();
            juce::TextEditor::mouseDown (event);
        }
    };

    static juce::Colour backgroundColour()
    {
        return juce::Colour (0xff0e0f13);
    }
    static juce::Colour brandColour()
    {
        return juce::Colour (0xff160d13);
    }
    static juce::Colour cardColour()
    {
        return juce::Colour (0xff181c22);
    }
    static juce::Colour fieldColour()
    {
        return juce::Colour (0xff22272f);
    }
    static juce::Colour borderColour()
    {
        return juce::Colour (0xff39414d);
    }
    static juce::Colour primaryTextColour()
    {
        return juce::Colour (0xfff5f6f8);
    }
    static juce::Colour secondaryTextColour()
    {
        return juce::Colour (0xffa9afb8);
    }
    static juce::Colour placeholderColour()
    {
        return juce::Colour (0xff818893);
    }
    static juce::Colour redColour()
    {
        return juce::Colour (0xffea1747);
    }
    static juce::Colour redPressedColour()
    {
        return juce::Colour (0xffc90f39);
    }
    static juce::Colour successColour()
    {
        return juce::Colour (0xff54c77a);
    }
    static juce::Colour errorColour()
    {
        return juce::Colour (0xffff4d67);
    }

    static float readAndroidFontScale()
    {
        auto* env = juce::getEnv();
        const auto activity = juce::getCurrentActivity();
        if (env == nullptr || activity == nullptr)
            return 1.0f;

        const auto clazz = ChurchDAWServices::findAndroidAppClass (
            env, activity.get(),
            "com.churchdaw.app.ChurchDAWLicenseBridge");
        if (clazz == nullptr)
        {
            env->ExceptionClear();
            return 1.0f;
        }
        const auto method = env->GetStaticMethodID (
            clazz, "getFontScale", "(Landroid/app/Activity;)F");
        if (method == nullptr)
        {
            env->ExceptionClear();
            env->DeleteLocalRef (clazz);
            return 1.0f;
        }
        const float value = env->CallStaticFloatMethod (
            clazz, method, activity.get());
        const bool ok = ! env->ExceptionCheck();
        if (! ok)
            env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return ok ? juce::jlimit (1.0f, 2.0f, value) : 1.0f;
    }

    static bool readAndroidImeVisible()
    {
        auto* env = juce::getEnv();
        const auto activity = juce::getCurrentActivity();
        if (env == nullptr || activity == nullptr)
            return false;

        const auto clazz = ChurchDAWServices::findAndroidAppClass (
            env, activity.get(),
            "com.churchdaw.app.ChurchDAWLicenseBridge");
        if (clazz == nullptr)
        {
            env->ExceptionClear();
            return false;
        }

        const auto method = env->GetStaticMethodID (
            clazz, "isImeVisible", "(Landroid/app/Activity;)Z");
        if (method == nullptr)
        {
            env->ExceptionClear();
            env->DeleteLocalRef (clazz);
            return false;
        }

        const bool visible = env->CallStaticBooleanMethod (
                                 clazz, method, activity.get()) == JNI_TRUE;
        const bool ok = ! env->ExceptionCheck();
        if (! ok)
            env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return ok && visible;
    }

    static bool readAndroidPortraitOrientation()
    {
        auto* env = juce::getEnv();
        const auto activity = juce::getCurrentActivity();
        if (env == nullptr || activity == nullptr)
            return true;

        const auto clazz = ChurchDAWServices::findAndroidAppClass (
            env, activity.get(),
            "com.churchdaw.app.ChurchDAWLicenseBridge");
        if (clazz == nullptr)
        {
            env->ExceptionClear();
            return true;
        }

        const auto method = env->GetStaticMethodID (
            clazz, "isPortraitOrientation",
            "(Landroid/app/Activity;)Z");
        if (method == nullptr)
        {
            env->ExceptionClear();
            env->DeleteLocalRef (clazz);
            return true;
        }

        const bool portrait =
            env->CallStaticBooleanMethod (
                clazz, method, activity.get()) == JNI_TRUE;
        const bool ok = ! env->ExceptionCheck();
        if (! ok)
            env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return ok ? portrait : true;
    }

    juce::Font font (
        float base, float maximum, bool bold = false) const
    {
        auto result = juce::Font (
            juce::FontOptions (
                juce::jmin (maximum, base * fontScale)));
        return bold ? result.boldened() : result;
    }

    void calculateMode()
    {
        const auto width = getWidth();
        const auto height = getHeight();
        const bool portraitConfiguration =
            readAndroidPortraitOrientation();
        const bool wide = imeVisible
                              ? ! portraitConfiguration
                              : width > juce::roundToInt (
                                    static_cast<float> (height)
                                    * 1.18f);
        accessibilityCompact = fontScale >= 1.75f;
        reducedHeight = imeVisible
                        || (! wide && height < 500)
                        || (wide && height < 430)
                        || (accessibilityCompact && ! wide);

        if (reducedHeight)
            layoutMode = LayoutMode::reducedHeight;
        else if (width < 600 && ! wide)
            layoutMode = LayoutMode::compactPortrait;
        else if (width < 840 && wide)
            layoutMode = LayoutMode::compactLandscape;
        else if (width < 840)
            layoutMode = LayoutMode::medium;
        else
            layoutMode = LayoutMode::expanded;

        windowClass = width < 600
                        ? WindowClass::compact
                        : (width < 840
                               ? WindowClass::medium
                               : WindowClass::expanded);
        twoColumn =
            layoutMode == LayoutMode::compactLandscape
            || layoutMode == LayoutMode::medium
            || layoutMode == LayoutMode::expanded
            || (layoutMode == LayoutMode::reducedHeight && wide);
        ultraCompactIme =
            imeVisible && ! twoColumn && height < 300;
        dense = height < 700 || reducedHeight
                || accessibilityCompact;
    }

    void layoutAreas()
    {
        const int outer = reducedHeight ? 8
                         : (windowClass == WindowClass::compact
                                ? 12 : 18);
        auto available = getLocalBounds().reduced (outer);
        const int maxWidth = layoutMode == LayoutMode::expanded
                                 ? 1240
                                 : 1080;
        if (available.getWidth() > maxWidth)
            available = available.withSizeKeepingCentre (
                maxWidth, available.getHeight());

        // Em aparelhos altos, preencher toda a altura criava um card enorme
        // com uma faixa vazia abaixo das acoes. Limitar a composicao preserva
        // espaco negativo fora dos paineis e mantem branding/formulario
        // centralizados como uma unica peca. Telas pequenas continuam usando
        // toda a altura de que realmente precisam.
        if (! reducedHeight)
        {
            const int maxCompositionHeight =
                twoColumn
                    ? (layoutMode == LayoutMode::expanded ? 680 : 660)
                    : 690;
            if (available.getHeight() > maxCompositionHeight)
                available = available.withSizeKeepingCentre (
                    available.getWidth(), maxCompositionHeight);
        }

        if (layoutMode == LayoutMode::reducedHeight && ! twoColumn)
        {
            brandArea = available.removeFromTop (
                ultraCompactIme ? 36 : 44);
            available.removeFromTop (
                ultraCompactIme ? 4 : 6);
            const int compactCardHeight =
                juce::jmin (available.getHeight(),
                            (ultraCompactIme ? 184 : 286)
                                + visibleErrorHeight());
            cardArea = available.removeFromTop (compactCardHeight);
            return;
        }

        if (layoutMode == LayoutMode::reducedHeight && twoColumn)
        {
            const int brandWidth = juce::roundToInt (
                static_cast<float> (available.getWidth()) * 0.26f);
            brandArea = available.removeFromLeft (brandWidth);
            available.removeFromLeft (8);
            cardArea = available;
            return;
        }

        if (! twoColumn)
        {
            const int brandHeight = dense ? 68 : 118;
            brandArea = available.removeFromTop (
                juce::jmin (brandHeight,
                            available.getHeight() / 4));
            available.removeFromTop (dense ? 6 : 10);
            cardArea = available;
            return;
        }

        const float brandRatio =
            layoutMode == LayoutMode::expanded ? 0.38f : 0.36f;
        const int brandWidth = juce::roundToInt (
            static_cast<float> (available.getWidth())
            * brandRatio);
        brandArea = available.removeFromLeft (brandWidth);
        available.removeFromLeft (12);
        if (available.getWidth() > 560)
            available = available.withSizeKeepingCentre (
                560, available.getHeight());
        cardArea = available;
    }

    void layoutForm()
    {
        hideAllFormComponents();
        const bool landscape =
            layoutMode == LayoutMode::compactLandscape;
        const bool reducedLandscape =
            reducedHeight && twoColumn;
        const int pad = imeVisible
                            ? (ultraCompactIme ? 4
                               : (twoColumn ? 6 : 8))
                            : ((landscape || reducedLandscape)
                                   ? 8 : (dense ? 8 : 18));
        auto form = cardArea.reduced (pad);
        const bool onlyLogin =
            reducedHeight && ! landscape;
        const int inputHeight =
            imeVisible ? 52 : 54;
        const int buttonHeight =
            imeVisible ? 50 : 52;
        const int labelHeight = dense ? 16 : 18;
        const int headingHeight =
            (imeVisible && twoColumn) ? 30
            : reducedLandscape ? 48
            : landscape ? 42
                      : (onlyLogin ? 38
                                   : (dense ? 38 : 62));

        headingArea = form.removeFromTop (headingHeight);
        form.removeFromTop (
            (imeVisible && twoColumn) || reducedLandscape
                ? 0 : (dense ? 0 : 8));

        if (ultraCompactIme)
        {
            auto forgotArea = headingArea;
            forgotArea = forgotArea.removeFromRight (
                juce::jmin (128, forgotArea.getWidth() / 2));
            forgotButton.setBounds (forgotArea);
            forgotButton.setVisible (true);

            auto fields = form.removeFromTop (
                labelHeight + 50 + visibleErrorHeight());
            auto email = fields.removeFromLeft (
                (fields.getWidth() - 6) / 2);
            fields.removeFromLeft (6);
            layoutField (
                email, emailLabel, emailEditor,
                emailErrorLabel, labelHeight, 50);
            layoutField (
                fields, passwordLabel, passwordEditor,
                passwordErrorLabel, labelHeight, 50);
            showPasswordButton.setBounds (
                passwordEditor.getBounds().removeFromRight (48));
            showPasswordButton.setVisible (true);

            form.removeFromTop (4);
            loginButton.setBounds (
                form.removeFromTop (48));
            loginButton.setVisible (true);
            return;
        }

        if (imeVisible && twoColumn)
        {
            auto fields = form.removeFromTop (
                labelHeight + inputHeight
                + visibleErrorHeight());
            auto loginArea = fields.removeFromRight (
                juce::jmin (176, fields.getWidth() / 3));
            fields.removeFromRight (6);
            auto email = fields.removeFromLeft (
                (fields.getWidth() - 8) / 2);
            fields.removeFromLeft (8);
            layoutField (
                email, emailLabel, emailEditor,
                emailErrorLabel, labelHeight, inputHeight);
            layoutField (
                fields, passwordLabel, passwordEditor,
                passwordErrorLabel, labelHeight, inputHeight);
            showPasswordButton.setBounds (
                passwordEditor.getBounds().removeFromRight (48));
            showPasswordButton.setVisible (true);

            loginArea.removeFromTop (labelHeight);
            loginButton.setBounds (
                loginArea.removeFromTop (inputHeight));
            loginButton.setVisible (true);

            auto forgotArea = headingArea;
            forgotArea = forgotArea.removeFromRight (
                juce::jmin (170, forgotArea.getWidth() / 2));
            forgotButton.setBounds (forgotArea);
            forgotButton.setVisible (true);
            return;
        }

        if (reducedLandscape)
        {
            auto fields = form.removeFromTop (
                labelHeight + inputHeight
                + visibleErrorHeight());
            auto email = fields.removeFromLeft (
                (fields.getWidth() - 8) / 2);
            fields.removeFromLeft (8);
            layoutField (
                email, emailLabel, emailEditor,
                emailErrorLabel, labelHeight, inputHeight);
            layoutField (
                fields, passwordLabel, passwordEditor,
                passwordErrorLabel, labelHeight, inputHeight);
            showPasswordButton.setBounds (
                passwordEditor.getBounds().removeFromRight (48));
            showPasswordButton.setVisible (true);

            auto forgotArea = headingArea;
            forgotArea = forgotArea.removeFromRight (
                juce::jmin (170, forgotArea.getWidth() / 2));
            forgotButton.setBounds (forgotArea);
            forgotButton.setVisible (true);

            auto actions = form.removeFromTop (buttonHeight);
            auto loginArea = actions.removeFromLeft (
                (actions.getWidth() - 8) / 2);
            actions.removeFromLeft (8);
            loginButton.setBounds (loginArea);
            trialButton.setBounds (actions);
            loginButton.setVisible (true);
            trialButton.setVisible (true);

            form.removeFromTop (2);
            libraryStatusLabel.setBounds (
                form.removeFromTop (30));
            libraryStatusLabel.setVisible (true);
            layoutBottomLinks (
                form.removeFromTop (48));
            return;
        }

        if (landscape)
        {
            auto fields = form.removeFromTop (
                labelHeight + inputHeight
                + visibleErrorHeight());
            auto email = fields.removeFromLeft (
                (fields.getWidth() - 8) / 2);
            fields.removeFromLeft (8);
            layoutField (
                email, emailLabel, emailEditor,
                emailErrorLabel, labelHeight, inputHeight);
            layoutField (
                fields, passwordLabel, passwordEditor,
                passwordErrorLabel, labelHeight, inputHeight);
            showPasswordButton.setBounds (
                passwordEditor.getBounds()
                    .removeFromRight (48));
            showPasswordButton.setVisible (true);

            forgotButton.setBounds (
                form.removeFromTop (48)
                    .removeFromRight (
                        juce::jmin (210, form.getWidth())));
            forgotButton.setVisible (true);
            auto actions = form.removeFromTop (buttonHeight);
            auto loginArea = actions.removeFromLeft (
                (actions.getWidth() - 8) / 2);
            actions.removeFromLeft (8);
            loginButton.setBounds (loginArea);
            trialButton.setBounds (actions);
            loginButton.setVisible (true);
            trialButton.setVisible (true);

            form.removeFromTop (2);
            auto footer = form.removeFromTop (48);
            auto libraryArea = footer.removeFromLeft (
                juce::roundToInt (
                    static_cast<float> (footer.getWidth()) * 0.56f));
            footer.removeFromLeft (4);
            libraryStatusLabel.setBounds (libraryArea);
            libraryStatusLabel.setVisible (true);
            layoutBottomLinks (footer);
            return;
        }

        layoutField (
            form, emailLabel, emailEditor, emailErrorLabel,
            labelHeight, inputHeight);
        form.removeFromTop (dense ? 0 : 6);
        layoutField (
            form, passwordLabel, passwordEditor,
            passwordErrorLabel, labelHeight, inputHeight);
        showPasswordButton.setBounds (
            passwordEditor.getBounds().removeFromRight (48));
        showPasswordButton.setVisible (true);

        forgotButton.setBounds (
            form.removeFromTop (48)
                .removeFromRight (
                    juce::jmin (210, form.getWidth())));
        forgotButton.setVisible (true);
        form.removeFromTop (dense ? 0 : 7);
        loginButton.setBounds (
            form.removeFromTop (buttonHeight));
        loginButton.setVisible (true);

        if (onlyLogin)
            return;

        form.removeFromTop (dense ? 0 : 7);
        if (dense)
        {
            libraryStatusLabel.setBounds (
                form.removeFromTop (34));
            form.removeFromTop (2);
            trialButton.setBounds (
                form.removeFromTop (buttonHeight));
        }
        else
        {
            libraryStatusLabel.setBounds (
                form.removeFromTop (38));
            form.removeFromTop (4);
            trialButton.setBounds (
                form.removeFromTop (buttonHeight));
        }
        libraryStatusLabel.setVisible (true);
        trialButton.setVisible (true);
        if (statusLabel.getText().isNotEmpty())
            layoutAuthStatus (
                form.removeFromTop (dense ? 22 : 28));
        layoutBottomLinks (
            form.removeFromTop (48));
    }

    void layoutField (
        juce::Rectangle<int>& area,
        juce::Label& label,
        juce::TextEditor& editor,
        juce::Label& error,
        int labelHeight,
        int inputHeight)
    {
        label.setBounds (area.removeFromTop (labelHeight));
        editor.setBounds (area.removeFromTop (inputHeight));
        const int errorHeight = error.isVisible() ? 18 : 0;
        error.setBounds (area.removeFromTop (errorHeight));
        label.setVisible (true);
        editor.setVisible (true);
    }

    int visibleErrorHeight() const
    {
        return emailErrorLabel.isVisible()
                   || passwordErrorLabel.isVisible()
               ? 18 : 0;
    }

    void layoutAuthStatus (juce::Rectangle<int> area)
    {
        if (statusLabel.getText().isNotEmpty())
        {
            statusLabel.setBounds (area);
            statusLabel.setVisible (true);
        }
    }

    void layoutBottomLinks (juce::Rectangle<int> area)
    {
        if (area.isEmpty())
            return;

        area = area.withSizeKeepingCentre (
            juce::jmin (220, area.getWidth()), area.getHeight());
        if (manageButton.isVisible())
            manageButton.setBounds (area);
    }

    void hideAllFormComponents()
    {
        // Os controles essenciais permanecem visiveis durante a recomposicao.
        // No Android, ocultar por um unico frame o TextEditor focado faz o IME
        // interpretar a mudanca de layout como perda de foco e fechar.
        for (auto* component : {
                 static_cast<juce::Component*> (&emailLabel),
                 static_cast<juce::Component*> (&emailEditor),
                 static_cast<juce::Component*> (&passwordLabel),
                 static_cast<juce::Component*> (&passwordEditor),
                 static_cast<juce::Component*> (&showPasswordButton),
                 static_cast<juce::Component*> (&forgotButton),
                 static_cast<juce::Component*> (&loginButton) })
            component->setVisible (true);

        for (auto* component : {
                 static_cast<juce::Component*> (&libraryStatusLabel),
                 static_cast<juce::Component*> (&trialButton),
                 static_cast<juce::Component*> (&statusLabel),
                 static_cast<juce::Component*> (&manageButton) })
            component->setVisible (false);
        manageButton.setVisible (
            ! imeVisible
            && state
                == ChurchDAWAndroidLicensing::State::activationConflict);
        emailErrorLabel.setVisible (
            emailErrorLabel.getText().isNotEmpty());
        passwordErrorLabel.setVisible (
            passwordErrorLabel.getText().isNotEmpty());
    }

    void validateEssentialBounds()
    {
        essentialBoundsValid = true;
        const auto available = getLocalBounds();
        if (available.isEmpty())
            return;
        for (auto* component : {
                 static_cast<juce::Component*> (&emailLabel),
                 static_cast<juce::Component*> (&emailEditor),
                 static_cast<juce::Component*> (&passwordLabel),
                 static_cast<juce::Component*> (&passwordEditor),
                 static_cast<juce::Component*> (&showPasswordButton),
                 static_cast<juce::Component*> (&forgotButton),
                 static_cast<juce::Component*> (&loginButton),
                 static_cast<juce::Component*> (&libraryStatusLabel),
                 static_cast<juce::Component*> (&trialButton),
                 static_cast<juce::Component*> (&manageButton) })
        {
            if (! component->isVisible())
                continue;

            const auto bounds = component->getBounds();
            const bool isEditor =
                component == &emailEditor
                || component == &passwordEditor;
            const bool isPrimaryAction =
                component == &loginButton
                || component == &trialButton;
            const bool isTouchAction =
                isPrimaryAction
                || component == &showPasswordButton
                || component == &forgotButton
                || component == &manageButton;
            const bool tooSmall =
                (isEditor && bounds.getHeight() < 50)
                || (isPrimaryAction
                    && bounds.getHeight() < 48)
                || (isTouchAction && ! isPrimaryAction
                    && (bounds.getWidth() < 48
                        || bounds.getHeight() < 48));
            const bool tooLarge =
                (isEditor && bounds.getHeight() > 60)
                || (isPrimaryAction
                    && bounds.getHeight() > 54);

            const bool outsideCard =
                ! cardArea.contains (bounds);

            if (! available.contains (bounds)
                || outsideCard
                || tooSmall || tooLarge)
            {
                essentialBoundsValid = false;
                juce::Logger::writeToLog (
                    "ANDROID_LOGIN_GEOMETRY_FAIL "
                    + component->getName()
                    + " bounds="
                    + bounds.toString()
                    + " window="
                    + available.toString()
                    + (tooSmall ? " reason=min-touch-size"
                          : tooLarge ? " reason=oversized-control"
                          : outsideCard ? " reason=outside-card"
                                        : " reason=outside-window"));
            }
        }
    }

    void drawBrand (juce::Graphics& g)
    {
        const bool minimal = layoutMode == LayoutMode::reducedHeight;
        auto brand = brandArea.toFloat();
        g.setColour (brandColour());
        g.fillRoundedRectangle (
            brand, minimal ? 12.0f : 20.0f);
        g.setColour (
            juce::Colours::white.withAlpha (0.065f));
        g.drawRoundedRectangle (
            brand.reduced (0.5f),
            minimal ? 12.0f : 20.0f, 1.0f);

        if (minimal)
        {
            auto content = brandArea.reduced (8, 4);

            if (twoColumn)
            {
                // Em 568x320 o painel lateral tem cerca de 140 dp. Logo e
                // label lado a lado faziam "CHURCHDAW / ANDROID" terminar em
                // reticencias. Empilhar preserva a marca completa sem reduzir
                // a fonte abaixo do minimo legivel.
                const int markSide = juce::jlimit (
                    32, 42, content.getWidth() - 12);
                auto group = content.withSizeKeepingCentre (
                    content.getWidth(),
                    juce::jmin (content.getHeight(), markSide + 8 + 36));
                auto mark = group.removeFromTop (markSide)
                                .withSizeKeepingCentre (
                                    markSide, markSide);
                NordLookAndFeel::drawChurchDAWMark (
                    g, mark.toFloat());
                group.removeFromTop (8);
                g.setColour (redColour());
                g.setFont (font (12.0f, 17.0f, true));
                g.drawFittedText (
                    "CHURCHDAW /\nANDROID", group,
                    juce::Justification::centred, 2, 0.82f);
                return;
            }

            auto mark = content.removeFromLeft (36)
                            .withSizeKeepingCentre (36, 36);
            NordLookAndFeel::drawChurchDAWMark (
                g, mark.toFloat());
            content.removeFromLeft (8);
            g.setColour (redColour());
            g.setFont (font (13.0f, 18.0f, true));
            g.drawFittedText (
                "CHURCHDAW / ANDROID", content,
                juce::Justification::centredLeft, 1, 0.78f);
            return;
        }

        if (! twoColumn)
        {
            auto content = brandArea.reduced (12, 8);
            const int logoSide = dense ? 50 : 58;
            auto logoBox = content.removeFromLeft (logoSide)
                               .withSizeKeepingCentre (
                                   logoSide, logoSide);
            NordLookAndFeel::drawChurchDAWMark (
                g, logoBox.toFloat());
            content.removeFromLeft (12);
            g.setColour (redColour());
            g.setFont (font (12.0f, 18.0f, true));
            g.drawFittedText (
                "CHURCHDAW / ANDROID",
                content.removeFromTop (dense ? 20 : 24),
                juce::Justification::centredLeft, 1, 0.74f);
            g.setColour (primaryTextColour());
            g.setFont (font (
                dense ? 22.0f : 25.0f, 34.0f, true));
            g.drawFittedText (
                "Seu worship, em qualquer lugar.",
                content.removeFromTop (dense ? 34 : 42),
                juce::Justification::centredLeft, 2, 0.67f);
            if (! dense && ! accessibilityCompact)
            {
                g.setColour (secondaryTextColour());
                g.setFont (font (13.0f, 19.0f));
                g.drawFittedText (
                    "Timbres nativos e performance ao vivo.",
                    content, juce::Justification::centredLeft,
                    1, 0.70f);
            }
            return;
        }

        auto content = brandArea.reduced (18);
        const bool phoneLandscape =
            layoutMode == LayoutMode::compactLandscape;
        // O painel de branding deve apoiar o formulario, nao competir com ele.
        // A reducao de cerca de 10% deixa mais espaco negativo nas composicoes
        // de duas colunas sem comprometer a leitura da marca.
        const int logoSide = phoneLandscape ? 50
                            : (layoutMode
                                   == LayoutMode::expanded
                                   ? 88 : 68);
        const int desiredContentHeight =
            logoSide + (phoneLandscape ? 4 : 12)
            + 22 + (phoneLandscape ? 70 : 104)
            + (accessibilityCompact ? 0 : 44);
        content = content.withSizeKeepingCentre (
            content.getWidth(),
            juce::jmin (
                content.getHeight(),
                desiredContentHeight));
        auto logoBox = content.removeFromTop (
            logoSide + (phoneLandscape ? 4 : 12))
                           .withSizeKeepingCentre (
                               logoSide, logoSide);
        NordLookAndFeel::drawChurchDAWMark (
            g, logoBox.toFloat());
        g.setColour (redColour());
        g.setFont (font (12.0f, 18.0f, true));
        g.drawFittedText (
            "CHURCHDAW / ANDROID",
            content.removeFromTop (22),
            juce::Justification::centredLeft, 1, 0.72f);
        g.setColour (primaryTextColour());
        g.setFont (font (
            phoneLandscape ? 22.0f : 28.0f,
            phoneLandscape ? 30.0f : 38.0f, true));
        g.drawFittedText (
            "Seu worship.\nEm qualquer lugar.",
            content.removeFromTop (
                phoneLandscape ? 70 : 104),
            juce::Justification::topLeft, 2, 0.64f);
        if (! accessibilityCompact)
        {
            g.setColour (secondaryTextColour());
            g.setFont (font (13.0f, 19.0f));
            g.drawFittedText (
                "Timbres nativos e performance ao vivo.",
                content, juce::Justification::topLeft,
                2, 0.72f);
        }
    }

    void drawCard (juce::Graphics& g)
    {
        auto card = cardArea.toFloat();
        juce::Path path;
        path.addRoundedRectangle (card, 20.0f);

        // DropShadow::drawForPath rasteriza e borra uma mascara temporaria.
        // Como o spinner repinta durante a retomada da sessao, o login repetia
        // esse trabalho sobre um card quase do tamanho da tela varias vezes
        // por segundo. O smoke em 2400x1080 sob traducao arm64 teve ANR sem um
        // stack de paint que prove causalidade; esta e uma reducao preventiva
        // de custo para esse caminho quente. Duas camadas deslocadas preservam
        // a profundidade visual sem blur, alocacao temporaria ou custo
        // proporcional a uma mascara gigante.
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.fillRoundedRectangle (
            card.translated (0.0f, 7.0f).expanded (2.0f, 1.0f), 22.0f);
        g.setColour (juce::Colours::black.withAlpha (0.11f));
        g.fillRoundedRectangle (
            card.translated (0.0f, 3.0f).expanded (1.0f), 21.0f);
        g.setColour (cardColour());
        g.fillPath (path);
        g.setColour (
            juce::Colours::white.withAlpha (0.075f));
        g.drawRoundedRectangle (
            card.reduced (0.5f), 20.0f, 1.0f);

        auto heading = headingArea;
        g.setColour (primaryTextColour());
        g.setFont (font (
            dense ? 22.0f : 27.0f, 36.0f, true));
        const bool titleOnly =
            layoutMode == LayoutMode::reducedHeight;
        if (imeVisible && (twoColumn || ultraCompactIme))
            heading.removeFromRight (
                ultraCompactIme
                    ? juce::jmin (132, heading.getWidth() / 2)
                    : juce::jmin (174, heading.getWidth() / 2));
        g.drawFittedText (
            titleOnly ? "Entre no ChurchDAW"
                      : "Bem-vindo ao ChurchDAW",
            heading.removeFromTop (
                titleOnly ? heading.getHeight()
                          : (dense ? 28 : 36)),
            juce::Justification::centredLeft, 1, 0.68f);
        if (! titleOnly)
        {
            g.setColour (secondaryTextColour());
            g.setFont (font (13.0f, 20.0f));
            g.drawFittedText (
                dense
                    ? "Entre ou experimente por 20 minutos."
                    : "Entre para continuar ou experimente por 20 minutos.",
                heading, juce::Justification::centredLeft,
                dense ? 1 : 2, 0.68f);
        }
    }

    void timerCallback() override
    {
        const bool editorFocused =
            emailEditor.hasKeyboardFocus (true)
            || passwordEditor.hasKeyboardFocus (true);

        // Consultar WindowInsets por JNI no boot ocioso nao produz informacao
        // util. Depois que o IME aparece, porem, continue acompanhando-o mesmo
        // se o toque tirar o foco do editor; assim a volta a false restaura o
        // layout normal quando o teclado finalmente fecha.
        if ((editorFocused || imeVisible) && ++imePollCounter >= 4)
        {
            imePollCounter = 0;
            const bool nextImeVisible = readAndroidImeVisible();
            if (nextImeVisible != imeVisible)
            {
                imeVisible = nextImeVisible;
                resized();
            }
        }
        else if (! editorFocused)
        {
            imePollCounter = 0;
        }

        if (busy)
        {
            // Mantem aproximadamente a mesma velocidade visual dos antigos
            // 30 Hz depois de reduzir a frequencia do timer para 20 Hz.
            spinnerPhase += 0.27f;
            if (spinnerPhase
                > juce::MathConstants<float>::twoPi)
                spinnerPhase -=
                    juce::MathConstants<float>::twoPi;
            repaint (loginButton.getBounds());
        }
    }

    bool validateEmail (bool showError)
    {
        const auto email = emailEditor.getText().trim();
        const bool valid =
            email.containsChar ('@')
            && email.fromLastOccurrenceOf (
                    "@", false, false).containsChar ('.');
        emailErrorLabel.setText (
            showError && ! valid
                ? juce::String::fromUTF8 (
                    "Informe um e-mail v\xc3\xa1lido") : "",
            juce::dontSendNotification);
        emailErrorLabel.setVisible (
            emailErrorLabel.getText().isNotEmpty());
        emailEditor.setColour (
            juce::TextEditor::outlineColourId,
            showError && ! valid
                ? errorColour() : borderColour());
        emailEditor.setColour (
            juce::TextEditor::focusedOutlineColourId,
            showError && ! valid
                ? errorColour() : redColour());
        return valid;
    }

    bool validatePassword (bool showError)
    {
        const bool valid =
            passwordEditor.getText().length() >= 8;
        passwordErrorLabel.setText (
            showError && ! valid
                ? "Informe sua senha" : "",
            juce::dontSendNotification);
        passwordErrorLabel.setVisible (
            passwordErrorLabel.getText().isNotEmpty());
        passwordEditor.setColour (
            juce::TextEditor::outlineColourId,
            showError && ! valid
                ? errorColour() : borderColour());
        passwordEditor.setColour (
            juce::TextEditor::focusedOutlineColourId,
            showError && ! valid
                ? errorColour() : redColour());
        return valid;
    }

    void submit()
    {
        if (busy)
            return;
        const bool emailValid = validateEmail (true);
        const bool passwordValid = validatePassword (true);
        resized();
        if (! emailValid || ! passwordValid)
            return;
        if (onLogin != nullptr)
            onLogin (
                emailEditor.getText().trim(),
                passwordEditor.getText());
    }

    void updateControls()
    {
        const bool fieldsValid =
            emailEditor.getText().trim().containsChar ('@')
            && passwordEditor.getText().length() >= 8;
        for (auto* component : {
                 static_cast<juce::Component*> (&emailEditor),
                 static_cast<juce::Component*> (&passwordEditor),
                 static_cast<juce::Component*> (&showPasswordButton),
                 static_cast<juce::Component*> (&forgotButton),
                 static_cast<juce::Component*> (&trialButton) })
            component->setEnabled (! busy);

        loginButton.setEnabled (! busy && fieldsValid);
        loginButton.setButtonText (
            busy ? "Validando..." : "Entrar");

        if (busy)
            trialButton.setButtonText ("Aguarde...");
        else if (libraryState == LibraryState::ready)
            trialButton.setButtonText (
                "Experimentar por 20 minutos");
        else if (libraryState == LibraryState::error)
            trialButton.setButtonText ("Tentar novamente");
        else if (libraryState == LibraryState::downloading)
            trialButton.setButtonText ("Baixando biblioteca");
        else if (libraryState == LibraryState::processing)
            trialButton.setButtonText ("Preparando timbres");
        else
            // Nao obriga download na entrada: demo inicia e o cliente baixa
            // timbres individuais (ou pack) depois, no seletor/Bibliotecas.
            trialButton.setButtonText (
                juce::String::fromUTF8 (
                    "Experimentar sem baixar"));
        trialButton.setDownloadIconVisible (false);

        trialButton.setEnabled (
            ! busy
            && libraryState != LibraryState::downloading
            && libraryState != LibraryState::processing);
    }

    void configureLabel (
        juce::Label& label,
        const juce::String& text,
        bool error)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (
            juce::Label::textColourId,
            error ? errorColour() : secondaryTextColour());
        label.setFont (
            error ? font (12.0f, 17.0f)
                  : font (12.0f, 18.0f, true));
        label.setJustificationType (
            juce::Justification::centredLeft);
        label.setMinimumHorizontalScale (0.72f);
    }

    void configureEditor (
        juce::TextEditor& editor,
        const juce::String& title,
        const juce::String& placeholder)
    {
        editor.setTitle (title);
        editor.setDescription (title);
        editor.setTextToShowWhenEmpty (
            placeholder, placeholderColour());
        editor.setFont (font (15.0f, 24.0f));
        editor.setMultiLine (false, false);
        editor.setReturnKeyStartsNewLine (false);
        editor.setScrollbarsShown (false);
        editor.setMouseClickGrabsKeyboardFocus (true);
        editor.setJustification (
            juce::Justification::centredLeft);
        editor.setIndents (16, 0);
        editor.setLookAndFeel (&inputLookAndFeel);
        editor.setColour (
            juce::TextEditor::backgroundColourId,
            fieldColour());
        editor.setColour (
            juce::TextEditor::textColourId,
            primaryTextColour());
        editor.setColour (
            juce::TextEditor::outlineColourId,
            borderColour());
        editor.setColour (
            juce::TextEditor::focusedOutlineColourId,
            redColour());
    }

    void configureButton (
        juce::TextButton& button, bool primary)
    {
        button.setColour (
            juce::TextButton::buttonColourId,
            primary ? redColour()
                    : juce::Colours::transparentBlack);
        button.setColour (
            juce::TextButton::buttonOnColourId,
            primary ? redPressedColour()
                    : juce::Colours::white.withAlpha (0.06f));
        button.setColour (
            juce::TextButton::textColourOffId,
            primary ? juce::Colours::white
                    : secondaryTextColour());
    }

    void configureLink (LinkButton& button)
    {
        button.setFont (font (13.0f, 19.0f));
    }

    juce::Image logo;
    const float fontScale;
    juce::Label emailLabel, passwordLabel;
    juce::Label emailErrorLabel, passwordErrorLabel;
    juce::Label libraryStatusLabel, statusLabel;
    LoginTextEditor emailEditor, passwordEditor;
    EyeButton showPasswordButton;
    LinkButton forgotButton { "Esqueci minha senha" };
    juce::TextButton loginButton { "Entrar" };
    LibraryActionButton trialButton {
        "Preparar biblioteca"
    };
    juce::TextButton manageButton {
        "Trocar aparelho ativo"
    };
    InputLookAndFeel inputLookAndFeel;
    ChurchDAWAndroidLicensing::State state {
        ChurchDAWAndroidLicensing::State::signedOut
    };
    WindowClass windowClass { WindowClass::compact };
    LayoutMode layoutMode {
        LayoutMode::compactPortrait
    };
    LibraryState libraryState {
        LibraryState::notStarted
    };
    juce::Rectangle<int> brandArea, cardArea, headingArea;
    juce::String libraryDetail;
    double libraryProgress = 0.0;
    bool twoColumn = false;
    bool dense = false;
    bool reducedHeight = false;
    bool accessibilityCompact = false;
    bool busy = false;
    bool passwordVisible = false;
    bool essentialBoundsValid = true;
    bool imeVisible = false;
    bool ultraCompactIme = false;
    int imePollCounter = 0;
    float spinnerPhase = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (
        AndroidLoginPanel)
};

#endif
