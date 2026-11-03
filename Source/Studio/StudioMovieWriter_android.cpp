#include "StudioMovieWriter.h"
#if JUCE_ANDROID
#include <jni.h>
#include <juce_core/native/juce_JNIHelpers_android.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include <array>

namespace juce
{
#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 METHOD(construct,"<init>","(Ljava/lang/String;II)V") \
 METHOD(error,"getError","()Ljava/lang/String;") \
 METHOD(append,"append","([ID)Ljava/lang/String;") \
 METHOD(finish,"finish","(D)Ljava/lang/String;") \
 METHOD(close,"close","()V") \
 STATICMETHOD(supported,"supported","()Z") \
 STATICMETHOD(combine,"combine","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;D)Ljava/lang/String;")
DECLARE_JNI_CLASS(ChurchCamMovieBridge,"com/churchdaw/app/ChurchCamMovieWriter")
#undef JNI_CLASS_MEMBERS
}
namespace churchstudio
{
static juce::Result checked(jobject value)
{
    auto* env=juce::getEnv();juce::LocalRef<jstring> message((jstring)value);
    if(env->ExceptionCheck()){env->ExceptionClear();return juce::Result::fail("O codificador Android encontrou um erro.");}
    if(message==nullptr)return juce::Result::fail("O codificador Android nao respondeu.");
    auto text=juce::juceString(message);return text.isEmpty()?juce::Result::ok():juce::Result::fail(text);
}
struct MovieWriter::Impl
{
    juce::GlobalRef writer;
    juce::GlobalRefImpl<jintArray> pixels;
    std::vector<jint> staging;
    int width=0,height=0;
    ~Impl(){if(writer!=nullptr){auto* env=juce::getEnv();env->CallVoidMethod(writer.get(),juce::ChurchCamMovieBridge.close);if(env->ExceptionCheck())env->ExceptionClear();}}
};
MovieWriter::MovieWriter():impl(new Impl){}
MovieWriter::~MovieWriter()=default;
bool MovieWriter::supported()
{
    // Codec availability is stable for the lifetime of this app process.
    // The panel checks this on every repaint tick; never enumerate codecs at 30 Hz.
    static const bool available=[]
    {
        auto* env=juce::getEnv();auto result=env->CallStaticBooleanMethod(juce::ChurchCamMovieBridge,juce::ChurchCamMovieBridge.supported);
        if(env->ExceptionCheck()){env->ExceptionClear();return false;}return result!=0;
    }();
    return available;
}
juce::Result MovieWriter::open(const juce::File& file,int width,int height)
{
    if(width<2||height<2||width>1920||height>1080)return juce::Result::fail("Dimensoes de video invalidas.");
    auto* env=juce::getEnv();impl=std::make_unique<Impl>();impl->width=width;impl->height=height;
    juce::LocalRef<jobject> object(env->NewObject(juce::ChurchCamMovieBridge,juce::ChurchCamMovieBridge.construct,juce::javaString(file.getFullPathName()).get(),(jint)width,(jint)height));
    if(env->ExceptionCheck()||object==nullptr){env->ExceptionClear();return juce::Result::fail("Nao foi possivel abrir o codificador Android.");}
    impl->writer=juce::GlobalRef(object);
    auto result=checked(env->CallObjectMethod(impl->writer.get(),juce::ChurchCamMovieBridge.error));
    if(result.failed())return result;
    impl->staging.resize((size_t)width*height);
    juce::LocalRef<jintArray> pixels(env->NewIntArray(width*height));
    if(env->ExceptionCheck()||pixels==nullptr){env->ExceptionClear();return juce::Result::fail("Memoria insuficiente para gravar nessa qualidade.");}
    impl->pixels=juce::GlobalRefImpl<jintArray>(pixels);return juce::Result::ok();
}
juce::Result MovieWriter::append(const juce::Image& image,double seconds)
{
    if(impl->writer==nullptr||impl->pixels==nullptr||image.getWidth()!=impl->width||image.getHeight()!=impl->height||!std::isfinite(seconds))return juce::Result::fail("Quadro de video invalido.");
    juce::Image::BitmapData source(image,juce::Image::BitmapData::readOnly);
    for(int y=0;y<impl->height;++y)for(int x=0;x<impl->width;++x)
        impl->staging[(size_t)y*impl->width+x]=(jint)source.getPixelColour(x,y).getARGB();
    auto* env=juce::getEnv();env->SetIntArrayRegion((jintArray)impl->pixels.get(),0,(jsize)impl->staging.size(),impl->staging.data());
    if(env->ExceptionCheck()){env->ExceptionClear();return juce::Result::fail("Nao foi possivel enviar o quadro ao codificador.");}
    return checked(env->CallObjectMethod(impl->writer.get(),juce::ChurchCamMovieBridge.append,impl->pixels.get(),seconds));
}
juce::Result MovieWriter::finish(double seconds)
{
    if(impl->writer==nullptr)return juce::Result::fail("Gravacao nao iniciada.");
    return checked(juce::getEnv()->CallObjectMethod(impl->writer.get(),juce::ChurchCamMovieBridge.finish,seconds));
}
// Offline mix on the encoder worker. Bounded buffers; no file or codec work in the audio callback.
static juce::Result mixPCM(const std::vector<AudioFile>& tracks,const juce::File& destination,double duration)
{
    constexpr int rate=48000,block=1024;
    if(!std::isfinite(duration)||duration<=0)return juce::Result::fail("Duracao de gravacao invalida.");
    struct Input
    {
        std::unique_ptr<juce::AudioFormatReaderSource> reader;
        std::unique_ptr<juce::ResamplingAudioSource> resampler;
        juce::int64 start=0;float gain=1;
    };
    std::vector<std::unique_ptr<Input>> inputs;juce::AudioFormatManager formats;formats.registerBasicFormats();
    float maximum=0;for(const auto& track:tracks)maximum+=track.peak*track.gain;
    const float protection=maximum>.98f?.98f/maximum:1.f;
    for(const auto& track:tracks)
    {
        if(track.gain<=0||track.offsetSeconds>=duration)continue;
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(track.file));
        if(!reader||reader->sampleRate<=0)return juce::Result::fail("Faixa de audio invalida.");
        auto input=std::make_unique<Input>();const double ratio=reader->sampleRate/rate;
        input->reader=std::make_unique<juce::AudioFormatReaderSource>(reader.release(),true);
        input->resampler=std::make_unique<juce::ResamplingAudioSource>(input->reader.get(),false,2);
        input->resampler->setResamplingRatio(ratio);input->resampler->prepareToPlay(block,rate);
        input->start=(juce::int64)std::llround(juce::jmax(0.0,track.offsetSeconds)*rate);input->gain=track.gain*protection;
        inputs.push_back(std::move(input));
    }
    if(inputs.empty())return juce::Result::fail("Nenhuma faixa de audio foi recebida.");
    auto stream=destination.createOutputStream();if(!stream||!stream->setPosition(0)||stream->truncate().failed())return juce::Result::fail("Nao foi possivel preparar o audio do video.");
    juce::AudioBuffer<float> mixed(2,block),scratch(2,block);std::array<uint8_t,block*4> pcm{};
    const auto total=(juce::int64)std::ceil(duration*rate);
    for(juce::int64 position=0;position<total;position+=block)
    {
        const int count=(int)juce::jmin<juce::int64>(block,total-position);mixed.clear();
        for(auto& input:inputs)
        {
            const int offset=(int)juce::jlimit<juce::int64>(0,count,input->start-position);if(offset==count)continue;
            scratch.clear();input->resampler->getNextAudioBlock({&scratch,0,count-offset});
            for(int channel=0;channel<2;++channel)mixed.addFrom(channel,offset,scratch,channel,0,count-offset,input->gain);
        }
        for(int i=0;i<count;++i)for(int channel=0;channel<2;++channel)
        {
            const float sample=mixed.getSample(channel,i);
            if(!std::isfinite(sample))return juce::Result::fail("A faixa de audio contem amostras invalidas.");
            const auto value=(int16_t)std::lround(juce::jlimit(-1.f,1.f,sample)*32767.f);
            const size_t index=(size_t)(i*2+channel)*2;pcm[index]=(uint8_t)(value&255);pcm[index+1]=(uint8_t)((value>>8)&255);
        }
        if(!stream->write(pcm.data(),(size_t)count*4))return juce::Result::fail("Sem espaco para finalizar o audio.");
    }
    stream->flush();return stream->getStatus();
}
juce::Result MovieWriter::combine(const juce::File& video,const std::vector<AudioFile>& audio,const juce::File& destination,double duration)
{
    const auto pcm=destination.getSiblingFile("mix.pcm"),aac=destination.getSiblingFile("audio.m4a");
    auto result=mixPCM(audio,pcm,duration);if(result.failed())return result;
    return checked(juce::getEnv()->CallStaticObjectMethod(juce::ChurchCamMovieBridge,juce::ChurchCamMovieBridge.combine,
        juce::javaString(video.getFullPathName()).get(),juce::javaString(pcm.getFullPathName()).get(),juce::javaString(aac.getFullPathName()).get(),juce::javaString(destination.getFullPathName()).get(),duration));
}
void requestMediaPermission(bool camera,std::function<void(bool)> callback)
{
    juce::RuntimePermissions::request(camera?juce::RuntimePermissions::camera:juce::RuntimePermissions::recordAudio,
        [callback=std::move(callback)](bool allowed){if(juce::MessageManager::getInstance()->isThisTheMessageThread())callback(allowed);else juce::MessageManager::callAsync([callback,allowed]{callback(allowed);});});
}
}
#endif
