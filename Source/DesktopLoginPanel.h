#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "DesktopLicenseClient.h"
#include "NordLookAndFeel.h"

class DesktopLoginPanel final : public juce::Component,
                                private juce::Timer
{
public:
    explicit DesktopLoginPanel (juce::Image logoImage);

    std::function<void (juce::String email,
                        juce::String password,
                        bool remember)> onLogin;
    std::function<void (juce::String email)> onForgotPassword;
    std::function<void()> onBuy;
    std::function<void()> onManageActivation;

    void applySnapshot (
        const ChurchDAWDesktopLicensing::Snapshot& snapshot);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void submit();
    void updateControls();
    void configureEditor (juce::TextEditor& editor,
                          const juce::String& title,
                          const juce::String& placeholder);
    void configureButton (juce::TextButton& button, bool primary);

    juce::Image logo;
    juce::Label emailLabel;
    juce::Label passwordLabel;
    juce::Label statusLabel;
    juce::TextEditor emailEditor;
    juce::TextEditor passwordEditor;
    juce::ToggleButton rememberToggle {
        juce::String::fromUTF8 ("Manter conectado neste computador")
    };
    juce::TextButton showPasswordButton {
        juce::String::fromUTF8 ("Mostrar")
    };
    juce::TextButton loginButton {
        juce::String::fromUTF8 ("Entrar no ChurchDAW")
    };
    juce::TextButton forgotButton {
        juce::String::fromUTF8 ("Esqueci minha senha")
    };
    juce::TextButton buyButton {
        juce::String::fromUTF8 ("Comprar uma licenca")
    };
    juce::TextButton manageButton {
        juce::String::fromUTF8 ("Gerenciar computadores")
    };

    ChurchDAWDesktopLicensing::State state {
        ChurchDAWDesktopLicensing::State::signedOut
    };
    juce::Rectangle<int> brandArea;
    juce::Rectangle<int> cardArea;
    bool busy = false;
    bool passwordVisible = false;
    float spinnerPhase = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DesktopLoginPanel)
};
