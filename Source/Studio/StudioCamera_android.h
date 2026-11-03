#pragma once
#include <juce_video/juce_video.h>
#if JUCE_ANDROID
namespace churchstudio
{
class AndroidCamera
{
public:
    using Listener=juce::CameraDevice::Listener;
    using Callback=std::function<void(AndroidCamera*,const juce::String&)>;
    static juce::StringArray getAvailableDevices();
    static void openDeviceAsync(int,Callback,int=320,int=240,int=1280,int=720,bool=false);
    ~AndroidCamera();
    void addListener(Listener*);
    void removeListener(Listener*);
    void setErrorCallback(std::function<void(const juce::String&)>);
    struct Impl;
    explicit AndroidCamera(std::shared_ptr<Impl>);
private:
    std::shared_ptr<Impl> impl;
};
}
#endif
