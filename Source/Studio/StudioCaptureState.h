#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_graphics/juce_graphics.h>
#include <atomic>
#include <array>
#include <memory>

namespace churchstudio
{
// One producer (the device callback), one consumer (the encoder). Recording
// never waits, allocates, accesses files or invokes UI from an audio callback.
class AudioTap
{
public:
    static constexpr int capacity = 128, blockSize = 2048;
    struct Block
    {
        std::array<float, blockSize> left {}, right {};
        int count = 0;
        double rate = 48000, timeMs = 0;
        uint64_t position = 0, epoch = 0;
    };
    AudioTap() : blocks (new Block[capacity]) {}
    void prepare (double newRate) noexcept
    {
        rate = newRate > 0 ? newRate : 48000; position = 0; anchor = 0; ++epoch;
    }
    void push (const float* const* data, int channels, int samples) noexcept
    {
        if (data == nullptr || channels < 1 || samples < 1) return;
        if (! enabled.load (std::memory_order_relaxed))
        {
            anchor = 0; position = 0;
            if (monitoring.load (std::memory_order_relaxed))
            {
                float maximum=0;
                for(int c=0;c<juce::jmin(2,channels);++c)
                    if(data[c])for(int i=0;i<samples;++i)
                        if(std::isfinite(data[c][i]))maximum=juce::jmax(maximum,std::abs(data[c][i]));
                publishPeak(maximum);
                callbacks.fetch_add(1,std::memory_order_relaxed);
            }
            return;
        }
        callbacks.fetch_add(1,std::memory_order_relaxed);
        if (anchor == 0) { anchor = juce::Time::getMillisecondCounterHiRes(); position = 0; ++epoch; }
        for (int offset = 0; offset < samples;)
        {
            const int n = juce::jmin (blockSize, samples-offset);
            int a=0,b=0,c=0,d=0; fifo.prepareToWrite (1,a,b,c,d);
            if (b == 0) { dropped.fetch_add (1); position += (uint64_t)n; offset += n; continue; }
            auto& block = blocks[(size_t)a];
            block.count=n; block.rate=rate; block.position=position; block.epoch=epoch;
            block.timeMs=anchor+1000.0*(double)position/rate;
            float maximum=0;
            for (int i=0;i<n;++i)
            {
                float l=data[0] != nullptr ? data[0][offset+i] : 0;
                float r=channels>1 && data[1]!=nullptr ? data[1][offset+i] : l;
                if (! std::isfinite(l)) l=0;
                if (! std::isfinite(r)) r=0;
                block.left[(size_t)i]=l; block.right[(size_t)i]=r;
                maximum=juce::jmax(maximum,std::abs(l),std::abs(r));
            }
            publishPeak(maximum);
            fifo.finishedWrite(1); position+=(uint64_t)n; offset+=n;
        }
    }
    template<class Function> bool consume (Function&& fn)
    {
        int a=0,b=0,c=0,d=0; fifo.prepareToRead(1,a,b,c,d);
        if (b==0) return false;
        fn(blocks[(size_t)a]); fifo.finishedRead(1); return true;
    }
    void publishPeak(float value) noexcept
    {
        auto previous=peak.load(std::memory_order_relaxed);
        while(value>previous && !peak.compare_exchange_weak(previous,value,std::memory_order_relaxed)){}
    }
    std::atomic<bool> enabled {false};
    std::atomic<bool> monitoring {false};
    std::atomic<uint64_t> callbacks {0};
    std::atomic<float> peak {0};
    std::atomic<uint64_t> dropped {0};
private:
    juce::AbstractFifo fifo {capacity};
    std::unique_ptr<Block[]> blocks;
    double rate=48000,anchor=0;
    uint64_t position=0,epoch=0;
};

struct Frame
{
    juce::Image image;
    double timeMs=0;
};
struct VisualState
{
    static constexpr int width=1280,height=720; // composition design coordinates
    std::atomic<int> outputWidth {1920};
    int frameWidth() const noexcept{return outputWidth.load()==1280?1280:1920;}
    int frameHeight() const noexcept{return frameWidth()*9/16;}
    juce::CriticalSection cameraLock;
    juce::Image cameraImage;
    std::atomic<uint64_t> cameraSequence {0};
    // Only the message thread paints the DAW. The compositor takes an immutable
    // image reference under this short lock, never touching the live components.
    juce::CriticalSection dawLock;
    juce::Image dawImage;
    std::atomic<uint64_t> dawSequence {0};
    std::atomic<bool> enabled {true}, recording {false}, cameraOn {false};
    std::atomic<bool> performanceMode {false}; // Webcam + MIDI keyboard, no DAW snapshot.
    std::atomic<int> faceCorner {1}; // 0 left, 1 right
    std::atomic<float> faceSize {.18f};
    std::atomic<bool> mirror {true};
    juce::AbstractFifo queue {4};
    std::array<Frame,4> frames;
    std::atomic<uint64_t> droppedFrames {0};
    bool submit (const juce::Image& image,double time)
    {
        if (!recording.load()) return false;
        int a=0,b=0,c=0,d=0; queue.prepareToWrite(1,a,b,c,d);
        if (b==0) { droppedFrames.fetch_add(1); return false; }
        frames[(size_t)a]={image,time}; queue.finishedWrite(1); return true;
    }
    bool pop (Frame& frame)
    {
        int a=0,b=0,c=0,d=0; queue.prepareToRead(1,a,b,c,d);
        if (b==0) return false;
        frame=std::move(frames[(size_t)a]); queue.finishedRead(1); return true;
    }
};
}
