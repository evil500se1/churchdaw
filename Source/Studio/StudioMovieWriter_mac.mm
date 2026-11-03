#include "StudioMovieWriter.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <juce_events/juce_events.h>

namespace churchstudio
{
static NSString* ns (const juce::String& s) { return [NSString stringWithUTF8String:s.toRawUTF8()]; }
static juce::Result failure (NSError* error)
{
    return juce::Result::fail(error != nil ? juce::String::fromUTF8([[error localizedDescription] UTF8String])
                                         : juce::String("Falha no codificador de video."));
}
struct MovieWriter::Impl
{
    AVAssetWriter* writer=nil;
    AVAssetWriterInput* video=nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor=nil;
    int width=1280,height=720;
    double lastTime=-1;
    ~Impl() { [adaptor release]; [video release]; [writer release]; }
};
MovieWriter::MovieWriter() : impl(new Impl) {}
MovieWriter::~MovieWriter() = default;
bool MovieWriter::supported() { return true; }
juce::Result MovieWriter::open(const juce::File& file,int w,int h)
{
    @autoreleasepool {
        impl->width=w;impl->height=h;
        NSError* error=nil;
        impl->writer=[[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:ns(file.getFullPathName())]
                                             fileType:AVFileTypeQuickTimeMovie error:&error];
        if(impl->writer==nil) return failure(error);
        NSDictionary* settings=@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(w),AVVideoHeightKey:@(h),
            AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@(w>1280?12000000:6000000),AVVideoExpectedSourceFrameRateKey:@30,
                                             AVVideoMaxKeyFrameIntervalKey:@60}};
        impl->video=[[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:settings];
        impl->video.expectsMediaDataInRealTime=YES;
        impl->adaptor=[[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:impl->video
            sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
                                          (id)kCVPixelBufferWidthKey:@(w),(id)kCVPixelBufferHeightKey:@(h),
                                          (id)kCVPixelBufferIOSurfacePropertiesKey:@{}}];
        if(![impl->writer canAddInput:impl->video]) return juce::Result::fail("Formato de video indisponivel.");
        [impl->writer addInput:impl->video];
        if(![impl->writer startWriting]) return failure(impl->writer.error);
        [impl->writer startSessionAtSourceTime:kCMTimeZero];
        return juce::Result::ok();
    }
}
juce::Result MovieWriter::append(const juce::Image& image,double seconds)
{
    @autoreleasepool {
        if(seconds<=impl->lastTime) return juce::Result::ok();
        for(int tries=0;!impl->video.readyForMoreMediaData && tries<200;++tries)
        {
            if(impl->writer.status==AVAssetWriterStatusFailed) return failure(impl->writer.error);
            juce::Thread::sleep(2);
        }
        if(!impl->video.readyForMoreMediaData) return juce::Result::fail("O codificador nao acompanhou a gravacao.");
        CVPixelBufferRef buffer=nullptr;
        if(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,impl->adaptor.pixelBufferPool,&buffer)!=kCVReturnSuccess)
            return juce::Result::fail("Sem memoria para o quadro de video.");
        CVPixelBufferLockBaseAddress(buffer,0);
        auto* destination=(uint8_t*)CVPixelBufferGetBaseAddress(buffer);
        auto stride=CVPixelBufferGetBytesPerRow(buffer);
        juce::Image::BitmapData source(image,juce::Image::BitmapData::readOnly);
        if(image.getWidth()!=impl->width || image.getHeight()!=impl->height || source.pixelStride!=4)
        {
            CVPixelBufferUnlockBaseAddress(buffer,0);CVPixelBufferRelease(buffer);
            return juce::Result::fail("Dimensoes de quadro invalidas.");
        }
        for(int y=0;y<impl->height;++y) memcpy(destination+(size_t)y*stride,source.getLinePointer(y),(size_t)impl->width*4);
        CVPixelBufferUnlockBaseAddress(buffer,0);
        bool ok=[impl->adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMakeWithSeconds(seconds,60000)];
        CVPixelBufferRelease(buffer);impl->lastTime=seconds;
        return ok?juce::Result::ok():failure(impl->writer.error);
    }
}
juce::Result MovieWriter::finish(double seconds)
{
    @autoreleasepool {
        if(impl->writer==nil) return juce::Result::fail("Gravacao nao iniciada.");
        [impl->writer endSessionAtSourceTime:CMTimeMakeWithSeconds(seconds,60000)];
        [impl->video markAsFinished];
        dispatch_semaphore_t done=dispatch_semaphore_create(0);
        [impl->writer finishWritingWithCompletionHandler:^{dispatch_semaphore_signal(done);}];
        dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER);
        dispatch_release(done);
        return impl->writer.status==AVAssetWriterStatusCompleted?juce::Result::ok():failure(impl->writer.error);
    }
}
juce::Result MovieWriter::combine(const juce::File& video,const std::vector<AudioFile>& audio,
                                 const juce::File& destination,double duration)
{
    @autoreleasepool {
        AVMutableComposition* composition=[AVMutableComposition composition];
        AVURLAsset* movie=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:ns(video.getFullPathName())] options:nil];
        AVAssetTrack* vt=[[movie tracksWithMediaType:AVMediaTypeVideo] firstObject];
        if(vt==nil) return juce::Result::fail("O video nao possui quadros validos.");
        NSError* error=nil;
        auto range=CMTimeRangeMake(kCMTimeZero,CMTimeMakeWithSeconds(duration,60000));
        auto* outputVideo=[composition addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
        if(![outputVideo insertTimeRange:range ofTrack:vt atTime:kCMTimeZero error:&error]) return failure(error);
        NSMutableArray* mixes=[NSMutableArray array];
        float maximum=0;
        for(const auto& source:audio) maximum+=source.gain*source.peak;
        const float protection=maximum>.98f?.98f/maximum:1.0f;
        for(const auto& source:audio)
        {
            if(!source.file.existsAsFile() || source.gain<=0 || source.offsetSeconds>=duration) continue;
            AVURLAsset* a=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:ns(source.file.getFullPathName())] options:nil];
            AVAssetTrack* at=[[a tracksWithMediaType:AVMediaTypeAudio] firstObject];
            if(at==nil) return juce::Result::fail("Faixa de audio invalida.");
            double length=juce::jmin(CMTimeGetSeconds(a.duration),duration-source.offsetSeconds);
            if(length<=0) continue;
            auto* track=[composition addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid];
            if(![track insertTimeRange:CMTimeRangeMake(kCMTimeZero,CMTimeMakeWithSeconds(length,60000)) ofTrack:at
                               atTime:CMTimeMakeWithSeconds(juce::jmax(0.0,source.offsetSeconds),60000) error:&error]) return failure(error);
            auto* mix=[AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:track];
            [mix setVolume:source.gain*protection atTime:kCMTimeZero];[mixes addObject:mix];
        }
        auto* mix=[AVMutableAudioMix audioMix];mix.inputParameters=mixes;
        AVAssetExportSession* exporter=[[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetHighestQuality];
        exporter.outputURL=[NSURL fileURLWithPath:ns(destination.getFullPathName())];
        exporter.outputFileType=AVFileTypeMPEG4;exporter.shouldOptimizeForNetworkUse=YES;exporter.audioMix=mix;
        dispatch_semaphore_t done=dispatch_semaphore_create(0);
        [exporter exportAsynchronouslyWithCompletionHandler:^{dispatch_semaphore_signal(done);}];
        dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER);dispatch_release(done);
        auto result=exporter.status==AVAssetExportSessionStatusCompleted?juce::Result::ok():failure(exporter.error);
        [exporter release];return result;
    }
}
void requestMediaPermission(bool camera,std::function<void(bool)> callback)
{
    auto type=camera?AVMediaTypeVideo:AVMediaTypeAudio;
    auto status=[AVCaptureDevice authorizationStatusForMediaType:type];
    if(status==AVAuthorizationStatusAuthorized) { callback(true);return; }
    if(status==AVAuthorizationStatusDenied || status==AVAuthorizationStatusRestricted) { callback(false);return; }
    [AVCaptureDevice requestAccessForMediaType:type completionHandler:^(BOOL allowed) {
        juce::MessageManager::callAsync([callback,allowed]{callback(allowed);});
    }];
}
}
