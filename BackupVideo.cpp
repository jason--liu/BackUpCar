#define LOG_TAG "Backupcar"
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/misc.h>

#include <binder/IPCThreadState.h>
#include <cutils/properties.h>

#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/threads.h>
#include "BackupVideo.h"

#include <linux/ipu.h>
#include <linux/mxc_v4l2.h>
#include <linux/mxcfb.h>

#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <utils/Log.h>
#include "android/log.h"

static char        v4l_capture_dev[100]  = "/dev/video0";
static char        v4l_output_dev[100]   = "/dev/video17";
static char        fb_device[100]        = "/dev/graphics/fb0";
static char        camera_dev[20]        = "/dev/car_rvs";
static char        camera_sgn_dev[30]    = "/dev/camera_sgn_seek";
static int         fd_capture_v4l        = 0;
static int         fd_output_v4l         = 0;
static int         g_cap_mode            = 0;
static int         g_input               = 1;
static int         g_fmt                 = V4L2_PIX_FMT_YUV420; //V4L2_PIX_FMT_YUYV;
static int         g_rotate              = 0;
static int         g_vflip               = 0;
static int         g_hflip               = 0;
static int         g_vdi_enable          = 0;
static int         g_vdi_motion          = 0;
static int         g_tb                  = 0;
static int         g_output              = 3;
static int         g_output_num_buffers  = 4;
static int         g_capture_num_buffers = 3;
static int         g_in_width            = 0;
static int         g_in_height           = 0;
static int         g_display_width       = 893;            // 1024;
static int         g_display_height      = (480 - 33 + 2); // 600;
static int         g_display_top         = 0;
static int         g_display_left        = 387; //387;
static int         g_frame_size;
static int         g_frame_period = 33333;
static v4l2_std_id g_current_std  = V4L2_STD_NTSC;
static int         qs_camera_fd;
static int         rvs_state; //倒车状态
static int         fb0_state;
static pthread_t   qcamera_thread_id;
static int         camera_sgn_fd = 0;
static int         startBackup   = 0;

FILE* fp_yuv = NULL;
int   fpd    = -1;
#define TFAIL -1
#define TPASS 0

#define BackTurnStatus 0x13
#define RadarDistanceLeft 0x15
#define RadarDistanceLeftM 0x16
#define RadarDistanceRight 0x17
#define RadarDistanceRightM 0x18
#define SteeringWheelAng 0x19

/* safe distance, default */
static int     LeftDistance   = 200;
static int     LeftMDistance  = 200;
static int     RightDistance  = 200;
static int     RightMDistance = 200;
static int     SteerAngle     = 0;
static int     hasSig         = 0;
unsigned char* out_buf;

struct testbuffer {
    unsigned char* start;
    size_t         offset;
    unsigned int   length;
};

struct testbuffer output_buffers[4];
struct testbuffer capture_buffers[3];

namespace android {
extern int start(unsigned char* out_buf);

BackupVideo::BackupVideo()
{
}
BackupVideo::~BackupVideo()
{
}
void BackupVideo::onFirstRef()
{
    ALOGI("BackupVideo onFirstRef");
    StartBackupVideo();
}

void BackupVideo::binderDied(const wp<IBinder>& who)
{
    // ALOGD("SurfaceFlinger died, exiting...");
    // kill(getpid(), SIGKILL);
    // requestExit();
    return;
}

int hasVideoSignal()
{
    // int hasSignal = 0;
    int ret = 0;

    ret = read(camera_sgn_fd, &hasSig, sizeof(hasSig));
    // LOGI("==>camera_sgn_fd hasSignal = 0x%x,ret = %d<==\n",hasSignal,ret);
    return hasSig;
}

int start_capturing(void)
{
    int                i;
    struct v4l2_buffer buf;
    enum v4l2_buf_type type;

    for (i = 0; i < g_capture_num_buffers; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_capture_v4l, VIDIOC_QUERYBUF, &buf) < 0) {
            ALOGI("VIDIOC_QUERYBUF error\n");
            return TFAIL;
        }

        capture_buffers[i].length = buf.length;
        capture_buffers[i].offset = (size_t)buf.m.offset;
        capture_buffers[i].start  = (unsigned char*)mmap(
            NULL, capture_buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED,
            fd_capture_v4l, capture_buffers[i].offset);
        memset(capture_buffers[i].start, 0xFF, capture_buffers[i].length);
    }

    for (i = 0; i < g_capture_num_buffers; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.offset = capture_buffers[i].offset;
        if (ioctl(fd_capture_v4l, VIDIOC_QBUF, &buf) < 0) {
            ALOGI("VIDIOC_QBUF error\n");
            return TFAIL;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_capture_v4l, VIDIOC_STREAMON, &type) < 0) {
        ALOGI("VIDIOC_STREAMON error\n");
        return TFAIL;
    }
    return 0;
}

int prepare_output(void)
{
    int                i;
    struct v4l2_buffer output_buf;

    for (i = 0; i < g_output_num_buffers; i++) {
        memset(&output_buf, 0, sizeof(output_buf));
        output_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        output_buf.memory = V4L2_MEMORY_MMAP;
        output_buf.index  = i;
        if (ioctl(fd_output_v4l, VIDIOC_QUERYBUF, &output_buf) < 0) {
            ALOGE("VIDIOC_QUERYBUF error\n");
            return TFAIL;
        }

        output_buffers[i].length = output_buf.length;
        output_buffers[i].offset = (size_t)output_buf.m.offset;
        output_buffers[i].start  = (unsigned char*)mmap(
            NULL, output_buffers[i].length, PROT_READ | PROT_WRITE, MAP_SHARED,
            fd_output_v4l, output_buffers[i].offset);
        if (output_buffers[i].start == NULL) {
            ALOGE("v4l2 tvin test: output mmap failed\n");
            return TFAIL;
        }
    }
    return 0;
}

int v4l_capture_setup(void)
{
    struct v4l2_capability     cap;
    struct v4l2_cropcap        cropcap;
    struct v4l2_crop           crop;
    struct v4l2_format         fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_dbg_chip_ident chip;
    struct v4l2_streamparm     parm;
    struct v4l2_control        ctrl;
    v4l2_std_id                id;
    unsigned int               min;
    out_buf = (unsigned char*)malloc(720 * 480 * 1.5);
    if (ioctl(fd_capture_v4l, VIDIOC_QUERYCAP, &cap) < 0) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n", v4l_capture_dev);
            ALOGE("%s is no V4L2 device\n", v4l_capture_dev);
            return TFAIL;
        } else {
            fprintf(stderr, "%s isn not V4L device,unknow error\n",
                v4l_capture_dev);
            ALOGE("%s isn not V4L device,unknow error\n", v4l_capture_dev);
            return TFAIL;
        }
    }
    ALOGI("driver:%s    \ncard:%s   \ncapabilities:%x\n", cap.driver, cap.card,
        cap.capabilities);

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device\n", v4l_capture_dev);
        ALOGE("%s is no video capture device\n", v4l_capture_dev);
        return TFAIL;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming i/o\n", v4l_capture_dev);
        return TFAIL;
    }

    if (ioctl(fd_capture_v4l, VIDIOC_DBG_G_CHIP_IDENT, &chip)) {
        ALOGE("VIDIOC_DBG_G_CHIP_IDENT failed.\n");
        close(fd_capture_v4l);
        return TFAIL;
    }
    ALOGI("TV decoder chip is %s\n", chip.match.name);

    if (ioctl(fd_capture_v4l, VIDIOC_S_INPUT, &g_input) < 0) {
        ALOGE("VIDIOC_S_INPUT failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    if (ioctl(fd_capture_v4l, VIDIOC_G_STD, &id) < 0) {
        ALOGE("VIDIOC_G_STD failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }
    g_current_std = id;

    if (ioctl(fd_capture_v4l, VIDIOC_S_STD, &id) < 0) {
        ALOGE("VIDIOC_S_STD failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    /* Select video input, video standard and tune here. */

    memset(&cropcap, 0, sizeof(cropcap));

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // if (ioctl (fd_capture_v4l, VIDIOC_CROPCAP, &cropcap) < 0)
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_capture_v4l, VIDIOC_G_CROP, &crop) < 0) {
            ALOGE("VIDIOC_G_CROP failed\n");
            return -1;
        }
        crop.c = cropcap.defrect; /* reset to default */

        crop.c.top    = 0; // g_display_top;
        crop.c.left   = 0; // g_display_left;
        crop.c.width  = 720;
        crop.c.height = 480;

        if (ioctl(fd_capture_v4l, VIDIOC_S_CROP, &crop) < 0) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                fprintf(stderr, "%s  doesn't support crop\n",
                    v4l_capture_dev);
                ALOGE("%s  doesn't support crop\n", v4l_capture_dev);
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    }
    ALOGD("\n");
    ALOGD(
        "*************************Get CID "
        "BRIGHTNESS*****************************\n");
    memset(&ctrl, 0, sizeof(ctrl));
#if 0
    ctrl.id    = V4L2_CID_BRIGHTNESS;
    ctrl.value = 110;
    if (ioctl(fd_capture_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
        ALOGE("ioctl error %s", strerror(errno));
        return TFAIL;
    }
#endif
    ctrl.value = 0;
    if (ioctl(fd_capture_v4l, VIDIOC_G_CTRL, &ctrl) < 0) {
        ALOGE("ioctl error %s", strerror(errno));
        return TFAIL;
    }
    ALOGD(">:Get CID BRIGHTNESS:[%d]\n", ctrl.value);
    ALOGD(
        "**********************************************************************"
        "*");
    ALOGD("\n");

    ALOGD(
        "************************Get "
        "Stream_Parm********************************\n");
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_capture_v4l, VIDIOC_G_PARM, &parm) == -1) {
        ALOGE("VIDIOC_G_PARM ioctl error");
        return TFAIL;
    }
    ALOGD(">:[Frame rate:%u] [%u]\n", parm.parm.capture.timeperframe.numerator,
        parm.parm.capture.timeperframe.denominator);
    ALOGD(">:[capability:%d] [capturemode:%d]\n", parm.parm.capture.capability,
        parm.parm.capture.capturemode);
    ALOGD(">:[extendemode:%d] [readbuffers:%d]\n",
        parm.parm.capture.extendedmode, parm.parm.capture.readbuffers);
    ALOGD(
        "**********************************************************************"
        "*");
    ALOGD("\n");
    memset(&parm, 0, sizeof(parm));

    parm.type                                  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = 0;
    parm.parm.capture.capturemode              = 0;
    if (ioctl(fd_capture_v4l, VIDIOC_S_PARM, &parm) < 0) {
        ALOGE("VIDIOC_S_PARM failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    memset(&fmt, 0, sizeof(fmt));

    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = 0;
    fmt.fmt.pix.height      = 0;
    fmt.fmt.pix.pixelformat = g_fmt;
    fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

    if (ioctl(fd_capture_v4l, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "%s iformat not supported \n", v4l_capture_dev);
        ALOGE("%s iformat not supported \n", v4l_capture_dev);
        return TFAIL;
    }

    /* Note VIDIOC_S_FMT may change width and height. */

    /* Buggy driver paranoia. */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;

    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    if (ioctl(fd_capture_v4l, VIDIOC_G_FMT, &fmt) < 0) {
        ALOGE("VIDIOC_G_FMT failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    g_in_width  = fmt.fmt.pix.width;
    g_in_height = fmt.fmt.pix.height;

    ALOGD(
        "************************Get format "
        "info********************************\n");
    ALOGI("g_in_width=%d, g_in_height=%d\n", g_in_width, g_in_height);
    ALOGD(">:[width:%d]\t[pixelformat:%d]\n", fmt.fmt.pix.width,
        fmt.fmt.pix.height);
    ALOGD(">:[format:%d]\t[field:%d]\n", fmt.fmt.pix.pixelformat,
        fmt.fmt.pix.field);
    ALOGD(">:[bytesperline:%d]\t[sizeimage:%d]\n", fmt.fmt.pix.bytesperline,
        fmt.fmt.pix.sizeimage);
    ALOGD(">:[colorspace:%d]\n", fmt.fmt.pix.colorspace);
    ALOGD(
        "**********************************************************************"
        "*"
        "\n");
    ALOGD("\n");

    memset(&req, 0, sizeof(req));

    req.count  = g_capture_num_buffers;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_capture_v4l, VIDIOC_REQBUFS, &req) < 0) {
        if (EINVAL == errno) {
            fprintf(stderr,
                "%s does not support "
                "memory mapping\n",
                v4l_capture_dev);
            ALOGE(
                "%s does not support "
                "memory mapping\n",
                v4l_capture_dev);
            return TFAIL;
        } else {
            fprintf(stderr,
                "%s does not support "
                "memory mapping, unknow error\n",
                v4l_capture_dev);
            ALOGE(
                "%s does not support "
                "memory mapping, unknow error\n",
                v4l_capture_dev);
            return TFAIL;
        }
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n", v4l_capture_dev);
        return TFAIL;
    }

    return 0;
}

int v4l_output_setup(void)
{
    struct v4l2_control        ctrl;
    struct v4l2_format         fmt;
    struct v4l2_frmsizeenum    fsize;
    struct v4l2_framebuffer    fb;
    struct v4l2_cropcap        cropcap;
    struct v4l2_crop           crop;
    struct v4l2_capability     cap;
    struct v4l2_fmtdesc        fmtdesc;
    struct v4l2_requestbuffers buf_req;
    int                        ret;
    ALOGI("#### %s %d", __FUNCTION__, __LINE__);
    if (!ioctl(fd_output_v4l, VIDIOC_QUERYCAP, &cap)) {
        ALOGE(
            "driver=%s, card=%s, bus=%s, "
            "version=0x%08x, "
            "capabilities=0x%08x\n",
            cap.driver, cap.card, cap.bus_info, cap.version, cap.capabilities);
    }
#if 0
    ALOGD("*********************Enum fsize***********************");
    memset(&fsize, 0, sizeof(fsize));

    while (!ioctl(fd_capture_v4l, VIDIOC_ENUM_FRAMESIZES, &fsize)) {
        fsize.index++;

        if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            ALOGD("{ discrete: width = %u, height = %u }",
                fsize.discrete.width, fsize.discrete.height);

        } else if (fsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            ALOGD("{ continuous: min { width = %u, height = %u } .. "
                  "max { width = %u, height = %u } }",
                fsize.stepwise.min_width, fsize.stepwise.min_height,
                fsize.stepwise.max_width, fsize.stepwise.max_height);
            ALOGD("  will not enumerate frame intervals.\n");
        } else if (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
            ALOGD("{ stepwise: min { width = %u, height = %u } .. "
                  "max { width = %u, height = %u } / "
                  "stepsize { width = %u, height = %u } }",
                fsize.stepwise.min_width, fsize.stepwise.min_height,
                fsize.stepwise.max_width, fsize.stepwise.max_height,
                fsize.stepwise.step_width, fsize.stepwise.step_height);
            ALOGD("  will not enumerate frame intervals.");
        } else {
            ALOGE("  fsize.type not supported: %d\n", fsize.type);
            ALOGE(" 	(Discrete: %d	Continuous: %d	Stepwise: %d)",
                V4L2_FRMSIZE_TYPE_DISCRETE,
                V4L2_FRMSIZE_TYPE_CONTINUOUS,
                V4L2_FRMSIZE_TYPE_STEPWISE);
        }
    }
#endif
    ALOGD("*****************************************************");
    fmtdesc.index = 0;
    fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ALOGD("*********************Enum Fmt***********************");
    while (!ioctl(fd_output_v4l, VIDIOC_ENUM_FMT, &fmtdesc)) {
        ALOGD("fmt %s: fourcc = 0x%08x\n", fmtdesc.description,
            fmtdesc.pixelformat);
        fmtdesc.index++;
    }
    ALOGD("*****************************************************");
    ALOGD("\n");

    memset(&cropcap, 0, sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd_output_v4l, VIDIOC_CROPCAP, &cropcap) < 0) {
        ALOGE("get crop capability failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }

    crop.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    crop.c.top  = g_display_top;
    crop.c.left = g_display_left;
    // crop.c.width = 900;//g_display_width;
    // crop.c.height = 500;//g_display_height;
    crop.c.width  = g_display_width;
    crop.c.height = g_display_height;
    if (ioctl(fd_output_v4l, VIDIOC_S_CROP, &crop) < 0) {
        ALOGE("set crop failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    // Set rotation
    ctrl.id    = V4L2_CID_ROTATE;
    ctrl.value = g_rotate;
    if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
        ALOGE("set ctrl rotate failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }
    ctrl.id    = V4L2_CID_VFLIP;
    ctrl.value = g_vflip;
    if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
        ALOGE("set ctrl vflip failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }
    ctrl.id    = V4L2_CID_HFLIP;
    ctrl.value = g_hflip;
    if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
        ALOGE("set ctrl hflip failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }
    if (g_vdi_enable) {
        ctrl.id    = V4L2_CID_MXC_MOTION;
        ctrl.value = g_vdi_motion;
        if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
            ALOGE("set ctrl motion failed\n");
            close(fd_output_v4l);
            return TFAIL;
        }
    }

    fb.flags = V4L2_FBUF_FLAG_OVERLAY;
    ioctl(fd_output_v4l, VIDIOC_S_FBUF, &fb);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type                 = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width        = g_in_width;
    fmt.fmt.pix.height       = g_in_height;
    fmt.fmt.pix.pixelformat  = g_fmt;
    fmt.fmt.pix.bytesperline = g_in_width;
    fmt.fmt.pix.priv         = 0;
    fmt.fmt.pix.sizeimage    = 0;
    if (g_tb)
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED_TB;
    else
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED_BT;
    if (ioctl(fd_output_v4l, VIDIOC_S_FMT, &fmt) < 0) {
        ALOGE("set format failed\n");
        return TFAIL;
    }

    if (ioctl(fd_output_v4l, VIDIOC_G_FMT, &fmt) < 0) {
        ALOGE("get format failed\n");
        return TFAIL;
    }
    g_frame_size = fmt.fmt.pix.sizeimage;
    ALOGD("fmt.fmt.pix.height %d, g_frame_size %d", fmt.fmt.pix.height, g_frame_size);
    memset(&buf_req, 0, sizeof(buf_req));
    buf_req.count  = g_output_num_buffers;
    buf_req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf_req.memory = V4L2_MEMORY_MMAP;
    ret            = ioctl(fd_output_v4l, VIDIOC_REQBUFS, &buf_req);
    if (ret < 0) {
        ALOGE("request buffers failed,ret = 0x%x\n", ret);
        return TFAIL;
    }

    return 0;
}

int yuv422toyuv420(unsigned char* out, const unsigned char* in, unsigned int width, unsigned int height)
{
    unsigned char* y = out;
    unsigned char* u = out + width * height;
    unsigned char* v = out + width * height + width * height / 4;

    unsigned int i, j;
    unsigned int base_h;
    unsigned int is_y = 1, is_u = 1;
    unsigned int y_index = 0, u_index = 0, v_index = 0;

    unsigned long yuv422_length = 2 * width * height;

    for (i = 0; i < yuv422_length; i += 2) {
        *(y + y_index) = *(in + i);
        y_index++;
    }

    for (i = 0; i < height; i += 2) {
        base_h = i * width * 2;
        for (j = base_h + 1; j < base_h + width * 2; j += 2) {
            if (is_u) {
                *(u + u_index) = *(in + j);
                u_index++;
                is_u = 0;
            } else {
                *(v + v_index) = *(in + j);
                v_index++;
                is_u = 1;
            }
        }
    }
    return 1;
}

int mxc_v4l_tvin_test(void)
{
    struct v4l2_buffer capture_buf, output_buf;
    v4l2_std_id        id;
    int                i, j;
    enum v4l2_buf_type type;
    int                total_time;
    struct timeval     tv_start, tv_current;

    if (prepare_output() < 0) {
        ALOGE("prepare_output failed\n");
        return TFAIL;
    }

    if (start_capturing() < 0) {
        ALOGE("start_capturing failed\n");
        return TFAIL;
    }

    gettimeofday(&tv_start, 0);
    ALOGI("start time = %d s, %d us\n", (unsigned int)tv_start.tv_sec,
        (unsigned int)tv_start.tv_usec);

    for (i = 0;; i++) {
    begin:
        if (rvs_state == 0) { // exit camera
            break;
        }
        if (ioctl(fd_capture_v4l, VIDIOC_G_STD, &id)) {
            ALOGE("VIDIOC_G_STD failed.\n");
            return TFAIL;
        }
        if (!hasVideoSignal()) {
            ALOGE("no video singal\n");
            break;
        }

        if (id == g_current_std)
            goto next;
        else if (id == V4L2_STD_PAL || id == V4L2_STD_NTSC) {
            type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ioctl(fd_output_v4l, VIDIOC_STREAMOFF, &type);

            type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_capture_v4l, VIDIOC_STREAMOFF, &type);

            for (j = 0; j < g_output_num_buffers; j++) {
                munmap(output_buffers[j].start, output_buffers[j].length);
            }
            for (j = 0; j < g_capture_num_buffers; j++) {
                munmap(capture_buffers[j].start, capture_buffers[j].length);
            }

            if (v4l_capture_setup() < 0) {
                ALOGE("Setup v4l capture failed.\n");
                return TFAIL;
            }

            if (v4l_output_setup() < 0) {
                ALOGE("Setup v4l output failed.\n");
                return TFAIL;
            }

            if (prepare_output() < 0) {
                ALOGE("prepare_output failed\n");
                return TFAIL;
            }

            if (start_capturing() < 0) {
                ALOGE("start_capturing failed\n");
                return TFAIL;
            }
            i = 0;
            ALOGI("TV standard changed\n");
        } else {
            sleep(1);
            /* Try again */
            if (ioctl(fd_capture_v4l, VIDIOC_G_STD, &id)) {
                ALOGE("VIDIOC_G_STD failed.\n");
                return TFAIL;
            }

            if (id != V4L2_STD_ALL) {
                ALOGW("id != V4L2_STD_ALL");
                goto begin;
            }

            ALOGE("Cannot detect TV standard\n");
            return 0;
        }
    next:
        memset(&capture_buf, 0, sizeof(capture_buf));
        capture_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        capture_buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_capture_v4l, VIDIOC_DQBUF, &capture_buf) < 0) {
            ALOGE("VIDIOC_DQBUF failed %s. (%s:%d)", strerror(errno),
                __FUNCTION__, __LINE__);
            return TFAIL;
        }

        memset(&output_buf, 0, sizeof(output_buf));
        output_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        output_buf.memory = V4L2_MEMORY_MMAP;
        if (i < g_output_num_buffers) {
            output_buf.index = i;
            if (ioctl(fd_output_v4l, VIDIOC_QUERYBUF, &output_buf) < 0) {
                ALOGE("VIDIOC_QUERYBUF failed\n");
                return TFAIL;
            }
        } else {
            output_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            output_buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(fd_output_v4l, VIDIOC_DQBUF, &output_buf) < 0) {
                ALOGE("VIDIOC_DQBUF failed %s. (%s:%d)", strerror(errno),
                    __FUNCTION__, __LINE__);
                return TFAIL;
            }
        }
        memcpy(output_buffers[output_buf.index].start,
            capture_buffers[capture_buf.index].start, g_frame_size);
        if (ioctl(fd_capture_v4l, VIDIOC_QBUF, &capture_buf) < 0) {
            ALOGE("VIDIOC_QBUF failed\n");
            return TFAIL;
        }

        output_buf.timestamp.tv_sec  = tv_start.tv_sec;
        output_buf.timestamp.tv_usec = tv_start.tv_usec + (g_frame_period * i);
        if (g_vdi_enable)
            output_buf.field = g_tb ? V4L2_FIELD_INTERLACED_TB : V4L2_FIELD_INTERLACED_BT;
        if (ioctl(fd_output_v4l, VIDIOC_QBUF, &output_buf) < 0) {
            ALOGE("VIDIOC_QBUF failed\n");
            return TFAIL;
        }
        if (i == 1) {
            type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            if (ioctl(fd_output_v4l, VIDIOC_STREAMON, &type) < 0) {
                ALOGE("Could not start stream\n");
                return TFAIL;
            }
        }
    }
    gettimeofday(&tv_current, 0);
    total_time = (tv_current.tv_sec - tv_start.tv_sec) * 1000000L;
    total_time += tv_current.tv_usec - tv_start.tv_usec;
    ALOGI("total time for %u frames = %u us =  %lld fps\n", i, total_time,
        (i * 1000000ULL) / total_time);

    return 0;
}

int init_graphics_fb0(int fd_fb)
{
    struct fb_var_screeninfo info;

    if (ioctl(fd_fb, FBIOGET_VSCREENINFO, &info) < 0) {
        ALOGE("FBIOPUT_VSCREENINFO failed\n");
        return TFAIL;
    }
    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset     = 0;
    info.yoffset     = 0;
    info.activate    = FB_ACTIVATE_NOW;
    ALOGD("info.bits_per_pixel %d", info.bits_per_pixel);
    if (info.bits_per_pixel == 32) {
        /*
        * Explicitly request RGBA 8/8/8/8
        */
        info.red.offset       = 0;
        info.red.length       = 8;
        info.red.msb_right    = 0;
        info.green.offset     = 8;
        info.green.length     = 8;
        info.green.msb_right  = 0;
        info.blue.offset      = 16;
        info.blue.length      = 8;
        info.blue.msb_right   = 0;
        info.transp.offset    = 24;
        info.transp.length    = 8;
        info.transp.msb_right = 0;
    } else {
        /*
         * Explicitly request 5/6/5
         */
        info.bits_per_pixel   = 16;
        info.red.offset       = 11;
        info.red.length       = 5;
        info.red.msb_right    = 0;
        info.green.offset     = 5;
        info.green.length     = 6;
        info.green.msb_right  = 0;
        info.blue.offset      = 0;
        info.blue.length      = 5;
        info.blue.msb_right   = 0;
        info.transp.offset    = 0;
        info.transp.length    = 0;
        info.transp.msb_right = 0;
    }
    // info.yres_virtual = ALIGN_PIXEL_128(info.yres) * 3;
    // info.xres_virtual = ALIGN_PIXEL(info.xres);
    /*
    info.bits_per_pixel  = 24;
    info.red.length = 8;
    info.blue.length = 8;
    info.green.length = 8;
    info.transp.length = 0;
    info.red.offset = 16;
    info.blue.offset = 0;
    info.green.offset = 8;
    info.transp.offset = 0;*/
    if (ioctl(fd_fb, FBIOPUT_VSCREENINFO, &info) < 0) {
        ALOGE("FBIOPUT_VSCREENINFO failed\n");
        return TFAIL;
    }
    return 0;
}

int start_priview()
{
    int                    fd_fb = 0, i;
    struct mxcfb_gbl_alpha alpha;
    enum v4l2_buf_type     type;
    ALOGD("###%s %d", __FUNCTION__, __LINE__);
    if ((fd_fb = open(fb_device, O_RDWR)) < 0) {
        ALOGE("Unable to open frame buffer\n");
        return TFAIL;
    }
    if (fb0_state == 0) {
        init_graphics_fb0(fd_fb);
    }
    if ((fd_capture_v4l = open(v4l_capture_dev, O_RDWR, 0)) < 0) {
        ALOGE("Unable to open %s\n", v4l_capture_dev);
        return TFAIL;
    }

    if ((fd_output_v4l = open(v4l_output_dev, O_RDWR, 0)) < 0) {
        ALOGE("Unable to open %s\n", v4l_output_dev);
        return TFAIL;
    }

    if (v4l_capture_setup() < 0) {
        ALOGE("Setup v4l capture failed.\n");
        return TFAIL;
    }

    if (v4l_output_setup() < 0) {
        ALOGE("Setup v4l output failed.\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    /* Overlay setting */
    alpha.alpha  = 0; //0;
    alpha.enable = 1;
#if 1
    if (ioctl(fd_fb, MXCFB_SET_GBL_ALPHA, &alpha) < 0) {
        ALOGE("Set global alpha failed\n");
        close(fd_fb);
        close(fd_capture_v4l);
        close(fd_output_v4l);
        return TFAIL;
    }
#endif

#if 0
    struct mxcfb_loc_alpha l_alpha;
    l_alpha.enable         = true;
    l_alpha.alpha_in_pixel = true;
    if (ioctl(fd_fb1, MXCFB_SET_LOC_ALPHA, &l_alpha) < 0) {
        ALOGE("Set local alpha failed\n");
    }
#endif

    /*
    struct mxcfb_color_key color_key;
    color_key.color_key = 0x0;
    color_key.enable    = 1;
    if (ioctl(fd_fb, MXCFB_SET_CLR_KEY, &color_key) < 0) {
        ALOGE("Error in applying Color Key\n");
    }*/

    mxc_v4l_tvin_test();
    ALOGD("###%s %d", __FUNCTION__, __LINE__);
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(fd_output_v4l, VIDIOC_STREAMOFF, &type);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_capture_v4l, VIDIOC_STREAMOFF, &type);

    for (i = 0; i < g_output_num_buffers; i++) {
        munmap(output_buffers[i].start, output_buffers[i].length);
    }
    for (i = 0; i < g_capture_num_buffers; i++) {
        munmap(capture_buffers[i].start, capture_buffers[i].length);
    }
    ALOGD("###%s %d", __FUNCTION__, __LINE__);
    close(fd_capture_v4l);
    close(fd_output_v4l);
    close(fd_fb);
    return 0;
}

void* BackupVideo::qc_thread_func(void* arg)
{
    int hasSignal = 0;
    int i;
    ALOGI("==>quick_camera_thread_func start<==\n");

retry:
    for (i = 0; i < 60; i++) {
        // usleep(100 * 1000);
        hasSignal = hasVideoSignal();
        ALOGI("hasSignal = %d,rvs_state = %d\n", hasSignal, rvs_state);
        if (rvs_state == 0)
            break;
        if ((hasSignal == 1) && (rvs_state == 1)) { //有视频信号且进入倒车
            property_set("backupcar.angle.value", "27");
            property_set("ctl.start", "livestream");
            ALOGI("run ctl.start livestream");
            break;
        } else { //无视频信号时不打开摄像头，延时再判断
            usleep(600 * 1000);
            property_set("backupcar.angle.value", "32");
            ALOGI("no video signal, sleep 600ms");
        }
    }
    if (rvs_state == 1) {
        // int ret = start_priview(); //这里会操作摄像头，直到退出倒车才结束次函数
        //ALOGD("ret== %d goto retry", ret);
        usleep(500 * 1000);
        goto retry;
    }
    ALOGI("==>quick_camera_thread_func end<====\n");
    pthread_exit(NULL);

    return NULL;
}
static void RightRadarDistanceProcess(int tag, int distance)
{
    ALOGI("right radar tag 0x%02x, distance %d", tag, distance);
    switch (tag) {
    case RadarDistanceRight:
        if (distance != 0x3F)
            RightDistance = distance * 5 + 25;
        else
            RightDistance = 0;
        break;
    case RadarDistanceRightM:
        if (distance != 0x3F)
            RightMDistance = distance * 5 + 25;
        else
            RightMDistance = 0;
        break;
    default:
        ALOGE("not support tag 0x%02x, distance %d", tag, distance);
        break;
    }
    /*************************right side************************/
    if (RightDistance > 0 && RightDistance <= 35) {
        property_set("backupcar.direction.right", "1");
        ALOGD("draw right line:%d", __LINE__);
        if (RightMDistance > 0 && RightMDistance <= 35) {
            property_set("backupcar.rightnum.value", "22");
        }
        if (RightMDistance > 35 && RightMDistance <= 60) {
            property_set("backupcar.rightnum.value", "22");
        }
        if (RightMDistance > 60 && RightMDistance <= 90) {
            property_set("backupcar.rightnum.value", "6");
        }
        if (RightMDistance > 90 && RightMDistance <= 150) {
            property_set("backupcar.rightnum.value", "21");
        }
        if (RightMDistance > 150) {
            property_set("backupcar.rightnum.value", "20");
        }
        if (RightMDistance == 0) {
            property_set("backupcar.rightnum.value", "45");
        }
        // return;
    }
    if (RightDistance > 35 && RightDistance <= 60) {
        property_set("backupcar.direction.right", "1");
        ALOGD("draw right line:%d", __LINE__);
        if (RightMDistance > 0 && RightMDistance <= 35) {
            property_set("backupcar.rightnum.value", "19");
        }
        if (RightMDistance > 35 && RightMDistance <= 60) {
            property_set("backupcar.rightnum.value", "19");
        }
        if (RightMDistance > 60 && RightMDistance <= 90) {
            property_set("backupcar.rightnum.value", "7");
        }
        if (RightMDistance > 90 && RightMDistance <= 150) {
            property_set("backupcar.rightnum.value", "18");
        }
        if (RightMDistance > 150) {
            property_set("backupcar.rightnum.value", "17");
        }
        if (RightMDistance == 0) {
            property_set("backupcar.rightnum.value", "44");
        }
        // return;
    }
    if (RightDistance > 60) {
        property_set("backupcar.direction.right", "1");
        ALOGD("draw right line:%d", __LINE__);
        if (RightMDistance > 0 && RightMDistance <= 35) {
            property_set("backupcar.rightnum.value", "16");
        }
        if (RightMDistance > 35 && RightMDistance <= 60) {
            property_set("backupcar.rightnum.value", "16");
        }
        if (RightMDistance > 60 && RightMDistance <= 90) {
            property_set("backupcar.rightnum.value", "8");
        }
        if (RightMDistance > 90 && RightMDistance <= 150) {
            property_set("backupcar.rightnum.value", "15");
        }
        if (RightMDistance > 150) {
            property_set("backupcar.rightnum.value", "14");
        }
        if (RightMDistance == 0) {
            property_set("backupcar.rightnum.value", "41");
        }
        // return;
    }
    // property_set("backupcar.direction.left", "0");
    if (RightDistance == 0) {
        property_set("backupcar.direction.right", "1");
        ALOGD("draw right line:%d", __LINE__);
        if (RightMDistance > 0 && RightMDistance <= 35) {
            property_set("backupcar.rightnum.value", "46");
        }
        if (RightMDistance > 35 && RightMDistance <= 60) {
            property_set("backupcar.rightnum.value", "46");
        }
        if (RightMDistance > 60 && RightMDistance <= 90) {
            property_set("backupcar.rightnum.value", "42");
        }
        if (RightMDistance > 90 && RightMDistance <= 150) {
            property_set("backupcar.rightnum.value", "40");
        }
        if (RightMDistance > 150) {
            property_set("backupcar.rightnum.value", "43");
        }
        if (RightMDistance == 0) {
            property_set("backupcar.rightnum.value", "48");
        }
    }
    return;
}

static void LeftRadarDistanceProcess(int tag, int distance)
{
    ALOGI("left radar tag 0x%02x, distance %d", tag, distance);
    /*default its safe distance*/
    switch (tag) {
    case RadarDistanceLeft:
        if (distance != 0x3F)
            LeftDistance = distance * 5 + 25;
        else
            LeftDistance = 0;
        break;
    case RadarDistanceLeftM:
        if (distance != 0x3F)
            LeftMDistance = distance * 5 + 25;
        else
            LeftMDistance = 0;
        break;
    }
    /*************************left side************************/
    if (LeftDistance > 0 && LeftDistance <= 35) {
        /*left radar all red*/
        property_set("backupcar.direction.left", "1");
        ALOGD("draw left line:%d", __LINE__);
        if (LeftMDistance > 0 && LeftMDistance <= 35) {
            property_set("backupcar.leftnum.value", "13");
        }
        if (LeftMDistance > 35 && LeftMDistance <= 60) {
            property_set("backupcar.leftnum.value", "13");
        }
        if (LeftMDistance >= 60 && LeftMDistance <= 90) {
            property_set("backupcar.leftnum.value", "2");
        }
        if (LeftMDistance > 90 && LeftMDistance <= 150) {
            property_set("backupcar.leftnum.value", "12");
        }
        if (LeftMDistance > 150) {
            property_set("backupcar.leftnum.value", "11");
        }
        if (LeftMDistance == 0) {
            property_set("backupcar.leftnum.value", "38");
        }
        // return;
    }
    if (LeftDistance > 35 && LeftDistance <= 60) {
        ALOGD("draw left line:%d", __LINE__);
        property_set("backupcar.direction.left", "1");
        if (LeftMDistance > 0 && LeftMDistance <= 35) {
            property_set("backupcar.leftnum.value", "10");
        }
        if (LeftMDistance > 35 && LeftMDistance <= 60) {
            property_set("backupcar.leftnum.value", "10");
        }
        if (LeftMDistance > 60 && LeftMDistance <= 90) {
            property_set("backupcar.leftnum.value", "3");
        }
        if (LeftMDistance > 90 && LeftMDistance <= 150) {
            property_set("backupcar.leftnum.value", "9");
        }
        if (LeftMDistance > 150) {
            property_set("backupcar.leftnum.value", "23");
        }
        if (LeftMDistance == 0) {
            property_set("backupcar.leftnum.value", "37");
        }
        // return;
    }
    if (LeftDistance > 60) {
        property_set("backupcar.direction.left", "1");
        ALOGD("draw left line:%d", __LINE__);
        if (LeftMDistance > 0 && LeftMDistance <= 35) {
            property_set("backupcar.leftnum.value", "24");
        }
        if (LeftMDistance > 35 && LeftMDistance <= 60) {
            property_set("backupcar.leftnum.value", "24");
        }
        if (LeftMDistance > 60 && LeftMDistance <= 90) {
            property_set("backupcar.leftnum.value", "4");
        }
        if (LeftMDistance > 90 && LeftMDistance <= 150) {
            property_set("backupcar.leftnum.value", "5");
        }
        if (LeftMDistance > 150) {
            property_set("backupcar.leftnum.value", "1");
        }
        if (LeftMDistance == 0) {
            property_set("backupcar.leftnum.value", "34");
        }
        // return;
    }
    // property_set("backupcar.direction.right", "0");
    if (LeftDistance == 0) {
        property_set("backupcar.direction.left", "1");
        ALOGD("draw left line:%d", __LINE__);
        if (LeftMDistance > 0 && LeftMDistance <= 35) {
            property_set("backupcar.leftnum.value", "39");
        }
        if (LeftMDistance > 35 && LeftMDistance <= 60) {
            property_set("backupcar.leftnum.value", "39");
        }
        if (LeftMDistance > 60 && LeftMDistance <= 90) {
            property_set("backupcar.leftnum.value", "35");
        }
        if (LeftMDistance > 90 && LeftMDistance <= 150) {
            property_set("backupcar.leftnum.value", "33");
        }
        if (LeftMDistance > 150) {
            property_set("backupcar.leftnum.value", "36");
        }
        if (LeftMDistance == 0) {
            property_set("backupcar.leftnum.value", "47");
        }
        // return;
    }

    return;
}

static void drawaAngLeft(int ang)
{
    SteerAngle = ang;

    if (SteerAngle > 0 && SteerAngle <= 14)
        property_set("backupcar.angle.value", "49");
    if (SteerAngle > 14 && SteerAngle <= 28)
        property_set("backupcar.angle.value", "50");
    if (SteerAngle > 28 && SteerAngle <= 42)
        property_set("backupcar.angle.value", "51");
    if (SteerAngle > 42 && SteerAngle <= 56)
        property_set("backupcar.angle.value", "52");
    if (SteerAngle > 56 && SteerAngle <= 70)
        property_set("backupcar.angle.value", "53");
    if (SteerAngle > 70 && SteerAngle <= 84)
        property_set("backupcar.angle.value", "54");
    if (SteerAngle > 84 && SteerAngle <= 98)
        property_set("backupcar.angle.value", "55");
    if (SteerAngle > 98 && SteerAngle <= 112)
        property_set("backupcar.angle.value", "56");
    if (SteerAngle > 112 && SteerAngle <= 126)
        property_set("backupcar.angle.value", "57");
    if (SteerAngle > 126 && SteerAngle <= 140)
        property_set("backupcar.angle.value", "58");
    if (SteerAngle > 140 && SteerAngle <= 154)
        property_set("backupcar.angle.value", "59");
    if (SteerAngle > 154 && SteerAngle <= 168)
        property_set("backupcar.angle.value", "60");
    if (SteerAngle > 168 && SteerAngle <= 182)
        property_set("backupcar.angle.value", "61");
    if (SteerAngle > 182 && SteerAngle <= 196)
        property_set("backupcar.angle.value", "62");
    if (SteerAngle > 196 && SteerAngle <= 210)
        property_set("backupcar.angle.value", "63");
    if (SteerAngle > 210 && SteerAngle <= 224)
        property_set("backupcar.angle.value", "64");
    if (SteerAngle > 224 && SteerAngle <= 238)
        property_set("backupcar.angle.value", "65");
    if (SteerAngle > 238 && SteerAngle <= 252)
        property_set("backupcar.angle.value", "66");
    if (SteerAngle > 252 && SteerAngle <= 266)
        property_set("backupcar.angle.value", "67");
    if (SteerAngle > 266 && SteerAngle <= 280)
        property_set("backupcar.angle.value", "68");
    if (SteerAngle > 280 && SteerAngle <= 294)
        property_set("backupcar.angle.value", "69");
    if (SteerAngle > 294 && SteerAngle <= 308)
        property_set("backupcar.angle.value", "70");
    if (SteerAngle > 308 && SteerAngle <= 322)
        property_set("backupcar.angle.value", "71");
    if (SteerAngle > 322 && SteerAngle <= 336)
        property_set("backupcar.angle.value", "72");
    if (SteerAngle > 336 && SteerAngle <= 350)
        property_set("backupcar.angle.value", "73");
    if (SteerAngle > 350 && SteerAngle <= 364)
        property_set("backupcar.angle.value", "74");
    if (SteerAngle > 364 && SteerAngle <= 378)
        property_set("backupcar.angle.value", "75");
    if (SteerAngle > 378 && SteerAngle <= 392)
        property_set("backupcar.angle.value", "76");
    if (SteerAngle > 392 && SteerAngle <= 406)
        property_set("backupcar.angle.value", "77");
    if (SteerAngle > 406 && SteerAngle <= 420)
        property_set("backupcar.angle.value", "78");
    if (SteerAngle > 420 && SteerAngle <= 434)
        property_set("backupcar.angle.value", "79");
    if (SteerAngle > 434 && SteerAngle <= 448)
        property_set("backupcar.angle.value", "80");
    if (SteerAngle > 448 && SteerAngle <= 462)
        property_set("backupcar.angle.value", "81");
    if (SteerAngle > 462 && SteerAngle <= 476)
        property_set("backupcar.angle.value", "82");
    if (SteerAngle > 476 && SteerAngle <= 490)
        property_set("backupcar.angle.value", "83");
    if (SteerAngle > 490 && SteerAngle <= 504)
        property_set("backupcar.angle.value", "84");
    if (SteerAngle > 504 && SteerAngle <= 518)
        property_set("backupcar.angle.value", "85");
    if (SteerAngle > 518 && SteerAngle <= 532)
        property_set("backupcar.angle.value", "86");
    if (SteerAngle > 532 && SteerAngle <= 546)
        property_set("backupcar.angle.value", "87");
    if (SteerAngle > 546 && SteerAngle <= 560)
        property_set("backupcar.angle.value", "88");
    if (SteerAngle > 560 && SteerAngle <= 574)
        property_set("backupcar.angle.value", "89");
    if (SteerAngle > 574 && SteerAngle <= 588)
        property_set("backupcar.angle.value", "90");
    if (SteerAngle > 588 && SteerAngle <= 602)
        property_set("backupcar.angle.value", "91");
    if (SteerAngle > 602 && SteerAngle <= 616)
        property_set("backupcar.angle.value", "92");
    if (SteerAngle > 616 && SteerAngle <= 630)
        property_set("backupcar.angle.value", "93");
    if (SteerAngle > 630 && SteerAngle <= 644)
        property_set("backupcar.angle.value", "94");
    if (SteerAngle > 644 && SteerAngle <= 658)
        property_set("backupcar.angle.value", "95");
    if (SteerAngle > 658 && SteerAngle <= 672)
        property_set("backupcar.angle.value", "96");
    if (SteerAngle > 672 && SteerAngle <= 686)
        property_set("backupcar.angle.value", "97");
    if (SteerAngle > 686 && SteerAngle <= 700)
        property_set("backupcar.angle.value", "98");
    if (SteerAngle > 700 && SteerAngle <= 720)
        property_set("backupcar.angle.value", "98");

    if (SteerAngle == 0)
        property_set("backupcar.angle.value", "27");
    ALOGD("draw angle left %d ", SteerAngle);
    return;
}

static void drawAngRight(int ang)
{
    SteerAngle = ang;
    if (SteerAngle > 0 && SteerAngle <= 360)
        property_set("backupcar.angle.value", "30");
    else if (SteerAngle > 360 && SteerAngle <= 720)
        property_set("backupcar.angle.value", "31");
    if (SteerAngle == 0)
        property_set("backupcar.angle.value", "27");
    ALOGD("draw angle right %d ", SteerAngle);
    return;
}

void* cmdSocket(void* param)
{
    int msocket = -1;
    int ret     = -1;
    int cFd     = -1;

    struct sockaddr_un paddr;
    socklen_t          socklen  = sizeof(paddr);
    uint8_t            buff[50] = {0};
    // char*              buffer = (char*)malloc(16);
    int distance = 0;

    msocket = android_get_control_socket("backmodecar");
    if (msocket < 0) {
        ALOGE("can not get local socket=%d %s", msocket, strerror(errno));
        return NULL;
    }
    ret = listen(msocket, 4);
    if (ret < 0) {
        ALOGE("listen socket error %s", strerror(errno));
    }
    cFd = accept(msocket, (struct sockaddr*)&paddr, &socklen);
    if (cFd < 0) {
        ALOGE("accept socket error %s", strerror(errno));
    }
    ALOGI("accept from middle fd(%d)", cFd);
    struct timeval timeout;
    fd_set         read_fds;
    int            ang;
    FD_ZERO(&read_fds);
    for (;;) {
        timeout.tv_sec  = 0;
        timeout.tv_usec = 2000;
        FD_SET(cFd, &read_fds);
        memset(buff, 0, 50);
        // ALOGI("wait data...");
        ret = select(cFd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret <= 0) {
            // ALOGE("select error %s", strerror(errno));
            continue;
        }

        if (FD_ISSET(cFd, &read_fds)) {
            ret = recv(cFd, buff, 50, 0);
            if (ret <= 0) {
                // ALOGE("receive record data error %s. (%s:%d)",
                // strerror(errno),
                // __FUNCTION__, __LINE__);
                continue;
            }
            ALOGI("receive %d bytes from client 0x%02x 0x%02x 0x%02x", ret,
                buff[0], buff[1], buff[2]);
            switch (buff[0]) {
            case BackTurnStatus:
                if (buff[1] == 1) {
#if 0
                    startBackup = 1;
                    property_set("backupcar.angle.value", "27");
                    property_set("service.bkcar.exit", "0");
#endif
                    // property_set("debug.backcar.start", "1");
                    // ALOGI("start backup car");
                } else if (buff[1] == 0) {
#if 0
                    startBackup = 0;
                    property_set("service.bkcar.exit", "1");
                    ALOGD("service.bkcar.exit 1");
                    property_set("backupcar.direction.right", "0");
                    property_set("backupcar.direction.left", "0");
                    property_set("backupcar.angle.value", "27");
                    LeftDistance   = 200;
                    LeftMDistance  = 200;
                    RightDistance  = 200;
                    RightMDistance = 200;
                    ALOGI("stop backup car");
#endif
                }
                break;

            case RadarDistanceLeft:
            case RadarDistanceLeftM:
                LeftRadarDistanceProcess(int(buff[0]), (int)(buff[1]));
                break;
            case RadarDistanceRight:
            case RadarDistanceRightM:
                RightRadarDistanceProcess(int(buff[0]), (int)(buff[1]));
                break;

            case SteeringWheelAng:
                ALOGI("steering wheel angle 0x%02x 0x%02x", buff[1],
                    buff[2]);
                ang = (int)((buff[1] << 8) | buff[2]);
                ang = ang * 0.0625 - 2048;
                if (ang < 0) {
                    ALOGD("left ang == %d", ang);
                    //drawaAngLeft(abs(ang));
                } else {
                    ALOGD("right ang == %d", ang);
                    //drawAngRight(ang);
                }
                break;
            default:
                // RadarDistanceProcess(int(buff[0]), int(buff[1]));
                ALOGW("unsupport command");
                break;
            }
        }
    }
    return NULL;
}

int BackupVideo::StartBackupVideo()
{
    ALOGI("Start Back Video");

    int ret           = 0;
    int data          = 0;
    int camera_opened = 0;

    char value[PROP_VALUE_MAX];
    qs_camera_fd  = open(camera_dev, O_RDWR);
    camera_sgn_fd = open(camera_sgn_dev, O_RDWR);
    ALOGI("==>open qs_camera_io = %d,camera_sgn_fd = %d<==\n", qs_camera_fd,
        camera_sgn_fd);
    if (qs_camera_fd < 0) {
        ALOGE("open /dev/car_rvs error %s ", strerror(errno));
        return -1;
    }
    pthread_t pThreadcmdSocket;
    pthread_create(&pThreadcmdSocket, NULL, cmdSocket, NULL);
    int stopBoot = 0;
    int bootin   = 1;

    while (1) {
        ret = read(qs_camera_fd, &data, sizeof(data));
        ALOGI("==>qc_v4l2_tvin data = 0x%x<==\n", data);
        startBackup = data & 0x01;
        fb0_state   = (data >> 1) & 0x01;
        if (startBackup == 1) {
            property_set("ctl.stop", "bootanim");
            rvs_state = 1;
            if (camera_opened == 0) {
                camera_opened = 1;
                // property_set("debug.backcar.start", "1");
                ALOGI("create qc_thread_func thread");
                if (pthread_create(&state_thread_id, NULL, qc_thread_func,
                        NULL)
                    != 0) {
                    ALOGI("Create thread error!");
                }
                property_set("backupcar.angle.value", "27");
                property_set("service.bkcar.exit", "0");
                property_set("backcar.live.stop", "0");
                usleep(100 * 1000);
                if (1 /*hasSig*/) {
                    property_set("debug.backcar.start", "1");
                    ALOGD("set debug.backcar.start 1 (%s:%d)", __FUNCTION__,
                        __LINE__);
                }
            }
        }
        if (startBackup == 0) {
            property_set("service.bkcar.exit", "1");
            property_set("backcar.live.stop", "1");
            ALOGD("service.bkcar.exit 1");
            rvs_state = 0;
            usleep(50000); //  0.5s
            camera_opened = 0;
            pthread_join(state_thread_id, NULL);
            property_set("backupcar.direction.right", "0");
            property_set("backupcar.direction.left", "0");
            property_set("backupcar.angle.value", "27");
            LeftDistance   = 200;
            LeftMDistance  = 200;
            RightDistance  = 200;
            RightMDistance = 200;
            hasSig         = 0;
            ALOGI("stop backup car");
            //
            property_get("service.bootanim.exit", value, "0");
            int bm = atoi(value);
            if (!bm) {
                ALOGI("continue bottanim");
                property_set("ctl.start", "bootanim");
            }
        }
        // usleep(200 * 1000);
    }
    return 0;
}
};
