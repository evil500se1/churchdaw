#include "Studio/StudioPanel.h"
#include <iostream>
#include "PerformanceRenderTest.h"
#include "ChordRecognitionTests.h"
#include "MovieWriterContractTests.h"

class StudioPreview final : public juce::JUCEApplication,private juce::MidiInputCallback
{
public:
    const juce::String getApplicationName() override{return "ChurchDAW Studio Preview";}
    const juce::String getApplicationVersion() override{return "0.2";}
    void initialise(const juce::String& args) override
    {
        if(args.startsWith("--movie-writer-contract "))
        {
            const auto folder=juce::File(args.fromFirstOccurrenceOf("--movie-writer-contract ",false,false).trim().unquoted());
            setApplicationReturnValue(runMovieWriterContractTests(folder)==0?0:1);quit();return;
        }
        if(args.startsWith("--performance-test "))
        {
            testWindow=std::make_unique<juce::DocumentWindow>("Performance render test",juce::Colours::black,0);
            testWindow->setContentOwned(new PerformanceRenderTest(juce::File(args.fromFirstOccurrenceOf("--performance-test ",false,false).trim().unquoted()),[this](bool ok){setApplicationReturnValue(ok?0:1);quit();}),true);
            testWindow->centreWithSize(960,540);testWindow->setVisible(true);return;
        }
        if(args.startsWith("--record-test-fullhd ")){recordTest(juce::File(args.fromFirstOccurrenceOf("--record-test-fullhd ",false,false).trim().unquoted()),1920);quit();return;}
        if(args.startsWith("--record-test ")){recordTest(juce::File(args.fromFirstOccurrenceOf("--record-test ",false,false).trim().unquoted()));quit();return;}
        if(args.contains("--self-test")){runTests();quit();return;}
        notes=std::make_shared<PianoVisualState>();window=std::make_unique<Window>(notes);
        for(auto device:juce::MidiInput::getAvailableDevices())
            if(!device.name.containsIgnoreCase("DAW"))
                if(auto input=juce::MidiInput::openDevice(device.identifier,this)){input->start();inputs.push_back(std::move(input));}
    }
    void shutdown() override{testWindow.reset();inputs.clear();window.reset();notes.reset();}
    void systemRequestedQuit() override{quit();}
private:
    void recordTest(const juce::File& output,int videoWidth=1280)
    {
        auto visual=std::make_shared<churchstudio::VisualState>();visual->outputWidth.store(videoWidth);
        auto daw=std::make_shared<churchstudio::AudioTap>(),mic=std::make_shared<churchstudio::AudioTap>();
        daw->prepare(44100);mic->prepare(48000);
        churchstudio::Recorder recorder(visual,daw,mic);
        if(!recorder.start(output,true,true,.85f,.75f)){std::cerr<<recorder.getError()<<"\n";setApplicationReturnValue(1);return;}
        auto preparingDeadline=juce::Time::getMillisecondCounterHiRes()+30000;
        while(!recorder.isReady() && !recorder.isFinished() && juce::Time::getMillisecondCounterHiRes()<preparingDeadline)juce::Thread::sleep(5);
        if(!recorder.isReady()){recorder.stop();std::cerr<<"Recorder did not become ready\n";setApplicationReturnValue(1);return;}
        double start=juce::Time::getMillisecondCounterHiRes();
        uint64_t dp=0,mp=0;
        for(int frame=0;frame<90;++frame)
        {
            float d[1470],m[1600];
            for(int n=0;n<1470;++n)d[n]=.2f*std::sin((float)(2*juce::MathConstants<double>::pi*440*(double)(dp++)/44100));
            for(int n=0;n<1600;++n)m[n]=.15f*std::sin((float)(2*juce::MathConstants<double>::pi*660*(double)(mp++)/48000));
            for(int offset=0;offset<1470;offset+=64){const float* ds[]={d+offset,d+offset};daw->push(ds,2,juce::jmin(64,1470-offset));}
            for(int offset=0;offset<1600;offset+=256){const float* ms[]={m+offset,m+offset};mic->push(ms,2,juce::jmin(256,1600-offset));}
            juce::Image image(juce::Image::ARGB,videoWidth,videoWidth*9/16,true,juce::SoftwareImageType());
            {juce::Graphics g(image);g.addTransform(juce::AffineTransform::scale((float)videoWidth/1280.0f));g.fillAll(juce::Colour(0xff131923));g.setColour(juce::Colour(0xffc3b28e));g.setFont(juce::FontOptions(34.0f));
             g.drawText("CHURCHDAW / TESTE DE GRAVACAO",50,45,1180,80,juce::Justification::centredLeft);
             g.setColour(juce::Colour(0xffb83d5c));g.fillRoundedRectangle(50+frame*10.0f,260,180,180,20);
             g.setColour(juce::Colours::white);g.setFont(juce::FontOptions(23.0f));g.drawText("Audio sintetico: DAW 440 Hz + voz 660 Hz",50,560,1180,60,juce::Justification::centredLeft);}
            visual->submit(image,juce::Time::getMillisecondCounterHiRes());
            int delay=(int)(start+(frame+1)*1000.0/30-juce::Time::getMillisecondCounterHiRes());if(delay>0)juce::Thread::sleep(delay);
        }
        recorder.stop();double deadline=juce::Time::getMillisecondCounterHiRes()+60000;
        while(!recorder.isFinished() && juce::Time::getMillisecondCounterHiRes()<deadline)juce::Thread::sleep(10);
        auto error=recorder.getError();
        bool ok=recorder.isFinished() && error.isEmpty() && recorder.getDestination().existsAsFile() && recorder.framesWritten.load()>=80;
        std::cout<<"Studio recording: "<<(ok?"PASS":"FAIL")<<"; frames="<<recorder.framesWritten.load()<<"; "<<error<<"; "<<recorder.getDestination().getFullPathName()<<"\n";
        setApplicationReturnValue(ok?0:1);
    }
    void runTests()
    {
        int failures=runChordRecognitionTests();
       #if JUCE_WINDOWS || JUCE_MAC
        if(!churchstudio::MovieWriter::supported())
        {std::cerr<<"Native desktop movie writer is unavailable\n";++failures;}
       #endif
        auto check=[&](bool ok){if(!ok)++failures;};
       #if JUCE_WINDOWS || JUCE_MAC
        {
            // Regress the actual disabled Record button, without opening an
            // audio device, camera, commercial session or file chooser.
            juce::Component source;
            auto liveAudio=std::make_shared<churchstudio::AudioTap>();liveAudio->prepare(48000);
            churchstudio::Panel panel(std::make_shared<PianoVisualState>(),liveAudio,nullptr,&source);
            juce::Button* recordButton=nullptr;
            for(auto* child:panel.getChildren())
                if(child->getComponentID()=="liveCamRecord")recordButton=dynamic_cast<juce::Button*>(child);
            check(recordButton!=nullptr && !recordButton->isEnabled());
            float silence[64]{};const float* channels[]{silence,silence};
            for(int attempt=0;attempt<5 && recordButton && !recordButton->isEnabled();++attempt)
            {
                liveAudio->push(channels,2,64);
                juce::Thread::sleep(40);juce::Timer::callPendingTimersSynchronously();
            }
            check(recordButton!=nullptr && recordButton->isEnabled());
            juce::Thread::sleep(1100);juce::Timer::callPendingTimersSynchronously();
            check(recordButton!=nullptr && !recordButton->isEnabled());
        }
       #endif
        churchstudio::AudioTap tap;tap.prepare(44100);
        float left[4096],right[4096];for(int i=0;i<4096;++i){left[i]=.2f;right[i]=-.4f;}
        const float* data[]={left,right};tap.push(data,2,4096);
        check(!tap.consume([](const churchstudio::AudioTap::Block&){}));
        tap.monitoring.store(true);tap.push(data,2,4096);
        check(tap.peak.load()==.4f && tap.callbacks.load()==1);
        check(!tap.consume([](const churchstudio::AudioTap::Block&){}));
        tap.enabled.store(true);tap.push(data,2,4096);int count=0;uint64_t next=0;
        while(tap.consume([&](const churchstudio::AudioTap::Block& b){check(b.position==next);next+=b.count;count+=b.count;check(b.left[0]==.2f && b.right[0]==-.4f && b.rate==44100);})){}
        check(count==4096);
        for(int i=0;i<200;++i)tap.push(data,2,64);check(tap.dropped.load()>0);
        int total=0;while(tap.consume([&](const churchstudio::AudioTap::Block&){++total;})){}check(total==127);
        tap.prepare(48000);left[0]=std::numeric_limits<float>::quiet_NaN();tap.push(data,1,1);
        tap.consume([&](const churchstudio::AudioTap::Block& b){check(b.left[0]==0 && b.right[0]==0 && b.rate==48000);});
        for(float size:{.18f,.27f,.38f})for(int corner:{0,1})
        {
            auto layout=churchstudio::layoutComposition(true,size,corner);
            check(!layout.daw.intersects(layout.face));
            check(juce::Rectangle<float>(0,0,1280,720).contains(layout.daw));
            check(juce::Rectangle<float>(0,0,1280,720).contains(layout.face));
        }
        auto visual=std::make_shared<churchstudio::VisualState>();
        auto fakeTarget=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("must-not-record-"+juce::Uuid().toString()+".mp4");
        churchstudio::Recorder disconnected(visual,nullptr,nullptr);
        check(!disconnected.start(fakeTarget,true,false,1,0));
        check(!fakeTarget.exists());
        auto silentTap=std::make_shared<churchstudio::AudioTap>();
        churchstudio::Recorder silent(visual,silentTap,nullptr);
        check(!silent.start(fakeTarget,true,false,0,0));
        // A meter poll must not lose a short note occurring between UI frames.
        tap.enabled.store(false);tap.monitoring.store(true);tap.peak.store(0);
        left[0]=.8f;tap.push(data,1,1);left[0]=0;tap.push(data,1,1);
        check(tap.peak.exchange(0)==.8f);check(tap.peak.load()==0);
        check(visual->frameWidth()==1920 && visual->frameHeight()==1080);
        visual->outputWidth.store(1280);check(visual->frameHeight()==720);
        {
            churchstudio::Panel panel(std::make_shared<PianoVisualState>());
            bool qualityTextValid=false;juce::Button* performanceButton=nullptr;juce::Button* dawButton=nullptr;
            for(auto* child:panel.getChildren())
            {
                if(child->getComponentID()=="liveCamPerformanceMode")performanceButton=dynamic_cast<juce::Button*>(child);
                if(child->getComponentID()=="liveCamDawMode")dawButton=dynamic_cast<juce::Button*>(child);
            }
            check(performanceButton && dawButton);
            if(performanceButton)performanceButton->onClick();
            check(performanceButton && performanceButton->getToggleState() && !dawButton->getToggleState());
            if(dawButton)dawButton->onClick();
            check(dawButton && dawButton->getToggleState() && !performanceButton->getToggleState());
            for(auto* child:panel.getChildren())if(auto* combo=dynamic_cast<juce::ComboBox*>(child))
                if(combo->getTitle()==juce::String::fromUTF8("Qualidade do vídeo"))qualityTextValid=combo->getText()==juce::String::fromUTF8("Full HD · 1080p");
            check(qualityTextValid);
            for(auto size:{juce::Point<int>(1060,780),juce::Point<int>(1280,800),juce::Point<int>(1600,1000)})
            {
                panel.setSize(size.x,size.y);
                const auto children=panel.getChildren();
                for(int a=0;a<children.size();++a)
                {
                    auto bounds=children[a]->getBounds();check(!bounds.isEmpty() && panel.getLocalBounds().contains(bounds));
                    for(int b=a+1;b<children.size();++b)check(!bounds.intersects(children[b]->getBounds()));
                }
            }
        }
        {
            churchstudio::Panel panel(std::make_shared<PianoVisualState>());
            panel.setTouchInterface(true);
            // Full-screen content after notch/home indicator safe insets.
            for(auto size:{juce::Point<int>(568,320),juce::Point<int>(667,375),juce::Point<int>(726,369),
                           juce::Point<int>(844,369),juce::Point<int>(1024,748),juce::Point<int>(1194,790),
                           juce::Point<int>(1366,980),juce::Point<int>(390,760),juce::Point<int>(320,640),juce::Point<int>(360,640)})
            {
                panel.setSize(size.x,size.y);
                const auto l=churchstudio::TouchLayout::make(panel.getLocalBounds());
                check(!l.preview.isEmpty() && l.stage.contains(l.preview));
                check(!l.settings.intersects(l.stage));
                for(auto r:{l.back,l.quality,l.last,l.record,l.dawMode,l.performanceMode})
                    check(r.getHeight()>=44 && r.getWidth()>=44 && panel.getLocalBounds().contains(r));
                const auto children=panel.getChildren();
                for(int a=0;a<children.size();++a)
                {
                    if(!children[a]->isVisible())continue;
                    const auto bounds=children[a]->getBounds();
                    check(!bounds.isEmpty() && panel.getLocalBounds().contains(bounds));
                    for(int b=a+1;b<children.size();++b)
                        if(children[b]->isVisible())check(!bounds.intersects(children[b]->getBounds()));
                }
            }
            panel.setDocked(true);panel.setSize(232,192);
            for(auto* child:panel.getChildren())if(child->isVisible())
                check(!child->getBounds().isEmpty() && panel.getLocalBounds().contains(child->getBounds()));
            panel.setDocked(false);panel.setSize(1024,748);
            panel.suspendCapture();panel.resumeCapture();
        }
        {
            const auto documents=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("new-sandbox/Documents");
            const auto legacy=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("old-sandbox/Documents/ChurchCam/session.mp4");
            check(churchstudio::resolveMobileRecording(documents,{},legacy.getFullPathName())==documents.getChildFile("ChurchCam/session.mp4"));
            check(churchstudio::resolveMobileRecording(documents,"new.mp4",legacy.getFullPathName()).getFileName()=="new.mp4");
            check(churchstudio::resolveMobileRecording(documents,"../outside.mp4",{})==juce::File());
            check(churchstudio::resolveMobileRecording(documents,{}, {})==juce::File());
        }
        std::cout<<"Studio audio ring: "<<(failures==0?"PASS":"FAIL")<<" ("<<failures<<" failures)\n";
        setApplicationReturnValue(failures==0?0:1);
    }
    void handleIncomingMidiMessage(juce::MidiInput*,const juce::MidiMessage& message) override{notes->observe(message);}
    struct Window final:juce::DocumentWindow
    {
        explicit Window(std::shared_ptr<PianoVisualState> notes):DocumentWindow("ChurchDAW Studio Preview",juce::Colour(0xff101319),allButtons)
        {
            setUsingNativeTitleBar(true);setContentOwned(new churchstudio::Panel(std::move(notes)),true);
            setResizable(true,false);setResizeLimits(1100,740,2560,1600);centreWithSize(1180,780);setVisible(true);
        }
        void closeButtonPressed() override{juce::JUCEApplication::getInstance()->systemRequestedQuit();}
    };
    std::shared_ptr<PianoVisualState> notes;
    std::unique_ptr<Window> window;
    std::unique_ptr<juce::DocumentWindow> testWindow;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
};
START_JUCE_APPLICATION(StudioPreview)
