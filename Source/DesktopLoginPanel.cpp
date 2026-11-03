#include "DesktopLoginPanel.h"

namespace
{
juce::Colour panelBackground()
{
    return juce::Colour (0xff0b0d11);
}

juce::Colour cardBackground()
{
    return juce::Colour (0xff171b22);
}
}

DesktopLoginPanel::DesktopLoginPanel (juce::Image logoImage)
    : logo (std::move (logoImage))
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setTitle ("Acesso ao ChurchDAW");
    setDescription (
        "Entre com seu e-mail ou chave de licenca e ative este computador.");

    configureEditor (emailEditor, "E-mail ou chave de licenca",
                     "voce@exemplo.com ou CHD-...");
    configureEditor (passwordEditor, "Senha", "Sua senha");
    emailEditor.setInputRestrictions (254);
    passwordEditor.setInputRestrictions (256);
    passwordEditor.setPasswordCharacter (0x2022);

    emailLabel.setText ("E-MAIL OU CHAVE DE LICENCA",
                        juce::dontSendNotification);
    passwordLabel.setText ("SENHA", juce::dontSendNotification);

    for (auto* label : { &emailLabel, &passwordLabel })
    {
        label->setColour (juce::Label::textColourId,
                          NordLookAndFeel::txtSecondary());
        label->setFont (juce::Font (juce::FontOptions (11.5f)).boldened()
                           .withExtraKerningFactor (0.08f));
        addAndMakeVisible (*label);
    }

    statusLabel.setColour (juce::Label::textColourId,
                           NordLookAndFeel::txtSecondary());
    statusLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setMinimumHorizontalScale (0.75f);
    addAndMakeVisible (statusLabel);

    rememberToggle.setToggleState (true, juce::dontSendNotification);
    rememberToggle.setColour (juce::ToggleButton::textColourId,
                              NordLookAndFeel::txtSecondary());
    rememberToggle.setColour (juce::ToggleButton::tickColourId,
                              NordLookAndFeel::nordRedBright());
    rememberToggle.setColour (juce::ToggleButton::tickDisabledColourId,
                              NordLookAndFeel::txtDisabled());
    rememberToggle.setTitle ("Manter conectado");
    rememberToggle.setDescription (
        "Salva um token protegido no cofre do sistema. A senha nao e salva.");
    addAndMakeVisible (rememberToggle);

    configureButton (loginButton, true);
    configureButton (buyButton, false);
    configureButton (forgotButton, false);
    configureButton (manageButton, false);

    showPasswordButton.setColour (juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
    showPasswordButton.setColour (juce::TextButton::buttonOnColourId,
                                  juce::Colours::transparentBlack);
    showPasswordButton.setColour (juce::TextButton::textColourOffId,
                                  NordLookAndFeel::txtSecondary());
    showPasswordButton.setColour (juce::TextButton::textColourOnId,
                                  NordLookAndFeel::txtPrimary());
    showPasswordButton.setTitle ("Mostrar ou ocultar senha");

    for (auto* component : {
             static_cast<juce::Component*> (&emailEditor),
             static_cast<juce::Component*> (&passwordEditor),
             static_cast<juce::Component*> (&showPasswordButton),
             static_cast<juce::Component*> (&loginButton),
             static_cast<juce::Component*> (&forgotButton),
             static_cast<juce::Component*> (&buyButton),
             static_cast<juce::Component*> (&manageButton) })
        addAndMakeVisible (*component);

    showPasswordButton.onClick = [this]
    {
        passwordVisible = ! passwordVisible;
        passwordEditor.setPasswordCharacter (
            passwordVisible ? 0 : juce::juce_wchar (0x2022));
        showPasswordButton.setButtonText (
            passwordVisible ? "Ocultar" : "Mostrar");
    };

    loginButton.onClick = [this] { submit(); };
    passwordEditor.onReturnKey = [this] { submit(); };
    emailEditor.onReturnKey = [this] { passwordEditor.grabKeyboardFocus(); };
    emailEditor.onTextChange = [this] { updateControls(); };
    passwordEditor.onTextChange = [this] { updateControls(); };

    forgotButton.onClick = [this]
    {
        const auto identifier =
            ChurchDAWDesktopLicensing::normaliseLoginIdentifier (
                emailEditor.getText());
        if (onForgotPassword != nullptr
            && identifier.kind
                 == ChurchDAWDesktopLicensing::LoginIdentifierKind::email)
            onForgotPassword (identifier.value);
    };

    buyButton.onClick = [this]
    {
        if (onBuy != nullptr)
            onBuy();
    };

    manageButton.onClick = [this]
    {
        if (state
              == ChurchDAWDesktopLicensing::State::clientUpdateRequired)
        {
            if (onUpdateClient != nullptr)
                onUpdateClient();
        }
        else if (onManageActivation != nullptr)
            onManageActivation();
    };

    manageButton.setVisible (false);
    startTimerHz (30);
    updateControls();
}

void DesktopLoginPanel::applySnapshot (
    const ChurchDAWDesktopLicensing::Snapshot& next)
{
    state = next.state;
    busy = next.busy;

    if (next.email.isNotEmpty() && emailEditor.getText().isEmpty())
        emailEditor.setText (next.email, false);

    statusLabel.setText (next.message, juce::dontSendNotification);
    const bool errorState =
        (ChurchDAWDesktopLicensing::isTerminalState (next.state)
         && next.state
              != ChurchDAWDesktopLicensing::State::licensePending)
        || next.state
               == ChurchDAWDesktopLicensing::State::activationConflict;
    const bool pendingState =
        next.state == ChurchDAWDesktopLicensing::State::licensePending;
    const bool onlineState =
        next.state == ChurchDAWDesktopLicensing::State::activeOnline;
    const bool offlineState =
        next.state == ChurchDAWDesktopLicensing::State::activeOffline;

    statusLabel.setColour (
        juce::Label::textColourId,
        errorState ? juce::Colour (0xffff6b79)
                   : (pendingState
                        ? juce::Colour (0xffffc857)
                        : (onlineState
                             ? NordLookAndFeel::greenOk()
                             : (offlineState
                                  ? juce::Colour (0xffffc857)
                                  : NordLookAndFeel::txtSecondary()))));

    const bool actionRequired =
        next.state == ChurchDAWDesktopLicensing::State::activationConflict
        || next.state
             == ChurchDAWDesktopLicensing::State::passwordChangeRequired
        || next.state
             == ChurchDAWDesktopLicensing::State::clientUpdateRequired;
    manageButton.setVisible (actionRequired);
    if (next.state
          == ChurchDAWDesktopLicensing::State::passwordChangeRequired)
        manageButton.setButtonText ("Alterar senha na minha conta");
    else if (next.state
               == ChurchDAWDesktopLicensing::State::clientUpdateRequired)
        manageButton.setButtonText ("Baixar atualizacao obrigatoria");
    else
        manageButton.setButtonText ("Gerenciar computadores");

    if (next.state != ChurchDAWDesktopLicensing::State::signingIn)
        passwordEditor.clear();

    updateControls();
    resized();
    repaint();
}

void DesktopLoginPanel::paint (juce::Graphics& g)
{
    g.fillAll (panelBackground());

    const auto full = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (
        juce::Colour (0xff171018), full.getX(), full.getY(),
        panelBackground(), full.getRight(), full.getBottom(), false));
    g.fillRect (full);

    if (! brandArea.isEmpty())
    {
        const auto brand = brandArea.toFloat();
        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xff270711), brand.getX(), brand.getY(),
            juce::Colour (0xff0d1015), brand.getRight(), brand.getBottom(),
            true));
        g.fillRoundedRectangle (brand, 28.0f);

        auto content = brandArea.reduced (48);
        auto logoBox = content.removeFromTop (
            juce::jmin (230, content.getWidth() - 12))
            .withSizeKeepingCentre (
                juce::jmin (218, content.getWidth()),
                juce::jmin (218, content.getWidth()));

        if (logo.isValid())
            g.drawImageWithin (logo, logoBox.getX(), logoBox.getY(),
                               logoBox.getWidth(), logoBox.getHeight(),
                               juce::RectanglePlacement::centred);
        else
            NordLookAndFeel::drawChurchDAWMark (
                g, logoBox.toFloat());

        content.removeFromTop (22);
        g.setColour (NordLookAndFeel::nordRedBright());
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened()
                       .withExtraKerningFactor (0.16f));
        g.drawText ("CHURCHDAW  /  DESKTOP",
                    content.removeFromTop (24),
                    juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (35.0f)).boldened());
        g.drawFittedText (
            "Seu instrumento.\nSua licenca.",
            content.removeFromTop (96),
            juce::Justification::topLeft, 2, 0.82f);

        content.removeFromTop (8);
        g.setColour (NordLookAndFeel::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawFittedText (
            juce::String::fromUTF8 (
                "Uma compra, um computador ativo. "
                "Troque de maquina com seguranca pela sua conta."),
            content.removeFromTop (72),
            juce::Justification::topLeft, 3, 0.82f);

        auto seal = content.removeFromBottom (54).toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (seal, 12.0f);
        g.setColour (NordLookAndFeel::greenOk());
        g.fillEllipse (seal.getX() + 16.0f, seal.getCentreY() - 4.0f,
                       8.0f, 8.0f);
        g.setColour (NordLookAndFeel::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText ("SESSAO PROTEGIDA PELO SISTEMA",
                    seal.reduced (36.0f, 0.0f),
                    juce::Justification::centredLeft);
    }

    if (! cardArea.isEmpty())
    {
        const auto card = cardArea.toFloat();
        juce::Path cardPath;
        cardPath.addRoundedRectangle (card, 24.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f),
                          34, { 0, 12 }).drawForPath (g, cardPath);
        g.setColour (cardBackground());
        g.fillPath (cardPath);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (card.reduced (0.5f), 24.0f, 1.0f);

        auto top = cardArea.reduced (42).removeFromTop (86);
        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (29.0f)).boldened());
        g.drawText ("Bem-vindo de volta",
                    top.removeFromTop (40),
                    juce::Justification::centredLeft);
        g.setColour (NordLookAndFeel::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (13.5f)));
        g.drawFittedText (
            "Entre com o e-mail da compra ou com sua chave de licenca.",
            top, juce::Justification::centredLeft, 2, 0.86f);

        if (busy)
        {
            auto spinner = juce::Rectangle<float> (
                (float) cardArea.getRight() - 61.0f,
                (float) cardArea.getY() + 48.0f, 18.0f, 18.0f);
            juce::Path arc;
            arc.addCentredArc (spinner.getCentreX(), spinner.getCentreY(),
                               spinner.getWidth() * 0.5f,
                               spinner.getHeight() * 0.5f,
                               0.0f, spinnerPhase,
                               spinnerPhase + juce::MathConstants<float>::pi * 1.35f,
                               true);
            g.setColour (NordLookAndFeel::nordRedBright());
            g.strokePath (arc, juce::PathStrokeType (2.2f,
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }
    }
}

void DesktopLoginPanel::resized()
{
    auto bounds = getLocalBounds().reduced (30);
    const bool compact = bounds.getWidth() < 860;

    if (compact)
    {
        brandArea = bounds.removeFromTop (180);
        bounds.removeFromTop (18);
        cardArea = bounds.withSizeKeepingCentre (
            juce::jmin (560, bounds.getWidth()), bounds.getHeight());
    }
    else
    {
        brandArea = bounds.removeFromLeft (
            juce::jmin (430, juce::roundToInt (bounds.getWidth() * 0.43f)));
        bounds.removeFromLeft (26);
        cardArea = bounds.withSizeKeepingCentre (
            juce::jmin (520, bounds.getWidth()),
            juce::jmin (600, bounds.getHeight()));
    }

    auto form = cardArea.reduced (42);
    form.removeFromTop (102);

    emailLabel.setBounds (form.removeFromTop (20));
    emailEditor.setBounds (form.removeFromTop (48));
    form.removeFromTop (18);

    passwordLabel.setBounds (form.removeFromTop (20));
    auto passwordRow = form.removeFromTop (48);
    showPasswordButton.setBounds (passwordRow.removeFromRight (82));
    passwordEditor.setBounds (passwordRow);
    form.removeFromTop (12);

    rememberToggle.setBounds (form.removeFromTop (30));
    form.removeFromTop (16);
    loginButton.setBounds (form.removeFromTop (48));
    form.removeFromTop (10);
    statusLabel.setBounds (form.removeFromTop (44));
    form.removeFromTop (8);

    auto helpRow = form.removeFromTop (34);
    forgotButton.setBounds (helpRow.removeFromLeft (
        juce::jmin (180, helpRow.getWidth() / 2)));
    buyButton.setBounds (helpRow);
    form.removeFromTop (8);
    manageButton.setBounds (form.removeFromTop (38));
}

void DesktopLoginPanel::timerCallback()
{
    if (! busy)
        return;

    spinnerPhase += 0.16f;
    if (spinnerPhase >= juce::MathConstants<float>::twoPi)
        spinnerPhase -= juce::MathConstants<float>::twoPi;
    repaint (cardArea);
}

void DesktopLoginPanel::submit()
{
    if (busy || ! loginButton.isEnabled())
        return;

    auto password = passwordEditor.getText();
    passwordEditor.clear();
    const auto identifier =
        ChurchDAWDesktopLicensing::normaliseLoginIdentifier (
            emailEditor.getText());

    if (onLogin != nullptr
        && ChurchDAWDesktopLicensing::loginCredentialsAreValid (
            identifier, password.length()))
        onLogin (identifier.value,
                 std::move (password),
                 rememberToggle.getToggleState());
}

void DesktopLoginPanel::updateControls()
{
    const auto identifier =
        ChurchDAWDesktopLicensing::normaliseLoginIdentifier (
            emailEditor.getText());
    const bool fieldsValid =
        ChurchDAWDesktopLicensing::loginCredentialsAreValid (
            identifier, passwordEditor.getText().length());

    emailEditor.setEnabled (! busy);
    passwordEditor.setEnabled (! busy);
    showPasswordButton.setEnabled (! busy);
    rememberToggle.setEnabled (! busy);
    forgotButton.setEnabled (
        ! busy
        && identifier.kind
             == ChurchDAWDesktopLicensing::LoginIdentifierKind::email);
    buyButton.setEnabled (! busy);
    loginButton.setEnabled (! busy && fieldsValid);

    loginButton.setButtonText (
        busy ? "Validando com seguranca..."
             : "Entrar no ChurchDAW");
}

void DesktopLoginPanel::configureEditor (juce::TextEditor& editor,
                                         const juce::String& title,
                                         const juce::String& placeholder)
{
    editor.setTitle (title);
    editor.setDescription (title);
    editor.setTextToShowWhenEmpty (placeholder,
                                   NordLookAndFeel::txtDisabled());
    editor.setFont (juce::Font (juce::FontOptions (15.0f)));
    editor.setColour (juce::TextEditor::backgroundColourId,
                      NordLookAndFeel::bgControl());
    editor.setColour (juce::TextEditor::textColourId,
                      NordLookAndFeel::txtPrimary());
    editor.setColour (juce::TextEditor::highlightColourId,
                      NordLookAndFeel::nordRed().withAlpha (0.45f));
    editor.setColour (juce::TextEditor::outlineColourId,
                      NordLookAndFeel::borderDefault());
    editor.setColour (juce::TextEditor::focusedOutlineColourId,
                      NordLookAndFeel::nordRedBright());
}

void DesktopLoginPanel::configureButton (juce::TextButton& button,
                                         bool primary)
{
    button.setColour (juce::TextButton::buttonColourId,
                      primary ? NordLookAndFeel::nordRed()
                              : juce::Colours::transparentBlack);
    button.setColour (juce::TextButton::buttonOnColourId,
                      primary ? NordLookAndFeel::nordRedBright()
                              : juce::Colours::white.withAlpha (0.05f));
    button.setColour (juce::TextButton::textColourOffId,
                      primary ? juce::Colours::white
                              : NordLookAndFeel::txtSecondary());
    button.setColour (juce::TextButton::textColourOnId,
                      juce::Colours::white);
}
