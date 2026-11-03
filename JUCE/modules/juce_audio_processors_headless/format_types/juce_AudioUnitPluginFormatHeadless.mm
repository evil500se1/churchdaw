/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-9-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

#if JUCE_INTERNAL_HAS_AU

#include <juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h>

#if JUCE_IOS
 #import <AVFAudio/AVFAudio.h>
#endif

namespace juce
{

void AudioUnitPluginFormatHeadless::findAllTypesForFile (OwnedArray<PluginDescription>& results,
                                                         const String& fileOrIdentifier)
{
    if (! fileMightContainThisPluginType (fileOrIdentifier))
        return;

    PluginDescription desc;
    desc.fileOrIdentifier = fileOrIdentifier;
    desc.uniqueId = desc.deprecatedUid = 0;

    if (MessageManager::getInstance()->isThisTheMessageThread()
          && requiresUnblockedMessageThreadDuringCreation (desc))
        return;

    try
    {
        auto createdInstance = createInstanceFromDescription (desc, 44100.0, 512);

        if (auto auInstance = dynamic_cast<AudioUnitPluginInstanceHeadless*> (createdInstance.get()))
            results.add (new PluginDescription (auInstance->getPluginDescription()));
    }
    catch (...)
    {
        // crashed while loading...
    }
}

void AudioUnitPluginFormatHeadless::createPluginInstance (const PluginDescription& desc,
                                                          double rate,
                                                          int blockSize,
                                                          PluginCreationCallback callback)
{
    createAudioUnitPluginInstance<AudioUnitPluginInstanceHeadless> (*this, desc, rate, blockSize, callback);
}

void AudioUnitPluginFormatHeadless::createARAFactoryAsync (const PluginDescription& desc, ARAFactoryCreationCallback callback)
{
    auto auComponentResult = getAudioComponent (*this, desc);

    if (! auComponentResult.isValid())
    {
        callback ({ {}, "Failed to create AudioComponent for " + desc.descriptiveName });
        return;
    }

    getOrCreateARAAudioUnit (auComponentResult.component, [cb = std::move (callback)] (auto dylibKeepAliveAudioUnit)
    {
        cb ([&]() -> ARAFactoryResult
            {
                if (dylibKeepAliveAudioUnit != nullptr)
                    return { ARAFactoryWrapper { ::juce::getARAFactory (std::move (dylibKeepAliveAudioUnit)) }, "" };

                return { {}, "Failed to create ARAFactory from the provided AudioUnit" };
            }());
    });
}

bool AudioUnitPluginFormatHeadless::requiresUnblockedMessageThreadDuringCreation (const PluginDescription& desc) const
{
    String pluginName, version, manufacturer;
    AudioComponentDescription componentDesc;

    if (AudioUnitFormatHelpers::getComponentDescFromIdentifier (desc.fileOrIdentifier, componentDesc,
                                                                pluginName, version, manufacturer)
           || AudioUnitFormatHelpers::getComponentDescFromFile (desc.fileOrIdentifier, componentDesc,
                                                                pluginName, version, manufacturer))
    {
        if (AudioComponent auComp = AudioComponentFindNext (nullptr, &componentDesc))
        {
            if (AudioComponentGetDescription (auComp, &componentDesc) == noErr)
                return AudioUnitFormatHelpers::isPluginAUv3 (componentDesc);
        }
    }

    return false;
}

StringArray AudioUnitPluginFormatHeadless::searchPathsForPlugins (const FileSearchPath&, bool /*recursive*/, bool allowPluginsWhichRequireAsynchronousInstantiation)
{
    StringArray result;

    const auto isWantedType = [] (const AudioComponentDescription& desc)
    {
        return desc.componentType == kAudioUnitType_MusicDevice
            || desc.componentType == kAudioUnitType_MusicEffect
            || desc.componentType == kAudioUnitType_Effect
            || desc.componentType == kAudioUnitType_Generator
            || desc.componentType == kAudioUnitType_Panner
            || desc.componentType == kAudioUnitType_Mixer
            || desc.componentType == kAudioUnitType_MIDIProcessor;
    };

   #if JUCE_IOS
    // PATCH ChurchDAW (18/ago/2026): no iOS, AudioComponentFindNext NAO
    // enumera extensoes AUv3 de OUTROS apps — so componentes da Apple e do
    // proprio bundle. O registro de extensoes do sistema (bs-16i, Ravenscroft
    // etc.) e consultado pelo AVAudioUnitComponentManager, o mesmo caminho
    // dos hosts reais (AUM, Cubasis, GarageBand). Consultar o manager tambem
    // popula o registro do processo, de modo que os AudioComponentFindNext
    // seguintes (instanciacao) passam a resolver essas descricoes.
    @autoreleasepool
    {
        // componentsPassingTest com YES incondicional e o wildcard REAL: o
        // matchingDescription com struct zerada devolvia apenas os
        // componentes da Apple em campo (iPad do dono, 18/ago) — as
        // extensoes de terceiros so vieram por este caminho.
        NSArray<AVAudioUnitComponent*>* components =
            [[AVAudioUnitComponentManager sharedAudioUnitComponentManager]
                componentsPassingTest: ^BOOL (AVAudioUnitComponent*, BOOL*)
                {
                    return YES;
                }];

        Logger::writeToLog ("AUV3_SCAN: manager devolveu "
                            + String ((int) components.count)
                            + " componente(s)");

        for (AVAudioUnitComponent* component in components)
        {
            const AudioComponentDescription desc = [component audioComponentDescription];

            Logger::writeToLog ("AUV3_SCAN: "
                                + nsStringToJuce (component.manufacturerName)
                                + " / " + nsStringToJuce (component.name)
                                + (isWantedType (desc) ? String()
                                                       : String (" [tipo ignorado]")));

            if (! isWantedType (desc))
                continue;

            // Todo AUv3 requer instanciacao assincrona; sem a permissao, o
            // iOS nao tem o que listar (nao existem AUv2 de terceiros aqui).
            if (allowPluginsWhichRequireAsynchronousInstantiation
                 || ! AudioUnitFormatHelpers::isPluginAUv3 (desc))
                result.addIfNotAlreadyThere (AudioUnitFormatHelpers::createPluginIdentifier (desc));
        }
    }
   #endif

    AudioComponent comp = nullptr;

    for (;;)
    {
        AudioComponentDescription desc;
        zerostruct (desc);

        comp = AudioComponentFindNext (comp, &desc);

        if (comp == nullptr)
            break;

        if (AudioComponentGetDescription (comp, &desc) != noErr)
            continue;

        if (isWantedType (desc))
        {
            if (allowPluginsWhichRequireAsynchronousInstantiation || ! AudioUnitFormatHelpers::isPluginAUv3 (desc))
                result.addIfNotAlreadyThere (AudioUnitFormatHelpers::createPluginIdentifier (desc));
        }
    }

    return result;
}

bool AudioUnitPluginFormatHeadless::fileMightContainThisPluginType (const String& fileOrIdentifier)
{
    AudioComponentDescription desc;
    String name, version, manufacturer;

    if (AudioUnitFormatHelpers::getComponentDescFromIdentifier (fileOrIdentifier, desc, name, version, manufacturer))
        return AudioComponentFindNext (nullptr, &desc) != nullptr;

    auto f = File::createFileWithoutCheckingPath (fileOrIdentifier);

    return (f.hasFileExtension (".component") || f.hasFileExtension (".appex"))
             && f.isDirectory();
}

String AudioUnitPluginFormatHeadless::getNameOfPluginFromIdentifier (const String& fileOrIdentifier)
{
    AudioComponentDescription desc;
    String name, version, manufacturer;
    AudioUnitFormatHelpers::getComponentDescFromIdentifier (fileOrIdentifier, desc, name, version, manufacturer);

    if (name.isEmpty())
        name = fileOrIdentifier;

    return name;
}

bool AudioUnitPluginFormatHeadless::pluginNeedsRescanning (const PluginDescription& desc)
{
    AudioComponentDescription newDesc;
    String name, version, manufacturer;

    return ! (AudioUnitFormatHelpers::getComponentDescFromIdentifier (desc.fileOrIdentifier, newDesc,
                                                                      name, version, manufacturer)
               && version == desc.version);
}

bool AudioUnitPluginFormatHeadless::doesPluginStillExist (const PluginDescription& desc)
{
    if (desc.fileOrIdentifier.startsWithIgnoreCase (AudioUnitFormatHelpers::auIdentifierPrefix))
        return fileMightContainThisPluginType (desc.fileOrIdentifier);

    return File (desc.fileOrIdentifier).exists();
}

FileSearchPath AudioUnitPluginFormatHeadless::getDefaultLocationsToSearch()
{
    return {};
}

} // namespace juce

#endif
