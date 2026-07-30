package com.churchdaw.miditest;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

public final class SendCcReceiver extends BroadcastReceiver {
    private static final String TAG = "ChurchMidiTest";

    @Override
    public void onReceive(Context context, Intent intent) {
        final String type = intent.getStringExtra("type");
        final int channel = intent.getIntExtra("channel", 1);
        final boolean sent;

        if ("note_on".equals(type) || "note_off".equals(type)) {
            final int note = intent.getIntExtra("note", 60);
            final int velocity = intent.getIntExtra("velocity", 90);
            sent = MidiSourceService.sendNote(
                    channel, note, velocity, "note_on".equals(type));
        } else {
            final int cc = intent.getIntExtra("cc", 74);
            final int value = intent.getIntExtra("value", 64);
            sent = MidiSourceService.sendCc(channel, cc, value);
        }

        setResultCode(sent ? 1 : 0);
        Log.i(TAG, "Broadcast result=" + sent);
    }
}
