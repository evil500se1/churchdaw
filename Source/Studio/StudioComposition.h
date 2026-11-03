#pragma once
#include "StudioCaptureState.h"
#include "../ChordDetect.h"

namespace churchstudio
{
struct CompositionLayout {juce::Rectangle<float> daw,face;};
inline CompositionLayout layoutComposition(bool hasFace,float faceSize,int corner)
{
    constexpr float w=1280,h=720;
    CompositionLayout result {{16,78,w-32,h-94},{}};
    if(hasFace)
    {
        const float fw=w*juce::jlimit(.18f,.38f,faceSize),fh=fw*.75f;
        const bool left=corner==0;
        result.face={left?16:w-fw-16,78+(h-94-fh)*.5f,fw,fh};
        result.daw={left?fw+32:16,78,w-fw-48,h-94};
    }
    return result;
}
inline void cameraFrameIcon(juce::Graphics& g,juce::Rectangle<float> r)
{
    const float lens=r.getWidth()*.28f;
    g.drawRoundedRectangle(r.withTrimmedRight(lens).reduced(.5f,r.getHeight()*.16f),r.getHeight()*.12f,1.2f);
    juce::Path p;p.startNewSubPath(r.getRight(),r.getY()+r.getHeight()*.18f);p.lineTo(r.getRight()-lens,r.getCentreY()-r.getHeight()*.12f);
    p.lineTo(r.getRight()-lens,r.getCentreY()+r.getHeight()*.12f);p.lineTo(r.getRight(),r.getBottom()-r.getHeight()*.18f);p.closeSubPath();g.strokePath(p,juce::PathStrokeType(1.2f));
}
// Shared design coordinates for the camera and the genuine 3D keyboard viewport.
inline juce::Rectangle<int> performanceKeyboardBounds(){return {42,548,1196,154};}
inline void paintPerformance(juce::Graphics& g,VisualState& state,const juce::Image& brand)
{
    juce::Image face;
    {const juce::ScopedLock lock(state.cameraLock);face=state.cameraImage;}
    g.fillAll(juce::Colour(0xff101116));
    const juce::Rectangle<float> cameraArea(0,0,1280,508);
    if(state.cameraOn.load() && face.isValid())
    {
        juce::Graphics::ScopedSaveState save(g);g.reduceClipRegion(cameraArea.toNearestInt());
        if(state.mirror.load())g.addTransform(juce::AffineTransform(-1,0,1280,0,1,0));
        g.drawImage(face,cameraArea,juce::RectanglePlacement::fillDestination);
    }
    else
    {
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff30212c),120,0,juce::Colour(0xff12161e),1000,500,false));g.fillRect(cameraArea);
        g.setColour(juce::Colour(0xffb4a3af));cameraFrameIcon(g,{611,189,58,48});
        g.setFont(juce::FontOptions(25,juce::Font::bold));g.setColour(juce::Colour(0xfff1edf2));
        g.drawText(juce::String::fromUTF8("Seu palco, seus timbres"),0,266,1280,38,juce::Justification::centred);
        g.setColour(juce::Colour(0xffa7a3ae));g.setFont(juce::FontOptions(16));
        g.drawText(juce::String::fromUTF8("Ligue a webcam para aparecer no vídeo"),0,309,1280,28,juce::Justification::centred);
    }
    // Watermark is composited into every exported frame, independently of mirroring.
    g.setColour(juce::Colour(0x9a101116));g.fillRoundedRectangle(1050,24,206,58,29);
    {juce::Graphics::ScopedSaveState save(g);g.setOpacity(.92f);juce::Path clip;clip.addEllipse(1060,33,40,40);g.reduceClipRegion(clip);g.drawImageWithin(brand,1060,33,40,40,juce::RectanglePlacement::fillDestination);}
    g.setColour(juce::Colour(0xf0f5f1f4));g.setFont(juce::FontOptions(15,juce::Font::bold));
    g.drawText("CHURCHDAW",1110,37,132,30,juce::Justification::centredLeft);
    // Fade into a dedicated harmony ribbon, keeping text away from the face
    // and every key. The same compositor is used for preview and exported MP4.
    g.setGradientFill(juce::ColourGradient(juce::Colour(0x00101116),0,399,juce::Colour(0xff111117),0,478,false));g.fillRect(0,399,1280,79);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff221923),0,478,juce::Colour(0xff101116),0,720,false));g.fillRect(0,478,1280,242);
    g.setColour(juce::Colour(0xff49313f));g.drawHorizontalLine(478,42,1238);
    g.setColour(juce::Colour(0xffed5172));g.drawHorizontalLine(478,42,112);
    g.setColour(juce::Colour(0xff49313f).withAlpha(.7f));g.drawVerticalLine(518,494,531);
    g.setColour(juce::Colour(0xff080a0e));g.fillRoundedRectangle(36,546,1208,164,9);
    g.setColour(juce::Colour(0xff443340));g.drawRoundedRectangle(36,546,1208,164,9,1);
    g.setColour(juce::Colours::white.withAlpha(.045f));g.drawHorizontalLine(547,46,1234);
}
inline void paintPerformanceNotes(juce::Graphics& g,const ChordDetect::Result& harmony,bool sustain)
{
    const bool empty=harmony.kind==ChordDetect::Kind::silence;
    const bool chord=harmony.kind==ChordDetect::Kind::chord;
    auto heading=chord?juce::String::fromUTF8("ACORDE AO VIVO"):harmony.kind==ChordDetect::Kind::interval?juce::String::fromUTF8("INTERVALO"):harmony.kind==ChordDetect::Kind::note?juce::String("NOTA"):juce::String("CHURCHDAW KEYS");
    g.setFont(juce::FontOptions(10.0f,juce::Font::bold));g.setColour(juce::Colour(0xffbba4b4));
    g.drawText(heading,44,485,452,16,juce::Justification::centredLeft);
    g.setColour(empty?juce::Colour(0xffc1b7c4):juce::Colour(0xfff8eff5));
    g.setFont(juce::FontOptions(empty?26.0f:harmony.name.length()>15?28.0f:34.0f,juce::Font::bold));
    g.drawFittedText(empty?juce::String::fromUTF8("Toque para começar"):harmony.name,43,500,458,42,juce::Justification::centredLeft,1,.65f);
    g.setFont(juce::FontOptions(10,juce::Font::bold));g.setColour(juce::Colour(0xffa797a8));
    g.drawText(empty?"NOTAS AO VIVO":harmony.kind==ChordDetect::Kind::interval?harmony.description.toUpperCase():chord?"NOTAS DO ACORDE":"NOTAS TOCADAS",542,486,500,16,juce::Justification::centredLeft);
    g.setColour(empty?juce::Colour(0xffa698a9):juce::Colour(0xfff2afc4));g.setFont(juce::FontOptions(16,juce::Font::bold));
    g.drawFittedText(empty?juce::String::fromUTF8("Seu teclado. Seu som. Sua identidade."):harmony.notes,542,505,560,29,juce::Justification::centredLeft,1,.68f);
    auto badge=juce::Rectangle<float>(1123,501,112,28);
    g.setColour(sustain?juce::Colour(0xff4b263c):juce::Colour(0xff25212b));g.fillRoundedRectangle(badge,14);
    g.setColour(sustain?juce::Colour(0xfff595b3):juce::Colour(0xffa99dae));g.setFont(juce::FontOptions(10,juce::Font::bold));
    g.drawText(sustain?"SUSTAIN":"88 TECLAS",badge,juce::Justification::centred);
}
inline void paintComposition(juce::Graphics& g,VisualState& state,const juce::Image& brand)
{
    if(state.performanceMode.load()){paintPerformance(g,state,brand);return;}
    juce::Image daw,face;
    {const juce::ScopedLock lock(state.dawLock);daw=state.dawImage;}
    {const juce::ScopedLock lock(state.cameraLock);face=state.cameraImage;}
    const bool hasFace=state.cameraOn.load() && face.isValid();
    const auto layout=layoutComposition(hasFace,state.faceSize.load(),state.faceCorner.load());
    g.fillAll(juce::Colour(0xff0e1014));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff25171f),0,0,juce::Colour(0xff111319),600,500,false));g.fillAll();
    {juce::Graphics::ScopedSaveState saved(g);juce::Path clip;clip.addEllipse(20,17,40,40);g.reduceClipRegion(clip);g.drawImageWithin(brand,20,17,40,40,juce::RectanglePlacement::fillDestination);}
    g.setColour(juce::Colour(0xffeeeef2));g.setFont(juce::FontOptions(18,juce::Font::bold));
    g.drawText("CHURCHDAW",72,25,240,23,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xffdd526f));cameraFrameIcon(g,{1147,27,19,18});
    g.setFont(juce::FontOptions(11,juce::Font::bold));g.drawText("LIVE CAM",1178,23,86,26,juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff2d2a32));g.drawHorizontalLine(66,16,1264);
    g.setColour(juce::Colour(0xffe8496a));g.drawHorizontalLine(66,16,68);
    g.setColour(juce::Colour(0xff090b0f));g.fillRoundedRectangle(layout.daw,10);
    if(daw.isValid())g.drawImage(daw,layout.daw.reduced(2),juce::RectanglePlacement::centred);
    else
    {
        g.setColour(juce::Colour(0xffb9b7c2));g.setFont(juce::FontOptions(19));
        g.drawText("Sua DAW aparece aqui",layout.daw,juce::Justification::centred);
    }
    if(hasFace)
    {
        g.setColour(juce::Colour(0x80000000));g.fillRoundedRectangle(layout.face.expanded(5).translated(0,3),16);
        g.setColour(juce::Colour(0xff5b3747));g.drawRoundedRectangle(layout.face.expanded(1.5f),13,1.5f);
        juce::Graphics::ScopedSaveState saved(g);
        juce::Path clip;clip.addRoundedRectangle(layout.face,12);g.reduceClipRegion(clip);
        if(state.mirror.load())g.addTransform(juce::AffineTransform(-1,0,2*layout.face.getX()+layout.face.getWidth(),0,1,0));
        g.drawImage(face,layout.face,juce::RectanglePlacement::fillDestination);
    }
}
}
