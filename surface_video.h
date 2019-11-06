#ifndef SURFACE_VIDEO_H_
#define SURFACE_VIDEO_H_

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
namespace android {
typedef void *EGLContext;
typedef void *EGLDisplay;
typedef void *EGLSurface;

class SurfaceVideo {
private:
    EGLDisplay display;
    EGLSurface winsurface;
    EGLContext context;
    sp<SurfaceComposerClient> client;
    sp<SurfaceControl> control;
    sp<Surface> s;
    int width,hight;
    unsigned int vsh ;
    unsigned int fsh;
    unsigned int program ;
    unsigned int texts[10];
public:
    int GLSurfaceInit();
    void GetTexture(unsigned int index, int width, int height,
                    unsigned char *buf);
    void Draw();
    SurfaceVideo(int mwidth, int mhight);
    virtual ~SurfaceVideo();
};
};
#endif /* SURFACE_VIDEO_H */
