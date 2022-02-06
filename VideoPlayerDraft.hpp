#include "yasio/detail/byte_buffer.hpp"

#include "renderer/backend/ProgramCache.h"

#include <deque>

#if defined(_WIN32)
#include "MFMediaPlayer.h"
#include "ntcvt/ntcvt.hpp"
#include <thread>
#endif

#if defined(__ANDROID__)
#include "platform/android/jni/AndroidJavaMediaPlayer.h"
#endif

using namespace cocos2d;

// pal7xuan1.mp4

#define PS_SET_UNIFORM(ps,k,v) ps->setUniform(ps->getUniformLocation(k), &v, sizeof(v))

std::string fragNV12 = R"(
#ifdef GL_ES
varying lowp vec4 v_fragmentColor;
varying mediump vec2 v_texCoord;
#else
varying vec4 v_fragmentColor;
varying vec2 v_texCoord;
#endif

uniform sampler2D u_texture; // Y sample
uniform sampler2D u_texture1; // UV sample

void main()
{
    // refer to: 
    // a. https://gist.github.com/crearo/0d50442145b63c6c288d1c1675909990
    // b. https://github.com/tqk2811/TqkLibrary.Media.VideoPlayer/blob/38a2dce908215045cc27cffb741a6e4b8492c9cd/TqkLibrary.Media.VideoPlayer.OpenGl/Renders/NV12Render.cs#L14
    
    float cy = v_texCoord.y + 0.01625; // why needs adjust?
    vec4 uvColor = texture2D(u_texture1, vec2(v_texCoord.x, cy));
    vec3 yuv = vec3(texture2D(u_texture, v_texCoord).r, uvColor.r - 0.5, uvColor.a - 0.5);

    vec3 rgb = mat3( 1.0,       1.0,       1.0,
                     0,        -0.39465,   2.03211,
                     1.13983,  -0.58060,   0 ) * yuv;

    gl_FragColor = v_fragmentColor * vec4(rgb, 1.0);
}
)";

class VideoPlayer : public cocos2d::Sprite
{
public:
    bool initWithVideoFile(const std::string& videoFile) {
#if defined(__ANDROID__)
        mplayer = new FJavaAndroidMediaPlayer(false, false, false);
        auto fullPath = FileUtils::getInstance()->fullPathForFilename(videoFile);
        mplayer->SetDataSource(fullPath);
        mplayer->Prepare();

        vw = mplayer->GetVideoWidth();
        vh = mplayer->GetVideoHeight();

        texelsCount = vw * vh * 4;
        yasio::byte_buffer vbuf{ texelsCount, 0x00, std::true_type{} };

        auto texture = new Texture2D();
        bool succeed = texture->initWithData(vbuf.data(), vbuf.size(), PixelFormat::RGBA8, vw, vh, Vec2{ (float)vw, (float)vh }, false);
        initWithTexture(texture);

        setScale(1280.0f / vw);
        return true;
#endif
        HWND hwndGLView = Director::getInstance()->getOpenGLView()->getWin32Window();
        auto hr = MFMediaPlayer::CreateInstance(hwndGLView, &mplayer);
        bool ok = hr == S_OK;

        if (ok) {
            // Invoke at other thread
            mplayer->SampleEvent = [=](uint8_t* sampleBuffer, size_t len) {

                {
                    std::lock_guard<std::recursive_mutex> lck(_samplesMtx);
                    // _samples.push_front(yasio::byte_buffer{ sampleBuffer, sampleBuffer + len, std::true_type{} });

                    _frameData.assign(sampleBuffer, sampleBuffer + len);
                    _framesDirty = true;
                }

                // _sampleCV.notify_one();
                //_lastFrameData.resize_fit(vw * vh * 4);
                //int ret = libyuv::YUY2ToARGB(yuvData, vw * 2, _lastFrameData.data(), vw * 4, vw, vh);
                //_frameDataDirty = true;
            };

#if 0 // libyuv decoder too slow, needs do at thread to ensure GL thread 60fps
            std::thread decoder([=] {
                for (;;) {
                    std::unique_lock<std::recursive_mutex> lck(_samplesMtx);
                    while (_samples.empty())
                        _sampleCV.wait(lck);

                    auto samples = std::move(_samples);
                    lck.unlock();

                    if (!samples.empty()) {
                        auto& front = samples.front();
#if 0
                        _frameData.resize_fit(vw * vh * 4, 0xaf);
                        // libyuv::NV12ToARGB(front.data(), vw * 2, _frameData.data(), vw * 4, vw, vh);
                        int ret = libyuv::YUY2ToARGB(front.data(), vw * 2, _frameData.data(), vw * 4, vw, vh);
#else
                        _frameData = front;
#endif
                        samples.clear(); // clear all
                        _framesDirty = true;
                    }

                    //std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                });
            decoder.detach();
#endif

            auto fullPath = FileUtils::getInstance()->fullPathForFilename(videoFile);
            auto wpath = ntcvt::from_chars(fullPath);
            hr = mplayer->OpenURL(wpath.c_str());
            mplayer->SetLooping(TRUE);
            // mplayer->SetRate(2.5f);
        }
        return ok;
    }

    void setLoop(bool bVal) {
#if defined(__ANDROID__)
        mplayer->SetLooping(bVal);
#endif
    }

    void draw(cocos2d::Renderer* renderer, const cocos2d::Mat4 &transform, uint32_t flags) override
    {
//        if (isPlaying)
//        {
//            if (mode == PlayMode::STEP)
//            {
//                update(step);
//            }
//            else if (mode == PlayMode::FRAME)
//            {
//                update(-1);
//            }
//            if (texureDirty && vbuf)
//            {
//                _texture->updateWithData(vbuf, 0, 0, _texture->getPixelsWide(), _texture->getPixelsHigh());
//            }
//        }

        if (texureDirty)
        {
            // _texture->updateWithData(vbuf, 0, 0, _texture->getPixelsWide(), _texture->getPixelsHigh(), false, 0, 0);
        }
#if defined(__ANDROID__)
        if(mplayer) {
            if(!started && mplayer->IsPrepared()) {
                mplayer->SetLooping(true);
                mplayer->SetVideoEnabled();
                mplayer->SetAudioEnabled();
                mplayer->Start();

                started = true;
            }
            void* Buffer = nullptr;
            int64 BufferCount = 0;

            const int32 CurrentFramePosition = mplayer->GetCurrentPosition();
            // const FTimespan Time = FTimespan::FromMilliseconds(CurrentFramePosition);
            if (mplayer->GetVideoLastFrameData(Buffer, BufferCount)) {
                //memcpy(vbuf.data(), Buffer, vbuf.size()); // vbuf.copy_n((char*)Buffer, BufferCount);
                //_texture->updateWithData(vbuf.data(), vbuf.size(), PixelFormat::RGBA8, PixelFormat::RGBA8, vw, vh, Vec2{(float)vw, (float)vh}, 0);

                _texture->updateWithData((uint8_t*)Buffer, BufferCount, PixelFormat::RGBA8, PixelFormat::RGBA8, vw, vh, Vec2{(float)vw, (float)vh}, 0);
            }
        }
#endif
        if (_framesDirty) {
            std::lock_guard<std::recursive_mutex> lck(_samplesMtx);
            if (!_frameData.empty()) {
#if 0
                _texture->updateWithData((uint8_t*)_frameData.data(), _frameData.size(), 
                    PixelFormat::RGBA8, PixelFormat::RGBA8, 
                    vw, 
                    vh, 
                    Vec2{ (float)vw, (float)vh }, 
                    0);
#else
                if (mplayer->GetState() != PlayerState::Started)
                    return;

                if (!_initialized) {
                    vw = mplayer->GetVideoWidth();
                    vh = mplayer->GetVideoHeight();

                    auto programCache = backend::ProgramCache::getInstance();

                    

                    // fragNV12 = NV12_FRAG;

                    programCache->registerCustomProgramFactory(2022, positionTextureColor_vert, fragNV12);
                    auto program = programCache->getCustomProgram(2022);
                    setProgramState(new backend::ProgramState(program), false);

                    _initialized = true;
                }

                uint8_t* nv12Data = _frameData.data();
                auto w = vw;
                auto h = vh;


                // refer to: https://www.cnblogs.com/nanqiang/p/10224867.html
                bool needsInit = !_texture;
                if (!_texture) {
                    _texture = new Texture2D();
                    _texture1 = new Texture2D();
                }

                _texture->updateWithData(nv12Data, w * h,
                    PixelFormat::L8, PixelFormat::L8,
                    w,
                    h,
                    false,
                    0);

                _texture->updateWithData(nv12Data + w * h, _frameData.size(),
                    PixelFormat::LA8, PixelFormat::LA8,
                    w >> 1,
                    h >> 1,
                    false,
                    1);

                if (needsInit) {
                    initWithTexture(_texture);
                    setScale(1280.0f / vw * 0.7);

                    //float myScale = 1280.0f / vw * 0.6;
                    //
                    //
                    //float frameWidth = vw;
                    //float frameHeight = vh;
                    //float texl_w = 1 / frameWidth;
                    //PS_SET_UNIFORM(_programState, "tex_w", frameWidth);
                    //PS_SET_UNIFORM(_programState, "tex_h", frameHeight);
                    //PS_SET_UNIFORM(_programState, "texl_w", texl_w);

                    //_texture1Location = _programState->getUniformLocation("u_texture1");
                    //_programState->setTexture(_texture1Location, 0, _texture1->getBackendTexture());
                }
                // updateProgramStateTexture(_texture);

                                // Force set premultipliedAlpha to true
                //_texture->setPremultipliedAlpha(true);
                //_texture1->setPremultipliedAlpha(true);

                //updateBlendFunc();
#endif
            }
            _framesDirty = false;
        }

        cocos2d::Sprite::draw(renderer, transform, flags);
    }

private:
#if defined(__ANDROID__)
    FJavaAndroidMediaPlayer* mplayer = nullptr;
#endif
    bool texureDirty = false;
    size_t texelsCount = 0;
    int vw = 0, vh = 0;

    bool started = false;

    MFMediaPlayer* mplayer = nullptr;

    Texture2D* _texture1 = nullptr;
    backend::UniformLocation _texture1Location;

    bool _initialized = false; // does custom video render program state initialized


    bool _framesDirty = false;
    std::deque<yasio::byte_buffer> _samples; // The YUY2 samples
    std::recursive_mutex _samplesMtx;
    std::condition_variable_any _sampleCV;

    yasio::byte_buffer _frameData; // The RGBA frame data
};
