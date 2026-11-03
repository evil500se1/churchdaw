package com.churchdaw.app;

import android.app.Activity;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.*;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;

/** Video-only Camera2 capture. It never opens an audio device or changes the DAW route. */
public final class ChurchCamCamera {
    private final Activity activity;
    private final long token;
    private final HandlerThread thread=new HandlerThread("ChurchCam Camera");
    private final Handler handler;
    private CameraDevice device;
    private CameraCaptureSession session;
    private ImageReader reader;
    private volatile boolean closed;
    private boolean reported;
    private int sensorOrientation;
    private boolean front;
    private long lastFrame;

    private static native void opened(long token,String error);
    private static native void failed(long token,String error);
    private static native void frame(long token,ByteBuffer y,ByteBuffer u,ByteBuffer v,
            int width,int height,int yRow,int uRow,int vRow,int uPixel,int vPixel,int rotation);

    private static ArrayList<String> ids(Context context) throws CameraAccessException {
        CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
        ArrayList<String> result=new ArrayList<>();
        if(manager==null)return result;
        for(String id:manager.getCameraIdList()) {
            Integer facing=manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
            if(facing!=null&&facing==CameraCharacteristics.LENS_FACING_FRONT)result.add(0,id);
            else result.add(id);
        }
        return result;
    }
    public static String[] names(Context context) {
        try {
            CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
            ArrayList<String> result=new ArrayList<>();
            for(String id:ids(context)) {
                Integer facing=manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                String name=facing!=null&&facing==CameraCharacteristics.LENS_FACING_FRONT?"Câmera frontal":"Câmera traseira";
                result.add(name+" · "+id);
            }
            return result.toArray(new String[0]);
        } catch(Exception error) { return new String[0]; }
    }
    public ChurchCamCamera(Activity activity,int index,long token) {
        this.activity=activity;this.token=token;
        thread.start();handler=new Handler(thread.getLooper());
        handler.post(()->open(index));
        handler.postDelayed(()->{if(!closed&&!reported)problem("A câmera demorou demais para iniciar.");},10000);
    }
    private void report(String error) {
        if(reported)return;
        reported=true;opened(token,error);
    }
    private void problem(String message) {
        if(closed)return;
        if(reported)failed(token,message);else report(message);
        close();
    }
    @SuppressWarnings({"MissingPermission","deprecation"})
    private void open(int index) {
        try {
            if(closed)return;
            CameraManager manager=(CameraManager)activity.getSystemService(Context.CAMERA_SERVICE);
            ArrayList<String> choices=ids(activity);
            if(index<0||index>=choices.size())throw new IllegalArgumentException("Câmera indisponível.");
            String id=choices.get(index);CameraCharacteristics info=manager.getCameraCharacteristics(id);
            Integer orientation=info.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sensorOrientation=orientation==null?0:orientation;
            Integer facing=info.get(CameraCharacteristics.LENS_FACING);
            front=facing!=null&&facing==CameraCharacteristics.LENS_FACING_FRONT;
            StreamConfigurationMap config=info.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size selected=null;double score=Double.MAX_VALUE;
            if(config!=null)for(Size size:config.getOutputSizes(ImageFormat.YUV_420_888)) {
                if(size.getWidth()>1280||size.getHeight()>720||size.getWidth()<320||size.getHeight()<240)continue;
                double candidate=Math.abs((double)size.getWidth()/size.getHeight()-16.0/9.0)*5000+Math.abs(size.getWidth()-1280);
                if(candidate<score){score=candidate;selected=size;}
            }
            if(selected==null)throw new IllegalStateException("Essa câmera não oferece uma prévia compatível.");
            reader=ImageReader.newInstance(selected.getWidth(),selected.getHeight(),ImageFormat.YUV_420_888,2);
            reader.setOnImageAvailableListener(r->{
                try(Image image=r.acquireLatestImage()) {
                    if(image==null||closed)return;
                    long now=System.nanoTime();if(now-lastFrame<30000000L)return;lastFrame=now;
                    int rotation=activity.getWindowManager().getDefaultDisplay().getRotation()*90;
                    int angle=(sensorOrientation+(front?rotation:-rotation)+360)%360;
                    Image.Plane[] p=image.getPlanes();
                    frame(token,p[0].getBuffer().slice(),p[1].getBuffer().slice(),p[2].getBuffer().slice(),
                        image.getWidth(),image.getHeight(),p[0].getRowStride(),p[1].getRowStride(),p[2].getRowStride(),p[1].getPixelStride(),p[2].getPixelStride(),angle);
                } catch(Exception error) {problem("A câmera interrompeu a captura: "+error.getMessage());}
            },handler);
            manager.openCamera(id,new CameraDevice.StateCallback(){
                @Override public void onOpened(CameraDevice camera) {
                    if(closed){camera.close();return;}device=camera;
                    try {
                        camera.createCaptureSession(Collections.singletonList(reader.getSurface()),new CameraCaptureSession.StateCallback(){
                            @Override public void onConfigured(CameraCaptureSession openedSession) {
                                if(closed){openedSession.close();return;}session=openedSession;
                                try {
                                    CaptureRequest.Builder request=device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                                    request.addTarget(reader.getSurface());
                                    int[] focusModes=info.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
                                    if(focusModes!=null)for(int mode:focusModes)
                                        if(mode==CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO){request.set(CaptureRequest.CONTROL_AF_MODE,mode);break;}
                                    Range<Integer>[] rates=info.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
                                    Range<Integer> best=null;int delta=Integer.MAX_VALUE;
                                    if(rates!=null)for(Range<Integer> rate:rates){int distance=Math.abs(rate.getUpper()-30)*100+Math.abs(rate.getLower()-30);if(distance<delta){best=rate;delta=distance;}}
                                    if(best!=null)request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,best);
                                    session.setRepeatingRequest(request.build(),null,handler);report("");
                                } catch(Exception error){problem("Não foi possível iniciar a câmera: "+error.getMessage());}
                            }
                            @Override public void onConfigureFailed(CameraCaptureSession failedSession){failedSession.close();problem("A câmera recusou a configuração de vídeo.");}
                        },handler);
                    }catch(Exception error){problem("Não foi possível configurar a câmera: "+error.getMessage());}
                }
                @Override public void onDisconnected(CameraDevice camera){camera.close();problem("A câmera foi desconectada.");}
                @Override public void onError(CameraDevice camera,int error){camera.close();problem("A câmera está indisponível ("+error+").");}
            },handler);
        }catch(Exception error){problem(error.getMessage()==null?"Não foi possível abrir a câmera.":error.getMessage());}
    }
    public void close() {
        if(closed)return;closed=true;
        handler.post(()->{
            if(session!=null){session.close();session=null;}
            if(device!=null){device.close();device=null;}
            if(reader!=null){reader.close();reader=null;}
            thread.quitSafely();
        });
    }
}
