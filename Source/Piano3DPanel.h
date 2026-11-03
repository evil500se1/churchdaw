#pragma once
#include <juce_opengl/juce_opengl.h>
#include "PianoVisualState.h"
#include "BinaryData.h"
#include "Studio/StudioComposition.h"

class Piano3DView final : public juce::Component, private juce::OpenGLRenderer,
                          private juce::Timer
{
public:
    explicit Piano3DView (std::shared_ptr<PianoVisualState> s, std::shared_ptr<churchstudio::VisualState> studioState = {}) : state (std::move (s)), studio (std::move(studioState))
    {
        for (size_t i = 0; i < seen.size(); ++i)
            seen[i] = state->strikes[i+21].load (std::memory_order_relaxed);
        brand = juce::ImageCache::getFromMemory (BinaryData::logo_round_png, BinaryData::logo_round_pngSize);
        setOpaque (true);
        context.setRenderer (this);
        context.setComponentPaintingEnabled (false);
        context.setContinuousRepainting (false);
        context.setMultisamplingEnabled (studio==nullptr);
        if(studio){animating.store(false);setComponentID("liveCamKeyboard");setTitle("Teclado 3D e acordes ao vivo");}
        context.attachTo (*this);
        startTimerHz (30);
    }
    ~Piano3DView() override { stopTimer(); context.detach(); }
    void refreshStudio() { if(!suspended)context.triggerRepaint(); }
    void suspendCapture(){suspended=true;context.detach();}
    void resumeCapture(){if(suspended){suspended=false;context.attachTo(*this);context.triggerRepaint();}}
    ChordDetect::Result currentHarmony() const{const juce::ScopedLock lock(harmonyLock);return harmony;}
    void setLid (float value) { lid.store (value); context.triggerRepaint(); }
    void keyboardCamera() { focusKeyboard.store (true); azimuth.store (0.0f); elevation.store (1.05f); zoom.store (1.8f); context.triggerRepaint(); }
    void resetCamera() { focusKeyboard.store (false); azimuth.store (0.45f); elevation.store (0.52f); zoom.store (0.72f); context.triggerRepaint(); }
    void mouseDown (const juce::MouseEvent& e) override { lastMouse = e.position; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (studio) return;
        auto delta = e.position - lastMouse; lastMouse = e.position;
        azimuth.store (azimuth.load() + delta.x * .008f);
        elevation.store (juce::jlimit (.12f, 1.3f, elevation.load() + delta.y * .005f));
        context.triggerRepaint();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& d) override
    {
        if (studio) return;
        zoom.store (juce::jlimit (.4f, 2.4f, zoom.load() + d.deltaY * .3f));
        context.triggerRepaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { resetCamera(); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff13171f));
        g.setColour (juce::Colours::white);
        g.drawText (studio ? juce::String ("Studio: imagem indisponivel nesta GPU") : gpuFailed.load() ? juce::String::fromUTF8 ("Piano 3D indisponível nesta GPU. O áudio continua funcionando.") : juce::String ("Piano 3D: iniciando GPU"), getLocalBounds(), juce::Justification::centred);
    }
private:
    void timerCallback() override
    {
        if (suspended || ! isShowing()) return;
        if (gpuFailed.load())
        {
            context.setComponentPaintingEnabled (true);
            stopTimer(); repaint(); return;
        }
        bool changed = false;
        const bool harmonyMode=studio && studio->performanceMode.load();
        if(harmonyMode!=previousHarmonyMode)
        {
            previousHarmonyMode=harmonyMode;chordTracker.reset();
            {const juce::ScopedLock lock(harmonyLock);harmony={};}
            setTitle("Teclado 3D e acordes ao vivo");setDescription({});changed=true;
        }
        if(studio && studio->performanceMode.load())
        {
            bool held[128]{};
            for(size_t n=0;n<128;++n)held[n]=state->sounding[n].load(std::memory_order_relaxed)!=0;
            if(chordTracker.update(held,juce::Time::getMillisecondCounterHiRes()))
            {
                const auto& reading=chordTracker.value();
                {const juce::ScopedLock lock(harmonyLock);harmony=reading;}
                const auto prefix=reading.kind==ChordDetect::Kind::chord?"Acorde: ":reading.kind==ChordDetect::Kind::note?"Nota: ":reading.kind==ChordDetect::Kind::interval?"Intervalo: ":"Notas: ";
                setTitle(reading.name.isEmpty()?juce::String("Teclado 3D e acordes ao vivo"):juce::String(prefix)+reading.name);
                setDescription(reading.notes);changed=true;
            }
            const bool pedal=state->sustainChannels.load(std::memory_order_relaxed)!=0;
            if(pedalVisible.exchange(pedal)!=pedal)changed=true;
        }
        for (size_t i = 0; i < observed.size(); ++i)
        {
            const auto n = state->strikes[i+21].load (std::memory_order_relaxed);
            const auto c = state->channels[i+21].load (std::memory_order_relaxed);
            if (n != observed[i] || c != observedChannels[i]) changed = true;
            observed[i] = n; observedChannels[i] = c;
        }
        if (studio)
        {
            auto sequence=studio->cameraSequence.load();
            auto dawSequence=studio->dawSequence.load();
            changed=changed || sequence!=cameraSeen || dawSequence!=dawSeen || studio->recording.load();
            cameraSeen=sequence;dawSeen=dawSequence;
        }
        if (changed || animating.load()) context.triggerRepaint();
    }
    void newOpenGLContextCreated() override
    {
        using namespace juce::gl;

        shader = std::make_unique<juce::OpenGLShaderProgram> (context);
        const char* vertex = R"GLSL(
#ifdef GL_ES
precision highp float;
#endif
attribute vec3 position;
attribute vec3 normal;
attribute vec3 colour;
attribute float group;
uniform float keys[88];
uniform float lidAngle;
uniform float performance;
uniform vec4 camera;
uniform vec3 focus;
varying vec3 surfaceColour;
varying vec3 surfaceNormal;
varying vec3 worldPosition;
void main() {
    vec3 p = position; vec3 n = normal;
    float pressed = 0.0;
    if (group > 0.5 && group < 88.5) {
        int key = int(group - 1.0);
        pressed = keys[key];
        float a = pressed * 0.085;
        float c = cos(a), s = sin(a);
        p.y += 0.59; p.z -= 0.99;
        p.yz = mat2(c,s,-s,c) * p.yz;
        n.yz = mat2(c,s,-s,c) * n.yz;
        p.y -= 0.59; p.z += 0.99;
    } else if (group > 88.5) {
        float c = cos(lidAngle), s = sin(lidAngle);
        p.x += 0.78; p.z -= 1.11;
        p.xz = mat2(c,-s,s,c) * p.xz;
        n.xz = mat2(c,-s,s,c) * n.xz;
        p.x -= 0.78; p.z += 1.11;
    }
    worldPosition = p;
    vec3 q = p - focus;
    q.y *= mix(1.0, 0.48, performance);
    float ca = cos(camera.x), sa = sin(camera.x);
    float ce = cos(camera.y), se = sin(camera.y);
    float x = ca*q.x + sa*q.y;
    float depth = sa*q.x - ca*q.y;
    float y = ce*q.z - se*depth;
    float z = ce*depth + se*q.z;
    gl_Position = vec4(x*camera.z/camera.w, y*camera.z*mix(1.0,0.72,performance), -z*0.18, 1.0);
    surfaceColour = mix(colour, vec3(0.62,0.07,0.17), performance*pressed*0.78);
    surfaceNormal = n;
})GLSL";
        const char* fragment = R"GLSL(
#ifdef GL_ES
precision highp float;
#endif
varying vec3 surfaceColour;
varying vec3 surfaceNormal;
varying vec3 worldPosition;
void main() {
    vec3 n = normalize(surfaceNormal);
    vec3 light = normalize(vec3(-1.5,-2.0,4.0));
    float diffuse = max(dot(n, light), 0.0);
    float fill = max(dot(n, normalize(vec3(2.0,1.0,1.0))), 0.0);
    float spec = pow(max(dot(n, normalize(vec3(0.1,-0.6,1.0))), 0.0), 48.0);
    float lacquer = 1.0 - smoothstep(0.025, 0.08, max(surfaceColour.r, max(surfaceColour.g,surfaceColour.b)));
    float softbox = pow(max(dot(n,normalize(vec3(-0.7,-0.25,1.0))),0.0),8.0);
    vec3 reflection = vec3(0.014,0.020,0.029) + vec3(0.055,0.065,0.080)*softbox;
    vec3 c = surfaceColour*(0.32+0.75*diffuse+0.25*fill) + lacquer*reflection + vec3(0.12,0.15,0.18)*spec;
    gl_FragColor = vec4(pow(c,vec3(0.4545)),1.0);
})GLSL";
        if (! shader->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (vertex))
            || ! shader->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragment))
            || ! shader->link())
        {
            juce::Logger::writeToLog ("Piano 3D shader: " + shader->getLastError());
            shader.reset(); gpuFailed.store (true); return;
        }
        context.extensions.glGenBuffers (1, &buffer);
        context.extensions.glBindBuffer (GL_ARRAY_BUFFER, buffer);
        // Reuse the beveled Blender keys, uploading only 11,616 vertices for Live Cam.
        // The full piano, lid and strings never enter this lightweight composition.
        if(studio)
        {
            std::vector<float> vertices;
            const auto* bytes=reinterpret_cast<const unsigned char*>(BinaryData::concert_grand_mesh);
            for(int offset=0;offset+40<=BinaryData::concert_grand_meshSize;offset+=40)
            {
                std::array<float,10> v;std::memcpy(v.data(),bytes+offset,40);
                if(v[9]>.5f && v[9]<88.5f)vertices.insert(vertices.end(),v.begin(),v.end());
            }
            vertexCount=(int)vertices.size()/10;
            context.extensions.glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vertices.size()*sizeof(float)),vertices.data(),GL_STATIC_DRAW);
        }
        else
        {
            vertexCount=BinaryData::concert_grand_meshSize/(10*(int)sizeof(float));
            context.extensions.glBufferData(GL_ARRAY_BUFFER,BinaryData::concert_grand_meshSize,BinaryData::concert_grand_mesh,GL_STATIC_DRAW);
        }
        for (int i = 0; i < 4; ++i)
            attributes[(size_t) i] = context.extensions.glGetAttribLocation (shader->getProgramID(),
                std::array<const char*, 4> { "position", "normal", "colour", "group" }[(size_t) i]);
        keyUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "keys");
        performanceUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform>(*shader,"performance");
        lidUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "lidAngle");
        cameraUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "camera");
        focusUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "focus");
    }
    void renderOpenGL() override
    {
        using namespace juce::gl;
        const auto originalTarget = juce::OpenGLFrameBuffer::getCurrentFrameBufferTarget();
        const int requestedWidth=studio?studio->frameWidth():0;
        if(studio && (!studioBuffer.isValid() || studioBuffer.getWidth()!=requestedWidth))
        {
            studioBuffer.release();
            if(!studioBuffer.initialise(context,requestedWidth,requestedWidth*9/16)){gpuFailed.store(true);return;}
        }
        if(studio)studioBuffer.makeCurrentRenderingTarget();
        juce::OpenGLHelpers::clear (juce::Colour (0xff13171f));
        if (!studio && !shader) { context.extensions.glBindFramebuffer(GL_FRAMEBUFFER,originalTarget); return; }
        auto scale = context.getRenderingScale();
        const int renderWidth=studio?requestedWidth:juce::roundToInt(getWidth()*scale);
        const int renderHeight=studio?requestedWidth*9/16:juce::roundToInt(getHeight()*scale);
        glViewport (0, 0, renderWidth, renderHeight);
        const bool performance=studio && studio->performanceMode.load();
        if(performance && !wasPerformance)
        {
            // Enter on the current chord; old strikes from DAW mode must not flash.
            for(size_t i=0;i<keys.size();++i)
            {seen[i]=state->strikes[i+21].load();pulse[i]=0;keys[i]=state->channels[i+21].load()!=0?1.0f:0.0f;}
        }
        wasPerformance=performance;
        if(!studio || performance)updateKeys();else animating.store(false);
        if(!studio)drawKeys(renderWidth,renderHeight,false);
        glDisable (GL_DEPTH_TEST);
        if(studio)
        {
            if(performance && !renderKeyboardLayer(renderWidth))return;
            studioBuffer.makeCurrentRenderingTarget();glViewport(0,0,renderWidth,renderHeight);glDisable(GL_DEPTH_TEST);
            {
                auto graphicsContext=juce::createOpenGLGraphicsContext(context,studioBuffer);
                if(graphicsContext)
                {
                    juce::Graphics g(*graphicsContext);
                    g.addTransform(juce::AffineTransform::scale((float)renderWidth/1280.0f));
                    churchstudio::paintComposition(g,*studio,brand);
                    if(performance)
                    {
                        // GPU texture composition: no intermediate CPU image/readback.
                        g.setOpacity(1.0f);
                        g.drawImage(keyboardLayer,churchstudio::performanceKeyboardBounds().toFloat());
                        churchstudio::paintPerformanceNotes(g,currentHarmony(),pedalVisible.load());
                    }
                }
            }
            if(studio->recording.load())
            {
                juce::Image image(juce::Image::ARGB,renderWidth,renderHeight,false,juce::SoftwareImageType());
                {juce::Image::BitmapData data(image,juce::Image::BitmapData::writeOnly);
                 studioBuffer.readPixels(reinterpret_cast<juce::PixelARGB*>(data.data),image.getBounds(),juce::OpenGLFrameBuffer::RowOrder::fromTopDown);}
                studio->submit(image,juce::Time::getMillisecondCounterHiRes());
            }
            context.extensions.glBindFramebuffer(GL_FRAMEBUFFER,originalTarget);
            const int sw=juce::roundToInt(getWidth()*scale),sh=juce::roundToInt(getHeight()*scale);
            glViewport(0,0,sw,sh);glBindTexture(GL_TEXTURE_2D,studioBuffer.getTextureID());
            context.copyTexture({0,0,sw,sh},{0,0,sw,sh},sw,sh,false,false);
        }
    }
    bool renderKeyboardLayer(int renderWidth)
    {
        using namespace juce::gl;
        const auto r=(churchstudio::performanceKeyboardBounds().toFloat()*((float)renderWidth/1280.0f)).toNearestInt();
        if(!keyboardLayer.isValid() || keyboardLayer.getWidth()!=r.getWidth())
        {
            if(depthBuffer){context.extensions.glDeleteRenderbuffers(1,&depthBuffer);depthBuffer=0;}
            keyboardLayer=juce::Image(juce::Image::ARGB,r.getWidth(),r.getHeight(),true,juce::OpenGLImageType());
            auto* target=juce::OpenGLImageType::getFrameBufferFrom(keyboardLayer);
            if(!target){gpuFailed.store(true);return false;}
            target->makeCurrentRenderingTarget();
            context.extensions.glGenRenderbuffers(1,&depthBuffer);
            context.extensions.glBindRenderbuffer(GL_RENDERBUFFER,depthBuffer);
            context.extensions.glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,r.getWidth(),r.getHeight());
            context.extensions.glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depthBuffer);
            context.extensions.glBindRenderbuffer(GL_RENDERBUFFER,0);
            if(context.extensions.glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){gpuFailed.store(true);return false;}
        }
        auto* target=juce::OpenGLImageType::getFrameBufferFrom(keyboardLayer);
        if(!target)return false;
        target->makeCurrentRenderingTarget();glViewport(0,0,r.getWidth(),r.getHeight());
        glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
       #if JUCE_OPENGL_ES
        glClearDepthf(1.0f);
       #else
        glClearDepth(1.0);
       #endif
        juce::OpenGLHelpers::clear(juce::Colours::transparentBlack);
        drawKeys(r.getWidth(),r.getHeight(),true);glDisable(GL_DEPTH_TEST);
        return true;
    }
    void updateKeys()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit (0.001, 0.1, (now-lastFrame)*.001); lastFrame = now;
        bool moving = false;
        for (size_t i = 0; i < keys.size(); ++i)
        {
            const auto serial = state->strikes[i+21].load (std::memory_order_relaxed);
            if (serial != seen[i]) { seen[i] = serial; pulse[i] = .055f; }
            const bool down = state->channels[i+21].load (std::memory_order_relaxed) != 0 || pulse[i] > 0;
            pulse[i] = juce::jmax (0.0f, pulse[i]-dt);
            moving = moving || pulse[i] > 0 || std::abs ((down ? 1.0f : 0.0f)-keys[i]) > .001f;
            keys[i] += ((down ? 1.0f : 0.0f)-keys[i]) * (1.0f-std::exp (-dt*(down ? 45.0f : 20.0f)));
        }
        lidPosition += (lid.load()-lidPosition)*(1.0f-std::exp (-dt*9.0f));
        animating.store (moving || std::abs (lid.load()-lidPosition) > .001f);
    }
    void drawKeys(int renderWidth,int renderHeight,bool performance)
    {
        using namespace juce::gl;
        if(!shader)return;
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
        shader->use(); performanceUniform->set(performance?1.0f:0.0f); keyUniform->set (keys.data(), 88); lidUniform->set (-lidPosition*.837758f);
        focusUniform->set (0.0f, performance?-.792f:focusKeyboard.load()?-.65f:.15f, performance?1.003f:focusKeyboard.load()?.99f:.92f);
        cameraUniform->set (performance?0.0f:azimuth.load(), performance?1.0f:elevation.load(), performance?10.3f:zoom.load(),
                             (float) renderWidth / (float) juce::jmax (1, renderHeight));
        context.extensions.glBindBuffer (GL_ARRAY_BUFFER, buffer);
        for (int i = 0; i < 4; ++i)
        {
            const auto index = (GLuint) attributes[(size_t) i];
            context.extensions.glEnableVertexAttribArray (index);
            context.extensions.glVertexAttribPointer (index, i==3 ? 1 : 3, GL_FLOAT, GL_FALSE,
                10*sizeof(float), reinterpret_cast<const void*> ((size_t) i*3*sizeof(float)));
        }
        glDrawArrays (GL_TRIANGLES, 0, vertexCount);
        for (auto index : attributes) context.extensions.glDisableVertexAttribArray ((GLuint) index);
        context.extensions.glBindBuffer (GL_ARRAY_BUFFER, 0);
    }
    void openGLContextClosing() override
    {
        if(depthBuffer)context.extensions.glDeleteRenderbuffers(1,&depthBuffer);depthBuffer=0;
        keyboardLayer={};studioBuffer.release();
        performanceUniform.reset();keyUniform.reset(); lidUniform.reset(); cameraUniform.reset(); focusUniform.reset(); shader.reset();
        if (buffer != 0) context.extensions.glDeleteBuffers (1, &buffer);
        buffer = 0;
    }
    LiveChordTracker chordTracker;
    bool previousHarmonyMode=false;
    mutable juce::CriticalSection harmonyLock;
    ChordDetect::Result harmony;
    std::atomic<bool> pedalVisible{false};
    std::shared_ptr<PianoVisualState> state;
    std::shared_ptr<churchstudio::VisualState> studio;
    juce::OpenGLFrameBuffer studioBuffer;
    uint64_t cameraSeen=0,dawSeen=0;
    juce::Image brand,keyboardLayer;
    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> keyUniform, lidUniform, cameraUniform, focusUniform, performanceUniform;
    unsigned int buffer = 0,depthBuffer=0;
    int vertexCount=0;
    bool wasPerformance=false,suspended=false;
    std::array<int, 4> attributes {};
    std::array<float, 88> keys {}, pulse {};
    std::array<uint32_t, 88> seen {}, observed {}, observedChannels {};
    std::atomic<bool> animating { true }, gpuFailed { false }, focusKeyboard { false };
    std::atomic<float> lid {1.0f}, azimuth {.45f}, elevation {.52f}, zoom {.72f};
    float lidPosition = 1.0f;
    double lastFrame = 0;
    juce::Point<float> lastMouse;
};

class Piano3DPanel final : public juce::Component
{
public:
    explicit Piano3DPanel (std::shared_ptr<PianoVisualState> s) : view (std::move (s))
    {
        addAndMakeVisible (view); addAndMakeVisible (lid); addAndMakeVisible (title); addAndMakeVisible (reset); addAndMakeVisible (keyboard);
        title.setText ("CONCERT GRAND   /   MIDI AO VIVO", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, juce::Colour (0xffd2c4a9));
        lid.setRange (0, 100, 1); lid.setValue (100); lid.setTextValueSuffix ("% tampa");
        lid.setSliderStyle (juce::Slider::LinearHorizontal);
        lid.setTextBoxStyle (juce::Slider::TextBoxRight, false, 96, 24);
        lid.onValueChange = [this] { view.setLid ((float) lid.getValue()*.01f); };
        reset.onClick = [this] { view.resetCamera(); };
        keyboard.onClick = [this] { view.keyboardCamera(); };
        setSize (980, 700);
    }
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff13171f)); }
    void resized() override
    {
        auto r = getLocalBounds(); auto bar = r.removeFromTop (52).reduced (16, 8);
        reset.setBounds (bar.removeFromRight (110)); bar.removeFromRight (12);
        keyboard.setBounds (bar.removeFromRight (90)); bar.removeFromRight (12);
        lid.setBounds (bar.removeFromRight (240)); title.setBounds (bar);
        view.setBounds (r);
    }
private:
    Piano3DView view;
    juce::Slider lid;
    juce::Label title;
    juce::TextButton reset { "Piano inteiro" }, keyboard { "Teclado" };
};
