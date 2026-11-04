#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace churchstudio
{
// Logical points, shared by the iOS page and desktop layout probes.
// Settings scroll on small displays; primary actions never leave the screen.
struct TouchLayout
{
    juce::Rectangle<int> stage,settings,preview,dawMode,performanceMode;
    juce::Rectangle<int> back,quality,record,last,status,clock;
    static TouchLayout make(juce::Rectangle<int> safe)
    {
        TouchLayout l;const int margin=12;
       #if CHURCHDAW_MOBILE_SHELL
        const bool shortH=safe.getHeight()<=420;
        auto body=safe.reduced(margin,0);body.removeFromTop(shortH?50:60);
        auto footer=body.removeFromBottom(shortH?62:72);body.removeFromBottom(shortH?8:10);
       #else
        const bool shortH=false;
        auto body=safe.reduced(margin,0);body.removeFromTop(60);
        auto footer=body.removeFromBottom(72);body.removeFromBottom(10);
       #endif
        const bool portrait=safe.getWidth()<safe.getHeight() && safe.getWidth()<700;
        if(portrait)
        {
            l.stage=body.removeFromTop(juce::jmin(280,body.getHeight()/2));body.removeFromTop(10);l.settings=body;
        }
        else
        {
            const int settingsWidth=safe.getWidth()>=1000?300:safe.getWidth()>=700?272:232;
            l.settings=body.removeFromRight(settingsWidth);body.removeFromRight(12);l.stage=body;
        }
        auto stageContent=l.stage.reduced(shortH?8:10);
        auto modes=stageContent.removeFromTop(shortH?36:44);stageContent.removeFromTop(shortH?6:8);
        l.dawMode=modes.removeFromLeft((modes.getWidth()-6)/2);modes.removeFromLeft(6);l.performanceMode=modes;
        const int w=juce::jmin(stageContent.getWidth(),stageContent.getHeight()*16/9);
        l.preview=stageContent.withSizeKeepingCentre(w,w*9/16);
        auto actions=footer.removeFromTop(shortH?40:44);
        const bool narrow=safe.getWidth()<700,tiny=safe.getWidth()<390;
        l.back=actions.removeFromLeft(tiny?48:narrow?64:112);actions.removeFromLeft(6);
        l.quality=actions.removeFromLeft(tiny?64:narrow?82:146);actions.removeFromLeft(6);
        l.last=actions.removeFromRight(tiny?100:narrow?112:118);actions.removeFromRight(6);
        l.record=actions.removeFromRight(juce::jmin(narrow?88:150,actions.getWidth()));
        l.clock=actions.reduced(5,0);
        footer.removeFromTop(shortH?2:4);l.status=footer;
        return l;
    }
};
class TouchSettings final : public juce::Component
{
public:std::function<void(juce::Graphics&)> draw;
    void paint(juce::Graphics& g) override{if(draw)draw(g);}
};
}
