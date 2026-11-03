#pragma once
#include <juce_core/juce_core.h>
namespace churchstudio
{
inline juce::File resolveMobileRecording(const juce::File& documents,const juce::String& savedName,const juce::String& legacyPath)
{
    auto name=savedName;
    if(name.isEmpty() && juce::File::isAbsolutePath(legacyPath))name=juce::File(legacyPath).getFileName();
    if(!name.endsWithIgnoreCase(".mp4") || juce::File::createLegalFileName(name)!=name
        || name.containsAnyOf("/\\") || juce::File::isAbsolutePath(name))return {};
    return documents.getChildFile("ChurchCam").getChildFile(name);
}
}
