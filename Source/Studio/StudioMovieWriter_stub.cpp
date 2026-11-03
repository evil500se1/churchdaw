#include "StudioMovieWriter.h"
namespace churchstudio
{
struct MovieWriter::Impl {};
MovieWriter::MovieWriter():impl(new Impl){}
MovieWriter::~MovieWriter()=default;
bool MovieWriter::supported(){return false;}
juce::Result MovieWriter::open(const juce::File&,int,int){return juce::Result::fail("Gravacao MP4 disponivel nesta previa para Mac.");}
juce::Result MovieWriter::append(const juce::Image&,double){return juce::Result::fail("Codificador indisponivel.");}
juce::Result MovieWriter::finish(double){return juce::Result::fail("Codificador indisponivel.");}
juce::Result MovieWriter::combine(const juce::File&,const std::vector<AudioFile>&,const juce::File&,double){return juce::Result::fail("Codificador indisponivel.");}
void requestMediaPermission(bool,std::function<void(bool)> fn){fn(true);}
}
