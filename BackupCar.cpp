/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Backupcar"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/misc.h>

#include <cutils/properties.h>

#include <androidfw/AssetManager.h>
#include <binder/IPCThreadState.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/threads.h>

#include <ui/DisplayInfo.h>
#include <ui/FramebufferNativeWindow.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Region.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include <core/SkBitmap.h>
#include <core/SkImageDecoder.h>
#include <core/SkStream.h>

#include "android/log.h"
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#include "BackupCar.h"

#define USER_BOOTANIMATION_FILE "/data/local/backupcar.zip"
#define SYSTEM_BOOTANIMATION_FILE "/system/media/backupcar.zip"
#define SYSTEM_ENCRYPTED_BOOTANIMATION_FILE "/system/media/backupcar-encrypted.zip"
#define EXIT_PROP_NAME "service.bkcar.exit"

extern "C" int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* request, struct timespec* remain);

namespace android {

// ---------------------------------------------------------------------------

BackupCar::BackupCar()
    : Thread(false)
{
    mSession = new SurfaceComposerClient();
}

BackupCar::~BackupCar()
{
}
void BackupCar::startDrawThread()
{
    for (;;) {
        char value[PROPERTY_VALUE_MAX];
        property_get("debug.backcar.start", value, "0");
        int noBk = atoi(value);
        if (noBk == 1) {
            property_set("debug.backcar.start", 0);
            run("BackupCar", PRIORITY_DISPLAY);
        }
        usleep(200 * 1000);
    }
    return;
}

template <typename TYPE, void (TYPE::*startDrawThread)()>
void* _thread_t(void* param)

{
    TYPE* This = (TYPE*)param;
    This->startDrawThread();
    return NULL;
}

void BackupCar::onFirstRef()
{
    status_t err = mSession->linkToComposerDeath(this);
    ALOGE_IF(err, "linkToComposerDeath failed (%s) ", strerror(-err));
    if (err == NO_ERROR) {
        // run("BackupCar", PRIORITY_DISPLAY);
        pthread_create(&_Handle, NULL, _thread_t<BackupCar, &BackupCar::startDrawThread>, this);
    }
}

sp<SurfaceComposerClient> BackupCar::session() const
{
    return mSession;
}

void BackupCar::binderDied(const wp<IBinder>& who)
{
    // woah, surfaceflinger died!
    ALOGD("SurfaceFlinger died, exiting...");

    // calling requestExit() is not enough here because the Surface code
    // might be blocked on a condition variable that will never be updated.
    kill(getpid(), SIGKILL);
    requestExit();
}

status_t BackupCar::initTexture(Texture* texture, AssetManager& assets, const char* name)
{
    Asset* asset = assets.open(name, Asset::ACCESS_BUFFER);
    if (!asset)
        return NO_INIT;
    SkBitmap bitmap;
    SkImageDecoder::DecodeMemory(
        asset->getBuffer(false), asset->getLength(), &bitmap, SkBitmap::kNo_Config, SkImageDecoder::kDecodePixels_Mode);
    asset->close();
    delete asset;

    // ensure we can call getPixels(). No need to call unlock, since the
    // bitmap will go out of scope when we return from this method.
    bitmap.lockPixels();

    const int   w = bitmap.width();
    const int   h = bitmap.height();
    const void* p = bitmap.getPixels();

    GLint crop[4] = {0, h, w, -h};
    texture->w    = w;
    texture->h    = h;

    glGenTextures(1, &texture->name);
    glBindTexture(GL_TEXTURE_2D, texture->name);

    switch (bitmap.getConfig()) {
    case SkBitmap::kA8_Config:
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, p);
        break;
    case SkBitmap::kARGB_4444_Config:
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, p);
        break;
    case SkBitmap::kARGB_8888_Config:
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        break;
    case SkBitmap::kRGB_565_Config:
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
        break;
    default:
        break;
    }

    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return NO_ERROR;
}

status_t BackupCar::initTexture(void* buffer, size_t len)
{
    // StopWatch watch("blah");

    SkBitmap        bitmap;
    SkMemoryStream  stream(buffer, len);
    SkImageDecoder* codec = SkImageDecoder::Factory(&stream);
    codec->setDitherImage(false);
    if (codec) {
        codec->decode(&stream, &bitmap, SkBitmap::kARGB_8888_Config, SkImageDecoder::kDecodePixels_Mode);
        delete codec;
    }

    // ensure we can call getPixels(). No need to call unlock, since the
    // bitmap will go out of scope when we return from this method.
    bitmap.lockPixels();

    const int   w = bitmap.width();
    const int   h = bitmap.height();
    const void* p = bitmap.getPixels();

    GLint crop[4] = {0, h, w, -h};
    int   tw      = 1 << (31 - __builtin_clz(w));
    int   th      = 1 << (31 - __builtin_clz(h));

    if (tw < w)
        tw <<= 1;
    if (th < h)
        th <<= 1;

    switch (bitmap.getConfig()) {
    case SkBitmap::kARGB_8888_Config:
        if (tw != w || th != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        }
        break;

    case SkBitmap::kRGB_565_Config:
        if (tw != w || th != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
        }
        break;
    default:
        break;
    }

    glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);

    return NO_ERROR;
}

status_t BackupCar::readyToRun()
{
    mAssets.addDefaultAssets();

    sp<IBinder> dtoken(SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain));
    DisplayInfo dinfo;
    status_t    status = SurfaceComposerClient::getDisplayInfo(dtoken, &dinfo);
    if (status)
        return -1;

    // create the native surface
    sp<SurfaceControl> control = session()->createSurface(String8("BackupCar"), dinfo.w, dinfo.h, PIXEL_FORMAT_RGB_565);

    SurfaceComposerClient::openGlobalTransaction();
    control->setLayer(0x40000000);
    SurfaceComposerClient::closeGlobalTransaction();

    sp<Surface> s = control->getSurface();

    // initialize opengl and egl
    const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE};
    EGLint       w, h, dummy;
    EGLint       numConfigs;
    EGLConfig    config;
    EGLSurface   surface;
    EGLContext   context;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    eglInitialize(display, 0, 0);
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);
    surface = eglCreateWindowSurface(display, config, s.get(), NULL);
    context = eglCreateContext(display, config, NULL, NULL);
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);

    if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE)
        return NO_INIT;

    mDisplay               = display;
    mContext               = context;
    mSurface               = surface;
    mWidth                 = w;
    mHeight                = h;
    mFlingerSurfaceControl = control;
    mFlingerSurface        = s;

    mAndroidAnimation = true;

    // If the device has encryption turned on or is in process
    // of being encrypted we show the encrypted boot animation.
    char decrypt[PROPERTY_VALUE_MAX];
    property_get("vold.decrypt", decrypt, "");

    bool encryptedAnimation = atoi(decrypt) != 0 || !strcmp("trigger_restart_min_framework", decrypt);

    if ((encryptedAnimation && (access(SYSTEM_ENCRYPTED_BOOTANIMATION_FILE, R_OK) == 0)
            && (mZip.open(SYSTEM_ENCRYPTED_BOOTANIMATION_FILE) == NO_ERROR))
        ||

        ((access(USER_BOOTANIMATION_FILE, R_OK) == 0) && (mZip.open(USER_BOOTANIMATION_FILE) == NO_ERROR)) ||

        ((access(SYSTEM_BOOTANIMATION_FILE, R_OK) == 0) && (mZip.open(SYSTEM_BOOTANIMATION_FILE) == NO_ERROR))) {
        mAndroidAnimation = false;
    }

    return NO_ERROR;
}

bool BackupCar::threadLoop()
{
    bool r = movie();

    // No need to force exit anymore
    property_set(EXIT_PROP_NAME, "0");

    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(mDisplay, mContext);
    eglDestroySurface(mDisplay, mSurface);
    mFlingerSurface.clear();
    mFlingerSurfaceControl.clear();
    eglTerminate(mDisplay);

    // IPCThreadState::self()->stopProcess();
    return false;
}

void BackupCar::checkExit()
{
    // Allow surface flinger to gracefully request shutdown
    char value[PROPERTY_VALUE_MAX];
    property_get(EXIT_PROP_NAME, value, "0");
    int exitnow = atoi(value);
    if (exitnow) {
        ALOGD("request exit!");
        requestExit();
    }
}

void BackupCar::clearScreen()
{
    // clear screen
    ALOGI("#%s", __FUNCTION__);
    /*glShadeModel(GL_FLAT);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);

    eglSwapBuffers(mDisplay, mSurface);*/
    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(mDisplay, mContext);
    eglDestroySurface(mDisplay, mSurface);
    mFlingerSurface.clear();
    mFlingerSurfaceControl.clear();
    eglTerminate(mDisplay);
}

bool BackupCar::movie()
{

    ZipFileRO& zip(mZip);

    char lang[PROPERTY_VALUE_MAX];
    char drawleft[PROPERTY_VALUE_MAX];
    char drawright[PROPERTY_VALUE_MAX];
    char leftnum[PROPERTY_VALUE_MAX];
    char rightnum[PROPERTY_VALUE_MAX];

    property_get("persist.sys.language", lang, "0");

    size_t numEntries = zip.getNumEntries();
    // ALOGD("numEntries %d %d\n", numEntries, __LINE__);
    ZipEntryRO desc    = zip.findEntryByName("desc.txt");
    FileMap*   descMap = zip.createEntryFileMap(desc);
    ALOGE_IF(!descMap, "descMap is null");
    if (!descMap) {
        return false;
    }

    String8     desString((char const*)descMap->getDataPtr(), descMap->getDataLength());
    char const* s = desString.string();

    Animation animation;

    // Parse the description file

    for (;;) {
        const char* endl = strstr(s, "\n");
        if (!endl)
            break;
        String8     line(s, endl - s);
        const char* l = line.string();
        int         fps, width, height, count, pause;
        char        path[256];
        char        pathType;
        if (sscanf(l, "%d %d %d", &width, &height, &fps) == 3) {
            animation.width  = width;
            animation.height = height;
            animation.fps    = fps;
        } else if (sscanf(l, " %c %d %d %s", &pathType, &count, &pause, path) == 4) {
            Animation::Part part;
            part.playUntilComplete = pathType == 'c';
            part.count             = count;
            part.pause             = pause;
            part.path              = path;
            animation.parts.add(part);
        }
        s = ++endl;
    }

    animation.width  = 1280;
    animation.height = 480;
    animation.fps    = 6;
    Animation::Part part;
    part.playUntilComplete = 'p' == 'c';
    part.count             = 0;
    part.pause             = 10;
    part.path              = "part1";
    animation.parts.add(part);
    // read all the data structures
    const size_t pcount = animation.parts.size();
    ALOGD("pcount---numEntries-- %d %d", pcount, numEntries);
    for (size_t i = 0; i < numEntries; i++) {
        char       name[256];
        ZipEntryRO entry = zip.findEntryByIndex(i);
        if (zip.getEntryFileName(entry, name, 256) == 0) {
            const String8 entryName(name);
            const String8 path(entryName.getPathDir());
            const String8 leaf(entryName.getPathLeaf());
            if (leaf.size() > 0) {
                for (int j = 0; j < pcount; j++) {
                    if (path == animation.parts[j].path) {
                        // ALOGD("-bch---path ----parts %d %s", j, name);
                        int method;
                        // supports only stored png files
                        if (zip.getEntryInfo(entry, &method, 0, 0, 0, 0, 0)) {
                            if (method == ZipFileRO::kCompressStored) {
                                FileMap* map = zip.createEntryFileMap(entry);
                                if (map) {
                                    Animation::Frame frame;
                                    frame.name = leaf;
                                    frame.map  = map;
                                    Animation::Part& part(animation.parts.editItemAt(j));
                                    part.frames.add(frame);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // clear screen
    glShadeModel(GL_FLAT);
    glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    eglSwapBuffers(mDisplay, mSurface);

    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // const int xc = (mWidth - animation.width) / 2;
    // const int yc = ((mHeight - animation.height) / 2);
    int     xc            = (mWidth - animation.width) / 2;
    int     yc            = ((mHeight - animation.height) / 2);
    nsecs_t lastFrame     = systemTime();
    nsecs_t frameDuration = s2ns(1) / animation.fps;
    // ALOGD("lastFrame frameDuration----- %lld, %lld", ns2ms(lastFrame), ns2ms(frameDuration));
    ALOGD("mWidth %d, mHeight %d, animation.width %d, animation.height %d", mWidth, mHeight, animation.width, animation.height);
    int    kk;
    GLuint textName[4];
    Region clearReg(Rect(mWidth, mHeight));
    clearReg.subtractSelf(Rect(xc, yc, xc + animation.width, yc + animation.height));

    for (int i = 0; i < pcount; i++) {
        const Animation::Part& part(animation.parts[i]);
        const size_t           fcount = part.frames.size();
        ALOGD("pcount(%d) fcount(%d) i(%d)----", pcount, fcount, i);
        glBindTexture(GL_TEXTURE_2D, 0);

        // for (int r=0 ; !part.count || r<part.count ; r++) {
        int kk = 0;
        // glGenTextures(4, textName);
        for (int r = 0;;) {
            // Exit any non playuntil complete parts immediately
            if (exitPending() && !part.playUntilComplete)
                break;

            for (int j = 0; j < fcount && (!exitPending() || part.playUntilComplete); j++) {
                // if ((!exitPending() || part.playUntilComplete)) {
                Animation::Frame mFrame;

                memset(drawleft, 0, PROPERTY_VALUE_MAX);
                memset(drawright, 0, PROPERTY_VALUE_MAX);
                memset(leftnum, 0, PROPERTY_VALUE_MAX);
                memset(rightnum, 0, PROPERTY_VALUE_MAX);
                property_get("backupcar.direction.left", drawleft, "0");
                property_get("backupcar.direction.right", drawright, "0");
                property_get("backupcar.leftnum.value", leftnum, "0");
                property_get("backupcar.rightnum.value", rightnum, "0");

                //ALOGI("kk=%d", kk);
                switch (kk) {
                case 1:
                    mFrame = part.frames[1]; //left
                    if (!strcmp(drawleft, "1"))
                        mFrame = part.frames[atoi(leftnum)];
                    break;
                case 2:
                    mFrame = part.frames[6]; //right
                    if (!strcmp(drawright, "1"))
                        mFrame = part.frames[atoi(rightnum)];
                    break;
                case 3:
                    mFrame = part.frames[20]; // line
                    break;
                case 0:
                    mFrame = part.frames[0]; // major
                    break;
                case 4:
                    if (!strcmp(lang, "zh"))
                        mFrame = part.frames[19]; //bottom
                    else if (!strcmp(lang, "en"))
                        mFrame = part.frames[18];
                    break;
                default:
                    break;
                }

                // const Animation::Frame& frame(part.frames[j]);
                const Animation::Frame& frame(mFrame);
                nsecs_t                 lastFrame = systemTime();
                if (r > 0) {
                    glBindTexture(GL_TEXTURE_2D, frame.tid);
                    ALOGD("r>0 frame tid--  %d", frame.tid);
                } else {
                    if (part.count != 1) {
                        // glGenTextures(1, &frame.tid);
                        if (kk <= 4) {
                            // ALOGD("-bch----r<=0 kk=%d, textName[kk]=%d, glIsTexture- %d", kk, textName[kk],
                            //glIsTexture(textName[kk]);
                            glBindTexture(GL_TEXTURE_2D, textName[0]);
                        } else {
                            // ALOGD("-bch----r<=0 kk=%d, textName[2]=%d, glIsTexture- %d", kk, textName[2],
                            //glIsTexture(textName[2]);
                            //glBindTexture(GL_TEXTURE_2D, textName[2]);
                        }

                        glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    }
                    initTexture(frame.map->getDataPtr(), frame.map->getDataLength());
                }

                if (!clearReg.isEmpty()) {
                    Region::const_iterator head(clearReg.begin());
                    Region::const_iterator tail(clearReg.end());
                    glEnable(GL_SCISSOR_TEST);
                    ALOGD("-------clear reg not empty-----------  ");
                    while (head != tail) {
                        const Rect& r(*head++);
                        glScissor(r.left, mHeight - r.bottom, r.width(), r.height());
                        glClear(GL_COLOR_BUFFER_BIT);
                    }
                    glDisable(GL_SCISSOR_TEST);
                }

                if (kk == 0) { // mjor
                    glDrawTexiOES(0, 172, 0, 387, 308);
                    kk++;
                    continue;
                }
                if (kk == 1) { //left
                    glDrawTexiOES(0, 0, 0, 195, 172);
                    kk++;
                    continue;
                }
                if (kk == 2) { // right
                    glDrawTexiOES(195, 0, 0, 192, 172);
                    kk++;
                    continue;
                }
                if (kk == 3) { // line
                    glDrawTexiOES(387, 33, 0, 893, 447);
                    kk++;
                    continue;
                }
                if (kk == 4) { //bottom
                    glDrawTexiOES(387, 0, 0, 893, 33);
                    kk++;
                    continue;
                }

                kk = 0;
                eglSwapBuffers(mDisplay, mSurface);

                nsecs_t now   = systemTime();
                nsecs_t delay = frameDuration - (now - lastFrame);
                lastFrame     = now;

                if (delay > 0) {
                    struct timespec spec;
                    spec.tv_sec  = (now + delay) / 1000000000;
                    spec.tv_nsec = (now + delay) % 1000000000;
                    int err;
                    do {
                        err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &spec, NULL);
                    } while (err < 0 && errno == EINTR);
                }
                checkExit();
            }

            usleep(part.pause * ns2us(frameDuration));

            // For infinite parts, we've now played them at least once, so perhaps
            // exit
            //ALOGD("exitPending---%d", exitPending());
            if (exitPending() && !part.count)
                break;
        }
        ALOGI("**end for int r=0");
        // free the textures for this part
        if (part.count != 1) {
            for (int j = 0; j < fcount; j++) {
                //ALOGD("glelelete --fcount--j----  %d,%d", fcount, j);
                const Animation::Frame& frame(part.frames[j]);
                glDeleteTextures(1, &frame.tid);
            }
        }

        // eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        // eglDestroyContext(mDisplay, mContext);
        // eglDestroySurface(mDisplay, mSurface);
        // mFlingerSurface.clear();
        // mFlingerSurfaceControl.clear();
        // eglTerminate(mDisplay);
        // for(;;){
        // ALOGD("---------bch ---usleep to wait quit---");
        //     usleep(ns2us(frameDuration/1000));
        //}
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------

}; // namespace android
