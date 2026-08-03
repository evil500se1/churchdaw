#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Helpers de produto Android: pack culto offline, deteccao MIDI BLE e USB.
// Logica pura — sem UI e sem alocacao no callback de audio.
//==============================================================================
namespace ChurchDAWAndroidCulto
{

// Packs que permitem culto sem baixar o Church Live (~12 GB).
inline bool isOfflineCultoPackId (const juce::String& id) noexcept
{
    return id.equalsIgnoreCase ("pad-continuo")
        || id.equalsIgnoreCase ("culto-offline")
        || id.equalsIgnoreCase ("culto-offline-v1");
}

// Preferir packs leves no starter: Pad Continuo cabe no celular e basta pra
// pad + uso basico. Church Live continua disponivel nas Bibliotecas.
inline bool isHeavyLibraryPackId (const juce::String& id) noexcept
{
    return id.equalsIgnoreCase ("church-live");
}

inline juce::String displayNameForPack (const juce::String& id,
                                        const juce::String& catalogName)
{
    if (isOfflineCultoPackId (id))
        return "Culto offline (Pad)";

    return catalogName;
}

inline juce::String descriptionHintForPack (const juce::String& id,
                                            const juce::String& catalogDescription)
{
    if (isOfflineCultoPackId (id))
        return "Pacote leve para culto sem Wi-Fi: pads A–G. "
               "Baixe o Church Live depois se quiser pianos e a biblioteca completa.";

    if (isHeavyLibraryPackId (id))
        return catalogDescription.isNotEmpty()
                 ? catalogDescription
                 : juce::String ("Biblioteca completa (vários GB). "
                                 "Para culto rápido use o pack Culto offline.");

    return catalogDescription;
}

// Heuristicas de nome do Android/JUCE — nao ha API estavel de "tipo BLE" no
// MidiDeviceInfo exposto pelo JUCE em todas as versoes.
inline bool nameLooksLikeBluetoothMidi (const juce::String& name) noexcept
{
    return name.containsIgnoreCase ("Bluetooth")
        || name.containsIgnoreCase ("BLE")
        || name.containsIgnoreCase ("BT-MIDI")
        || name.containsIgnoreCase ("BT MIDI")
        || name.containsIgnoreCase ("BluetoothMIDI")
        || name.containsIgnoreCase ("Midi Through Bluetooth");
}

inline bool nameLooksLikeUsbMidi (const juce::String& name) noexcept
{
    return name.containsIgnoreCase ("USB")
        || name.containsIgnoreCase ("OTG");
}

inline bool nameLooksLikeUsbAudio (const juce::String& name) noexcept
{
    return name.containsIgnoreCase (" USB device")
        || name.containsIgnoreCase (" USB accessory")
        || name.containsIgnoreCase (" USB headset")
        || name.containsIgnoreCase ("USB Audio")
        || (name.containsIgnoreCase ("USB") && name.containsIgnoreCase ("Audio"));
}

inline juce::String midiTransportTag (const juce::String& deviceName)
{
    if (nameLooksLikeBluetoothMidi (deviceName))
        return "BLE";

    if (nameLooksLikeUsbMidi (deviceName))
        return "USB";

    return {};
}

inline juce::String audioTransportTag (const juce::String& deviceName)
{
    if (nameLooksLikeUsbAudio (deviceName))
        return "USB";

    if (deviceName.containsIgnoreCase ("Bluetooth")
         || deviceName.containsIgnoreCase ("A2DP")
         || deviceName.containsIgnoreCase ("SCO"))
        return "BT";

    return {};
}

// Prefixo curto pro chip do cabecalho: "BLE Launchkey" / "USB Focusrite".
inline juce::String withTransportPrefix (const juce::String& prettyName,
                                         const juce::String& tag)
{
    if (tag.isEmpty() || prettyName.isEmpty())
        return prettyName;

    if (prettyName.startsWithIgnoreCase (tag + " "))
        return prettyName;

    return tag + " " + prettyName;
}

} // namespace ChurchDAWAndroidCulto
