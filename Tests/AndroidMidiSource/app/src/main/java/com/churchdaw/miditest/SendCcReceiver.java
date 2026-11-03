package com.churchdaw.miditest;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Base64;
import android.util.Log;

import java.nio.charset.StandardCharsets;

public final class SendCcReceiver extends BroadcastReceiver {
    private static final String TAG = "ChurchMidiTest";

    @Override
    public void onReceive(Context context, Intent intent) {
        if ("com.churchdaw.miditest.EMIT_CHORDS".equals(intent.getAction())) {
            emitChordsAsync(intent);
            return;
        }

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

    private void emitChordsAsync(Intent intent) {
        final PendingResult pending = goAsync();
        final String encoded = intent.getStringExtra("chords_b64");
        final int cycles = clamp(intent.getIntExtra("cycles", 1), 1, 64);
        final int velocity = clamp(intent.getIntExtra("velocity", 110), 1, 127);
        final int holdMs = clamp(intent.getIntExtra("hold_ms", 420), 10, 5000);
        final int gapMs = clamp(intent.getIntExtra("gap_ms", 140), 0, 5000);
        final boolean sustain = intent.getBooleanExtra("sustain", true);

        new Thread(() -> {
            boolean sentAll = false;

            try {
                if (encoded == null || encoded.isEmpty()) {
                    throw new IllegalArgumentException("missing chords_b64");
                }

                final String decoded = new String(
                        Base64.decode(encoded, Base64.DEFAULT),
                        StandardCharsets.UTF_8);
                final String[] chordSpecs = decoded.split("\\|");

                if (chordSpecs.length == 0) {
                    throw new IllegalArgumentException("empty chord list");
                }

                sentAll = !sustain || MidiSourceService.sendCc(1, 64, 127);

                for (int cycle = 0; cycle < cycles; ++cycle) {
                    for (String chordSpec : chordSpecs) {
                        final int[] notes = parseNotes(chordSpec);

                        for (int note : notes) {
                            sentAll &= MidiSourceService.sendNote(
                                    1, note, velocity, true);
                        }

                        Thread.sleep(holdMs);

                        for (int note : notes) {
                            sentAll &= MidiSourceService.sendNote(
                                    1, note, 0, false);
                        }

                        if (gapMs > 0) {
                            Thread.sleep(gapMs);
                        }
                    }
                }

                if (sustain) {
                    sentAll &= MidiSourceService.sendCc(1, 64, 0);
                }

                sentAll &= MidiSourceService.sendCc(1, 123, 0);
                Log.i(TAG, "Dense chord run complete: cycles=" + cycles
                        + " chords=" + chordSpecs.length
                        + " sent=" + sentAll);
            } catch (Exception error) {
                Log.e(TAG, "Dense chord run failed", error);
                MidiSourceService.sendCc(1, 64, 0);
                MidiSourceService.sendCc(1, 123, 0);
            } finally {
                pending.setResultCode(sentAll ? 1 : 0);
                pending.finish();
            }
        }, "ChurchMidiDenseChords").start();
    }

    private static int[] parseNotes(String chordSpec) {
        final String[] values = chordSpec.split("-");
        final int[] notes = new int[values.length];

        for (int i = 0; i < values.length; ++i) {
            notes[i] = clamp(Integer.parseInt(values[i]), 0, 127);
        }

        return notes;
    }

    private static int clamp(int value, int low, int high) {
        return Math.max(low, Math.min(high, value));
    }
}
