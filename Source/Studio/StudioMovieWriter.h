#pragma once
#include <juce_graphics/juce_graphics.h>
#include <memory>
namespace churchstudio
{
struct AudioFile
{
    juce::File file;
    double offsetSeconds=0;
    float gain=1, peak=0;
};
class MovieWriter
{
public:
    MovieWriter();
    ~MovieWriter();
    juce::Result open (const juce::File& file,int width,int height);
    juce::Result append (const juce::Image&,double seconds);
    juce::Result finish (double seconds);
    static juce::Result combine (const juce::File& video,const std::vector<AudioFile>& audio,
                                const juce::File& destination,double duration);
    static bool supported();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
void requestMediaPermission (bool camera,std::function<void(bool)> callback);
}
