#pragma once
#include <juce_video/juce_video.h>
#if JUCE_IOS
namespace churchstudio
{
// A video-only AVCaptureSession. JUCE's iOS CameraDevice opens an audio input
// and delivers repeated still photos; neither is suitable for a live instrument.
class MobileCamera
{
public:
    using Listener=juce::CameraDevice::Listener;
    using Callback=std::function<void(MobileCamera*,const juce::String&)>;
    static juce::StringArray getAvailableDevices();
    static void openDeviceAsync(int index,Callback,int minWidth=320,int minHeight=240,
                                int maxWidth=1280,int maxHeight=720,bool highQuality=false);
    ~MobileCamera();
    void addListener(Listener*);
    void removeListener(Listener*);
private:
    struct Impl;
    explicit MobileCamera(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl;
};
class CaptureBackgroundTask
{
public:
    CaptureBackgroundTask();
    ~CaptureBackgroundTask();
private:
    struct Impl;std::shared_ptr<Impl> impl;
};
}
#endif
