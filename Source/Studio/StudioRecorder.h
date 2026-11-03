#pragma once
#include "StudioCaptureState.h"
#include "StudioMovieWriter.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace churchstudio
{
class Recorder final : private juce::Thread
{
public:
    Recorder(std::shared_ptr<VisualState> v,std::shared_ptr<AudioTap> d,std::shared_ptr<AudioTap> m)
        : Thread("ChurchDAW Video Encoder"),visual(std::move(v)),daw(std::move(d)),mic(std::move(m)) {}
    ~Recorder() override { stop(); waitForThreadToExit(-1); }
    bool start(const juce::File& target,bool includeDaw,bool includeMic,float dg,float mg)
    {
        if(isThreadRunning() || !MovieWriter::supported()) return false;
        if((includeDaw && !daw) || (includeMic && !mic))
        {setError("A fonte de audio selecionada nao esta conectada.");return false;}
        if((!includeDaw || dg<=0) && (!includeMic || mg<=0))
        {setError("Selecione uma fonte de audio com volume antes de gravar.");return false;}
        videoWidth=visual->frameWidth();videoHeight=videoWidth*9/16;
        destination=target.getNonexistentSibling();
        working=destination.getParentDirectory().getChildFile(".churchdaw-recording-"+juce::Uuid().toString());
        auto result=working.createDirectory();
        if(result.failed()) { setError(result.getErrorMessage()); return false; }
        useDaw=includeDaw && daw!=nullptr;useMic=includeMic && mic!=nullptr;
        dawGain=dg;micGain=mg;
        stopRequested.store(false);finished.store(false);ready.store(false);framesWritten.store(0);stopMs.store(0);startMs.store(0);
        if(daw)daw->enabled.store(false);
        if(mic)mic->enabled.store(false);
        if(daw) while(daw->consume([](const AudioTap::Block&){})) {}
        if(mic) while(mic->consume([](const AudioTap::Block&){})) {}
        Frame discarded;while(visual->pop(discarded)){}
        startThread();return true;
    }
    void stop()
    {
        if(!stopRequested.exchange(true)) stopMs.store(juce::Time::getMillisecondCounterHiRes());
        visual->recording.store(false);
        if(daw) daw->enabled.store(false);
        if(mic) mic->enabled.store(false);
        notify();
    }
    bool isFinished() const {return finished.load();}
    bool isStopping() const {return stopRequested.load();}
    bool isReady() const {return ready.load();}
    bool running() const {return isThreadRunning();}
    juce::String getError() const {const juce::ScopedLock lock(errorLock);return error;}
    juce::String getWarning() const {const juce::ScopedLock lock(errorLock);return warning;}
    juce::File getDestination() const {return destination;}
    double elapsed() const {const auto start=startMs.load();return start>0?juce::jmax(0.0,((isStopping()?stopMs.load():juce::Time::getMillisecondCounterHiRes())-start)*.001):0;}
    std::atomic<int> framesWritten {0};
private:
    struct Track
    {
        std::unique_ptr<juce::AudioFormatWriter> writer;
        AudioFile output;
        double rate=0;
        uint64_t expected=0,epoch=0;
        float peak=0;
    };
    void setError(const juce::String& text) {const juce::ScopedLock lock(errorLock);if(error.isEmpty())error=text;}
    bool writeBlock(Track& track,const AudioTap::Block& block,const juce::File& file,float gain)
    {
        const double endTime=stopRequested.load()?stopMs.load():std::numeric_limits<double>::max();
        int skip=juce::jlimit(0,block.count,(int)std::ceil((startMs-block.timeMs)*block.rate*.001));
        int end=block.count;
        if(block.timeMs+1000.0*block.count/block.rate>endTime)
            end=juce::jlimit(0,block.count,(int)std::floor((endTime-block.timeMs)*block.rate*.001));
        if(end<=skip)return true;
        if(!track.writer)
        {
            auto stream=file.createOutputStream();
            if(!stream)return false;
            juce::WavAudioFormat format;
            track.writer.reset(format.createWriterFor(stream.release(),block.rate,2,24,{},0));
            if(!track.writer)return false;
            track.rate=block.rate;track.epoch=block.epoch;track.expected=block.position+(uint64_t)skip;
            track.output={file,juce::jmax(0.0,(block.timeMs-startMs)*.001+(double)skip/block.rate),gain,0};
        }
        if(track.epoch!=block.epoch || track.rate!=block.rate)
        {setError("O dispositivo de audio mudou durante a gravacao. Os arquivos parciais foram preservados.");return false;}
        if(track.expected!=block.position+(uint64_t)skip)
        {setError("Houve perda de blocos de audio. Os arquivos parciais foram preservados.");return false;}
        const float* samples[]={block.left.data()+skip,block.right.data()+skip};
        if(!track.writer->writeFromFloatArrays(samples,2,end-skip))return false;
        for(int i=skip;i<end;++i)track.peak=juce::jmax(track.peak,std::abs(block.left[(size_t)i]),std::abs(block.right[(size_t)i]));
        track.output.peak=track.peak;track.expected=block.position+(uint64_t)end;return true;
    }
    void run() override
    {
        auto movieFile=working.getChildFile("video.mov");MovieWriter writer;
        auto result=writer.open(movieFile,videoWidth,videoHeight);
        Track d,m;
        double lastVideo=0;
        // AVFoundation may take seconds on a cold start. Arm the real-time
        // sources only after it is ready, so small audio buffers cannot overflow
        // and the bounded video queue does not lose the beginning of the take.
        if(result.failed())setError(result.getErrorMessage());
        else if(stopRequested.load())setError("Gravacao cancelada antes de iniciar.");
        else
        {
            if(daw)while(daw->consume([](const AudioTap::Block&){})){}
            if(mic)while(mic->consume([](const AudioTap::Block&){})){}
            dawDrops=daw?daw->dropped.load():0;micDrops=mic?mic->dropped.load():0;
            startMs.store(juce::Time::getMillisecondCounterHiRes());
            if(daw)daw->enabled.store(useDaw);
            if(mic)mic->enabled.store(useMic);
            visual->recording.store(true);ready.store(true);
            // A close/stop request can race the short arming sequence.
            if(stopRequested.load())stop();
        }
        auto drain=[&]
        {
            if(useDaw)while(daw->consume([&](const AudioTap::Block& b){if(!writeBlock(d,b,working.getChildFile("daw.wav"),dawGain))setError("Nao foi possivel gravar o audio da DAW.");})){}
            if(useMic)while(mic->consume([&](const AudioTap::Block& b){if(!writeBlock(m,b,working.getChildFile("microfone.wav"),micGain))setError("Nao foi possivel gravar o microfone.");})){}
        };
        while(getError().isEmpty())
        {
            drain();Frame frame;
            if(visual->pop(frame))
            {
                double time=juce::jmax(0.0,(frame.timeMs-startMs)*.001);
                if(stopRequested.load() && frame.timeMs>stopMs.load())continue;
                if(framesWritten.load()==0)time=0;
                result=writer.append(frame.image,time);
                if(result.failed()){setError(result.getErrorMessage());break;}
                lastVideo=time;framesWritten.fetch_add(1);
            }
            else if(stopRequested.load())break;
            else wait(3);
        }
        if(!stopRequested.load())stop();
        drain();d.writer.reset();m.writer.reset();
        if((daw && daw->dropped.load()!=dawDrops && useDaw) || (mic && mic->dropped.load()!=micDrops && useMic))setError("Perda de audio durante a gravacao; arquivos parciais preservados.");
        if(useDaw && !d.output.file.existsAsFile())setError("Nenhum audio chegou da DAW. Abra o Studio dentro do ChurchDAW.");
        if(useMic && !m.output.file.existsAsFile())setError("Nenhum audio chegou do microfone.");
        if(framesWritten.load()<1)setError("Nenhum quadro foi recebido. Deixe a janela do Studio visivel durante a gravacao.");
        const double duration=juce::jmax(lastVideo+1.0/30.0,elapsed());
        if(framesWritten.load()>0)
        {
            result=writer.finish(duration);
            if(result.failed())setError(result.getErrorMessage());
        }
        if(getError().isEmpty())
        {
            std::vector<AudioFile> tracks;
            if(d.output.file.existsAsFile())tracks.push_back(d.output);
            if(m.output.file.existsAsFile())tracks.push_back(m.output);
            auto exportFile=working.getChildFile("final.mp4");
            result=MovieWriter::combine(movieFile,tracks,exportFile,duration);
            if(result.wasOk() && !exportFile.moveFileTo(destination))result=juce::Result::fail("Nao foi possivel mover o video para a pasta escolhida.");
            if(result.failed())setError(result.getErrorMessage());
            else working.deleteRecursively();
        }
        if(getError().isEmpty() && ((useDaw && d.peak<.0000001f) || (useMic && m.peak<.0000001f)))
        {
            const juce::ScopedLock lock(errorLock);
            warning=useDaw && d.peak<.0000001f?juce::String::fromUTF8("Vídeo salvo, mas não houve sinal da DAW. Confira o instrumento e o MIDI."):juce::String::fromUTF8("Vídeo salvo, mas o microfone não recebeu sinal. Confira a entrada selecionada.");
        }
        finished.store(true);
    }
    std::shared_ptr<VisualState> visual;
    std::shared_ptr<AudioTap> daw,mic;
    juce::File destination,working;
    mutable juce::CriticalSection errorLock;
    juce::String error,warning;
    int videoWidth=1280,videoHeight=720;
    std::atomic<bool> stopRequested {true},finished {false},ready {false};
    std::atomic<double> stopMs {0},startMs {0};
    bool useDaw=false,useMic=false;
    float dawGain=1,micGain=1;
    uint64_t dawDrops=0,micDrops=0;
};
}
