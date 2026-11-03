#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>

#include <array>
#include <chrono>
#include <thread>

namespace
{
MIDIClientRef midiClient = 0;
MIDIEndpointRef midiSource = 0;

bool sendMidi (UInt8 status, UInt8 data1, UInt8 data2)
{
    MIDIPacketList packetList {};
    auto* packet = MIDIPacketListInit (&packetList);
    const UInt8 bytes[] { status, data1, data2 };
    packet = MIDIPacketListAdd (&packetList, sizeof (packetList), packet,
                                0, sizeof (bytes), bytes);
    return packet != nullptr && MIDIReceived (midiSource, &packetList) == noErr;
}

void sendDenseSequence()
{
    static constexpr std::array<std::array<UInt8, 16>, 4> chords {{
        {{ 36,40,43,48,52,55,60,64,67,72,76,79,84,88,91,96 }},
        {{ 38,41,45,50,53,57,62,65,69,74,77,81,86,89,93,98 }},
        {{ 33,36,40,45,48,52,57,60,64,69,72,76,81,84,88,93 }},
        {{ 35,38,43,47,50,55,59,62,67,71,74,79,83,86,91,95 }}
    }};

    std::this_thread::sleep_for (std::chrono::seconds (5));
    for (int cycle = 0; cycle < 24; ++cycle)
    {
        const auto& chord = chords[(size_t) cycle % chords.size()];
        sendMidi (0xb0, 64, 127);
        for (const auto note : chord) sendMidi (0x90, note, 118);
        std::this_thread::sleep_for (std::chrono::milliseconds (420));
        for (const auto note : chord) sendMidi (0x80, note, 0);
        sendMidi (0xb0, 64, 0);
        std::this_thread::sleep_for (std::chrono::milliseconds (45));
    }
}
}

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) AVAudioEngine* audioEngine;
@property(nonatomic, strong) AVAudioPlayerNode* silentPlayer;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options
{
    (void) application; (void) options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController* controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.blackColor;
    UILabel* label = [[UILabel alloc] initWithFrame:controller.view.bounds];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.textColor = UIColor.whiteColor;
    label.text = @"ChurchDAW MIDI QA\nAcordes densos começam em 5 segundos";
    [controller.view addSubview:label];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];

    MIDIClientCreate (CFSTR("ChurchDAW iOS QA Client"), nullptr, nullptr, &midiClient);
    MIDISourceCreate (midiClient, CFSTR("ChurchDAW iOS QA Source"), &midiSource);

    AVAudioSession* session = AVAudioSession.sharedInstance;
    [session setCategory:AVAudioSessionCategoryPlayback error:nil];
    [session setActive:YES error:nil];
    self.audioEngine = [AVAudioEngine new];
    self.silentPlayer = [AVAudioPlayerNode new];
    [self.audioEngine attachNode:self.silentPlayer];
    AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
    [self.audioEngine connect:self.silentPlayer to:self.audioEngine.mainMixerNode format:format];
    AVAudioPCMBuffer* silence = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:4800];
    silence.frameLength = 4800;
    [self.silentPlayer scheduleBuffer:silence atTime:nil options:AVAudioPlayerNodeBufferLoops completionHandler:nil];
    [self.audioEngine startAndReturnError:nil];
    [self.silentPlayer play];

    std::thread (sendDenseSequence).detach();
    return YES;
}

- (void)dealloc
{
    if (midiSource != 0) MIDIEndpointDispose (midiSource);
    if (midiClient != 0) MIDIClientDispose (midiClient);
}
@end

int main (int argc, char* argv[])
{
    @autoreleasepool
    {
        return UIApplicationMain (argc, argv, nil, NSStringFromClass (AppDelegate.class));
    }
}
