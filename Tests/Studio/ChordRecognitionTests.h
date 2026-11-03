#pragma once
#include "ChordDetect.h"
#include "PianoVisualState.h"
#include <iostream>

inline int runChordRecognitionTests()
{
    int failures=0;
    auto check=[&](bool ok,const juce::String& detail){if(!ok){++failures;std::cerr<<"Chord FAIL: "<<detail<<"\n";}};
    auto expect=[&](std::initializer_list<int> notes,const char* expected)
    {
        bool held[128]{};for(int n:notes)held[n]=true;
        const auto value=ChordDetect::analyse(held);
        check(value.name==expected,juce::String(expected)+" got "+value.name);
    };
    expect({48,60,64,67,72},"C");expect({60,63,67},"Cm");expect({60,63,66},"Cdim");
    expect({60,64,68},"Caug");expect({60,62,67},"Csus2");expect({60,65,67},"Csus4");
    expect({60,64,67,69},"C6");expect({57,60,64,67},"Am7");expect({60,63,67,69},"Cm6");
    expect({60,64,67,70},"C7");expect({60,64,67,71},"Cmaj7");expect({60,63,67,70},"Cm7");
    expect({60,63,67,71},"Cm(maj7)");expect({60,63,66,70},"Cm7b5");expect({60,63,66,69},"Cdim7");
    expect({52,60,67},"C/E");expect({43,60,64},"C/G");expect({60,67},"C5");expect({55,60},"C5/G");
    expect({60,62,64,67},"Cadd9");expect({60,62,63,67},"Cm(add9)");expect({60,62,64,67,69},"C6/9");
    expect({60,62,64,67,70},"C9");expect({60,62,64,67,71},"Cmaj9");expect({60,62,63,67,70},"Cm9");
    expect({60,62,63,65,67,70},"Cm11");expect({60,62,64,65,67,70},"C11");
    expect({60,62,64,67,69,70},"C13");expect({60,62,64,66,67,69,71},"Cmaj13(#11)");
    expect({60,61,64,67,70},"C7(b9)");expect({60,63,64,67,70},"C7(#9)");
    expect({60,61,64,67,68,70},"C7(b9,b13)");expect({60,61,63,64,66,68,70},"C7alt");
    expect({60,61,64,66,67,70},"C7(b9,#11)");
    expect({60,64,70},"C7");expect({60,64,71},"Cmaj7");expect({60,62,63,70},"Cm9");
    expect({59,63,66},"B");expect({61,64,68},"C#m");
    auto spelling=ChordDetect::analyse(ChordDetect::maskOf(11,3,6),11);
    check(spelling.notes==juce::String::fromUTF8("B  ·  D#  ·  F#"),"B major spelling: "+spelling.notes);
    spelling=ChordDetect::analyse(ChordDetect::maskOf(0,3,6,9),0);
    check(spelling.notes==juce::String::fromUTF8("C  ·  Eb  ·  Gb  ·  Bbb"),"dim7 spelling: "+spelling.notes);
    check(ChordDetect::analyse(ChordDetect::maskOf(0,4,7,9),4,0).name=="C6/E","keep previous root through an ambiguous inversion");
    check(ChordDetect::analyse(ChordDetect::maskOf(0,4,7,9),9,0).name=="Am7","played root in bass takes precedence over continuity");
    // Every pitch-class set and every possible played bass has a deterministic
    // reading. Unconventional sets remain explicit note sets, never guessed roots.
    int combinations=0,named=0,fallback=0;
    for(int mask=1;mask<4096;++mask)for(int bass=0;bass<12;++bass)if(mask&(1<<bass))
    {
        const auto value=ChordDetect::analyse((juce::uint16)mask,bass);
        check(value.classes==mask && value.bass==bass && value.name.isNotEmpty() && value.notes.isNotEmpty(),"set coverage");
        if(value.kind==ChordDetect::Kind::chord){++named;check(value.root>=0 && (mask&(1<<value.root)),"root must be played");}
        else ++fallback;
        ++combinations;
    }
    for(int root=0;root<12;++root)for(auto shape:{ChordDetect::maskOf(0,4,7),ChordDetect::maskOf(0,3,7),ChordDetect::maskOf(0,4,7,10),ChordDetect::maskOf(0,2,4,7,11)})
    {
        const auto mask=(juce::uint16)(((shape<<root)|(shape>>(12-root)))&0xfff);
        const auto value=ChordDetect::analyse(mask,root);
        check(value.root==root && value.kind==ChordDetect::Kind::chord,"transposition");
    }
    LiveChordTracker tracker;bool held[128]{};
    held[60]=true;check(!tracker.update(held,1000),"wait for rolled attack");
    held[64]=true;tracker.update(held,1010);held[67]=true;tracker.update(held,1020);
    check(!tracker.update(held,1060),"no intermediate chord flicker");
    check(tracker.update(held,1090) && tracker.value().name=="C","settled major chord");
    held[64]=false;tracker.update(held,1100);held[64]=true;
    check(!tracker.update(held,1120) && tracker.value().name=="C","brief release ignored");
    for(auto& n:held)n=false;
    tracker.update(held,1200);check(!tracker.update(held,1390) && tracker.value().name=="C","release grace");
    check(tracker.update(held,1425) && tracker.value().kind==ChordDetect::Kind::silence,"released chord clears");
    PianoVisualState midi;
    midi.observe(juce::MidiMessage::noteOn(1,60,(juce::uint8)80));
    midi.observe(juce::MidiMessage::controllerEvent(1,64,127));
    midi.observe(juce::MidiMessage::noteOff(1,60));
    check(midi.channels[60].load()==0 && midi.sounding[60].load()==1,"sustain holds harmony, not key");
    midi.observe(juce::MidiMessage::noteOn(2,60,(juce::uint8)80));
    midi.observe(juce::MidiMessage::controllerEvent(1,64,0));
    check(midi.sounding[60].load()==2,"release one pedal preserves another channel");
    midi.observe(juce::MidiMessage::controllerEvent(2,64,127));
    midi.observe(juce::MidiMessage::noteOff(2,60));
    midi.observe(juce::MidiMessage::controllerEvent(2,121,0));
    check(midi.sounding[60].load()==0 && midi.sustainChannels.load()==0,"reset controllers releases pedal");
    midi.observe(juce::MidiMessage::noteOn(16,108,(juce::uint8)80));midi.observe(juce::MidiMessage::allSoundOff(16));
    check(midi.channels[108].load()==0 && midi.sounding[108].load()==0,"panic releases chord");
    midi.observe(juce::MidiMessage::controllerEvent(2,64,127));midi.reset();check(midi.sustainChannels.load()==0,"transport reset");
    PianoVisualState host;
    host.observeHost(juce::MidiMessage::controllerEvent(1,64,127));
    host.observeHost(juce::MidiMessage::noteOn(4,60,(juce::uint8)80));host.observeHost(juce::MidiMessage::noteOff(4,60));
    check(host.channels[60].load()==0 && host.sounding[60].load()==8,"host aggregate pedal holds a routed channel");
    host.observeHost(juce::MidiMessage::controllerEvent(2,64,0));
    check(host.sounding[60].load()==0,"host aggregate release can arrive on another port/channel");
    host.setHostGlobalSustain(true);
    host.observeHost(juce::MidiMessage::noteOn(3,64,(juce::uint8)80));host.observeHost(juce::MidiMessage::noteOff(3,64));
    check(host.sounding[64].load()==4,"GUI sustain retains harmony");
    host.setHostGlobalSustain(false);check(host.sounding[64].load()==0,"GUI sustain release clears harmony without MIDI");
    host.observeHost(juce::MidiMessage::controllerEvent(1,64,0),true);
    host.observeHost(juce::MidiMessage::noteOn(5,67,(juce::uint8)80));host.observeHost(juce::MidiMessage::noteOff(5,67));
    check(host.sounding[67].load()==16,"inverted pedal holds harmony");
    host.observeHost(juce::MidiMessage::controllerEvent(1,64,127),true);
    check(host.sounding[67].load()==0,"inverted pedal releases harmony");
    host.observeHost(juce::MidiMessage::noteOn(8,72,(juce::uint8)80));host.observeHost(juce::MidiMessage::allNotesOff(1));
    check(host.channels[72].load()==0 && host.sounding[72].load()==0,"host panic clears every channel");
    std::cout<<"Chords: "<<(failures==0?"PASS":"FAIL")<<"; "<<combinations<<" pitch-set/bass cases, "<<named<<" chord readings, "<<fallback<<" note/interval/set readings, "<<failures<<" failures\n";
    return failures;
}
