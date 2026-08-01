package com.churchdaw.app;

import android.content.Context;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;

import java.util.Arrays;
import java.util.Comparator;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Observa apenas a topologia exposta pelo framework de audio do Android.
 *
 * Interfaces USB class-compliant chegam ao app como AudioDeviceInfo/AAudio;
 * nao abrimos endpoints USB e nao pedimos permissao UsbManager. O callback do
 * Android tambem nao entra no JUCE: ele somente incrementa uma geracao. O C++
 * consome essa geracao, uma vez por segundo, na message thread.
 */
public final class ChurchDAWAudioDeviceBridge {
    private static final AtomicLong topologyGeneration = new AtomicLong(0);
    private static final Object initLock = new Object();

    private static AudioManager audioManager;
    private static boolean initialized;

    private static final AudioDeviceCallback deviceCallback = new AudioDeviceCallback() {
        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] addedDevices) {
            if (containsUsbAudioDevice(addedDevices))
                topologyGeneration.incrementAndGet();
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removedDevices) {
            if (containsUsbAudioDevice(removedDevices))
                topologyGeneration.incrementAndGet();
        }
    };

    private ChurchDAWAudioDeviceBridge() {}

    public static void initialize(Context context) {
        synchronized (initLock) {
            if (initialized)
                return;

            final Context appContext = context.getApplicationContext();
            audioManager = (AudioManager) appContext.getSystemService(Context.AUDIO_SERVICE);

            if (audioManager != null) {
                audioManager.registerAudioDeviceCallback(
                    deviceCallback, new Handler(Looper.getMainLooper()));
                initialized = true;
            }

            // Forca um snapshot inicial mesmo quando nenhum callback de add e
            // entregue para os dispositivos que ja estavam conectados.
            topologyGeneration.incrementAndGet();
        }
    }

    public static void onResume() {
        // O Android pode reconstruir a rota enquanto a Activity esta em
        // background sem entregar o callback ao processo suspenso.
        topologyGeneration.incrementAndGet();
    }

    public static long getTopologyGeneration() {
        return topologyGeneration.get();
    }

    /** Retorna somente dados tecnicos da rota; nunca serial/USB endpoint. */
    public static String getSanitizedSnapshot() {
        return buildSnapshot(false);
    }

    /** Chave deterministica usada para ignorar callbacks/resumes sem mudanca USB. */
    public static String getUsbTopologyFingerprint() {
        return buildSnapshot(true);
    }

    private static String buildSnapshot(boolean usbOnly) {
        final AudioManager manager = audioManager;
        if (manager == null)
            return "audio_manager=unavailable";

        final AudioDeviceInfo[] devices = manager.getDevices(
            AudioManager.GET_DEVICES_INPUTS |
            AudioManager.GET_DEVICES_OUTPUTS).clone();

        Arrays.sort(devices, Comparator
            .comparingInt(AudioDeviceInfo::getId)
            .thenComparingInt(AudioDeviceInfo::getType)
            .thenComparing(device -> device.isSource() ? 0 : 1));

        final StringBuilder result = new StringBuilder();

        for (AudioDeviceInfo device : devices) {
            if (usbOnly && !isUsbAudioDevice(device))
                continue;

            if (result.length() != 0)
                result.append(';');

            result.append("id=").append(device.getId())
                  .append(",type=").append(device.getType())
                  .append(",role=").append(device.isSource() ? "in" : "out")
                  .append(",name=").append(sanitize(device.getProductName()))
                  .append(",rates=").append(Arrays.toString(device.getSampleRates()))
                  .append(",channels=").append(Arrays.toString(device.getChannelCounts()));
        }

        if (result.length() != 0)
            return result.toString();

        return usbOnly ? "usb=none" : "devices=none";
    }

    private static boolean containsUsbAudioDevice(AudioDeviceInfo[] devices) {
        if (devices == null)
            return false;

        for (AudioDeviceInfo device : devices)
            if (isUsbAudioDevice(device))
                return true;

        return false;
    }

    private static boolean isUsbAudioDevice(AudioDeviceInfo device) {
        if (device == null)
            return false;

        final int type = device.getType();
        return type == AudioDeviceInfo.TYPE_USB_DEVICE
            || type == AudioDeviceInfo.TYPE_USB_ACCESSORY
            || type == AudioDeviceInfo.TYPE_USB_HEADSET;
    }

    private static String sanitize(CharSequence value) {
        if (value == null)
            return "unknown";

        return value.toString()
            .replace(';', '_')
            .replace(',', '_')
            .replace('\n', ' ')
            .replace('\r', ' ')
            .trim();
    }
}
