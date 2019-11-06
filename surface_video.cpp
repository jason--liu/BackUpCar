#include "surface_video.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <string.h>
#include <unistd.h>

#define TAG "surfacevideo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
// using namespace android;
namespace android {
// 视频源大小
//顶点着色器glsl
#define GET_STR(x) #x  //将传入的x直接转换为字符串且加了引号，比较清晰
static const char *vertexShader =
    GET_STR(attribute vec4 aPosition;  //顶点坐标？
            attribute vec2 aTexCoord;  //材质顶点坐标
            varying vec2 vTexCoord;  //输出材质坐标,输出给片元着色器
            void main() {
                vTexCoord = vec2(
                    aTexCoord.x,
                    1.0 - aTexCoord.y);  //转换成LCD显示坐标，即原点在左上角
                gl_Position = aPosition;
            });  //参考ijkplay

// 片元着色器
// p表示平面存储，即Y存完了再存U,V ffmpeg软解码和部分x86硬解码出来的格式
static const char *fragYUV420p =
    GET_STR(precision mediump float;  //精度
            varying vec2 vTexCoord;   //顶点着色器传递的坐标
            // 三个输入参数，输入材质（灰度材质，单像素）
            uniform sampler2D yTexture; uniform sampler2D uTexture;
            uniform sampler2D vTexture; void main() {
                vec3 yuv;
                vec3 rgb;
                yuv.r = texture2D(yTexture, vTexCoord).r;
                yuv.g = texture2D(uTexture, vTexCoord).r - 0.5;
                yuv.b = texture2D(vTexture, vTexCoord).r - 0.5;
                rgb = mat3(1.0, 1.0, 1.0, 0.0, -0.39465, 2.03211, 1.13983,
                           -0.58060, 0.0) *
                      yuv;
                //输出像素颜色
                gl_FragColor = vec4(rgb, 1.0);
            });

GLint InitShader(const char *code, GLint type)
{
    // 创建shader
    GLint sh = glCreateShader(type);
    if (!sh) {
        LOGE("glCreateShader faild %d", type);
        return 0;
    }
    // 加载shader
    glShaderSource(sh,
                   1,      // shader数量
                   &code,  // shader执行代码
                   0);  //第4个参数表示代码长度，0表示直接找字符串结尾
    //编译shader
    glCompileShader(sh);
    //获取编译情况
    GLint status;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        LOGE("glGetShaderiv failed type 0x%04x", type);
        return 0;
    }
    return sh;
}

int SurfaceVideo::GLSurfaceInit()
{
    // sp<ProcessState> proc(ProcessState::self());
    //  ProcessState::self()->startThreadPool();

    // create a client to surfaceflinger
    client = new SurfaceComposerClient();

    control = client->createSurface(String8("surface"), 893, 447,
                                    PIXEL_FORMAT_RGB_565, 0);
    SurfaceComposerClient::openGlobalTransaction();
    control->setLayer(0x10000000);
    control->setPosition(387, 0);
    SurfaceComposerClient::closeGlobalTransaction();
    s = control->getSurface();

    // TODO
    // 1.获取原始窗口
    // ANativeWindow *nwin = ANativeWindow_fromSurface(env, surface);

    // 创建EGL
    // 1.create display
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        LOGE("get egldisplay failed");
        return -1;
    }
    if (EGL_TRUE != eglInitialize(display, 0, 0)) {
        LOGE("egl initialize failed");
        return -1;
    }

    // 2.create surface
    // 2.1 surface配置，surface可以理解为窗口
    EGLConfig config;  //下面函数的输出
    EGLint confignum;
    EGLint configSpec[] = {EGL_RED_SIZE,  8, EGL_GREEN_SIZE,   8,
                           EGL_BLUE_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                           EGL_NONE};  //输入
    eglChooseConfig(display, configSpec, &config, 1,
                    &confignum);  // 1表示最多存储1个配置项
                                  // 2.2 create surface
    winsurface = eglCreateWindowSurface(display, config, s.get(), NULL);
    if (winsurface == EGL_NO_SURFACE) {
        LOGE("egl surface initialize failed");
        return -1;
    }

    // 3.create context
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context =
        eglCreateContext(display, config, EGL_NO_CONTEXT,
                         ctxAttr);  //第个参数表示多个设备共享上下文，这里用不到
    if (context == EGL_NO_CONTEXT) {
        LOGE("egl context initialize failed");
        return -1;
    }

    if (EGL_TRUE != eglMakeCurrent(display, winsurface, winsurface, context)) {
        //保证opengl函数和egl关联起来
        LOGE("egl eglMakeCurrent failed");
        return -1;
    }
    LOGD("EGL init success");

    // shader初始化
    // 顶点shader初始化
    vsh = InitShader(vertexShader, GL_VERTEX_SHADER);
    // 片元yuv420p shader初始化
    fsh = InitShader(fragYUV420p, GL_FRAGMENT_SHADER);

    //////////////// 创建渲染程序 //////////////
    program = glCreateProgram();
    if (!program) {
        LOGE("glCreateProgram failed");
        return -1;
    }
    // 到这里就表示程序开始正常运行了
    // 渲染程序中加入着色器代码
    glAttachShader(program, vsh);
    glAttachShader(program, fsh);

    // 链接程序
    glLinkProgram(program);
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        LOGE("glLink failed");
        return -1;
    }
    LOGD("glLink success");
    // 激活渲染程序
    glUseProgram(program);
    ////////////////////////////////////////////

    // 加入三维顶点数据 由两个三角形组成正方形
    static float vers[] = {
        1.0f, -1.0f, 0.0f, -1.0f, -1.0f, 0.0f,
        1.0f, 1.0f,  0.0f, -1.0f, 1.0f,  0.0f,
    };
    GLuint apos = glGetAttribLocation(program, "aPosition");  //返回值要转换？
    glEnableVertexAttribArray(apos);
    // 传递顶点坐标
    glVertexAttribPointer(
        apos, 3, GL_FLOAT, GL_FALSE, 12,
        vers);  // 3表示一个点有xyz三个元素，12表示点存储间隔，3个浮点数占3x4=12字节
    // 加入材质坐标数据
    static float txts[] = {1.0f, 0.0f,  //右下
                           0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    GLuint atex = glGetAttribLocation(program, "aTexCoord");
    glEnableVertexAttribArray(atex);
    glVertexAttribPointer(atex, 2, GL_FLOAT, GL_FALSE, 8, txts);

    // 材质纹理初始化
    // 设置纹理层 将shader和yuv材质绑定？
    glUniform1i(glGetUniformLocation(program, "yTexture"), 0);  //对应材质第一层
    glUniform1i(glGetUniformLocation(program, "uTexture"), 1);  //对应材质第二层
    glUniform1i(glGetUniformLocation(program, "vTexture"), 2);  //对应材质第三层
    // IPCThreadState::self()->joinThreadPool();
    return 0;
}

void SurfaceVideo::GetTexture(unsigned int index, int width, int height,
                              unsigned char *buf)
{
    unsigned int format = GL_LUMINANCE;

    if (texts[index] == 0) {
        //材质初始化
        glGenTextures(1, &texts[index]);

        //设置纹理属性
        glBindTexture(GL_TEXTURE_2D, texts[index]);
        //缩小的过滤器
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        //设置纹理的格式和大小
        glTexImage2D(GL_TEXTURE_2D,
                     0,              //细节基本 0默认
                     format,         // gpu内部格式 亮度，灰度图
                     width, height,  //拉升到全屏
                     0,              //边框
                     format,  //数据的像素格式 亮度，灰度图 要与上面一致
                     GL_UNSIGNED_BYTE,  //像素的数据类型
                     NULL               //纹理的数据
                     );
    }

    //激活第1层纹理,绑定到创建的opengl纹理
    glActiveTexture(GL_TEXTURE0 + index);
    glBindTexture(GL_TEXTURE_2D, texts[index]);
    //替换纹理内容
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format,
                    GL_UNSIGNED_BYTE, buf);
}

SurfaceVideo::~SurfaceVideo()
{
    if (program) glDeleteProgram(program);
    if (fsh) glDeleteShader(fsh);
    if (vsh) glDeleteShader(vsh);

    for (int i = 0; i < sizeof(texts) / sizeof(unsigned int); i++) {
        if (texts[i]) {
            glDeleteTextures(1, &texts[i]);
        }
        texts[i] = 0;
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, winsurface);
    s.clear();
    control.clear();
    eglTerminate(display);
    IPCThreadState::self()->stopProcess();
    LOGI("call ~SurfaceVideo function");
}

void SurfaceVideo::Draw()
{
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(display, winsurface);
}

SurfaceVideo::SurfaceVideo(int mwidth, int mhight)
    : width(mwidth), hight(mhight), vsh(0), fsh(0), program(0)
{
    for (int i = 0; i < sizeof(texts) / sizeof(unsigned int); i++) {
        texts[i] = 0;
    }
}
};
