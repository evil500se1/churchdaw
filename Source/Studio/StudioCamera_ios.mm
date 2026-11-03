#include "StudioCamera_ios.h"
#if JUCE_IOS
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <atomic>
#include <cstring>

namespace churchstudio
{
struct CameraFrames
{
    juce::CriticalSection lock;
    juce::ListenerList<MobileCamera::Listener> listeners;
    std::atomic<bool> stopped{false};
};
}
@interface ChurchCamVideoDelegate : NSObject<AVCaptureVideoDataOutputSampleBufferDelegate>
{
@public std::weak_ptr<churchstudio::CameraFrames> frames;
}
@end
@implementation ChurchCamVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection
{
    (void)output;(void)connection;
    auto state=frames.lock();if(!state || state->stopped.load())return;
    CVPixelBufferRef pixels=CMSampleBufferGetImageBuffer(sample);if(pixels==nullptr)return;
    if(CVPixelBufferGetPixelFormatType(pixels)!=kCVPixelFormatType_32BGRA)return;
    CVPixelBufferLockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);
    const int width=(int)CVPixelBufferGetWidth(pixels),height=(int)CVPixelBufferGetHeight(pixels);
    juce::Image image(juce::Image::ARGB,width,height,false,juce::SoftwareImageType());
    {
        juce::Image::BitmapData target(image,juce::Image::BitmapData::writeOnly);
        auto* source=(const uint8_t*)CVPixelBufferGetBaseAddress(pixels);
        const auto stride=CVPixelBufferGetBytesPerRow(pixels);
        for(int y=0;y<height;++y)std::memcpy(target.getLinePointer(y),source+(size_t)y*stride,(size_t)width*4);
    }
    CVPixelBufferUnlockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);
    const juce::ScopedLock lock(state->lock);
    if(!state->stopped.load())state->listeners.call([&](churchstudio::MobileCamera::Listener& l){l.imageReceived(image);});
}
@end

namespace churchstudio
{
static NSArray<AVCaptureDevice*>* cameras()
{
    auto discovery=[AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
          mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
    NSMutableArray* sorted=[NSMutableArray array];
    for(AVCaptureDevice* d in discovery.devices)if(d.position==AVCaptureDevicePositionFront)[sorted addObject:d];
    for(AVCaptureDevice* d in discovery.devices)if(d.position!=AVCaptureDevicePositionFront)[sorted addObject:d];
    return sorted;
}
struct MobileCamera::Impl
{
    std::shared_ptr<CameraFrames> frames=std::make_shared<CameraFrames>();
    dispatch_queue_t queue=dispatch_queue_create("com.churchdaw.camera",DISPATCH_QUEUE_SERIAL);
    AVCaptureSession* session=[[AVCaptureSession alloc] init];
    AVCaptureVideoDataOutput* output=[[AVCaptureVideoDataOutput alloc] init];
    ChurchCamVideoDelegate* delegate=[[ChurchCamVideoDelegate alloc] init];
    id orientationObserver=nil;
    Impl(){delegate->frames=frames;session.automaticallyConfiguresApplicationAudioSession=NO;}
    ~Impl()
    {
        if(orientationObserver){[[NSNotificationCenter defaultCenter] removeObserver:orientationObserver];[orientationObserver release];}
        [output setSampleBufferDelegate:nil queue:nullptr];
        [session stopRunning];[session release];[output release];[delegate release];dispatch_release(queue);
    }
};
static AVCaptureVideoOrientation captureOrientation()
{
    const auto orientation=[UIApplication sharedApplication].statusBarOrientation;
    switch(orientation)
    {
        case UIInterfaceOrientationLandscapeLeft:return AVCaptureVideoOrientationLandscapeLeft;
        case UIInterfaceOrientationPortrait:return AVCaptureVideoOrientationPortrait;
        case UIInterfaceOrientationPortraitUpsideDown:return AVCaptureVideoOrientationPortraitUpsideDown;
        default:return AVCaptureVideoOrientationLandscapeRight;
    }
}
juce::StringArray MobileCamera::getAvailableDevices()
{
    juce::StringArray result;
    for(AVCaptureDevice* device in cameras())
        result.add(juce::String::fromUTF8(device.position==AVCaptureDevicePositionFront?"Câmera frontal":"Câmera traseira"));
    return result;
}
MobileCamera::MobileCamera(std::shared_ptr<Impl> p):impl(std::move(p)){}
MobileCamera::~MobileCamera()
{
    auto p=std::move(impl);p->frames->stopped.store(true);
    {const juce::ScopedLock lock(p->frames->lock);p->frames->listeners.clear();}
    // Stopping a camera can take seconds; never block the instrument's UI.
    dispatch_async(p->queue,^{[p->session stopRunning];});
}
void MobileCamera::addListener(Listener* l){const juce::ScopedLock lock(impl->frames->lock);impl->frames->listeners.add(l);}
void MobileCamera::removeListener(Listener* l){const juce::ScopedLock lock(impl->frames->lock);impl->frames->listeners.remove(l);}
void MobileCamera::openDeviceAsync(int index,Callback callback,int,int,int,int,bool)
{
    auto devices=cameras();
    if(index<0 || index>=(int)devices.count){callback(nullptr,"Camera indisponivel.");return;}
    auto p=std::make_shared<Impl>();AVCaptureDevice* device=[devices[(NSUInteger)index] retain];
    const auto orientation=captureOrientation();
    dispatch_async(p->queue,^{
        @autoreleasepool {
            NSError* error=nil;auto* input=[AVCaptureDeviceInput deviceInputWithDevice:device error:&error];[device release];
            juce::String problem;
            [p->session beginConfiguration];
            if([p->session canSetSessionPreset:AVCaptureSessionPreset1280x720])p->session.sessionPreset=AVCaptureSessionPreset1280x720;
            if(input && [p->session canAddInput:input])[p->session addInput:input];
            else problem=error?juce::String::fromUTF8(error.localizedDescription.UTF8String):juce::String("Nao foi possivel abrir a camera.");
            p->output.alwaysDiscardsLateVideoFrames=YES;
            p->output.videoSettings=@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)};
            [p->output setSampleBufferDelegate:p->delegate queue:p->queue];
            if([p->session canAddOutput:p->output])[p->session addOutput:p->output];
            else problem="Saida de video indisponivel.";
            auto* connection=[p->output connectionWithMediaType:AVMediaTypeVideo];
            if(connection.isVideoOrientationSupported)connection.videoOrientation=orientation;
            if(connection.isVideoMirroringSupported){connection.automaticallyAdjustsVideoMirroring=NO;connection.videoMirrored=NO;}
            [p->session commitConfiguration];
            if(problem.isEmpty())[p->session startRunning];
            if(problem.isEmpty() && !p->session.isRunning)problem="A camera nao iniciou. Verifique as permissoes do iOS.";
            juce::MessageManager::callAsync([p,callback,problem]{
                if(problem.isNotEmpty()){callback(nullptr,problem);return;}
                std::weak_ptr<Impl> weak=p;
                p->orientationObserver=[[[NSNotificationCenter defaultCenter] addObserverForName:UIDeviceOrientationDidChangeNotification object:nil
                    queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*){
                        auto current=weak.lock();if(!current || current->frames->stopped.load())return;
                        const auto angle=captureOrientation();
                        dispatch_async(current->queue,^{auto* c=[current->output connectionWithMediaType:AVMediaTypeVideo];if(c.isVideoOrientationSupported)c.videoOrientation=angle;});
                    }] retain];
                callback(new MobileCamera(p),{});
            });
        }
    });
}
struct CaptureBackgroundTask::Impl
{
    UIBackgroundTaskIdentifier token=UIBackgroundTaskInvalid;
    void end(){if(token!=UIBackgroundTaskInvalid){[[UIApplication sharedApplication] endBackgroundTask:token];token=UIBackgroundTaskInvalid;}}
    ~Impl(){end();}
};
CaptureBackgroundTask::CaptureBackgroundTask():impl(std::make_shared<Impl>())
{
    std::weak_ptr<Impl> weak=impl;
    impl->token=[[UIApplication sharedApplication] beginBackgroundTaskWithName:@"ChurchCam finalization" expirationHandler:^{if(auto p=weak.lock())p->end();}];
}
CaptureBackgroundTask::~CaptureBackgroundTask()=default;
}
#endif
