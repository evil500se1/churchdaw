package com.churchdaw.app;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.provider.Settings;

import java.util.ArrayList;
import java.util.List;

/**
 * Permissoes e atalhos para MIDI Bluetooth LE no Android.
 *
 * O stack MIDI em si e do sistema + JUCE (MidiManager). Sem BLUETOOTH_CONNECT
 * (API 31+) o sistema simplesmente nao lista dispositivos BLE — e o app
 * parece "sem teclado" sem erro na tela.
 */
public final class ChurchDAWMidiBleBridge {
    private ChurchDAWMidiBleBridge() {}

    public static boolean deviceSupportsBluetooth(Context context) {
        if (context == null)
            return false;

        final PackageManager pm = context.getPackageManager();
        return pm != null
            && pm.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH)
            && pm.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE);
    }

    public static boolean isBluetoothEnabled(Context context) {
        if (context == null)
            return false;

        final BluetoothManager manager =
            (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null)
            return false;

        final BluetoothAdapter adapter = manager.getAdapter();
        return adapter != null && adapter.isEnabled();
    }

    public static boolean hasMidiBlePermissions(Activity activity) {
        if (activity == null)
            return false;

        if (Build.VERSION.SDK_INT < 31)
            return true;

        return activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                == PackageManager.PERMISSION_GRANTED
            && activity.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)
                == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Pede as permissoes de BLE MIDI se faltarem. requestCode e so ecoado ao
     * onRequestPermissionsResult do Activity; o C++ reagenda a varredura MIDI
     * no resume / timer seguinte.
     */
    public static boolean requestMidiBlePermissions(Activity activity, int requestCode) {
        if (activity == null || Build.VERSION.SDK_INT < 31)
            return false;

        final List<String> missing = new ArrayList<>();
        if (activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED)
            missing.add(Manifest.permission.BLUETOOTH_CONNECT);
        if (activity.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)
                != PackageManager.PERMISSION_GRANTED)
            missing.add(Manifest.permission.BLUETOOTH_SCAN);

        if (missing.isEmpty())
            return false;

        activity.requestPermissions(
            missing.toArray(new String[0]),
            requestCode);
        return true;
    }

    /** Abre os ajustes de Bluetooth do sistema (pareamento BLE MIDI). */
    public static boolean openBluetoothSettings(Activity activity) {
        if (activity == null || activity.isFinishing())
            return false;

        // isDestroyed() existe desde API 17; ChurchDAW minSdk é bem maior.
        if (Build.VERSION.SDK_INT >= 17 && activity.isDestroyed())
            return false;

        try {
            activity.startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS));
            return true;
        } catch (Throwable ignored) {
            try {
                activity.startActivity(new Intent(Settings.ACTION_SETTINGS));
                return true;
            } catch (Throwable ignored2) {
                return false;
            }
        }
    }

    public static String statusLine(Context context, Activity activity) {
        if (!deviceSupportsBluetooth(context))
            return "ble=unsupported";

        final boolean enabled = isBluetoothEnabled(context);
        final boolean perms = activity == null || hasMidiBlePermissions(activity);
        return "ble=ok enabled=" + (enabled ? "1" : "0")
            + " perms=" + (perms ? "1" : "0");
    }
}
