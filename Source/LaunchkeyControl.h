#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>

//==============================================================================
// Integracao com o Novation Launchkey MK4 pela porta DAW (a segunda porta USB,
// "MIDIIN2/MIDIOUT2"). E a mesma interface que o Ableton usa: entrando em modo
// DAW, os faders e knobs passam a mandar controles fixos e a ChurchDAW pode
// escrever no VISOR do teclado.
//
// Protocolo (Launchkey MK4 Programmer's Reference Guide, SKUs normais):
//   entrar/sair do modo DAW : 9Fh 0Ch 7Fh  /  9Fh 0Ch 00h
//   cabecalho SysEx         : F0h 00h 20h 29h 02h 14h
//   configurar display      : <hdr> 04h <alvo> <config> F7h
//   escrever texto          : <hdr> 06h <alvo> <campo> <texto ASCII> F7h
//
// Faders e knobs medidos no aparelho (nao so na doc): ambos ABSOLUTOS no canal
// 16 — faders CC 5..13, knobs CC 21..28. Os alvos de display dos controles usam
// o mesmo numero do CC, entao o proprio teclado mostra o nome ao mexer.
//==============================================================================
class LaunchkeyControl : private juce::MidiInputCallback,
                         private juce::AsyncUpdater
{
public:
    static constexpr int numFaders = 9;    // 49/61 apenas
    static constexpr int numEncoders = 8;

    static constexpr int faderCC0 = 0x05;
    static constexpr int encoderCC0 = 0x15;

    // pads medidos no aparelho: fileira de cima 96..103, de baixo 112..119, canal 1
    static constexpr int padTop0 = 96;
    static constexpr int padBottom0 = 112;

    // paleta padrao Novation (velocidade = indice da cor)
    enum Colour { off = 0, dim = 1, white = 3, red = 5, orange = 9, yellow = 13, green = 21, blue = 45 };
    enum Blink  { staticOn = 0, flashing = 1, pulsing = 2 };   // vira o canal MIDI 1/2/3

    std::function<void (int index, float value01)> onFader;
    std::function<void (int index, float value01)> onEncoder;
    std::function<void (int note)> onPad;

    LaunchkeyControl()  { connect(); }
    ~LaunchkeyControl() override { disconnect(); }

    bool isConnected() const noexcept { return output != nullptr; }
    bool isListening() const noexcept { return input != nullptr; }   // pads/faders chegando?

    // A porta DAW e a SEGUNDA do aparelho. Publico porque o resto do app precisa
    // manter essa porta FORA da entrada MIDI normal (senao pad vira nota tocada,
    // e a porta fica ocupada — o Windows abre MIDI em modo exclusivo).
    static bool isDawPort (const juce::String& name)
    {
        return name.containsIgnoreCase ("Launchkey")
                && (name.containsIgnoreCase ("MIDIIN2") || name.containsIgnoreCase ("MIDIOUT2")
                     || name.containsIgnoreCase ("DAW"));
    }

    //== Visor =================================================================
    // Nome mostrado quando o controle e mexido. O teclado gera esse "display
    // temporario" sozinho — so precisamos deixar o texto pronto.
    void setFaderName (int index, const juce::String& text)
    {
        if (juce::isPositiveAndBelow (index, numFaders))
            setControlName (faderCC0 + index, text);
    }

    void setEncoderName (int index, const juce::String& text)
    {
        if (juce::isPositiveAndBelow (index, numEncoders))
            setControlName (encoderCC0 + index, text);
    }

    //== Pads ==================================================================
    void setPad (int note, int colour, int blink = staticOn)
    {
        if (output == nullptr || ! juce::isPositiveAndBelow (note, 128))
            return;

        const int key = colour | (blink << 8);

        if (padState[note] == key)
            return;   // ja esta assim: nao repete mensagem

        padState[note] = key;

        // canal 1 = estatico, 2 = piscando, 3 = pulsando
        output->sendMessageNow (juce::MidiMessage::noteOn (1 + blink, note, (juce::uint8) colour));
    }

    // Desenha um texto grande como BITMAP no visor (128x64). O guia especifica
    // 19 bytes por linha e 64 linhas, 7 pixels por byte, bit mais alto = pixel
    // mais a esquerda.
    void showLogo (const juce::String& text)
    {
        if (output == nullptr)
            return;

        juce::Image img (juce::Image::ARGB, 128, 64, true);

        {
            juce::Graphics g (img);
            g.fillAll (juce::Colours::black);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (26.0f)).boldened().italicised());
            g.drawText (text, 0, 8, 128, 34, juce::Justification::centred);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText ("DAW para igrejas", 0, 42, 128, 14, juce::Justification::centred);
        }

        juce::Array<juce::uint8> msg;
        msg.addArray (sysexHeader, 5);
        msg.add ((juce::uint8) 0x09);
        msg.add ((juce::uint8) 0x21);   // alvo: display temporario global

        for (int y = 0; y < 64; ++y)
        {
            for (int b = 0; b < 19; ++b)
            {
                juce::uint8 packed = 0;

                for (int bit = 0; bit < 7; ++bit)
                {
                    const int x = b * 7 + bit;

                    if (x < 128 && img.getPixelAt (x, y).getBrightness() > 0.5f)
                        packed |= (juce::uint8) (1 << (6 - bit));
                }

                msg.add (packed);
            }
        }

        output->sendMessageNow (juce::MidiMessage::createSysExMessage (msg.getRawDataPointer(),
                                                                       msg.size()));
    }

    // Display fixo (fica na tela): titulo + nome + valor
    void showStationary (const juce::String& title, const juce::String& name, const juce::String& value)
    {
        if (output == nullptr)
            return;

        if (title == statTitle && name == statName && value == statValue)
            return;   // nada mudou: nao enche a porta MIDI a toa

        statTitle = title; statName = name; statValue = value;

        configureDisplay (0x20, 0x02);   // arranjo 2: 3 linhas (titulo/nome/valor)
        sendText (0x20, 0, title);
        sendText (0x20, 1, name);
        sendText (0x20, 2, value);
        configureDisplay (0x20, 0x7F);   // dispara
    }

private:
    //== Ligacao ===============================================================
    void connect()
    {
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            if (isDawPort (d.name))
            {
                output = juce::MidiOutput::openDevice (d.identifier);
                break;
            }

        if (output == nullptr)
            return;

        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (isDawPort (d.name))
            {
                input = juce::MidiInput::openDevice (d.identifier, this);
                break;
            }

        if (input != nullptr)
            input->start();

        // entra em modo DAW e prepara cada controle pra mostrar o proprio nome
        output->sendMessageNow (juce::MidiMessage (0x9F, 0x0C, 0x7F));

        // PICKUP dos faders: sem isso o volume PULA pra posicao fisica do fader
        // assim que voce encosta nele — se o fader esta em cima e o canal esta
        // baixo, leva um susto no meio do culto. Com pickup o fader so assume
        // depois de passar pelo valor atual.
        sendFeature (0x46, 0x7F);

        for (int i = 0; i < numFaders; ++i)
            configureDisplay ((juce::uint8) (faderCC0 + i), 0x64);      // auto-temp + arranjo 4

        for (int i = 0; i < numEncoders; ++i)
            configureDisplay ((juce::uint8) (encoderCC0 + i), 0x64);
    }

    void disconnect()
    {
        cancelPendingUpdate();

        if (input != nullptr)
        {
            input->stop();
            input.reset();
        }

        if (output != nullptr)
        {
            for (int i = 0; i < 8; ++i)   // apaga os pads que acendemos
            {
                output->sendMessageNow (juce::MidiMessage::noteOn (1, padTop0 + i, (juce::uint8) 0));
                output->sendMessageNow (juce::MidiMessage::noteOn (1, padBottom0 + i, (juce::uint8) 0));
            }

            configureDisplay (0x20, 0x00);                              // apaga o display fixo
            output->sendMessageNow (juce::MidiMessage (0x9F, 0x0C, 0x00));   // volta pro modo normal
            output.reset();
        }
    }

    //== SysEx =================================================================
    // F0h 00h 20h 29h 02h 14h — Novation, Launchkey MK4 (SKUs normais)
    static constexpr juce::uint8 sysexHeader[5] = { 0x00, 0x20, 0x29, 0x02, 0x14 };

    void sysex (std::initializer_list<juce::uint8> body)
    {
        if (output == nullptr)
            return;

        juce::Array<juce::uint8> msg;
        msg.addArray (sysexHeader, 5);   // fabricante + modelo (o F0/F7 quem poe e o JUCE)

        for (auto b : body)
            msg.add (b);

        output->sendMessageNow (juce::MidiMessage::createSysExMessage (msg.getRawDataPointer(),
                                                                       msg.size()));
    }

    // "feature controls" do aparelho: CC no canal 7 pela porta DAW
    void sendFeature (juce::uint8 cc, juce::uint8 value)
    {
        if (output != nullptr)
            output->sendMessageNow (juce::MidiMessage::controllerEvent (7, cc, value));
    }

    void configureDisplay (juce::uint8 target, juce::uint8 config)
    {
        sysex ({ 0x04, target, config });
    }

    void sendText (juce::uint8 target, juce::uint8 field, const juce::String& text)
    {
        if (output == nullptr)
            return;

        juce::Array<juce::uint8> msg;
        msg.addArray (sysexHeader, 5);
        msg.add ((juce::uint8) 0x06);
        msg.add (target);
        msg.add (field);

        // o visor aceita ASCII 20h..7Eh; acentos viram a letra sem acento
        for (auto c : text.removeCharacters ("\r\n"))
        {
            const auto ascii = juce::String::charToString (c).toStdString();
            const auto ch = (juce::uint8) juce::jlimit (0x20, 0x7E, (int) ascii[0]);
            msg.add (ch);

            if (msg.size() > 40)
                break;   // o visor e pequeno: nao adianta mandar um romance
        }

        output->sendMessageNow (juce::MidiMessage::createSysExMessage (msg.getRawDataPointer(),
                                                                       msg.size()));
    }

    void setControlName (int cc, const juce::String& text)
    {
        if (output == nullptr || cc < 0 || cc >= 128)
            return;

        if (controlNames[cc] == text)
            return;

        controlNames[cc] = text;
        sendText ((juce::uint8) cc, 0, text);
    }

    //== MIDI de entrada =======================================================
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        // thread de MIDI: so guarda o valor e acorda a thread de mensagens
        if (m.isNoteOn() && m.getVelocity() > 0)
        {
            const int note = m.getNoteNumber();

            if ((note >= padTop0 && note < padTop0 + 8) || (note >= padBottom0 && note < padBottom0 + 8))
            {
                const juce::ScopedLock sl (padLock);
                pendingPads.add (note);
                triggerAsyncUpdate();
            }

            return;
        }

        if (! m.isController() || m.getChannel() != 16)
            return;

        const int cc = m.getControllerNumber();
        const float v = (float) m.getControllerValue() / 127.0f;

        if (cc >= faderCC0 && cc < faderCC0 + numFaders)
        {
            faderValue[cc - faderCC0].store (v);
            faderDirty[cc - faderCC0].store (true);
            triggerAsyncUpdate();
        }
        else if (cc >= encoderCC0 && cc < encoderCC0 + numEncoders)
        {
            encoderValue[cc - encoderCC0].store (v);
            encoderDirty[cc - encoderCC0].store (true);
            triggerAsyncUpdate();
        }
    }

    // junta a enxurrada de CCs de um movimento num unico repasse por quadro
    void handleAsyncUpdate() override
    {
        juce::Array<int> pads;

        {
            const juce::ScopedLock sl (padLock);
            pads.swapWith (pendingPads);
        }

        for (int note : pads)
            if (onPad != nullptr)
                onPad (note);

        for (int i = 0; i < numFaders; ++i)
            if (faderDirty[i].exchange (false) && onFader != nullptr)
                onFader (i, faderValue[i].load());

        for (int i = 0; i < numEncoders; ++i)
            if (encoderDirty[i].exchange (false) && onEncoder != nullptr)
                onEncoder (i, encoderValue[i].load());
    }

    std::unique_ptr<juce::MidiOutput> output;
    std::unique_ptr<juce::MidiInput> input;

    std::atomic<float> faderValue[numFaders] {};
    std::atomic<bool>  faderDirty[numFaders] {};
    std::atomic<float> encoderValue[numEncoders] {};
    std::atomic<bool>  encoderDirty[numEncoders] {};

    juce::String controlNames[128];
    juce::String statTitle, statName, statValue;

    juce::CriticalSection padLock;
    juce::Array<int> pendingPads;
    int padState[128] {};   // ultima combinacao cor+modo enviada, pra nao repetir

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LaunchkeyControl)
};
