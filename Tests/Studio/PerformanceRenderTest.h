#pragma once
#include "Studio/StudioPanel.h"
#include <iostream>

// Exercises the actual OpenGL/FBO/readback path, with deterministic camera pixels
// and MIDI. It never opens a physical camera or changes the user's MIDI devices.
class PerformanceRenderTest final : public juce::Component,private juce::Timer
{
public:
    explicit PerformanceRenderTest(juce::File folder,std::function<void(bool)> done)
        : output(std::move(folder)),completion(std::move(done)),notes(std::make_shared<PianoVisualState>()),visual(std::make_shared<churchstudio::VisualState>()),view(notes,visual)
    {
        output.createDirectory();visual->performanceMode.store(true);visual->cameraOn.store(true);visual->recording.store(true);
        juce::Image image(juce::Image::ARGB,1280,720,true,juce::SoftwareImageType());
        {juce::Graphics g(image);g.fillAll(juce::Colour(0xff224955));g.setColour(juce::Colour(0xffad6755));g.fillRect(0,0,250,720);
         g.setColour(juce::Colours::white);g.setFont(juce::FontOptions(28));g.drawText("RENDER TEST / SYNTHETIC CAMERA",0,220,1280,80,juce::Justification::centred);}
        {const juce::ScopedLock lock(visual->cameraLock);visual->cameraImage=image;}
        addAndMakeVisible(view);setSize(960,540);started=juce::Time::getMillisecondCounterHiRes();phaseStart=started;startTimerHz(30);
    }
    void resized() override{view.setBounds(getLocalBounds());}
private:
    void save(const juce::Image& image,const juce::String& name)
    {juce::FileOutputStream out(output.getChildFile(name+".png"));if(out.openedOk())juce::PNGImageFormat().writeImageToStream(image,out);}
    static int difference(const juce::Image& a,const juce::Image& b,juce::Rectangle<int> area)
    {
        int changed=0;
        for(int y=area.getY();y<area.getBottom();++y)for(int x=area.getX();x<area.getRight();++x)
        {auto ac=a.getPixelAt(x,y),bc=b.getPixelAt(x,y);if(std::abs(ac.getRed()-bc.getRed())+std::abs(ac.getGreen()-bc.getGreen())+std::abs(ac.getBlue()-bc.getBlue())>18)++changed;}
        return changed;
    }
    void finish(bool ok){stopTimer();visual->recording.store(false);std::cout<<"Performance GPU: "<<(ok?"PASS":"FAIL")<<"\n";completion(ok);}
    void timerCallback() override
    {
        const double now=juce::Time::getMillisecondCounterHiRes();
        if(now-started>18000){finish(false);return;}
        churchstudio::Frame frame;juce::Image newest;
        while(visual->pop(frame))newest=frame.image;
        if(!newest.isValid() || now-phaseStart<900)return;
        if(phase==0)
        {
            idle=newest;save(idle,"idle");
            for(int n:{48,60,64,67,72})notes->observe(juce::MidiMessage::noteOn(1,n,(juce::uint8)80));
        }
        else if(phase==1)
        {
            held=newest;save(held,"held");
            passed=passed && view.currentHarmony().name=="C";
            // A second channel retains C4 until it also releases it.
            notes->observe(juce::MidiMessage::noteOn(2,60,(juce::uint8)70));
            notes->observe(juce::MidiMessage::controllerEvent(1,64,127));
            for(int n:{48,60,64,67,72})notes->observe(juce::MidiMessage::noteOff(1,n));
        }
        else if(phase==2)
        {
            passed=passed && view.currentHarmony().name=="C";
            single=newest;save(single,"one-channel-held");notes->observe(juce::MidiMessage::noteOff(2,60));
        }
        else if(phase==3)
        {
            released=newest;save(released,"released");
            passed=passed && view.currentHarmony().name=="C";
            notes->observe(juce::MidiMessage::controllerEvent(1,64,0));
            visual->mirror.store(false);view.refreshStudio();
        }
        else if(phase==4)
        {
            save(newest,"unmirrored");
            passed=passed && view.currentHarmony().kind==ChordDetect::Kind::silence;
            const auto keyboard=(churchstudio::performanceKeyboardBounds().toFloat()*1.5f).toNearestInt();
            const int pressDiff=difference(idle,held,keyboard),singleDiff=difference(idle,single,keyboard),releaseDiff=difference(idle,released,keyboard);
            const auto camera=juce::Rectangle<int>(0,0,1920,600),logo=juce::Rectangle<int>(1575,36,309,87);
            const int cameraStable=difference(idle,held,camera),mirrorDiff=difference(released,newest,camera);
            // The translucent badge naturally changes with the camera behind it;
            // the bright wordmark must remain fixed and readable, never mirrored.
            int logoDiff=0,logoPixels=0;
            for(int y=logo.getY();y<logo.getBottom();++y)for(int x=logo.getX();x<logo.getRight();++x)
                if(released.getPixelAt(x,y).getGreen()>210)
                {++logoPixels;auto a=released.getPixelAt(x,y),b=newest.getPixelAt(x,y);if(std::abs(a.getRed()-b.getRed())+std::abs(a.getGreen()-b.getGreen())+std::abs(a.getBlue()-b.getBlue())>25)++logoDiff;}
            std::cout<<"pressed="<<pressDiff<<" single="<<singleDiff<<" released="<<releaseDiff<<" camera="<<cameraStable<<" mirror="<<mirrorDiff<<" watermark="<<logoDiff<<"\n";
            const auto harmonyArea=juce::Rectangle<int>(64,750,686,63),notesArea=juce::Rectangle<int>(813,757,830,44);
            const int chordPixels=difference(idle,held,harmonyArea),notePixels=difference(idle,held,notesArea);
            const int sustainStable=difference(held,released,harmonyArea),cleared=difference(released,newest,harmonyArea);
            std::cout<<"chord pixels="<<chordPixels<<" note pixels="<<notePixels<<" sustained="<<sustainStable<<" cleared="<<cleared<<"\n";
            passed=passed && chordPixels>500 && notePixels>300 && sustainStable==0 && cleared>500 && pressDiff>500 && singleDiff>50 && singleDiff<pressDiff && releaseDiff<50 && cameraStable==0 && mirrorDiff>10000 && logoPixels>400 && logoDiff<20;
            visual->outputWidth.store(1280);
            for(int n:{48,49,52,54,55,58})notes->observe(juce::MidiMessage::noteOn(1,n,(juce::uint8)80));
            view.refreshStudio();
        }
        else if(phase==5)
        {
            save(newest,"720p");passed=passed && newest.getWidth()==1280 && newest.getHeight()==720;
            passed=passed && view.currentHarmony().name=="C7(b9,#11)";
            int pink=0;for(int y=550;y<700;++y)for(int x=42;x<1238;++x)
            {auto c=newest.getPixelAt(x,y);if(c.getRed()>150 && c.getRed()>c.getGreen()*1.25f)++pink;}
            passed=passed && pink>500;std::cout<<"720p pressed pixels="<<pink<<"\n";
            notes->reset();visual->performanceMode.store(false);visual->cameraOn.store(false);
            juce::Image daw(juce::Image::ARGB,1280,720,true,juce::SoftwareImageType());
            {juce::Graphics g(daw);g.fillAll(juce::Colour(0xff095a72));}
            {const juce::ScopedLock lock(visual->dawLock);visual->dawImage=daw;}
            view.refreshStudio();
        }
        else if(phase==6)
        {
            save(newest,"daw-mode");
            passed=passed && newest.getPixelAt(640,400)==juce::Colour(0xff095a72);
            visual->performanceMode.store(true);visual->outputWidth.store(1920);
            for(int n:{48,60,64,67,72})notes->observe(juce::MidiMessage::noteOn(1,n,(juce::uint8)80));
            view.refreshStudio();
        }
        else if(phase==7)
        {
            passed=passed && view.currentHarmony().name=="C";
            save(newest,"camera-off-preview");finish(passed);return;
        }
        ++phase;phaseStart=now;
    }
    juce::File output;std::function<void(bool)> completion;
    std::shared_ptr<PianoVisualState> notes;std::shared_ptr<churchstudio::VisualState> visual;Piano3DView view;
    juce::Image idle,held,single,released;double started=0,phaseStart=0;int phase=0;bool passed=true;
};
