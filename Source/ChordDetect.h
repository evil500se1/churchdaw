#pragma once
#include <juce_core/juce_core.h>
#include <array>

// Pitch-set recognition, independent of audio processing. Symbols describe the
// played pitches; no missing root is inferred without harmonic context.
// Extensions may omit the fifth. Bass and previous root resolve equivalent
// readings deterministically (e.g. C6 / Am7), not as a claim of unique harmony.
struct ChordDetect
{
    enum class Kind { silence, note, interval, chord, noteSet };
    struct Result
    {
        Kind kind=Kind::silence;
        juce::String name,notes,description;
        juce::uint16 classes=0;
        int root=-1,bass=-1;
        bool fifthOmitted=false;
    };
    struct Shape {juce::uint16 mask;const char* suffix;int priority;bool optionalFifth=false;};
    template<typename... Notes> static constexpr juce::uint16 maskOf(Notes... notes)
    {return (juce::uint16)((0u | ... | (1u << notes)));}
    static int countBits(juce::uint16 value) noexcept
    {int count=0;while(value){value=(juce::uint16)(value&(value-1));++count;}return count;}
    static const char* noteName(int pitchClass) noexcept
    {
        static constexpr const char* names[]={"C","Db","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
        return names[(pitchClass%12+12)%12];
    }
    static juce::uint16 rotateDown(juce::uint16 classes,int root) noexcept
    {return (juce::uint16)(((classes>>root)|(classes<<(12-root)))&0xfff);}
    static const char* rootName(int root,const juce::String& suffix) noexcept
    {
        const bool minor=suffix.startsWith("m") && !suffix.startsWith("maj");
        if(minor && root==1)return "C#";
        if(minor && root==8)return "G#";
        return noteName(root);
    }
    static juce::String spell(int pitch,int root,const juce::String& suffix,juce::uint16 classes)
    {
        const char* letters="CDEFGAB";static constexpr int natural[]={0,2,4,5,7,9,11};
        int rootLetter=0;while(letters[rootLetter]!=rootName(root,suffix)[0])++rootLetter;
        const int interval=(pitch-root+12)%12;
        static constexpr int degrees[]={0,1,1,2,2,3,3,4,5,5,6,6};
        int degree=degrees[interval];const auto rel=rotateDown(classes,root);
        if(interval==3 && (rel&(1u<<4)))degree=1; // #9 above a major third.
        if(interval==6 && (suffix.contains("b5") || suffix.startsWith("dim")))degree=4;
        if(interval==8 && (suffix.contains("#5") || suffix.startsWith("aug")))degree=4;
        if(interval==9 && suffix.startsWith("dim7"))degree=6;
        const int letter=(rootLetter+degree)%7;
        const int accidental=(pitch-natural[letter]+18)%12-6;
        if(std::abs(accidental)>2)return noteName(pitch);
        return juce::String::charToString((juce::juce_wchar)letters[letter])
            + (accidental==-2?"bb":accidental==-1?"b":accidental==1?"#":accidental==2?"##":"");
    }
    static juce::String pitchNames(juce::uint16 classes,int first)
    {
        juce::StringArray names;
        for(int i=0;i<12;++i){const int n=(first+i)%12;if(classes&(1u<<n))names.add(noteName(n));}
        return names.joinIntoString(juce::String::fromUTF8("  ·  "));
    }
    static Result analyse(const bool held[128],int preferredRoot=-1)
    {
        juce::uint16 classes=0;int bass=-1;
        for(int n=0;n<128;++n)if(held[n]){classes|=(juce::uint16)(1u<<(n%12));if(bass<0)bass=n%12;}
        return analyse(classes,bass,preferredRoot);
    }
    static Result analyse(juce::uint16 classes,int bass,int preferredRoot=-1)
    {
        Result result;classes&=0xfff;result.classes=classes;
        if(classes==0)return result;
        if(bass<0 || bass>11 || !(classes&(1u<<bass)))
            for(bass=0;bass<12 && !(classes&(1u<<bass));++bass){}
        result.bass=bass;result.notes=pitchNames(classes,bass);
        const int count=countBits(classes);
        if(count==1)
        {result.kind=Kind::note;result.name=noteName(bass);result.description="Nota";return result;}
        if(count==2)
        {
            int other=0;while(other==bass || !(classes&(1u<<other)))++other;
            const int distance=(other-bass+12)%12;
            if(distance==7 || distance==5)
            {
                result.kind=Kind::chord;result.root=distance==7?bass:other;
                result.name=juce::String(noteName(result.root))+"5";
                if(result.root!=bass)result.name+="/"+juce::String(noteName(bass));
                result.description="Power chord";return result;
            }
            static constexpr const char* intervals[]={"Unissono","2ª menor","2ª maior","3ª menor","3ª maior","4ª justa","Tritono","5ª justa","6ª menor","6ª maior","7ª menor","7ª maior"};
            result.kind=Kind::interval;result.name=juce::String(noteName(bass))+juce::String::fromUTF8(" + ")+noteName(other);
            result.description=juce::String::fromUTF8(intervals[distance]);return result;
        }
        static constexpr Shape shapes[]={
            {maskOf(0,4,7),"",90},{maskOf(0,3,7),"m",90},
            {maskOf(0,3,6),"dim",80},{maskOf(0,4,8),"aug",76},
            {maskOf(0,2,7),"sus2",74},{maskOf(0,5,7),"sus4",78},
            {maskOf(0,4,7,9),"6",76},{maskOf(0,3,7,9),"m6",74},
            {maskOf(0,4,7,10),"7",90,true},{maskOf(0,4,7,11),"maj7",90,true},
            {maskOf(0,3,7,10),"m7",90,true},{maskOf(0,3,7,11),"m(maj7)",84,true},
            {maskOf(0,3,6,10),"m7b5",80},{maskOf(0,3,6,9),"dim7",76},
            {maskOf(0,4,8,10),"7(#5)",78},{maskOf(0,4,8,11),"maj7(#5)",76},
            {maskOf(0,4,6,10),"7(b5)",78},{maskOf(0,4,6,11),"maj7(b5)",72},
            {maskOf(0,2,7,10),"7sus2",74},{maskOf(0,5,7,10),"7sus4",80},
            {maskOf(0,2,7,11),"maj7sus2",72},{maskOf(0,5,7,11),"maj7sus4",74},
            {maskOf(0,2,4,7),"add9",84},{maskOf(0,2,3,7),"m(add9)",84},
            {maskOf(0,4,5,7),"add11",70},{maskOf(0,3,5,7),"m(add11)",78},
            {maskOf(0,4,6,7),"add#11",74},
            {maskOf(0,2,4,7,9),"6/9",82},{maskOf(0,2,3,7,9),"m6/9",80},
            {maskOf(0,2,4,7,10),"9",88,true},{maskOf(0,2,4,7,11),"maj9",88,true},
            {maskOf(0,2,3,7,10),"m9",88,true},{maskOf(0,2,3,7,11),"m(maj9)",82,true},
            {maskOf(0,2,3,6,10),"m9b5",76},{maskOf(0,2,4,8,10),"9(#5)",78},
            {maskOf(0,2,4,6,10),"9(b5)",76},{maskOf(0,2,5,7,10),"9sus4",80,true},
            {maskOf(0,2,4,5,7,10),"11",78,true},{maskOf(0,2,4,5,7,11),"maj11",72,true},
            {maskOf(0,2,3,5,7,10),"m11",88,true},{maskOf(0,3,5,7,10),"m11",84,true},
            {maskOf(0,2,3,5,7,11),"m(maj11)",78,true},
            {maskOf(0,4,7,9,10),"13",84,true},{maskOf(0,2,4,7,9,10),"13",88,true},
            {maskOf(0,2,4,5,7,9,10),"13",84,true},
            {maskOf(0,4,7,9,11),"maj13",84,true},{maskOf(0,2,4,7,9,11),"maj13",88,true},
            {maskOf(0,2,4,5,7,9,11),"maj13",78,true},
            {maskOf(0,3,7,9,10),"m13",78,true},{maskOf(0,2,3,7,9,10),"m13",84,true},
            {maskOf(0,2,3,5,7,9,10),"m13",82,true},
            {maskOf(0,2,5,7,9,10),"13sus4",78,true},
            {maskOf(0,1,4,7,10),"7(b9)",84,true},{maskOf(0,3,4,7,10),"7(#9)",82,true},
            {maskOf(0,4,6,7,10),"7(#11)",80,true},{maskOf(0,4,7,8,10),"7(b13)",78,true},
            {maskOf(0,2,4,6,7,10),"9(#11)",84,true},{maskOf(0,2,4,7,8,10),"9(b13)",80,true},
            {maskOf(0,4,6,7,11),"maj7(#11)",84,true},{maskOf(0,2,4,6,7,11),"maj9(#11)",88,true},
            {maskOf(0,2,4,6,7,9,11),"maj13(#11)",88,true},
            {maskOf(0,2,4,6,7,9,10),"13(#11)",84,true},
            {maskOf(0,1,4,7,9,10),"13(b9)",82,true},
            {maskOf(0,1,5,7,10),"7sus4(b9)",74,true},
            {maskOf(0,1,4,7,8,10),"7(b9,b13)",84,true},
            {maskOf(0,1,3,4,6,8,10),"7alt",88},
        };
        int bestScore=-1;juce::String suffix;
        auto offer=[&](int root,const juce::String& quality,int score,bool omit)
        {
            score+=(root==bass?100:0)+(root==preferredRoot?24:0);
            if(score<=bestScore)return;
            bestScore=score;result.root=root;suffix=quality;result.fifthOmitted=omit;
        };
        for(int root=0;root<12;++root)
        {
            if(!(classes&(1u<<root)))continue;
            const auto rel=rotateDown(classes,root);
            for(const auto& shape:shapes)
            {
                if(rel==shape.mask)offer(root,shape.suffix,500+shape.priority,false);
                else if(shape.optionalFifth && rel==(shape.mask&~(1u<<7)))
                    offer(root,shape.suffix,330+shape.priority,true);
            }
            // A seventh + third defines the family. Every additional pitch
            // must be explicitly explained as a supported extension/alteration.
            if(count>7)continue;
            for(int family=0;family<4;++family)
            {
                const bool minor=family>=2,majorSeventh=(family%2)!=0;
                const int third=minor?3:4,seventh=majorSeventh?11:10;
                const auto core=(juce::uint16)((1u<<0)|(1u<<third)|(1u<<seventh));
                if((rel&core)!=core || (rel&(1u<<(majorSeventh?10:11))))continue;
                if(minor && (rel&(1u<<4)))continue;
                const auto has=[&](int i){return (rel&(1u<<i))!=0;};
                if(has(1) && has(2))continue; // b9 and natural 9: keep a note-set reading.
                const int extension=has(9)?13:has(5)?11:has(2)?9:7;
                juce::String quality=minor?(majorSeventh?"m(maj":"m"):(majorSeventh?"maj":"");
                quality+=juce::String(extension);if(minor && majorSeventh)quality+=")";
                juce::StringArray altered;
                if(has(1))altered.add("b9");
                if(!minor && has(3))altered.add("#9");
                if(has(6))altered.add(!has(7) && !has(2) && !has(5) && !has(9)?"b5":"#11");
                if(has(8))altered.add(has(7)?"b13":"#5");
                if(altered.size()>3)continue;
                if(!altered.isEmpty())quality+="("+altered.joinIntoString(",")+")";
                offer(root,quality,300+(minor?7:8)-altered.size()*3,!has(7) && !has(6) && !has(8));
            }
        }
        if(result.root>=0)
        {
            result.kind=Kind::chord;result.name=juce::String(rootName(result.root,suffix))+suffix;
            if(result.root!=bass)result.name+="/"+spell(bass,result.root,suffix,classes);
            juce::StringArray names;
            for(int i=0;i<12;++i){const int pitch=(result.root+i)%12;if(classes&(1u<<pitch))names.add(spell(pitch,result.root,suffix,classes));}
            result.notes=names.joinIntoString(juce::String::fromUTF8("  ·  "));
            result.description=result.fifthOmitted?juce::String::fromUTF8("Quinta omitida"):result.root!=bass?juce::String::fromUTF8("Inversão"):juce::String("Acorde");
            return result;
        }
        result.kind=Kind::noteSet;result.name=count==12?"Cluster":"Notas";
        result.description=juce::String::fromUTF8("Conjunto de notas");return result;
    }
    // Existing DAW chord consumers continue to receive only chord symbols.
    static juce::String nameFor(const bool held[128])
    {const auto result=analyse(held);return result.kind==Kind::chord?result.name:juce::String();}
};

// Settles rolled attacks/releases before changing the label; the physical 3D
// keys remain immediate. Operates on the UI timer, never on the audio thread.
class LiveChordTracker
{
public:
    bool update(const bool held[128],double nowMs)
    {
        juce::uint16 classes=0;int bass=-1;
        for(int n=0;n<128;++n)if(held[n]){classes|=(juce::uint16)(1u<<(n%12));if(bass<0)bass=n%12;}
        const int key=(int)classes|((bass+1)<<12);
        if(key!=pendingKey){pendingKey=key;pendingSince=nowMs;}
        if(key==shownKey || nowMs-pendingSince<(classes==0?220.0:65.0))return false;
        displayed=ChordDetect::analyse(classes,bass,displayed.root);shownKey=key;return true;
    }
    const ChordDetect::Result& value() const noexcept{return displayed;}
    void reset(){displayed={};pendingKey=shownKey=0;pendingSince=0;}
private:
    ChordDetect::Result displayed;
    int pendingKey=0,shownKey=0;double pendingSince=0;
};
