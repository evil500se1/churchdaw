#include <atomic>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

namespace
{
using namespace Steinberg;
using namespace Steinberg::Vst;

const TUID validClassId = INLINE_UID (
    0x7c48d001, 0x6f734894, 0xb8512a61, 0x8f020001);
const TUID crashingClassId = INLINE_UID (
    0x7c48d002, 0x6f734894, 0xb8512a61, 0x8f020002);

constexpr const char* validClassName = "ChurchDAW Fixture Valid";
constexpr const char* crashingClassName = "ChurchDAW Fixture Crash";

bool matches (const void* lhs, const void* rhs)
{
    return FUnknownPrivate::iidEqual (lhs, rhs);
}

class ValidComponent final : public IComponent,
                             public IAudioProcessor,
                             public IEditController
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (object == nullptr)
            return kInvalidArgument;

        *object = nullptr;

        if (matches (iid, FUnknown::iid)
             || matches (iid, IPluginBase::iid)
             || matches (iid, IComponent::iid))
            *object = static_cast<IComponent*> (this);
        else if (matches (iid, IAudioProcessor::iid))
            *object = static_cast<IAudioProcessor*> (this);
        else if (matches (iid, IEditController::iid))
            *object = static_cast<IEditController*> (this);
        else
            return kNoInterface;

        addRef();
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override
    {
        return references.fetch_add (1, std::memory_order_relaxed) + 1;
    }

    uint32 PLUGIN_API release() override
    {
        const auto remaining = references.fetch_sub (
            1, std::memory_order_acq_rel) - 1;

        if (remaining == 0)
            delete this;

        return remaining;
    }

    tresult PLUGIN_API initialize (FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    tresult PLUGIN_API getControllerClassId (TUID classId) override
    {
        if (classId != nullptr)
            std::memset (classId, 0, sizeof (TUID));

        // O controller vive no mesmo objeto e e encontrado por queryInterface.
        return kResultFalse;
    }

    tresult PLUGIN_API setIoMode (IoMode) override { return kResultOk; }

    int32 PLUGIN_API getBusCount (MediaType type,
                                  BusDirection direction) override
    {
        if (type == kAudio && direction == kOutput)
            return 1;

        if (type == kEvent && direction == kInput)
            return 1;

        return 0;
    }

    tresult PLUGIN_API getBusInfo (MediaType type,
                                   BusDirection direction,
                                   int32 index,
                                   BusInfo& info) override
    {
        if (index != 0
             || getBusCount (type, direction) != 1)
            return kInvalidArgument;

        std::memset (&info, 0, sizeof (info));
        info.mediaType = type;
        info.direction = direction;
        info.channelCount = type == kAudio ? 2 : 16;
        info.busType = kMain;
        info.flags = BusInfo::kDefaultActive;
        info.name[0] = type == kAudio ? (TChar) 'O' : (TChar) 'M';
        return kResultTrue;
    }

    tresult PLUGIN_API getRoutingInfo (RoutingInfo&, RoutingInfo&) override
    {
        return kNotImplemented;
    }

    tresult PLUGIN_API activateBus (MediaType type,
                                    BusDirection direction,
                                    int32 index,
                                    TBool) override
    {
        return index == 0 && getBusCount (type, direction) == 1
            ? kResultTrue : kInvalidArgument;
    }

    tresult PLUGIN_API setActive (TBool) override { return kResultOk; }
    tresult PLUGIN_API setState (IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState (IBStream*) override { return kResultOk; }

    tresult PLUGIN_API setBusArrangements (
        SpeakerArrangement*, int32 numInputs,
        SpeakerArrangement* outputs, int32 numOutputs) override
    {
        return numInputs == 0 && numOutputs == 1
             && outputs != nullptr
             && outputs[0] == SpeakerArr::kStereo
            ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API getBusArrangement (BusDirection direction,
                                          int32 index,
                                          SpeakerArrangement& arrangement) override
    {
        if (direction != kOutput || index != 0)
            return kInvalidArgument;

        arrangement = SpeakerArr::kStereo;
        return kResultTrue;
    }

    tresult PLUGIN_API canProcessSampleSize (int32 symbolicSampleSize) override
    {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing (ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }
    tresult PLUGIN_API process (ProcessData&) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return kNoTail; }

    tresult PLUGIN_API setComponentState (IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 0; }

    tresult PLUGIN_API getParameterInfo (int32, ParameterInfo&) override
    {
        return kInvalidArgument;
    }

    tresult PLUGIN_API getParamStringByValue (ParamID, ParamValue,
                                              String128) override
    {
        return kInvalidArgument;
    }

    tresult PLUGIN_API getParamValueByString (ParamID, TChar*,
                                              ParamValue&) override
    {
        return kInvalidArgument;
    }

    ParamValue PLUGIN_API normalizedParamToPlain (ParamID,
                                                  ParamValue value) override
    {
        return value;
    }

    ParamValue PLUGIN_API plainParamToNormalized (ParamID,
                                                  ParamValue value) override
    {
        return value;
    }

    ParamValue PLUGIN_API getParamNormalized (ParamID) override { return 0.0; }

    tresult PLUGIN_API setParamNormalized (ParamID, ParamValue) override
    {
        return kInvalidArgument;
    }

    tresult PLUGIN_API setComponentHandler (IComponentHandler*) override
    {
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView (FIDString) override { return nullptr; }

private:
    std::atomic<uint32> references { 1 };
};

class FixtureFactory final : public IPluginFactory
{
public:
    tresult PLUGIN_API queryInterface (const TUID iid, void** object) override
    {
        if (object == nullptr)
            return kInvalidArgument;

        *object = nullptr;

        if (! matches (iid, FUnknown::iid)
             && ! matches (iid, IPluginFactory::iid))
            return kNoInterface;

        *object = static_cast<IPluginFactory*> (this);
        addRef();
        return kResultOk;
    }

    uint32 PLUGIN_API addRef() override
    {
        return references.fetch_add (1, std::memory_order_relaxed) + 1;
    }

    uint32 PLUGIN_API release() override
    {
        const auto remaining = references.fetch_sub (
            1, std::memory_order_acq_rel) - 1;

        if (remaining == 0)
            delete this;

        return remaining;
    }

    tresult PLUGIN_API getFactoryInfo (PFactoryInfo* info) override
    {
        if (info == nullptr)
            return kInvalidArgument;

        *info = PFactoryInfo { "ChurchDAW Tests", "https://churchdaw.com",
                              "support@churchdaw.com", PFactoryInfo::kUnicode };
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return 2; }

    tresult PLUGIN_API getClassInfo (int32 index, PClassInfo* info) override
    {
        if (info == nullptr || index < 0 || index >= countClasses())
            return kInvalidArgument;

        *info = index == 0
            ? PClassInfo { validClassId, PClassInfo::kManyInstances,
                           kVstAudioEffectClass, validClassName }
            : PClassInfo { crashingClassId, PClassInfo::kManyInstances,
                           kVstAudioEffectClass, crashingClassName };
        return kResultOk;
    }

    tresult PLUGIN_API createInstance (FIDString classId, FIDString iid,
                                       void** object) override
    {
        if (classId == nullptr || iid == nullptr || object == nullptr)
            return kInvalidArgument;

        *object = nullptr;

        if (matches (classId, crashingClassId))
        {
            // Falha fatal deterministica, sem dialogo WER. O processo que
            // carrega esta classe e sempre o filho descartavel do scanner.
            SetErrorMode (GetErrorMode() | SEM_FAILCRITICALERRORS
                          | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
            TerminateProcess (GetCurrentProcess(), 0xe0000001u);
            return kInternalError;
        }

        if (! matches (classId, validClassId))
            return kNoInterface;

        auto* component = new ValidComponent();
        const auto result = component->queryInterface (iid, object);
        component->release();
        return result;
    }

private:
    std::atomic<uint32> references { 1 };
};
}

extern "C" __declspec(dllexport) bool PLUGIN_API InitDll()
{
    return true;
}

extern "C" __declspec(dllexport) bool PLUGIN_API ExitDll()
{
    return true;
}

extern "C" __declspec(dllexport) Steinberg::IPluginFactory* PLUGIN_API
GetPluginFactory()
{
    return new FixtureFactory();
}
