#include "StudioCamera_android.h"
#include "StudioAndroidLifecycle.h"
#if JUCE_ANDROID
#include <jni.h>
#include <juce_core/native/juce_JNIHelpers_android.h>
#include <map>
#include <atomic>

namespace churchstudio
{
struct AndroidCaptureLifecycle::Impl final : juce::ActivityLifecycleCallbacks
{
    explicit Impl(std::function<void(bool)> callback): changed(std::move(callback)) {}
    void onActivityStopped(jobject activity) override { dispatch(activity, true); }
    void onActivityStarted(jobject activity) override { dispatch(activity, false); }
    void dispatch(jobject activity, bool stopped)
    {
        const auto main=juce::getMainActivity();
        if(main && juce::getEnv()->IsSameObject(main.get(),activity))changed(stopped);
    }
    std::function<void(bool)> changed;
    juce::ActivityLifecycleCallbackForwarder forwarder { juce::GlobalRef { juce::getAppContext() }, this };
};
AndroidCaptureLifecycle::AndroidCaptureLifecycle(std::function<void(bool)> callback):impl(std::make_unique<Impl>(std::move(callback))) {}
AndroidCaptureLifecycle::~AndroidCaptureLifecycle()=default;

struct AndroidCamera::Impl
{
    juce::GlobalRef camera;
    juce::CriticalSection lock;
    juce::ListenerList<Listener> listeners;
    Callback onOpen;
    std::function<void(const juce::String&)> onError;
    std::atomic<bool> stopped{false};
    juce::int64 token=0;
};
static juce::CriticalSection registryLock;
static std::map<juce::int64,std::shared_ptr<AndroidCamera::Impl>> cameras;
static std::atomic<juce::int64> nextToken{1};
static std::shared_ptr<AndroidCamera::Impl> lookup(jlong token)
{
    const juce::ScopedLock lock(registryLock);auto it=cameras.find(token);
    return it==cameras.end()?nullptr:it->second;
}
static void JNICALL opened(JNIEnv*,jclass,jlong token,jstring error);
static void JNICALL failed(JNIEnv*,jclass,jlong token,jstring error);
static void JNICALL frame(JNIEnv*,jclass,jlong,jobject,jobject,jobject,jint,jint,jint,jint,jint,jint,jint,jint);
}
namespace juce
{
#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
 METHOD(construct,"<init>","(Landroid/app/Activity;IJ)V") \
 METHOD(close,"close","()V") \
 STATICMETHOD(names,"names","(Landroid/content/Context;)[Ljava/lang/String;") \
 CALLBACK(churchstudio::opened,"opened","(JLjava/lang/String;)V") \
 CALLBACK(churchstudio::failed,"failed","(JLjava/lang/String;)V") \
 CALLBACK(churchstudio::frame,"frame","(JLjava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIIIIIII)V")
DECLARE_JNI_CLASS(ChurchCamCameraBridge,"com/churchdaw/app/ChurchCamCamera")
#undef JNI_CLASS_MEMBERS
}
namespace churchstudio
{
AndroidCamera::AndroidCamera(std::shared_ptr<Impl> value):impl(std::move(value)){}
AndroidCamera::~AndroidCamera()
{
    auto p=std::move(impl);p->stopped.store(true);
    {const juce::ScopedLock lock(p->lock);p->listeners.clear();p->onError=nullptr;}
    {const juce::ScopedLock lock(registryLock);cameras.erase(p->token);}
    if(p->camera!=nullptr)juce::getEnv()->CallVoidMethod(p->camera.get(),juce::ChurchCamCameraBridge.close);
}
void AndroidCamera::addListener(Listener* l){const juce::ScopedLock lock(impl->lock);impl->listeners.add(l);}
void AndroidCamera::removeListener(Listener* l){const juce::ScopedLock lock(impl->lock);impl->listeners.remove(l);}
void AndroidCamera::setErrorCallback(std::function<void(const juce::String&)> fn){const juce::ScopedLock lock(impl->lock);impl->onError=std::move(fn);}
juce::StringArray AndroidCamera::getAvailableDevices()
{
    auto* env=juce::getEnv();juce::LocalRef<jobjectArray> names((jobjectArray)env->CallStaticObjectMethod(juce::ChurchCamCameraBridge,juce::ChurchCamCameraBridge.names,juce::getAppContext().get()));
    if(env->ExceptionCheck()){env->ExceptionClear();return {};}
    return names!=nullptr?juce::javaStringArrayToJuce(names):juce::StringArray{};
}
void AndroidCamera::openDeviceAsync(int index,Callback callback,int,int,int,int,bool)
{
    auto p=std::make_shared<Impl>();p->token=nextToken.fetch_add(1);p->onOpen=std::move(callback);
    {const juce::ScopedLock lock(registryLock);cameras.emplace(p->token,p);}
    auto* env=juce::getEnv();auto activity=juce::getCurrentActivity();
    if(activity==nullptr){{const juce::ScopedLock lock(registryLock);cameras.erase(p->token);}p->onOpen(nullptr,"Abra o ChurchDAW para usar a camera.");return;}
    juce::LocalRef<jobject> object(env->NewObject(juce::ChurchCamCameraBridge,juce::ChurchCamCameraBridge.construct,activity.get(),(jint)index,(jlong)p->token));
    if(env->ExceptionCheck()||object==nullptr)
    {
        env->ExceptionClear();{const juce::ScopedLock lock(registryLock);cameras.erase(p->token);}
        p->onOpen(nullptr,"Nao foi possivel iniciar a camera.");return;
    }
    p->camera=juce::GlobalRef(object);
}
static void JNICALL opened(JNIEnv*,jclass,jlong token,jstring error)
{
    auto p=lookup(token);if(!p)return;auto message=juce::juceString(error);
    juce::MessageManager::callAsync([p,message]
    {
        if(p->stopped.load())return;auto callback=std::move(p->onOpen);if(!callback)return;
        if(message.isEmpty())callback(new AndroidCamera(p),{});
        else {auto close=std::unique_ptr<AndroidCamera>(new AndroidCamera(p));callback(nullptr,message);}
    });
}
static void JNICALL failed(JNIEnv*,jclass,jlong token,jstring error)
{
    auto p=lookup(token);if(!p)return;auto message=juce::juceString(error);
    juce::MessageManager::callAsync([p,message]{
        std::function<void(const juce::String&)> fn;
        {const juce::ScopedLock lock(p->lock);if(p->stopped.load())return;fn=p->onError;}
        if(fn)fn(message);
    });
}
static void JNICALL frame(JNIEnv* env,jclass,jlong token,jobject yData,jobject uData,jobject vData,
                         jint w,jint h,jint yr,jint ur,jint vr,jint up,jint vp,jint angle)
{
    auto p=lookup(token);if(!p||p->stopped.load()||w<2||h<2||w>1920||h>1080||yr<w||ur<=0||vr<=0||up<=0||vp<=0)return;
    auto* y=(const uint8_t*)env->GetDirectBufferAddress(yData);
    auto* u=(const uint8_t*)env->GetDirectBufferAddress(uData);
    auto* v=(const uint8_t*)env->GetDirectBufferAddress(vData);
    if(!y||!u||!v||env->GetDirectBufferCapacity(yData)<(jlong)(h-1)*yr+w
       ||env->GetDirectBufferCapacity(uData)<(jlong)((h-1)/2)*ur+((w-1)/2)*up+1
       ||env->GetDirectBufferCapacity(vData)<(jlong)((h-1)/2)*vr+((w-1)/2)*vp+1)return;
    const bool quarter=angle==90||angle==270;
    juce::Image image(juce::Image::ARGB,quarter?h:w,quarter?w:h,false,juce::SoftwareImageType());
    juce::Image::BitmapData target(image,juce::Image::BitmapData::writeOnly);
    for(int row=0;row<h;++row)for(int col=0;col<w;++col)
    {
        const int yy=juce::jmax(0,(int)y[row*yr+col]-16),uu=(int)u[(row/2)*ur+(col/2)*up]-128,vv=(int)v[(row/2)*vr+(col/2)*vp]-128;
        int dx=col,dy=row;
        if(angle==90){dx=h-1-row;dy=col;}else if(angle==180){dx=w-1-col;dy=h-1-row;}else if(angle==270){dx=row;dy=w-1-col;}
        auto* pixel=reinterpret_cast<juce::PixelARGB*>(target.getPixelPointer(dx,dy));
        pixel->setARGB(255,(uint8_t)juce::jlimit(0,255,(298*yy+409*vv+128)>>8),
                          (uint8_t)juce::jlimit(0,255,(298*yy-100*uu-208*vv+128)>>8),
                          (uint8_t)juce::jlimit(0,255,(298*yy+516*uu+128)>>8));
    }
    const juce::ScopedLock lock(p->lock);
    if(!p->stopped.load())p->listeners.call([&](AndroidCamera::Listener& l){l.imageReceived(image);});
}
}
#endif
