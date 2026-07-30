package com.churchdaw.miditest;

import android.media.midi.MidiDeviceService;
import android.media.midi.MidiReceiver;
import android.util.Log;

import java.io.IOException;

public final class MidiSourceService extends MidiDeviceService {
    private static final String TAG = "ChurchMidiTest";
    private static volatile MidiSourceService instance;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        Log.i(TAG, "Virtual MIDI source ready");
    }

    @Override
    public void onDestroy() {
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }

    @Override
    public MidiReceiver[] onGetInputPortReceivers() {
        return new MidiReceiver[0];
    }

    static boolean sendCc(int channel, int cc, int value) {
        channel = Math.max(1, Math.min(16, channel));
        cc = Math.max(0, Math.min(127, cc));
        value = Math.max(0, Math.min(127, value));
        return sendMessage(new byte[] {
                (byte) (0xB0 | (channel - 1)),
                (byte) cc,
                (byte) value
        }, "CC " + cc + " value " + value + " channel " + channel);
    }

    static boolean sendNote(int channel, int note, int velocity, boolean noteOn) {
        channel = Math.max(1, Math.min(16, channel));
        note = Math.max(0, Math.min(127, note));
        velocity = Math.max(0, Math.min(127, velocity));
        return sendMessage(new byte[] {
                (byte) ((noteOn ? 0x90 : 0x80) | (channel - 1)),
                (byte) note,
                (byte) velocity
        }, (noteOn ? "Note on " : "Note off ") + note
                + " velocity " + velocity + " channel " + channel);
    }

    private static boolean sendMessage(byte[] message, String description) {
        final MidiSourceService service = instance;

        if (service == null) {
            Log.e(TAG, "MIDI not sent: service is not open");
            return false;
        }

        final MidiReceiver[] outputs = service.getOutputPortReceivers();

        if (outputs.length == 0) {
            Log.e(TAG, "MIDI not sent: output port is missing");
            return false;
        }

        try {
            outputs[0].send(message, 0, message.length, 0);
            Log.i(TAG, "Sent " + description);
            return true;
        } catch (IOException error) {
            Log.e(TAG, "MIDI send failed", error);
            return false;
        }
    }
}
