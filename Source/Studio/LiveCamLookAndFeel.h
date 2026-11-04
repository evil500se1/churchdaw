#pragma once
#include "../NordLookAndFeel.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstudio
{
namespace palette
{
inline juce::Colour background(){return juce::Colour(0xff101115);}
inline juce::Colour surface(){return juce::Colour(0xff181a20);}
inline juce::Colour line(){return juce::Colour(0xff30333d);}
inline juce::Colour text(){return juce::Colour(0xfff1f0f3);}
inline juce::Colour muted(){return juce::Colour(0xff9296a4);}
inline juce::Colour accent(){return juce::Colour(0xffee486d);}
inline juce::Colour green(){return juce::Colour(0xff76d6b0);}
}

inline void cameraIcon(juce::Graphics& g,juce::Rectangle<float> r)
{
    g.drawRoundedRectangle(r.withTrimmedRight(r.getWidth()*.28f).reduced(.8f,2.5f),2.5f,1.4f);
    juce::Path lens;lens.startNewSubPath(r.getRight(),r.getY()+3);lens.lineTo(r.getRight()-r.getWidth()*.28f,r.getCentreY()-2);
    lens.lineTo(r.getRight()-r.getWidth()*.28f,r.getCentreY()+2);lens.lineTo(r.getRight(),r.getBottom()-3);lens.closeSubPath();
    g.strokePath(lens,juce::PathStrokeType(1.4f));
}

// Share desktop menus, text fields and tooltips with the main workspace while
// keeping Live Cam's camera/record controls and mobile touch presentation.
#if CHURCHDAW_MOBILE_SHELL
using LiveCamBaseLookAndFeel = juce::LookAndFeel_V4;
#else
using LiveCamBaseLookAndFeel = NordLookAndFeel;
#endif

class LiveCamLookAndFeel final : public LiveCamBaseLookAndFeel
{
public:
    void setTouchControls(bool enabled){touchControls=enabled;}
    void getIdealPopupMenuItemSize(const juce::String& text,bool separator,int standardHeight,int& width,int& height) override
    {
        LiveCamBaseLookAndFeel::getIdealPopupMenuItemSize(text,separator,standardHeight,width,height);
        if(touchControls && !separator)height=juce::jmax(44,height);
    }
    LiveCamLookAndFeel()
    {
        setColour(juce::ComboBox::backgroundColourId,juce::Colour(0xff22252d));
        setColour(juce::ComboBox::textColourId,palette::text());
        setColour(juce::ComboBox::outlineColourId,palette::line());
        setColour(juce::PopupMenu::backgroundColourId,juce::Colour(0xff202229));
        setColour(juce::PopupMenu::textColourId,palette::text());
        setColour(juce::PopupMenu::highlightedBackgroundColourId,juce::Colour(0xff4b2735));
        setColour(juce::PopupMenu::highlightedTextColourId,juce::Colours::white);
        setColour(juce::Slider::textBoxTextColourId,palette::text());
        setColour(juce::Slider::textBoxBackgroundColourId,juce::Colours::transparentBlack);
        setColour(juce::Slider::textBoxOutlineColourId,juce::Colours::transparentBlack);
        setColour(juce::Label::textColourId,palette::text());
        setColour(juce::TextEditor::highlightColourId,palette::accent().withAlpha(.35f));
    }
    juce::Font getTextButtonFont(juce::TextButton& b,int h) override{return juce::FontOptions((bool)b.getProperties()["churchCamTouch"]?12.5f:h>=40?14.0f:12.5f,juce::Font::bold);}
    void drawButtonBackground(juce::Graphics& g,juce::Button& b,const juce::Colour&,bool over,bool down) override
    {
        auto r=b.getLocalBounds().toFloat().reduced(.6f);
        const bool primary=b.getComponentID()=="liveCamRecord";
        const bool active=b.getToggleState();
        auto fill=primary?palette::accent():active?juce::Colour(0xff40232f):juce::Colour(0xff242730);
        if(over)fill=fill.brighter(.08f);if(down)fill=fill.darker(.15f);
        g.setColour(fill.withMultipliedAlpha(b.isEnabled()?1.0f:.35f));g.fillRoundedRectangle(r,9);
        g.setColour((primary?palette::accent().brighter(.12f):active?palette::accent().withAlpha(.45f):palette::line()).withMultipliedAlpha(b.isEnabled()?1.0f:.4f));
        g.drawRoundedRectangle(r,9,1);
        if(b.hasKeyboardFocus(true)){g.setColour(palette::text().withAlpha(.6f));g.drawRoundedRectangle(r.reduced(2),7,1);}
    }
    void drawButtonText(juce::Graphics& g,juce::TextButton& b,bool,bool) override
    {
        g.setFont(getTextButtonFont(b,b.getHeight()));g.setColour(palette::text().withAlpha(b.isEnabled()?1.0f:.32f));
        const bool compact=(bool)b.getProperties()["churchCamTouch"] && b.getWidth()<140;
        auto r=b.getLocalBounds().reduced(compact?8:12,3);const auto icon=compact?juce::String():b.getProperties()["liveCamIcon"].toString();
        if(icon.isNotEmpty())
        {
            auto i=r.removeFromLeft(19).toFloat().withSizeKeepingCentre(17,17);r.removeFromLeft(8);
            if(icon=="camera")cameraIcon(g,i);
            else if(icon=="record")g.fillEllipse(i.reduced(3));
            else if(icon=="stop")g.fillRoundedRectangle(i.reduced(3),2);
            else if(icon=="back"){g.drawLine(i.getRight()-2,i.getCentreY(),i.getX()+2,i.getCentreY(),1.5f);juce::Path p;p.startNewSubPath(i.getX()+7,i.getY()+3);p.lineTo(i.getX()+2,i.getCentreY());p.lineTo(i.getX()+7,i.getBottom()-3);g.strokePath(p,juce::PathStrokeType(1.5f));}
            else if(icon=="play"){juce::Path p;p.addTriangle(i.getX()+4,i.getY()+2,i.getRight()-2,i.getCentreY(),i.getX()+4,i.getBottom()-2);g.fillPath(p);}
        }
        g.drawFittedText(b.getButtonText(),r,icon.isEmpty()?juce::Justification::centred:juce::Justification::centredLeft,1,.85f);
    }
    void drawComboBox(juce::Graphics& g,int w,int h,bool,int,int,int,int,juce::ComboBox& box) override
    {
        auto r=juce::Rectangle<float>(.5f,.5f,(float)w-1,(float)h-1);
        g.setColour(juce::Colour(0xff22252d).withAlpha(box.isEnabled()?1.0f:.4f));g.fillRoundedRectangle(r,8);
        g.setColour(box.hasKeyboardFocus(true)?palette::accent():palette::line());g.drawRoundedRectangle(r,8,1);
        g.setColour(palette::muted().withAlpha(box.isEnabled()?1.0f:.35f));juce::Path p;
        p.startNewSubPath((float)w-19,(float)h*.5f-2);p.lineTo((float)w-15,(float)h*.5f+2);p.lineTo((float)w-11,(float)h*.5f-2);g.strokePath(p,juce::PathStrokeType(1.3f));
    }
    juce::Font getComboBoxFont(juce::ComboBox&) override{return juce::FontOptions(12.5f);}
    void positionComboBoxText(juce::ComboBox& box,juce::Label& label) override{label.setBounds(10,1,box.getWidth()-35,box.getHeight()-2);label.setFont(getComboBoxFont(box));}
    void drawLinearSlider(juce::Graphics& g,int x,int y,int w,int h,float pos,float,float,juce::Slider::SliderStyle,juce::Slider& s) override
    {
        const float cy=(float)y+h*.5f;const float alpha=s.isEnabled()?1.0f:.28f;
        g.setColour(palette::line().withAlpha(alpha));g.fillRoundedRectangle((float)x,cy-2,(float)w,4,2);
        g.setColour(palette::accent().withAlpha(alpha));g.fillRoundedRectangle((float)x,cy-2,juce::jmax(0.0f,pos-x),4,2);
        g.setColour(palette::text().withAlpha(alpha));g.fillEllipse(pos-5,cy-5,10,10);
    }
    void drawToggleButton(juce::Graphics& g,juce::ToggleButton& b,bool,bool) override
    {
        auto r=b.getLocalBounds().toFloat();const auto y=r.getCentreY()-8;
        g.setColour(b.getToggleState()?palette::accent():palette::line());g.fillRoundedRectangle(0,y,29,16,8);
        g.setColour(palette::text());g.fillEllipse(b.getToggleState()?15:3,y+3,10,10);
        g.setFont(juce::FontOptions(12));g.drawText(b.getButtonText(),r.withTrimmedLeft(37),juce::Justification::centredLeft);
    }
private:bool touchControls=false;
};

class LevelMeter final : public juce::Component
{
public:
    void setLevel(float value){level=juce::jlimit(0.0f,1.0f,value);repaint();}
    void paint(juce::Graphics& g) override
    {
        const auto db=juce::Decibels::gainToDecibels(level,-60.0f);
        auto bars=getLocalBounds().withTrimmedRight(48).toFloat();const int count=36;
        for(int n=0;n<count;++n)
        {
            const float threshold=-60.0f+60.0f*n/count;
            const auto colour=threshold>-3?palette::accent():threshold>-12?juce::Colour(0xffdab577):palette::green();
            g.setColour(db>threshold?colour:palette::line().withAlpha(.5f));
            g.fillRoundedRectangle(bars.getX()+bars.getWidth()*n/count,4,juce::jmax(1.0f,bars.getWidth()/count-2),8,1);
        }
        g.setColour(palette::muted());g.setFont(juce::FontOptions(10));
        g.drawText(level>.001f?juce::String(db,0)+" dB":juce::String::fromUTF8("−∞ dB"),getLocalBounds().removeFromRight(44),juce::Justification::centredRight);
    }
private:float level=0;
};
}
