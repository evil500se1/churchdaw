package com.churchdaw.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Servico em primeiro plano (mediaPlayback) do ChurchDAW.
 *
 * Nao toca nada: o audio continua no motor nativo. Ele existe para manter o
 * processo vivo e fora do congelador do sistema quando o musico troca de app
 * (letra, cifra) ou bloqueia a tela; sem ele o Android/HyperOS congela o app em
 * segundo plano e o stream de audio quebra. A notificacao e a contrapartida
 * obrigatoria do Android para esse tipo de servico.
 */
public final class ChurchDAWAudioService extends Service {
    private static final String CHANNEL_ID = "churchdaw_audio";
    private static final int NOTIFICATION_ID = 4601;

    /** Chamar com o app visivel: a partir do segundo plano o Android 12+ recusa iniciar o servico. */
    static void start(Context context) {
        final Intent intent = new Intent(context, ChurchDAWAudioService.class);
        try {
            if (Build.VERSION.SDK_INT >= 26)
                context.startForegroundService(intent);
            else
                context.startService(intent);
        } catch (RuntimeException ignored) {
            // Sem o servico o app segue como antes; so perde a protecao em segundo plano.
        }
    }

    static void stop(Context context) {
        context.stopService(new Intent(context, ChurchDAWAudioService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            final Notification notification = buildNotification();
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(NOTIFICATION_ID, notification,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            else
                startForeground(NOTIFICATION_ID, notification);
        } catch (RuntimeException e) {
            // Aparelho/ROM que recusa o tipo do servico: nao derruba o app.
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private Notification buildNotification() {
        final Notification.Builder builder;

        if (Build.VERSION.SDK_INT >= 26) {
            final NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null && manager.getNotificationChannel(CHANNEL_ID) == null) {
                final NotificationChannel channel = new NotificationChannel(
                        CHANNEL_ID, "Áudio do ChurchDAW", NotificationManager.IMPORTANCE_LOW);
                channel.setDescription("Mantém o som ativo enquanto você usa outro app.");
                channel.setShowBadge(false);
                manager.createNotificationChannel(channel);
            }
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }

        final Intent open = new Intent(this, ChurchDAWActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        final PendingIntent content = PendingIntent.getActivity(
                this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        final int icon = getResources().getIdentifier("ic_stat_churchdaw", "drawable", getPackageName());

        builder.setSmallIcon(icon != 0 ? icon : android.R.drawable.ic_media_play)
                .setContentTitle("ChurchDAW")
                .setContentText("Áudio ativo — toque para voltar ao palco")
                .setContentIntent(content)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setVisibility(Notification.VISIBILITY_PUBLIC);

        if (Build.VERSION.SDK_INT >= 31)
            builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);

        return builder.build();
    }
}
