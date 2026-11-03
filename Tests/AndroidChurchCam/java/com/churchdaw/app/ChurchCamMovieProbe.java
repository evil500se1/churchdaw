package com.churchdaw.app;

import java.io.File;
import java.io.FileOutputStream;

/** Runs with app_process on a test device; included only in debug APKs. */
public final class ChurchCamMovieProbe {
    private static void check(String error) { if(!error.isEmpty()) throw new IllegalStateException(error); }
    public static void main(String[] args) throws Exception {
        File directory=new File(args[0]);directory.mkdirs();
        if(!ChurchCamMovieWriter.supported())throw new IllegalStateException("H.264/AAC unavailable");
        for(int width:new int[]{1280,1920}) {
            int height=width*9/16,frames=60;double duration=frames/30.0;
            File rawVideo=new File(directory,"video-"+width+".mp4");
            ChurchCamMovieWriter writer=new ChurchCamMovieWriter(rawVideo.getPath(),width,height);
            try {
                check(writer.getError());int[] pixels=new int[width*height];
                for(int frame=0;frame<frames;frame++) {
                    for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
                        int colour=y<height/2?(x<width/2?0xffff2020:0xff20ff20):(x<width/2?0xff2020ff:0xfffff0d0);
                        if(Math.abs(x-(frame*width/frames))<12)colour=0xff080808;
                        pixels[y*width+x]=colour;
                    }
                    check(writer.append(pixels,frame/30.0));
                }
                check(writer.finish(duration));
            } finally {writer.close();}
            File pcm=new File(directory,"audio-"+width+".pcm");
            try(FileOutputStream output=new FileOutputStream(pcm)) {
                byte[] block=new byte[1024*4];
                for(int frame=0;frame<96000;) {
                    int count=Math.min(1024,96000-frame);
                    for(int i=0;i<count;i++)for(int channel=0;channel<2;channel++) {
                        int value=(int)Math.round(0.2*32767*Math.sin(2*Math.PI*(channel==0?440:880)*(frame+i)/48000.0));
                        int index=(i*2+channel)*2;block[index]=(byte)value;block[index+1]=(byte)(value>>8);
                    }
                    output.write(block,0,count*4);frame+=count;
                }
            }
            File movie=new File(directory,"ChurchCam-probe-"+width+".mp4");
            check(ChurchCamMovieWriter.combine(rawVideo.getPath(),pcm.getPath(),new File(directory,"audio-"+width+".m4a").getPath(),movie.getPath(),duration));
            if(movie.length()<1000)throw new IllegalStateException("Empty output");
            System.out.println("MP4_PASS "+width+"x"+height+" "+movie.length());
        }
    }
}
