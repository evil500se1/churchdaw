#pragma once
#include "StudioRecorder.h"
#include "StudioRecordingPaths.h"
#include "LiveCamLookAndFeel.h"
#include "ChurchCamLayout.h"
#include "StudioCamera_ios.h"
#include "StudioCamera_android.h"
#include "StudioAndroidLifecycle.h"
#include "../Piano3DPanel.h"
#include <juce_video/juce_video.h>
#include <juce_audio_devices/juce_audio_devices.h>

namespace churchstudio
{
#if JUCE_IOS
using StudioCameraDevice=MobileCamera;
static constexpr bool mobileStudio=true;
#elif JUCE_ANDROID
using StudioCameraDevice=AndroidCamera;
static constexpr bool mobileStudio=true;
#else
using StudioCameraDevice=juce::CameraDevice;
static constexpr bool mobileStudio=CHURCHDAW_MOBILE_SHELL;
#endif
class Panel final : public juce::Component, private juce::Timer,
                    private StudioCameraDevice::Listener,private juce::AudioIODeviceCallback
{
public:
    Panel(std::shared_ptr<PianoVisualState> notes,std::shared_ptr<AudioTap> audio={},juce::AudioDeviceManager* engine=nullptr,juce::Component* source=nullptr,juce::PropertiesFile* preferences=nullptr,
          std::shared_ptr<AudioTap> inputTap={},std::function<juce::Result(bool,int)> inputControl={})
        : prefs(preferences),visual(std::make_shared<VisualState>()),daw(std::move(audio)),mic(inputTap?std::move(inputTap):std::make_shared<AudioTap>()),piano(std::move(notes),visual),engineDeviceManager(engine),dawSource(source),sharedMicControl(std::move(inputControl))
    {
        setOpaque(true);setLookAndFeel(&look);
       #if JUCE_ANDROID
        // JUCE's default extension table lacks MP4. Register it before sharing
        // so Android offers video destinations instead of a generic file menu.
        static const bool mp4MimeRegistered=[] {
            juce::FileChooser::registerCustomMimeTypeForFileExtension("video/mp4","mp4");
            return true;
        }();
        juce::ignoreUnused(mp4MimeRegistered);
       #endif
        brand=juce::ImageCache::getFromMemory(BinaryData::logo_round_png,BinaryData::logo_round_pngSize);
        for(juce::Component* c : std::initializer_list<juce::Component*>{&piano,&cameraList,&cameraToggle,&refresh,&corner,&mirror,
            &faceSize,&micList,&micToggle,&micChannel,&dawLevel,&micLevel,&showDaw,&record,&status,&quality,&openLast,&dawMeterView,&micMeterView,&dawStatus,&dawMode,&performanceMode})addAndMakeVisible(c);
        dawMode.setComponentID("liveCamDawMode");performanceMode.setComponentID("liveCamPerformanceMode");
        dawMode.onClick=[this]{selectComposition(false);};performanceMode.onClick=[this]{selectComposition(true);};
        performanceMode.setTooltip(juce::String::fromUTF8("Webcam em destaque, acordes e teclas 3D ao vivo com sua marca ChurchDAW."));
        cameraToggle.getProperties().set("liveCamIcon","camera");
        showDaw.getProperties().set("liveCamIcon","back");
        openLast.getProperties().set("liveCamIcon","play");
        record.getProperties().set("liveCamIcon","record");record.setComponentID("liveCamRecord");
        openLast.onClick=[this]
        {
            if(!lastVideo.existsAsFile())return;
           #if JUCE_IOS || JUCE_ANDROID
            shareDialog=juce::ContentSharer::shareFilesScoped({juce::URL(lastVideo)},
                [safe=juce::Component::SafePointer<Panel>(this)](bool ok,const juce::String& error)
                {
                    // Release the native wrapper after its completion callback.
                    // Keeping a completed scoped share sheet retains an invisible
                    // child over the iPad panel and swallows the next touches.
                    juce::MessageManager::callAsync([safe,ok,error]
                    {
                        if(!safe)return;
                        safe->shareDialog.close();
                        if(!ok && error.isNotEmpty())safe->status.setText(error,juce::dontSendNotification);
                    });
                },this);
           #else
            lastVideo.startAsProcess();
           #endif
        };
        if(prefs)
        {
            if(mobileStudio)
            {
                // iOS can change the sandbox UUID on upgrade. Resolve only
                // the filename inside our recording folder, never an old root.
                lastVideo=resolveMobileRecording(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                    prefs->getValue("liveCam.lastVideoName"),prefs->getValue("liveCam.lastVideo"));
            }
            else lastVideo=juce::File(prefs->getValue("liveCam.lastVideo"));
        }
        openLast.setEnabled(lastVideo.existsAsFile());
        quality.addItem(juce::String::fromUTF8("Full HD · 1080p"),1);quality.addItem(juce::String::fromUTF8("HD · 720p"),2);
        const int defaultQuality=mobileStudio?2:1;
        quality.setSelectedId(prefs?juce::jlimit(1,2,prefs->getIntValue("liveCam.quality",defaultQuality)):defaultQuality,juce::dontSendNotification);
        visual->outputWidth.store(quality.getSelectedId()==1?1920:1280);
        quality.onChange=[this]{if(!busy()){visual->outputWidth.store(quality.getSelectedId()==1?1920:1280);savePreference("quality",quality.getSelectedId());piano.refreshStudio();}};
        quality.setTitle(juce::String::fromUTF8("Qualidade do vídeo"));
        dawStatus.setFont(juce::FontOptions(11));dawStatus.setColour(juce::Label::textColourId,palette::muted());
        cameraToggle.onClick=[this]{toggleCamera();};micToggle.onClick=[this]{toggleMic();};
        refresh.onClick=[this]{if(!busy() && !cameraPending && !micPending)refreshDevices();};
        corner.addItem("Rosto a direita",1);corner.addItem("Rosto a esquerda",2);corner.setSelectedId(prefs?juce::jlimit(1,2,prefs->getIntValue("liveCam.corner",1)):1,juce::dontSendNotification);
        visual->faceCorner.store(corner.getSelectedId()==1?1:0);
        corner.onChange=[this]{visual->faceCorner.store(corner.getSelectedId()==1?1:0);savePreference("corner",corner.getSelectedId());piano.refreshStudio();};
        mirror.setToggleState(prefs?prefs->getBoolValue("liveCam.mirror",true):true,juce::dontSendNotification);
        visual->mirror.store(mirror.getToggleState());
        mirror.onClick=[this]{visual->mirror.store(mirror.getToggleState());savePreference("mirror",mirror.getToggleState());piano.refreshStudio();};
        setupSlider(faceSize,18,32,prefs?prefs->getDoubleValue("liveCam.faceSize",18):18,"%");faceSize.onValueChange=[this]{visual->faceSize.store((float)faceSize.getValue()*.01f);savePreference("faceSize",faceSize.getValue());piano.refreshStudio();};
        visual->faceSize.store((float)faceSize.getValue()*.01f);
        setupSlider(dawLevel,0,100,prefs?prefs->getDoubleValue("liveCam.dawGain",85):85,"%");
        setupSlider(micLevel,0,100,prefs?prefs->getDoubleValue("liveCam.micGain",75):75,"%");
        dawLevel.onValueChange=[this]{savePreference("dawGain",dawLevel.getValue());};
        micLevel.onValueChange=[this]{savePreference("micGain",micLevel.getValue());};
        faceSize.setTitle("Tamanho da webcam");dawLevel.setTitle(juce::String::fromUTF8("Volume da DAW no vídeo"));micLevel.setTitle(juce::String::fromUTF8("Volume do microfone no vídeo"));
        dawLevel.setTooltip(juce::String::fromUTF8("Ajusta somente o áudio do vídeo. O volume das caixas não muda."));
        micLevel.setTooltip(juce::String::fromUTF8("Volume da voz no vídeo, sem retorno nas caixas."));
        dawLevel.setEnabled(daw!=nullptr);micLevel.setEnabled(false);
        showDaw.setEnabled(dawSource!=nullptr);
        showDaw.onClick=[this]{if(onShowMixer)onShowMixer();else if(dawSource)if(auto* window=dawSource->getTopLevelComponent())window->toFront(true);};
        if(daw)daw->monitoring.store(true);
        micChannel.addItem("Entrada 1",1);micChannel.addItem("Entrada 2",2);micChannel.setSelectedId(prefs?juce::jlimit(1,2,prefs->getIntValue("liveCam.micChannel",1)):1,juce::dontSendNotification);
        micChannel.onChange=[this]{savePreference("micChannel",micChannel.getSelectedId());};
        micChannel.setTooltip("Escolha a entrada fisica do microfone na interface.");
        record.onClick=[this]{if(recorder && !recorder->isFinished()){recorder->stop();record.setEnabled(false);record.setButtonText("Finalizando...");}else chooseRecording();};
        status.setColour(juce::Label::textColourId,palette::muted());status.setFont(juce::FontOptions(12.0f));
        status.setText(daw?juce::String::fromUTF8("Tudo pronto. Toque e grave sua sessão."):"Abra o Live Cam dentro do ChurchDAW para gravar.",juce::dontSendNotification);
        selectComposition(prefs && prefs->getBoolValue("liveCam.performanceMode",false));
        refreshDevices();setSize(1280,800);setTouchInterface(mobileStudio);startTimerHz(30);
        record.setEnabled(false);
        if(!connected())record.setButtonText("Abra na DAW");
        if(!MovieWriter::supported()){record.setEnabled(false);status.setText("A gravacao MP4 nao esta disponivel neste sistema. Confira os componentes de midia do sistema.",juce::dontSendNotification);}
    }
    ~Panel() override
    {
        if(recorder)recorder->stop();recorder.reset();
        stopTimer();stopCamera();stopMic();
        if(daw)daw->monitoring.store(false);
        if(prefs)prefs->saveIfNeeded();setLookAndFeel(nullptr);
    }
    std::function<void(bool)> onRecordingChanged;
    std::function<void()> onShowMixer;
    bool isRecordingOrFinalizing() const{return busy();}
    void setTouchInterface(bool enabled)
    {
        touchUI=enabled;look.setTouchControls(enabled);controlsViewport.setViewedComponent(nullptr,false);
        for(auto* button:{&showDaw,&openLast,&record,&dawMode,&performanceMode,&cameraToggle,&refresh,&micToggle})button->getProperties().set("churchCamTouch",enabled);
        for(auto* c:settingsControls())
            if(enabled)controlsContent.addAndMakeVisible(c);else addAndMakeVisible(c);
        controlsViewport.setScrollBarsShown(true,false);controlsViewport.setScrollBarThickness(5);
        controlsViewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
        for(auto* slider:{&faceSize,&dawLevel,&micLevel})slider->setViewportIgnoreDragFlag(true);
        if(enabled){addAndMakeVisible(controlsViewport);controlsViewport.setViewedComponent(&controlsContent,false);}
        else controlsViewport.setVisible(false);
        controlsContent.draw=[this](juce::Graphics& g){paintTouchSettings(g);};
        showDaw.setButtonText(enabled?"DAW":"Voltar para a DAW");
        openLast.setButtonText(enabled?"Compartilhar":juce::String::fromUTF8("Ver vídeo"));
        dawMode.setButtonText(enabled?"DAW + rosto":"DAW + webcam");
        performanceMode.setButtonText(enabled?juce::String::fromUTF8("Rosto + teclas"):"Webcam + teclado 3D");
        quality.changeItemText(1,enabled?juce::String("1080p"):juce::String::fromUTF8("Full HD · 1080p"));
        quality.changeItemText(2,enabled?juce::String("720p"):juce::String::fromUTF8("HD · 720p"));
        if(!busy())record.setButtonText(enabled?"Gravar":juce::String::fromUTF8("Gravar vídeo"));
        faceSize.setVisible(!visual->performanceMode.load());resized();repaint();
    }
    void setDocked(bool enabled){docked=enabled;resized();repaint();}
    void suspendCapture()
    {
        suspended=true;
       #if JUCE_IOS
        if(busy())backgroundTask=std::make_unique<CaptureBackgroundTask>();
       #endif
        if(busy())recorder->stop();
        stopCamera();stopMic();piano.suspendCapture();
    }
    void resumeCapture(){suspended=false;nextCapture=0;cameraOpenFinished();micToggle.setEnabled(!busy());micList.setEnabled(true);micChannel.setEnabled(true);piano.resumeCapture();}
    void requestClose(std::function<void()> completion)
    {
        if(!busy()){juce::MessageManager::callAsync(std::move(completion));return;}
        closeWhenFinished=std::move(completion);
        recorder->stop();record.setEnabled(false);record.setButtonText("Finalizando...");
    }
    void paint(juce::Graphics& g) override
    {
        if(touchUI){paintTouch(g);return;}
        g.fillAll(palette::background());
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff261820),0,0,palette::background(),440,180,false));
        g.fillRect(0,0,getWidth(),94);
        {juce::Graphics::ScopedSaveState save(g);juce::Path clip;clip.addEllipse(24,24,42,42);g.reduceClipRegion(clip);g.drawImageWithin(brand,24,24,42,42,juce::RectanglePlacement::fillDestination);}
        g.setColour(palette::text());g.setFont(juce::FontOptions(27,juce::Font::bold));g.drawText("Live Cam",80,17,260,35,juce::Justification::centredLeft);
        g.setColour(palette::muted());g.setFont(juce::FontOptions(12));g.drawText(juce::String::fromUTF8("CHURCHDAW  /  Sua sessão em vídeo"),82,54,360,19,juce::Justification::centredLeft);
        const bool recording=busy() && recorder->isReady() && !recorder->isStopping();
        auto badge=juce::Rectangle<float>((float)getWidth()-218,30,194,30);
        g.setColour(recording?juce::Colour(0xff42212f):palette::surface());g.fillRoundedRectangle(badge,15);
        g.setColour(recording?palette::accent():audioAlive?palette::green():palette::muted());g.fillEllipse(badge.getX()+14,badge.getY()+12,6,6);
        g.setFont(juce::FontOptions(10.5f,juce::Font::bold));g.drawText(recording?"GRAVANDO":connected()?juce::String::fromUTF8("CONECTADO À SUA DAW"):juce::String::fromUTF8("PRÉVIA"),badge.withTrimmedLeft(30),juce::Justification::centredLeft);
        const auto stage=stageBounds();drawCard(g,stage);
        g.setColour(palette::muted());g.setFont(juce::FontOptions(10.5f,juce::Font::bold));
        g.drawText(juce::String::fromUTF8("PRÉVIA DO VÍDEO"),stage.getX()+18,stage.getY()+10,180,24,juce::Justification::centredLeft);
        g.setColour(palette::text().withAlpha(.7f));g.setFont(juce::FontOptions(10.5f));
        g.drawText(visual->performanceMode.load()?juce::String::fromUTF8("Teclas ao vivo · 88 notas"):camera?"Mesa + webcam":"Mesa completa",stage.getRight()-168,stage.getY()+10,150,24,juce::Justification::centredRight);
        auto cameraCard=juce::Rectangle<int>(getWidth()-340,100,316,242);auto audioCard=juce::Rectangle<int>(getWidth()-340,354,316,getHeight()-464);
        drawCard(g,cameraCard);drawCard(g,audioCard);
        const int x=getWidth()-322;
        g.setColour(palette::text());g.setFont(juce::FontOptions(13,juce::Font::bold));
        g.drawText("Webcam",x,112,180,22,juce::Justification::centredLeft);g.drawText(juce::String::fromUTF8("Áudio do vídeo"),x,366,200,22,juce::Justification::centredLeft);
        g.setColour(palette::muted());g.setFont(juce::FontOptions(11));
        if(visual->performanceMode.load())
            g.drawText(juce::String::fromUTF8("Acordes e notas ao vivo com sustain"),x,282,280,30,juce::Justification::centredLeft);
        else g.drawText("Tamanho",x,282,78,30,juce::Justification::centredLeft);
        g.drawText("MICROFONE OPCIONAL",x,482,220,20,juce::Justification::centredLeft);
        g.drawText(juce::String::fromUTF8("Sua voz só no vídeo, sem retorno nas caixas."),x,646,280,20,juce::Justification::centredLeft);
        g.drawText(juce::String::fromUTF8("A mesa continua livre enquanto você grava."),210,getHeight()-143,juce::jmax(0,getWidth()-774),30,juce::Justification::centredLeft);
        g.setColour(palette::line());g.drawHorizontalLine(getHeight()-95,24.0f,(float)getWidth()-24);
        g.setFont(juce::FontOptions(21,juce::Font::bold));g.setColour(recording?palette::text():palette::muted());
        const int seconds=recorder?(int)recorder->elapsed():0;
        g.drawText(juce::String(seconds/60).paddedLeft('0',2)+":"+juce::String(seconds%60).paddedLeft('0',2),250,getHeight()-70,90,30,juce::Justification::centredLeft);
    }
    void resized() override
    {
        if(touchUI){resizeTouch();return;}
        auto stage=stageBounds();auto content=stage.withTrimmedTop(86).reduced(12,12);
        dawMode.setBounds(stage.getX()+16,stage.getY()+44,170,32);performanceMode.setBounds(stage.getX()+194,stage.getY()+44,220,32);
        const int w=juce::jmin(content.getWidth(),juce::roundToInt(content.getHeight()*16.0/9.0));const int h=juce::roundToInt(w*9.0/16.0);
        piano.setBounds(content.withSizeKeepingCentre(w,h));
        showDaw.setBounds(24,getHeight()-145,172,34);
        const int x=getWidth()-322,wc=280;
        cameraList.setBounds(x,146,wc,34);cameraToggle.setBounds(x,192,188,34);refresh.setBounds(x+198,192,82,34);
        corner.setBounds(x,239,140,32);mirror.setBounds(x+154,240,126,30);faceSize.setBounds(x+78,282,202,30);
        dawStatus.setBounds(x-4,397,wc+4,20);dawLevel.setBounds(x,423,wc,30);dawMeterView.setBounds(x,455,wc,18);
        micList.setBounds(x,507,wc,32);micChannel.setBounds(x,551,126,32);micToggle.setBounds(x+136,551,144,32);
        micLevel.setBounds(x,590,wc,28);micMeterView.setBounds(x,622,wc,16);
        quality.setBounds(stageBounds().getRight()-190,getHeight()-145,190,34);
        record.setBounds(24,getHeight()-77,210,48);status.setBounds(350,getHeight()-82,getWidth()-544,57);openLast.setBounds(getWidth()-176,getHeight()-72,152,38);
    }
private:
    std::array<juce::Component*,14> settingsControls()
    {return {{&cameraList,&cameraToggle,&refresh,&corner,&mirror,&faceSize,&micList,&micToggle,&micChannel,&dawLevel,&micLevel,&dawMeterView,&micMeterView,&dawStatus}};}
    void paintTouch(juce::Graphics& g)
    {
        g.fillAll(palette::background());
        if(docked)return;
        g.drawImageWithin(brand,14,10,40,40,juce::RectanglePlacement::centred);
        g.setColour(palette::text());g.setFont(juce::FontOptions(23,juce::Font::bold));
        g.drawText("ChurchCam",64,8,180,28,juce::Justification::centredLeft);
        g.setColour(palette::muted());g.setFont(juce::FontOptions(10));
        g.drawText(juce::String::fromUTF8("Seu som. Seu jeito de aparecer."),65,35,260,17,juce::Justification::centredLeft);
        auto layout=TouchLayout::make(getLocalBounds());drawCard(g,layout.stage);drawCard(g,layout.settings);
        const bool active=busy() && recorder->isReady() && !recorder->isStopping();
        g.setColour(active?palette::accent():palette::muted());g.setFont(juce::FontOptions(18,juce::Font::bold));
        const int seconds=recorder?(int)recorder->elapsed():0;
        if(layout.clock.getWidth()>55)g.drawText(juce::String(seconds/60).paddedLeft('0',2)+":"+juce::String(seconds%60).paddedLeft('0',2),layout.clock,juce::Justification::centred);
        g.setFont(juce::FontOptions(10,juce::Font::bold));
        if(getWidth()>=540)g.drawText(active?"GRAVANDO":audioAlive?"DAW CONECTADA":"CHURCHDAW",getWidth()-166,19,150,24,juce::Justification::centredRight);
    }
    void paintTouchSettings(juce::Graphics& g)
    {
        const int w=controlsContent.getWidth()-24;
        g.setColour(palette::text());g.setFont(juce::FontOptions(14,juce::Font::bold));
        g.drawText(juce::String::fromUTF8("Câmera"),12,10,w,26,juce::Justification::centredLeft);
        g.drawText(juce::String::fromUTF8("Áudio do vídeo"),12,340,w,26,juce::Justification::centredLeft);
        g.setColour(palette::muted());g.setFont(juce::FontOptions(11));
        g.drawText(visual->performanceMode.load()?juce::String::fromUTF8("Rosto, acordes e notas ao vivo."):"Tamanho do rosto",12,242,w,24,juce::Justification::centredLeft);
        g.drawText("MICROFONE OPCIONAL",12,480,w,24,juce::Justification::centredLeft);
        g.drawFittedText(juce::String::fromUTF8("Sua voz vai para o vídeo. Sem retorno nas caixas."),12,702,w,40,juce::Justification::centredLeft,2);
        g.setColour(palette::line());g.drawHorizontalLine(326,12.0f,(float)w+12);
    }
    void resizeTouch()
    {
        controlsViewport.setVisible(!docked);dawMode.setVisible(!docked);performanceMode.setVisible(!docked);
        quality.setVisible(!docked);openLast.setVisible(!docked);status.setVisible(!docked);
        if(docked)
        {
            auto area=getLocalBounds().reduced(6);auto actions=area.removeFromBottom(44);area.removeFromBottom(6);
            piano.setBounds(area);showDaw.setButtonText("Expandir");showDaw.setBounds(actions.removeFromLeft(actions.getWidth()/2-3));actions.removeFromLeft(6);record.setBounds(actions);return;
        }
        showDaw.setButtonText("DAW");auto layout=TouchLayout::make(getLocalBounds());
        piano.setBounds(layout.preview);dawMode.setBounds(layout.dawMode);performanceMode.setBounds(layout.performanceMode);
        showDaw.setBounds(layout.back);quality.setBounds(layout.quality);record.setBounds(layout.record);openLast.setBounds(layout.last);status.setBounds(layout.status);
        controlsViewport.setBounds(layout.settings.reduced(2));
        controlsContent.setSize(juce::jmax(1,controlsViewport.getWidth()-7),754);
        const int x=12,w=controlsContent.getWidth()-24;
        cameraList.setBounds(x,44,w,44);cameraToggle.setBounds(x,98,w-82,44);refresh.setBounds(x+w-76,98,76,44);
        mirror.setBounds(x,148,w,44);corner.setBounds(x,196,w,44);faceSize.setBounds(x,268,w,44);
        dawStatus.setBounds(x-4,371,w+4,24);dawLevel.setBounds(x,400,w,44);dawMeterView.setBounds(x,450,w,18);
        micList.setBounds(x,514,w,44);micChannel.setBounds(x,568,(w-8)/2,44);micToggle.setBounds(x+(w-8)/2+8,568,(w-8)/2,44);
        micLevel.setBounds(x,622,w,44);micMeterView.setBounds(x,674,w,18);
    }
    juce::Rectangle<int> stageBounds() const{return {24,100,juce::jmax(360,getWidth()-384),getHeight()-264};}
    static void drawCard(juce::Graphics& g,juce::Rectangle<int> r)
    {g.setColour(palette::surface());g.fillRoundedRectangle(r.toFloat(),14);g.setColour(palette::line().withAlpha(.6f));g.drawRoundedRectangle(r.toFloat().reduced(.5f),14,1);}
    void selectComposition(bool performance)
    {
        if(busy())return;
        visual->performanceMode.store(performance);savePreference("performanceMode",performance);
        dawMode.setToggleState(!performance,juce::dontSendNotification);performanceMode.setToggleState(performance,juce::dontSendNotification);
        corner.setEnabled(!performance);faceSize.setVisible(!performance);
        nextCapture=0;piano.refreshStudio();repaint();
        status.setText(!connected()?juce::String("Abra o Live Cam dentro do ChurchDAW para gravar."):
            performance && !camera?juce::String::fromUTF8("Ligue a webcam e toque: acordes e teclas acompanham seu MIDI."):
            performance?juce::String::fromUTF8("Toque e grave. Acordes, notas e sustain aparecem no vídeo."):
            juce::String::fromUTF8("Mesa completa pronta. A webcam é opcional neste modo."),juce::dontSendNotification);
    }
    void savePreference(const juce::String& name,const juce::var& value){if(prefs)prefs->setValue("liveCam."+name,value);}
    static void setupSlider(juce::Slider& slider,double a,double b,double initial,const juce::String& suffix)
    {
        slider.setRange(a,b,1);slider.setValue(initial);slider.setTextValueSuffix(suffix);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);slider.setTextBoxStyle(juce::Slider::TextBoxRight,false,55,24);
        slider.setColour(juce::Slider::thumbColourId,palette::text());
    }
    bool connected() const{return daw!=nullptr && dawSource!=nullptr;}
    bool busy() const{return recorder && !recorder->isFinished();}
    void refreshDevices()
    {
        const auto previousCamera=cameraList.getText().isNotEmpty()?cameraList.getText():prefs?prefs->getValue("liveCam.camera"):juce::String();
        cameraList.clear(juce::dontSendNotification);int i=1,selectedCamera=1;
        for(auto name:StudioCameraDevice::getAvailableDevices()){if(name==previousCamera)selectedCamera=i;cameraList.addItem(name,i++);}
        if(i>1)cameraList.setSelectedId(selectedCamera,juce::dontSendNotification);else cameraList.setTextWhenNothingSelected("Conecte uma webcam");
        auto previousMic=micList.getText().isNotEmpty()?micList.getText():prefs?prefs->getValue("liveCam.microphone"):juce::String();
        micList.clear(juce::dontSendNotification);micDevices.clear();i=1;int selectedMic=1;
        if(sharedMicControl)
        {
            const auto route=engineDeviceManager?engineDeviceManager->getAudioDeviceSetup().outputDeviceName:juce::String();
            micDevices.push_back({{},route});micList.addItem(route.isEmpty()?"Entrada do iPhone / iPad":route,i++);
        }
        else for(auto* type:micManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();for(auto name:type->getDeviceNames(true))
            {if(name==previousMic)selectedMic=i;micDevices.push_back({type->getTypeName(),name});micList.addItem(name,i++);}
        }
        if(i>1)micList.setSelectedId(selectedMic,juce::dontSendNotification);else micList.setTextWhenNothingSelected("Conecte um microfone");
    }
    void cameraOpenFinished()
    {
        cameraPending=false;cameraToggle.setEnabled(!suspended);cameraList.setEnabled(!camera && !suspended);
        cameraToggle.setButtonText(camera?"Desligar webcam":"Ligar webcam");
        cameraToggle.setToggleState(camera!=nullptr,juce::dontSendNotification);
        refresh.setEnabled(!camera && !busy() && !micPending && !suspended);
    }
    void toggleCamera()
    {
        if(cameraPending || suspended)return;
        if(camera){stopCamera();return;}
        if(cameraList.getSelectedId()==0){status.setText("Conecte uma webcam e clique Atualizar.",juce::dontSendNotification);return;}
        const auto name=cameraList.getText();cameraPending=true;
        const auto generation=++cameraGeneration;
        cameraToggle.setEnabled(false);cameraList.setEnabled(false);refresh.setEnabled(false);cameraToggle.setButtonText("Aguardando...");
        status.setText(juce::String::fromUTF8("Permita o acesso à câmera no aviso do sistema."),juce::dontSendNotification);
        requestMediaPermission(true,[safe=juce::Component::SafePointer<Panel>(this),name,generation](bool allowed)
        {
            if(!safe || generation!=safe->cameraGeneration)return;
            if(!allowed){safe->cameraOpenFinished();safe->status.setText(juce::String::fromUTF8("Câmera bloqueada. Libere o ChurchDAW nos ajustes de privacidade."),juce::dontSendNotification);return;}
            const int index=StudioCameraDevice::getAvailableDevices().indexOf(name);
            if(index<0){safe->cameraOpenFinished();safe->status.setText(juce::String::fromUTF8("A câmera foi desconectada. Atualize a lista."),juce::dontSendNotification);return;}
            StudioCameraDevice::openDeviceAsync(index,[safe,name,generation](StudioCameraDevice* opened,const juce::String& error)
            {
                std::unique_ptr<StudioCameraDevice> result(opened);
                if(!safe || generation!=safe->cameraGeneration || safe->suspended)return;
                safe->camera=std::move(result);safe->cameraOpenFinished();
                if(!safe->camera){safe->status.setText(error.isNotEmpty()?error:juce::String::fromUTF8("Não foi possível abrir a câmera."),juce::dontSendNotification);return;}
               #if JUCE_ANDROID
                safe->camera->setErrorCallback([safe,generation](const juce::String& message)
                {
                    if(!safe || generation!=safe->cameraGeneration)return;
                    if(safe->busy() && safe->visual->performanceMode.load())safe->recorder->stop();
                    safe->stopCamera();safe->status.setText(message,juce::dontSendNotification);
                });
               #endif
                safe->camera->addListener(safe.getComponent());safe->visual->cameraOn.store(true);safe->savePreference("camera",name);
                safe->lastCameraTime=juce::Time::getMillisecondCounterHiRes();
                safe->status.setText(juce::String::fromUTF8("Câmera ligada. Ajuste o enquadramento na prévia."),juce::dontSendNotification);safe->repaint();
            },320,240,1280,720,false);
        });
    }
    void stopCamera()
    {
        ++cameraGeneration;cameraPending=false;
        visual->cameraOn.store(false);
        if(camera){camera->removeListener(this);camera.reset();}
        visual->cameraSequence.store(0);
        {const juce::ScopedLock lock(visual->cameraLock);visual->cameraImage={};}
        cameraToggle.setButtonText("Ligar webcam");cameraToggle.setToggleState(false,juce::dontSendNotification);cameraToggle.setEnabled(!suspended);
        cameraList.setEnabled(!suspended);refresh.setEnabled(!busy() && !cameraPending && !micPending);piano.refreshStudio();repaint();
    }
    void imageReceived(const juce::Image& image) override
    {
        {const juce::ScopedLock lock(visual->cameraLock);visual->cameraImage=image;}
        visual->cameraSequence.fetch_add(1);
    }
    void toggleMic()
    {
        if(busy() || micPending)return;
        if(suspended)return;
        if(micOn){status.setText(juce::String::fromUTF8("Microfone desligado."),juce::dontSendNotification);stopMic();return;}
        const int index=micList.getSelectedId()-1;
        if(!juce::isPositiveAndBelow(index,(int)micDevices.size())){status.setText("Escolha um microfone disponivel.",juce::dontSendNotification);return;}
        const auto selection=micDevices[(size_t)index];
        const int selectedChannel=micChannel.getSelectedId()-1;
        const auto generation=++micGeneration;
        micPending=true;micToggle.setButtonText("Aguardando...");
        status.setText("Permita o microfone no aviso do sistema.",juce::dontSendNotification);
        micToggle.setEnabled(false);micList.setEnabled(false);micChannel.setEnabled(false);refresh.setEnabled(false);
        requestMediaPermission(false,[safe=juce::Component::SafePointer<Panel>(this),selection,selectedChannel,generation](bool allowed)
        {
            if(!safe || safe->suspended || generation!=safe->micGeneration)return;safe->micPending=false;safe->micToggle.setButtonText("Ligar mic");safe->micToggle.setEnabled(!safe->busy());safe->micList.setEnabled(true);safe->micChannel.setEnabled(true);safe->refresh.setEnabled(safe->camera==nullptr && !safe->busy() && !safe->cameraPending);
            if(!allowed){safe->status.setText("Permita o microfone nos Ajustes do Sistema para gravar sua voz.",juce::dontSendNotification);return;}
            if(safe->busy())return;
            auto selected=selection;
            if(safe->sharedMicControl)
            {
                auto result=safe->sharedMicControl(true,selectedChannel);
                if(result.failed()){safe->status.setText(result.getErrorMessage(),juce::dontSendNotification);return;}
                safe->micOn=true;safe->mic->enabled.store(true);
                safe->micToggle.setButtonText("Desligar mic");safe->micToggle.setToggleState(true,juce::dontSendNotification);
                safe->micList.setEnabled(false);safe->micChannel.setEnabled(false);safe->micLevel.setEnabled(true);
                safe->status.setText(juce::String::fromUTF8("Microfone ligado. Sua voz entra no vídeo."),juce::dontSendNotification);return;
            }
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.inputDeviceName=selected.second;setup.outputDeviceName={};setup.useDefaultInputChannels=false;
            setup.inputChannels.setBit(selectedChannel);setup.useDefaultOutputChannels=false;setup.outputChannels.clear();
            // Opening a second CoreAudio client must not change the live engine's
            // hardware rate or buffer when both clients share an interface.
            if(safe->engineDeviceManager)
            {
                auto live=safe->engineDeviceManager->getAudioDeviceSetup();
                if(live.outputDeviceName==selected.second || live.inputDeviceName==selected.second)
                {
                    setup.sampleRate=live.sampleRate;setup.bufferSize=live.bufferSize;
                    if(auto* device=safe->engineDeviceManager->getCurrentAudioDevice())
                        if(live.inputDeviceName==selected.second && device->getActiveInputChannels()[selectedChannel])
                        {safe->status.setText("Essa entrada ja esta no audio da DAW. Escolha outra para a voz separada.",juce::dontSendNotification);return;}
                }
            }
            // Initialise with an explicit input-only setup. Calling setCurrentAudioDeviceType
            // first would briefly open two default output channels on a fresh manager.
            juce::XmlElement config("DEVICESETUP");
            config.setAttribute("deviceType",selected.first);
            config.setAttribute("audioInputDeviceName",selected.second);
            config.setAttribute("audioOutputDeviceName",juce::String());
            config.setAttribute("audioDeviceInChans",setup.inputChannels.toString(2));
            config.setAttribute("audioDeviceOutChans","0");
            config.setAttribute("audioDeviceRate",setup.sampleRate);
            config.setAttribute("audioDeviceBufferSize",setup.bufferSize);
            auto error=safe->micManager.initialise(1,0,&config,false,selected.second,&setup);
            if(error.isNotEmpty()){safe->status.setText(error,juce::dontSendNotification);return;}
            safe->micManager.addAudioCallback(safe.getComponent());safe->micOn=true;safe->mic->enabled.store(true);
            safe->micToggle.setButtonText("Desligar mic");safe->micToggle.setToggleState(true,juce::dontSendNotification);safe->savePreference("microphone",selected.second);safe->micList.setEnabled(false);safe->micChannel.setEnabled(false);safe->micLevel.setEnabled(true);
            safe->status.setText(juce::String::fromUTF8("Microfone ligado. Sua voz entra no vídeo."),juce::dontSendNotification);
        });
    }
    void stopMic()
    {
        ++micGeneration;micPending=false;mic->enabled.store(false);
        if(sharedMicControl)
        {
            if(micOn)
            {
                const auto result=sharedMicControl(false,0);
                if(result.failed())status.setText(result.getErrorMessage(),juce::dontSendNotification);
            }
        }
        else {micManager.removeAudioCallback(this);micManager.closeAudioDevice();}
        micOn=false;micToggle.setButtonText("Ligar mic");micToggle.setToggleState(false,juce::dontSendNotification);
        micToggle.setEnabled(!suspended && !busy());micList.setEnabled(!suspended);micChannel.setEnabled(!suspended);micLevel.setEnabled(false);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override{mic->prepare(device->getCurrentSampleRate());}
    void audioDeviceStopped() override{}
    void audioDeviceIOCallbackWithContext(const float* const* input,int ins,float* const* output,int outs,int samples,const juce::AudioIODeviceCallbackContext&) override
    {
        for(int i=0;i<outs;++i)if(output[i])juce::FloatVectorOperations::clear(output[i],samples);
        mic->push(input,ins,samples);
    }
    void chooseRecording()
    {
        if(!connected() || !audioAlive)
        {status.setText("Abra o Studio na DAW com o dispositivo de audio ativo.",juce::dontSendNotification);return;}
        if(visual->performanceMode.load() && (!camera || visual->cameraSequence.load()==0))
        {status.setText(juce::String::fromUTF8("Ligue a webcam antes de gravar neste modo."),juce::dontSendNotification);return;}
        if(dawLevel.getValue()==0 && (!micOn || micLevel.getValue()==0))
        {status.setText("Aumente o volume da DAW ou ligue o microfone antes de gravar.",juce::dontSendNotification);return;}
        if(cameraPending || micPending){status.setText(juce::String::fromUTF8("Conclua a permissão da câmera ou do microfone antes de gravar."),juce::dontSendNotification);return;}
        auto name="ChurchDAW-"+juce::Time::getCurrentTime().formatted("%Y-%m-%d-%H%M%S")+".mp4";
       #if JUCE_IOS || JUCE_ANDROID
        const auto folder=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("ChurchCam");
        const auto result=folder.createDirectory();
        if(result.failed()){status.setText(result.getErrorMessage(),juce::dontSendNotification);return;}
        startRecordingAt(folder.getNonexistentChildFile(name,{},false));
       #else
        auto folder=lastVideo.existsAsFile()?lastVideo.getParentDirectory():juce::File::getSpecialLocation(juce::File::userMoviesDirectory);
        chooser=std::make_unique<juce::FileChooser>("Salvar video ChurchDAW",folder.getChildFile(name),"*.mp4");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,
            [safe=juce::Component::SafePointer<Panel>(this)](const juce::FileChooser& choice)
            {if(safe && choice.getResult()!=juce::File())safe->startRecordingAt(choice.getResult().withFileExtension("mp4"));});
       #endif
    }
    void startRecordingAt(const juce::File& destination)
    {
        if(suspended || busy())return;
        if(!connected() || !audioAlive){status.setText("O audio da DAW foi desconectado. Verifique Audio / MIDI.",juce::dontSendNotification);return;}
        recorder=std::make_unique<Recorder>(visual,daw,mic);
        if(!recorder->start(destination,daw!=nullptr && dawLevel.getValue()>0,micOn,
                            (float)dawLevel.getValue()*.01f,(float)micLevel.getValue()*.01f))
        {status.setText(recorder->getError(),juce::dontSendNotification);recorder.reset();return;}
        record.setButtonText(touchUI?"Salvar":"Parar e salvar");record.getProperties().set("liveCamIcon","stop");quality.setEnabled(false);dawMode.setEnabled(false);performanceMode.setEnabled(false);cameraToggle.setEnabled(!visual->performanceMode.load());openLast.setEnabled(false);dawLevel.setEnabled(false);micLevel.setEnabled(false);micToggle.setEnabled(false);refresh.setEnabled(false);
        piano.refreshStudio();
    }
    void captureDaw(double now)
    {
        if(suspended || visual->performanceMode.load() || !dawSource || now<nextCapture || (!isShowing() && !busy()))return;
        const auto bounds=dawSource->getLocalBounds();
        if(bounds.isEmpty())return;
        // Paint only our own interface, on the message thread. Independent
        // desktop windows (including this Studio) cannot enter the capture.
        const auto layout=layoutComposition(visual->cameraOn.load(),visual->faceSize.load(),visual->faceCorner.load());
        const float pixelScale=(float)visual->frameWidth()/1280.0f;
        const float scale=juce::jmin(2.0f,layout.daw.getWidth()*pixelScale/bounds.getWidth(),layout.daw.getHeight()*pixelScale/bounds.getHeight());
        const auto begin=juce::Time::getMillisecondCounterHiRes();
        auto image=dawSource->createComponentSnapshot(bounds,true,scale,juce::SoftwareImageType());
        const auto cost=juce::Time::getMillisecondCounterHiRes()-begin;
        nextCapture=now+juce::jlimit(33.0,200.0,cost*5.0);
        {const juce::ScopedLock lock(visual->dawLock);visual->dawImage=std::move(image);}
        visual->dawSequence.fetch_add(1);
    }
    void timerCallback() override
    {
        const auto now=juce::Time::getMillisecondCounterHiRes();
        captureDaw(now);
        if(camera && !suspended)
        {
            const auto sequence=visual->cameraSequence.load();
            if(sequence!=lastCameraSequence){lastCameraSequence=sequence;lastCameraTime=now;}
            if(now-lastCameraTime>4000)
            {
                if(busy())recorder->stop();stopCamera();
                status.setText(juce::String::fromUTF8("Câmera interrompida. Ligue novamente para continuar."),juce::dontSendNotification);
            }
        }
        if(daw)
        {
            auto count=daw->callbacks.load();
            if(count!=lastCallbacks){lastCallbacks=count;lastAudioTime=now;}
            audioAlive=lastAudioTime>0 && now-lastAudioTime<1000;
            dawMeter=juce::jmax(dawMeter*.9f,audioAlive?daw->peak.exchange(0):0.0f);
        }
        if(!busy())record.setEnabled(!suspended && connected() && audioAlive && !cameraPending && !micPending && MovieWriter::supported());
        if(busy())
        {
            if(recorder->isStopping())status.setText("Finalizando MP4 com imagem e audio...",juce::dontSendNotification);
            else if(!recorder->isReady())status.setText("Preparando gravacao... aguarde o contador para comecar a tocar.",juce::dontSendNotification);
            else status.setText(juce::String(juce::String::fromUTF8("Gravando · "))+(dawLevel.getValue()>0?(micOn?"DAW + voz":juce::String::fromUTF8("Áudio da DAW")) : "Somente microfone"),juce::dontSendNotification);
        }
        else
        {
            if(recorder)
            {
                auto error=recorder->getError();
                if(error.isEmpty())
                {
                    lastVideo=recorder->getDestination();savePreference("lastVideo",lastVideo.getFullPathName());
                    if(mobileStudio)savePreference("lastVideoName",lastVideo.getFileName());
                    const auto warning=recorder->getWarning();status.setText(warning.isNotEmpty()?warning:juce::String::fromUTF8("Vídeo salvo · ")+lastVideo.getFileName(),juce::dontSendNotification);
                    status.setTooltip(lastVideo.getFullPathName());
                }
                else status.setText(error,juce::dontSendNotification);
                record.setButtonText(touchUI?juce::String("Gravar"):juce::String::fromUTF8("Gravar vídeo"));record.getProperties().set("liveCamIcon","record");quality.setEnabled(true);dawMode.setEnabled(true);performanceMode.setEnabled(true);cameraToggle.setEnabled(!suspended && !cameraPending);
                openLast.setEnabled(lastVideo.existsAsFile());dawLevel.setEnabled(daw!=nullptr);micLevel.setEnabled(micOn);
                micToggle.setEnabled(!micPending);refresh.setEnabled(camera==nullptr && !cameraPending && !micPending);recorder.reset();
               #if JUCE_IOS
                backgroundTask.reset();
               #endif
                if(closeWhenFinished)
                {
                    auto close=std::move(closeWhenFinished);
                    if(error.isEmpty()){juce::MessageManager::callAsync(std::move(close));return;}
                }
            }
            if(micOn){mic->enabled.store(true);while(mic->consume([](const AudioTap::Block&){})){};}
        }
        if(micOn && sharedMicControl && engineDeviceManager)
        {
            auto* device=engineDeviceManager->getCurrentAudioDevice();
            if(device==nullptr || !device->getActiveInputChannels()[micChannel.getSelectedId()-1])
            {
                if(busy())recorder->stop();
                stopMic();status.setText(juce::String::fromUTF8("O microfone foi desconectado. Confira a entrada antes de gravar novamente."),juce::dontSendNotification);
            }
        }
        const bool recording=busy() && recorder->isReady() && !recorder->isStopping();
        if(recording!=reportedRecording){reportedRecording=recording;if(onRecordingChanged)onRecordingChanged(recording);}
        dawMeterView.setLevel(dawMeter*(float)dawLevel.getValue()*.01f);
        micMeter=juce::jmax(micMeter*.9f,micOn?mic->peak.exchange(0):0.0f);micMeterView.setLevel(micMeter*(float)micLevel.getValue()*.01f);
        if(++statusTick>=6)
        {
            statusTick=0;dawStatus.setText(!connected()?"Abra dentro do ChurchDAW":!audioAlive?juce::String::fromUTF8("Dispositivo de áudio desconectado"):dawLevel.getValue()==0?juce::String::fromUTF8("DAW silenciada no vídeo"):dawMeter>.001f?juce::String::fromUTF8("Áudio da DAW · sinal recebido"):juce::String::fromUTF8("Áudio da DAW · pronto para tocar"),juce::dontSendNotification);
            if(touchUI)repaint();else {repaint(getWidth()-230,20,210,50);repaint(246,getHeight()-78,95,46);}
        }
    }
    LiveCamLookAndFeel look;
    TouchSettings controlsContent;
    juce::Viewport controlsViewport;
    bool touchUI=false,docked=false,suspended=false;
    uint64_t cameraGeneration=0,micGeneration=0,lastCameraSequence=0;
    double lastCameraTime=0;
   #if JUCE_IOS || JUCE_ANDROID
    juce::ScopedMessageBox shareDialog;
   #endif
   #if JUCE_IOS
    std::unique_ptr<CaptureBackgroundTask> backgroundTask;
   #endif
    juce::Image brand;
    juce::PropertiesFile* prefs=nullptr;
    juce::File lastVideo;
    LevelMeter dawMeterView,micMeterView;
    std::shared_ptr<VisualState> visual;
    std::shared_ptr<AudioTap> daw,mic;
    Piano3DView piano;
    juce::AudioDeviceManager* engineDeviceManager=nullptr;
    juce::Component::SafePointer<juce::Component> dawSource;
    std::function<juce::Result(bool,int)> sharedMicControl;
    std::function<void()> closeWhenFinished;
    uint64_t lastCallbacks=0;
    double nextCapture=0,lastAudioTime=0;
    float dawMeter=0,micMeter=0;
    bool cameraPending=false,micPending=false,reportedRecording=false;
    int statusTick=0;
    bool audioAlive=false;
    juce::ComboBox cameraList,corner,micList,micChannel,quality;
    juce::TextButton dawMode{"DAW + webcam"},performanceMode{"Webcam + teclado 3D"};
    juce::TextButton cameraToggle{"Ligar webcam"},micToggle{"Ligar mic"},refresh{"Atualizar"},showDaw{"Voltar para a DAW"},record{juce::String::fromUTF8("Gravar vídeo")},openLast{juce::String::fromUTF8("Ver vídeo")};
    juce::ToggleButton mirror{"Espelhar"};
    juce::Slider faceSize,dawLevel,micLevel;
    juce::Label status,dawStatus;
    std::unique_ptr<StudioCameraDevice> camera;
    juce::AudioDeviceManager micManager;
    std::vector<std::pair<juce::String,juce::String>> micDevices;
    bool micOn=false;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<Recorder> recorder;
};

class Window final : public juce::DocumentWindow,private juce::Timer
{
public:
    Window(std::shared_ptr<PianoVisualState> notes,std::shared_ptr<AudioTap> audio,
           juce::AudioDeviceManager* engine,juce::Component* source,juce::PropertiesFile* prefs,
           std::shared_ptr<AudioTap> inputTap={},std::function<juce::Result(bool,int)> inputControl={})
        : DocumentWindow(juce::String::fromUTF8("ChurchDAW · Live Cam"),juce::Colour(0xff101319),allButtons)
    {
        setUsingNativeTitleBar(!mobileStudio);
        setContentOwned(new Panel(std::move(notes),std::move(audio),engine,source,prefs,std::move(inputTap),std::move(inputControl)),true);
       #if JUCE_ANDROID
        androidLifecycle=std::make_unique<AndroidCaptureLifecycle>([safe=juce::Component::SafePointer<Window>(this)](bool stopped)
        {
            if(!safe)return;
            if(stopped)safe->suspendCapture();else safe->resumeCapture();
        });
       #endif
        if(mobileStudio)
        {
            setTitleBarHeight(0);setResizable(false,false);setDropShadowEnabled(false);
            updateMobileBounds();startTimerHz(2);
            panel()->onShowMixer=[this]
            {
                if(!docked && !panel()->isRecordingOrFinalizing()){closeButtonPressed();return;}
                docked=!docked;panel()->setDocked(docked);updateMobileBounds();
            };
        }
        else {setResizable(true,false);setResizeLimits(1060,780,2560,1600);centreWithSize(1280,800);}
        setVisible(true);
        if(auto* panel=dynamic_cast<Panel*>(getContentComponent()))panel->onRecordingChanged=[this](bool active){if(onRecordingChanged)onRecordingChanged(active);};
    }
    std::function<void(bool)> onRecordingChanged;
    std::function<void()> onClosed;
    void suspendCapture(){panel()->suspendCapture();}
    void resumeCapture(){updateMobileBounds();panel()->resumeCapture();}
    void showStudio(){if(mobileStudio){docked=false;panel()->setDocked(false);updateMobileBounds();}toFront(true);}
    ~Window() override
    {
       #if JUCE_ANDROID
        androidLifecycle.reset();
       #endif
        if(onClosed)onClosed();
    }
    void closeButtonPressed() override
    {
        auto safe=juce::Component::SafePointer<Window>(this);
        if(auto* panel=dynamic_cast<Panel*>(getContentComponent()))
            panel->requestClose([safe]{if(safe)delete safe.getComponent();});
    }
private:
   #if JUCE_ANDROID
    std::unique_ptr<AndroidCaptureLifecycle> androidLifecycle;
   #endif
    Panel* panel() const{return static_cast<Panel*>(getContentComponent());}
    bool docked=false;
    void timerCallback() override{updateMobileBounds();}
    void updateMobileBounds()
    {
        if(!mobileStudio)return;
        if(auto* display=juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto safe=display->safeAreaInsets.subtractedFrom(display->logicalBounds.toNearestInt());
            auto bounds=safe;
            if(docked)bounds={safe.getRight()-240,safe.getY()+8,232,192};
            if(getBounds()!=bounds)setBounds(bounds);
        }
    }
};
}
