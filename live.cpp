#include <android/log.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "surface_video.h"
#include "video_capture.h"

// using namespace android;
#define TAG "surfacevideo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace android {
int main_a()
{
    sp<ProcessState> proc(ProcessState::self());
    ProcessState::self()->startThreadPool();
    unsigned char src_image[518400] = {0};  // 720*480*1.5
    init_video_capture();
    SurfaceVideo* sv = new SurfaceVideo(IM_WIDTH, IM_HEIGHT);
    sv->GLSurfaceInit();
    char key = 0;
    LOGI("start video stream live");

/*int ffd = open("/data/out.yuv", O_WRONLY | O_APPEND);
if (-1 == ffd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n", "/data/out.yuv", errno,
            strerror(errno));
    exit(EXIT_FAILURE);
}*/
#if 0
    FILE* fp = fopen("/data/out1.yuv", "rb");
    if (!fp) {
        printf("open file yuv failed");
        return -1;
    }
#endif
    unsigned char* buf[3] = {0};
    buf[0] = new unsigned char[IM_WIDTH * IM_HEIGHT];
    buf[1] = new unsigned char[IM_WIDTH * IM_HEIGHT / 4];  // 宽高都除以2
    buf[2] = new unsigned char[IM_WIDTH * IM_HEIGHT / 4];
    int state = 0;
    char value[PROP_VALUE_MAX] = {0};

    for (;;) {
        key = video_capture(src_image);
#if 1
        memcpy(buf[0], src_image, IM_WIDTH * IM_HEIGHT);
        memcpy(buf[1], src_image + (IM_WIDTH * IM_HEIGHT),
               IM_WIDTH * IM_HEIGHT / 4);
        memcpy(buf[2],
               src_image + static_cast<int>(IM_WIDTH * IM_HEIGHT * 1.25),
               IM_WIDTH * IM_HEIGHT / 4);
#endif
        // fwrite(buf[0], 1, IM_WIDTH * IM_HEIGHT, fp);
        // fwrite(buf[1], 1, IM_WIDTH * IM_HEIGHT / 4, fp);
        // fwrite(buf[2], 1, IM_WIDTH * IM_HEIGHT / 4, fp);
        // if (feof(fp) == 0) {
        //     fread(buf[0], 1, IM_WIDTH * IM_HEIGHT, fp);
        //     fread(buf[1], 1, IM_WIDTH * IM_HEIGHT / 4, fp);
        //     fread(buf[2], 1, IM_WIDTH * IM_HEIGHT / 4, fp);
        // }
        sv->GetTexture(0, IM_WIDTH, IM_HEIGHT, buf[0]);          // Y
        sv->GetTexture(1, IM_WIDTH / 2, IM_HEIGHT / 2, buf[1]);  // U
        sv->GetTexture(2, IM_WIDTH / 2, IM_HEIGHT / 2, buf[2]);  // V
        sv->Draw();
        // start(src_image);
        // int rz = write(ffd, src_image, 720*480*1.5);
        // printf("Wrote %d \n", rz);

        property_get("backcar.live.stop", value, "0");
        state = atoi(value);
        if (state) {
            LOGD("get live stream property %d ", state);
	    usleep(20*1000);
            break;
        }
        /*
            if (key == 'q') {
                break;
            }*/
        usleep(50 * 1000);
    }
    free_video_capture();
    delete sv;
    delete buf[0];
    delete buf[1];
    delete buf[2];
    // IPCThreadState::self()->joinThreadPool();
    return EXIT_SUCCESS;
}
};
int main()
{
    android::main_a();
    property_set("backcar.live.stop", "0");
    LOGI("set live stream  property 0");
    return 0;
}
