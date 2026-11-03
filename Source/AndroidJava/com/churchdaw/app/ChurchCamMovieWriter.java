package com.churchdaw.app;

import android.graphics.Bitmap;
import android.media.*;
import android.opengl.*;
import android.view.Surface;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/** Worker-thread MP4 encoder. No camera, microphone, network or audio-session ownership. */
public final class ChurchCamMovieWriter {
    private MediaCodec codec;
    private MediaMuxer muxer;
    private Surface input;
    private EGLDisplay display=EGL14.EGL_NO_DISPLAY;
    private EGLContext context=EGL14.EGL_NO_CONTEXT;
    private EGLSurface surface=EGL14.EGL_NO_SURFACE;
    private boolean codecStarted,muxerStarted,ended;
    private int track=-1,width,height,texture,program;
    private Bitmap bitmap;
    private FloatBuffer vertices;
    private long lastTime=-1;
    private String error="";

    public static boolean supported() {
        try {
            MediaCodecList list=new MediaCodecList(MediaCodecList.REGULAR_CODECS);
            MediaFormat v=MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC,1280,720);
            v.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            return list.findEncoderForFormat(v)!=null&&list.findEncoderForFormat(audioFormat())!=null;
        }catch(Exception failure){return false;}
    }
    private static String message(Exception e){return e.getMessage()==null?e.getClass().getSimpleName():e.getMessage();}
    public String getError(){return error;}
    public ChurchCamMovieWriter(String path,int w,int h) {
        try {
            if(w<2||h<2||(w&1)!=0||(h&1)!=0)throw new IllegalArgumentException("Dimensões de vídeo inválidas.");
            width=w;height=h;
            MediaFormat format=MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC,w,h);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            format.setInteger(MediaFormat.KEY_BIT_RATE,Math.min(16000000,w*h*8));
            format.setInteger(MediaFormat.KEY_FRAME_RATE,30);format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,2);
            format.setInteger(MediaFormat.KEY_PROFILE,MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
            codec=MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            codec.configure(format,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);
            input=codec.createInputSurface();codec.start();codecStarted=true;
            muxer=new MediaMuxer(path,MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            createGL();
            bitmap=Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);
        }catch(Exception failure){error="Não foi possível iniciar o codificador: "+message(failure);close();}
    }
    private static void require(boolean value,String message){if(!value)throw new IllegalStateException(message);}
    private int shader(int kind,String source) {
        int shader=GLES20.glCreateShader(kind);GLES20.glShaderSource(shader,source);GLES20.glCompileShader(shader);
        int[] status=new int[1];GLES20.glGetShaderiv(shader,GLES20.GL_COMPILE_STATUS,status,0);
        if(status[0]==0){String info=GLES20.glGetShaderInfoLog(shader);GLES20.glDeleteShader(shader);throw new IllegalStateException(info);}
        return shader;
    }
    private void createGL() {
        display=EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version=new int[2];require(EGL14.eglInitialize(display,version,0,version,1),"EGL não iniciou.");
        EGLConfig[] configs=new EGLConfig[1];int[] count=new int[1];
        int[] attrs={EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,
                EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,EGL14.EGL_SURFACE_TYPE,EGL14.EGL_WINDOW_BIT,
                0x3142,1,EGL14.EGL_NONE};
        require(EGL14.eglChooseConfig(display,attrs,0,configs,0,1,count,0)&&count[0]>0,"Superfície de gravação indisponível.");
        context=EGL14.eglCreateContext(display,configs[0],EGL14.EGL_NO_CONTEXT,new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE},0);
        surface=EGL14.eglCreateWindowSurface(display,configs[0],input,new int[]{EGL14.EGL_NONE},0);
        require(context!=EGL14.EGL_NO_CONTEXT&&surface!=EGL14.EGL_NO_SURFACE&&EGL14.eglMakeCurrent(display,surface,surface,context),"Contexto de vídeo indisponível.");
        int vs=shader(GLES20.GL_VERTEX_SHADER,"attribute vec2 position;attribute vec2 uv;varying vec2 tex;void main(){tex=uv;gl_Position=vec4(position,0.,1.);}");
        int fs=shader(GLES20.GL_FRAGMENT_SHADER,"precision mediump float;varying vec2 tex;uniform sampler2D image;void main(){gl_FragColor=texture2D(image,tex);}");
        program=GLES20.glCreateProgram();GLES20.glAttachShader(program,vs);GLES20.glAttachShader(program,fs);GLES20.glLinkProgram(program);
        GLES20.glDeleteShader(vs);GLES20.glDeleteShader(fs);
        int[] status=new int[1];GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,status,0);require(status[0]!=0,"Shader de vídeo indisponível.");
        int[] names=new int[1];GLES20.glGenTextures(1,names,0);texture=names[0];GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE);
        vertices=ByteBuffer.allocateDirect(16*4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        vertices.put(new float[]{-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0}).position(0);
    }
    private void draw(long timestamp) {
        drain(false);
        GLES20.glViewport(0,0,width,height);GLES20.glUseProgram(program);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
        int position=GLES20.glGetAttribLocation(program,"position"),uv=GLES20.glGetAttribLocation(program,"uv");
        vertices.position(0);GLES20.glVertexAttribPointer(position,2,GLES20.GL_FLOAT,false,16,vertices);GLES20.glEnableVertexAttribArray(position);
        vertices.position(2);GLES20.glVertexAttribPointer(uv,2,GLES20.GL_FLOAT,false,16,vertices);GLES20.glEnableVertexAttribArray(uv);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program,"image"),0);GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);
        require(GLES20.glGetError()==GLES20.GL_NO_ERROR,"Falha ao compor o quadro de vídeo.");
        require(EGLExt.eglPresentationTimeANDROID(display,surface,timestamp*1000),"Timestamp de vídeo recusado.");
        require(EGL14.eglSwapBuffers(display,surface),"Quadro de vídeo recusado.");lastTime=timestamp;
        drain(false);
    }
    public String append(int[] pixels,double seconds) {
        if(!error.isEmpty())return error;
        try {
            long timestamp=Math.max(lastTime+1,Math.round(seconds*1000000));
            bitmap.setPixels(pixels,0,width,0,0,width,height);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,texture);
            if(lastTime<0)GLUtils.texImage2D(GLES20.GL_TEXTURE_2D,0,bitmap,0);
            else GLUtils.texSubImage2D(GLES20.GL_TEXTURE_2D,0,0,0,bitmap);
            draw(timestamp);return "";
        }catch(Exception failure){error="Não foi possível gravar o quadro: "+message(failure);return error;}
    }
    private void drain(boolean finishing) {
        MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();long deadline=System.nanoTime()+10000000000L;
        while(true) {
            int index=codec.dequeueOutputBuffer(info,finishing?10000:0);
            if(index==MediaCodec.INFO_TRY_AGAIN_LATER){if(!finishing)return;require(System.nanoTime()<deadline,"O codificador de vídeo não finalizou.");continue;}
            if(index==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED){require(!muxerStarted,"Formato de vídeo mudou.");track=muxer.addTrack(codec.getOutputFormat());muxer.start();muxerStarted=true;continue;}
            if(index<0)continue;
            try {
                if((info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)!=0)info.size=0;
                if(info.size>0){require(muxerStarted,"Formato de vídeo ausente.");ByteBuffer data=codec.getOutputBuffer(index);data.position(info.offset);data.limit(info.offset+info.size);muxer.writeSampleData(track,data,info);}
                if((info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0){ended=true;return;}
            }finally{codec.releaseOutputBuffer(index,false);}
        }
    }
    public String finish(double seconds) {
        if(!error.isEmpty())return error;
        try {
            require(lastTime>=0,"A gravação não recebeu quadros.");
            long end=Math.round(seconds*1000000)-33333;
            if(end>lastTime+1000)draw(end);
            codec.signalEndOfInputStream();drain(true);require(ended,"Vídeo incompleto.");
            muxer.stop();muxerStarted=false;return "";
        }catch(Exception failure){error="Não foi possível finalizar o vídeo: "+message(failure);return error;}
        finally{close();}
    }
    public void close() {
        if(display!=EGL14.EGL_NO_DISPLAY){
            if(context!=EGL14.EGL_NO_CONTEXT&&surface!=EGL14.EGL_NO_SURFACE){
                EGL14.eglMakeCurrent(display,surface,surface,context);
                if(texture!=0)GLES20.glDeleteTextures(1,new int[]{texture},0);if(program!=0)GLES20.glDeleteProgram(program);
            }
            EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT);
            if(surface!=EGL14.EGL_NO_SURFACE)EGL14.eglDestroySurface(display,surface);
            if(context!=EGL14.EGL_NO_CONTEXT)EGL14.eglDestroyContext(display,context);
            EGL14.eglReleaseThread();EGL14.eglTerminate(display);
            display=EGL14.EGL_NO_DISPLAY;context=EGL14.EGL_NO_CONTEXT;surface=EGL14.EGL_NO_SURFACE;
        }
        if(input!=null){input.release();input=null;}
        if(codec!=null){try{if(codecStarted)codec.stop();}catch(Exception ignored){}codec.release();codec=null;codecStarted=false;}
        if(muxer!=null){try{if(muxerStarted)muxer.stop();}catch(Exception ignored){}muxer.release();muxer=null;muxerStarted=false;}
        if(bitmap!=null){bitmap.recycle();bitmap=null;}
    }
    private static MediaFormat audioFormat() {
        MediaFormat format=MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC,48000,2);
        format.setInteger(MediaFormat.KEY_AAC_PROFILE,MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        format.setInteger(MediaFormat.KEY_BIT_RATE,256000);format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE,16384);return format;
    }
    private static void encodeAudio(String raw,String destination) throws Exception {
        MediaCodec encoder=null;MediaMuxer mux=null;boolean running=false,started=false;int track=-1;
        try(RandomAccessFile pcm=new RandomAccessFile(raw,"r")) {
            require(pcm.length()>0&&pcm.length()%4==0,"Áudio PCM inválido.");
            encoder=MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);encoder.configure(audioFormat(),null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);encoder.start();running=true;
            mux=new MediaMuxer(destination,MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            long frames=0;boolean inputEnded=false,outputEnded=false;byte[] bytes=new byte[16384];MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            long deadline=System.nanoTime()+15000000000L;
            while(!outputEnded) {
                boolean progress=false;
                if(!inputEnded){int index=encoder.dequeueInputBuffer(10000);if(index>=0){
                    ByteBuffer input=encoder.getInputBuffer(index);input.clear();
                    int length=(int)Math.min(Math.min(bytes.length,input.capacity()&~3),pcm.length()-pcm.getFilePointer());
                    if(length>0){pcm.readFully(bytes,0,length);input.put(bytes,0,length);}
                    encoder.queueInputBuffer(index,0,length,frames*1000000L/48000,length==0?MediaCodec.BUFFER_FLAG_END_OF_STREAM:0);
                    frames+=length/4;inputEnded=length==0;progress=true;
                }}
                int output=encoder.dequeueOutputBuffer(info,10000);
                if(output==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED){require(!started,"Formato de áudio mudou.");track=mux.addTrack(encoder.getOutputFormat());mux.start();started=true;progress=true;}
                else if(output>=0){try{
                    if((info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)!=0)info.size=0;
                    if(info.size>0){require(started,"Formato de áudio ausente.");ByteBuffer data=encoder.getOutputBuffer(output);data.position(info.offset);data.limit(info.offset+info.size);mux.writeSampleData(track,data,info);}
                    outputEnded=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;progress=true;
                }finally{encoder.releaseOutputBuffer(output,false);}}
                if(progress)deadline=System.nanoTime()+15000000000L;else require(System.nanoTime()<deadline,"O codificador de áudio não respondeu.");
            }
            require(started,"Nenhum áudio foi codificado.");mux.stop();started=false;
        }finally{
            if(encoder!=null){try{if(running)encoder.stop();}catch(Exception ignored){}encoder.release();}
            if(mux!=null){try{if(started)mux.stop();}catch(Exception ignored){}mux.release();}
        }
    }
    private static MediaFormat select(MediaExtractor extractor,String prefix) {
        for(int i=0;i<extractor.getTrackCount();i++){MediaFormat format=extractor.getTrackFormat(i);String mime=format.getString(MediaFormat.KEY_MIME);if(mime!=null&&mime.startsWith(prefix)){extractor.selectTrack(i);return format;}}
        throw new IllegalStateException("Faixa "+prefix+" ausente.");
    }
    public static String combine(String video,String pcm,String audio,String destination,double duration) {
        MediaExtractor v=new MediaExtractor(),a=new MediaExtractor();MediaMuxer mux=null;boolean started=false;
        try {
            encodeAudio(pcm,audio);v.setDataSource(video);a.setDataSource(audio);
            MediaFormat vf=select(v,"video/"),af=select(a,"audio/");
            mux=new MediaMuxer(destination,MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            int vt=mux.addTrack(vf),at=mux.addTrack(af);mux.start();started=true;
            ByteBuffer buffer=ByteBuffer.allocateDirect(8*1024*1024);MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();long end=Math.round(duration*1000000);
            while(true){long tv=v.getSampleTime(),ta=a.getSampleTime();if(tv>end)tv=-1;if(ta>end)ta=-1;if(tv<0&&ta<0)break;
                boolean chooseVideo=tv>=0&&(ta<0||tv<=ta);MediaExtractor source=chooseVideo?v:a;
                buffer.clear();int size=source.readSampleData(buffer,0);require(size>=0&&size<=buffer.capacity(),"Amostra de mídia inválida.");
                // Extractor flags and codec flags are different namespaces.
                // These finalized local MP4 tracks contain complete, clear samples;
                // only the sync-frame flag is meaningful when remuxing them.
                int flags=(source.getSampleFlags()&MediaExtractor.SAMPLE_FLAG_SYNC)!=0
                    ?MediaCodec.BUFFER_FLAG_KEY_FRAME:0;
                info.set(0,size,source.getSampleTime(),flags);mux.writeSampleData(chooseVideo?vt:at,buffer,info);source.advance();
            }
            mux.stop();started=false;return "";
        }catch(Exception failure){return "Não foi possível juntar áudio e vídeo: "+message(failure);}
        finally{v.release();a.release();if(mux!=null){try{if(started)mux.stop();}catch(Exception ignored){}mux.release();}}
    }
}
