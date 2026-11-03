#pragma once
#if JUCE_ANDROID
#include <functional>
#include <memory>

namespace churchstudio
{
// A runtime permission dialog pauses the Activity without hiding the app.
// Stop capture only when the main Activity actually leaves the screen.
class AndroidCaptureLifecycle final
{
public:
    explicit AndroidCaptureLifecycle(std::function<void(bool)> callback);
    ~AndroidCaptureLifecycle();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif
