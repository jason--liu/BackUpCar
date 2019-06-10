#define LOG_TAG "Backupcar"
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/misc.h>

#include <cutils/properties.h>
#include <binder/IPCThreadState.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/threads.h>
#include "BackupVideo.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <asm/types.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <math.h>
#include <string.h>
#include <malloc.h>
#include <sys/time.h>
#include <pthread.h>

#include <linux/mxcfb.h>
#include <linux/mxc_v4l2.h>
#include <linux/ipu.h>

#include "android/log.h"
#include <cutils/properties.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <utils/Log.h>
#include <cutils/sockets.h>
#include <sys/un.h>

static char        v4l_capture_dev[100]  = "/dev/video0";
static char        v4l_output_dev[100]   = "/dev/video17";
static char        fb_device[100]        = "/dev/graphics/fb0";
static char        camera_dev[20]        = "/dev/car_rvs";
static char        camera_sgn_dev[30]    = "/dev/camera_sgn_seek";
static int         fd_capture_v4l        = 0;
static int         fd_output_v4l         = 0;
static int         g_cap_mode            = 0;
static int         g_input               = 1;
static int         g_fmt                 = V4L2_PIX_FMT_UYVY;
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
static int         g_display_width       = 1024;
static int         g_display_height      = 600;
static int         g_display_top         = 0;
static int         g_display_left        = 0;
static int         g_frame_size;
static int         g_frame_period = 33333;
static v4l2_std_id g_current_std  = V4L2_STD_NTSC;
static int         qs_camera_fd;
static int         rvs_state; //倒车状态
static int         fb0_state;
static pthread_t   qcamera_thread_id;
static int         camera_sgn_fd = 0;
#define TFAIL -1
#define TPASS 0
struct testbuffer {
    unsigned char* start;
    size_t         offset;
    unsigned int   length;
};

struct testbuffer output_buffers[4];
struct testbuffer capture_buffers[3];

namespace android {

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
    int hasSignal = 0;
    int ret       = 0;

    ret = read(camera_sgn_fd, &hasSignal, sizeof(hasSignal));
    // LOGI("==>camera_sgn_fd hasSignal = 0x%x,ret = %d<==\n",hasSignal,ret);
    return 1;
}
int start_capturing(void)
{
    unsigned int       i;
    struct v4l2_buffer buf;
    enum v4l2_buf_type type;

    for (i = 0; i < g_capture_num_buffers; i++)
    {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_capture_v4l, VIDIOC_QUERYBUF, &buf) < 0)
        {
            ALOGI("VIDIOC_QUERYBUF error\n");
            return TFAIL;
        }

        capture_buffers[i].length = buf.length;
        capture_buffers[i].offset = (size_t)buf.m.offset;
        capture_buffers[i].start  = (unsigned char*)mmap(NULL, capture_buffers[i].length, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd_capture_v4l, capture_buffers[i].offset);
        memset(capture_buffers[i].start, 0xFF, capture_buffers[i].length);
    }

    for (i = 0; i < g_capture_num_buffers; i++)
    {
        memset(&buf, 0, sizeof(buf));
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.m.offset = capture_buffers[i].offset;
        if (ioctl(fd_capture_v4l, VIDIOC_QBUF, &buf) < 0)
        {
            ALOGI("VIDIOC_QBUF error\n");
            return TFAIL;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_capture_v4l, VIDIOC_STREAMON, &type) < 0)
    {
        ALOGI("VIDIOC_STREAMON error\n");
        return TFAIL;
    }
    return 0;
}
int prepare_output(void)
{
    int                i;
    struct v4l2_buffer output_buf;

    for (i = 0; i < g_output_num_buffers; i++)
    {
        memset(&output_buf, 0, sizeof(output_buf));
        output_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        output_buf.memory = V4L2_MEMORY_MMAP;
        output_buf.index  = i;
        if (ioctl(fd_output_v4l, VIDIOC_QUERYBUF, &output_buf) < 0)
        {
            ALOGE("VIDIOC_QUERYBUF error\n");
            return TFAIL;
        }

        output_buffers[i].length = output_buf.length;
        output_buffers[i].offset = (size_t)output_buf.m.offset;
        output_buffers[i].start  = (unsigned char*)mmap(NULL, output_buffers[i].length, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd_output_v4l, output_buffers[i].offset);
        if (output_buffers[i].start == NULL)
        {
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
    v4l2_std_id                id;
    unsigned int               min;

    if (ioctl(fd_capture_v4l, VIDIOC_QUERYCAP, &cap) < 0)
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s is no V4L2 device\n", v4l_capture_dev);
            return TFAIL;
        } else
        {
            fprintf(stderr, "%s isn not V4L device,unknow error\n", v4l_capture_dev);
            return TFAIL;
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "%s is no video capture device\n", v4l_capture_dev);
        return TFAIL;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "%s does not support streaming i/o\n", v4l_capture_dev);
        return TFAIL;
    }

    if (ioctl(fd_capture_v4l, VIDIOC_DBG_G_CHIP_IDENT, &chip))
    {
        ALOGE("VIDIOC_DBG_G_CHIP_IDENT failed.\n");
        close(fd_capture_v4l);
        return TFAIL;
    }
    ALOGI("TV decoder chip is %s\n", chip.match.name);

    if (ioctl(fd_capture_v4l, VIDIOC_S_INPUT, &g_input) < 0)
    {
        ALOGE("VIDIOC_S_INPUT failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    if (ioctl(fd_capture_v4l, VIDIOC_G_STD, &id) < 0)
    {
        ALOGE("VIDIOC_G_STD failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }
    g_current_std = id;

    if (ioctl(fd_capture_v4l, VIDIOC_S_STD, &id) < 0)
    {
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
        if (ioctl(fd_capture_v4l, VIDIOC_G_CROP, &crop) < 0)
        {
            ALOGE("VIDIOC_G_CROP failed\n");
            return -1;
        }
        crop.c = cropcap.defrect; /* reset to default */

        crop.c.top    = g_display_top;
        crop.c.left   = g_display_left;
        crop.c.width  = 720;
        crop.c.height = 480;

        if (ioctl(fd_capture_v4l, VIDIOC_S_CROP, &crop) < 0)
        {
            switch (errno)
            {
            case EINVAL:
                /* Cropping not supported. */
                fprintf(stderr, "%s  doesn't support crop\n", v4l_capture_dev);
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    }

    parm.type                                  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = 0;
    parm.parm.capture.capturemode              = 0;
    if (ioctl(fd_capture_v4l, VIDIOC_S_PARM, &parm) < 0)
    {
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

    if (ioctl(fd_capture_v4l, VIDIOC_S_FMT, &fmt) < 0)
    {
        fprintf(stderr, "%s iformat not supported \n", v4l_capture_dev);
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

    if (ioctl(fd_capture_v4l, VIDIOC_G_FMT, &fmt) < 0)
    {
        ALOGE("VIDIOC_G_FMT failed\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    g_in_width  = fmt.fmt.pix.width;
    g_in_height = fmt.fmt.pix.height;
    ALOGE("---bch----g_in_width=%d, g_in_height=%d\n", g_in_width, g_in_height);

    memset(&req, 0, sizeof(req));

    req.count  = g_capture_num_buffers;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_capture_v4l, VIDIOC_REQBUFS, &req) < 0)
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s does not support "
                            "memory mapping\n",
                v4l_capture_dev);
            return TFAIL;
        } else
        {
            fprintf(stderr, "%s does not support "
                            "memory mapping, unknow error\n",
                v4l_capture_dev);
            return TFAIL;
        }
    }

    if (req.count < 2)
    {
        fprintf(stderr, "Insufficient buffer memory on %s\n", v4l_capture_dev);
        return TFAIL;
    }

    return 0;
}

int v4l_output_setup(void)
{
    struct v4l2_control        ctrl;
    struct v4l2_format         fmt;
    struct v4l2_framebuffer    fb;
    struct v4l2_cropcap        cropcap;
    struct v4l2_crop           crop;
    struct v4l2_capability     cap;
    struct v4l2_fmtdesc        fmtdesc;
    struct v4l2_requestbuffers buf_req;
    int                        ret;

    if (!ioctl(fd_output_v4l, VIDIOC_QUERYCAP, &cap))
    {
        ALOGE("driver=%s, card=%s, bus=%s, "
              "version=0x%08x, "
              "capabilities=0x%08x\n",
            cap.driver, cap.card, cap.bus_info, cap.version, cap.capabilities);
    }

    fmtdesc.index = 0;
    fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    while (!ioctl(fd_output_v4l, VIDIOC_ENUM_FMT, &fmtdesc))
    {
        ALOGE("fmt %s: fourcc = 0x%08x\n", fmtdesc.description, fmtdesc.pixelformat);
        fmtdesc.index++;
    }

    memset(&cropcap, 0, sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd_output_v4l, VIDIOC_CROPCAP, &cropcap) < 0)
    {
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
    if (ioctl(fd_output_v4l, VIDIOC_S_CROP, &crop) < 0)
    {
        ALOGE("set crop failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }

    // Set rotation
    ctrl.id    = V4L2_CID_ROTATE;
    ctrl.value = g_rotate;
    if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        ALOGE("set ctrl rotate failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }
    ctrl.id    = V4L2_CID_VFLIP;
    ctrl.value = g_vflip;
    if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        ALOGE("set ctrl vflip failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }
    ctrl.id    = V4L2_CID_HFLIP;
    ctrl.value = g_hflip;
    if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        ALOGE("set ctrl hflip failed\n");
        close(fd_output_v4l);
        return TFAIL;
    }
    if (g_vdi_enable)
    {
        ctrl.id    = V4L2_CID_MXC_MOTION;
        ctrl.value = g_vdi_motion;
        if (ioctl(fd_output_v4l, VIDIOC_S_CTRL, &ctrl) < 0)
        {
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
    if (ioctl(fd_output_v4l, VIDIOC_S_FMT, &fmt) < 0)
    {
        ALOGE("set format failed\n");
        return TFAIL;
    }

    if (ioctl(fd_output_v4l, VIDIOC_G_FMT, &fmt) < 0)
    {
        ALOGE("get format failed\n");
        return TFAIL;
    }
    g_frame_size = fmt.fmt.pix.sizeimage;

    memset(&buf_req, 0, sizeof(buf_req));
    buf_req.count  = g_output_num_buffers;
    buf_req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf_req.memory = V4L2_MEMORY_MMAP;
    ret            = ioctl(fd_output_v4l, VIDIOC_REQBUFS, &buf_req);
    if (ret < 0)
    {
        ALOGE("request buffers failed,ret = 0x%x\n", ret);
        return TFAIL;
    }

    return 0;
}

int mxc_v4l_tvin_test(void)
{
    struct v4l2_buffer capture_buf, output_buf;
    v4l2_std_id        id;
    int                i, j;
    enum v4l2_buf_type type;
    int                total_time;
    struct timeval     tv_start, tv_current;

    if (prepare_output() < 0)
    {
        ALOGE("prepare_output failed\n");
        return TFAIL;
    }

    if (start_capturing() < 0)
    {
        ALOGE("start_capturing failed\n");
        return TFAIL;
    }

    gettimeofday(&tv_start, 0);
    ALOGI("start time = %d s, %d us\n", (unsigned int)tv_start.tv_sec, (unsigned int)tv_start.tv_usec);

    for (i = 0;; i++)
    {
    begin:
        if (rvs_state == 0)
        { // exit camera
            break;
        }
        if (ioctl(fd_capture_v4l, VIDIOC_G_STD, &id))
        {
            ALOGE("VIDIOC_G_STD failed.\n");
            return TFAIL;
        }
        if (!hasVideoSignal())
        {
            ALOGE("no video singal\n");
            break;
        }

        if (id == g_current_std)
            goto next;
        else if (id == V4L2_STD_PAL || id == V4L2_STD_NTSC)
        {
            type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ioctl(fd_output_v4l, VIDIOC_STREAMOFF, &type);

            type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_capture_v4l, VIDIOC_STREAMOFF, &type);

            for (j = 0; j < g_output_num_buffers; j++)
            {
                munmap(output_buffers[j].start, output_buffers[j].length);
            }
            for (j = 0; j < g_capture_num_buffers; j++)
            {
                munmap(capture_buffers[j].start, capture_buffers[j].length);
            }

            if (v4l_capture_setup() < 0)
            {
                ALOGE("Setup v4l capture failed.\n");
                return TFAIL;
            }

            if (v4l_output_setup() < 0)
            {
                ALOGE("Setup v4l output failed.\n");
                return TFAIL;
            }

            if (prepare_output() < 0)
            {
                ALOGE("prepare_output failed\n");
                return TFAIL;
            }

            if (start_capturing() < 0)
            {
                ALOGE("start_capturing failed\n");
                return TFAIL;
            }
            i = 0;
            ALOGI("TV standard changed\n");
        } else
        {
            sleep(1);
            /* Try again */
            if (ioctl(fd_capture_v4l, VIDIOC_G_STD, &id))
            {
                ALOGE("VIDIOC_G_STD failed.\n");
                return TFAIL;
            }

            if (id != V4L2_STD_ALL)
                goto begin;

            ALOGE("Cannot detect TV standard\n");
            return 0;
        }
    next:
        memset(&capture_buf, 0, sizeof(capture_buf));
        capture_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        capture_buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_capture_v4l, VIDIOC_DQBUF, &capture_buf) < 0)
        {
            ALOGE("VIDIOC_DQBUF failed.\n");
            return TFAIL;
        }

        memset(&output_buf, 0, sizeof(output_buf));
        output_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        output_buf.memory = V4L2_MEMORY_MMAP;
        if (i < g_output_num_buffers)
        {
            output_buf.index = i;
            if (ioctl(fd_output_v4l, VIDIOC_QUERYBUF, &output_buf) < 0)
            {
                ALOGE("VIDIOC_QUERYBUF failed\n");
                return TFAIL;
            }
        } else
        {
            output_buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            output_buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(fd_output_v4l, VIDIOC_DQBUF, &output_buf) < 0)
            {
                ALOGE("VIDIOC_DQBUF failed\n");
                return TFAIL;
            }
        }

        memcpy(output_buffers[output_buf.index].start, capture_buffers[capture_buf.index].start, g_frame_size);
        if (ioctl(fd_capture_v4l, VIDIOC_QBUF, &capture_buf) < 0)
        {
            ALOGE("VIDIOC_QBUF failed\n");
            return TFAIL;
        }

        output_buf.timestamp.tv_sec  = tv_start.tv_sec;
        output_buf.timestamp.tv_usec = tv_start.tv_usec + (g_frame_period * i);
        if (g_vdi_enable)
            output_buf.field = g_tb ? V4L2_FIELD_INTERLACED_TB : V4L2_FIELD_INTERLACED_BT;
        if (ioctl(fd_output_v4l, VIDIOC_QBUF, &output_buf) < 0)
        {
            ALOGE("VIDIOC_QBUF failed\n");
            return TFAIL;
        }
        if (i == 1)
        {
            type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            if (ioctl(fd_output_v4l, VIDIOC_STREAMON, &type) < 0)
            {
                ALOGE("Could not start stream\n");
                return TFAIL;
            }
        }
    }
    gettimeofday(&tv_current, 0);
    total_time = (tv_current.tv_sec - tv_start.tv_sec) * 1000000L;
    total_time += tv_current.tv_usec - tv_start.tv_usec;
    ALOGI("total time for %u frames = %u us =  %lld fps\n", i, total_time, (i * 1000000ULL) / total_time);

    return 0;
}
int init_graphics_fb0(int fd_fb)
{
    struct fb_var_screeninfo info;

    if (ioctl(fd_fb, FBIOGET_VSCREENINFO, &info) < 0)
    {
        ALOGE("FBIOPUT_VSCREENINFO failed\n");
        return TFAIL;
    }
    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset     = 0;
    info.yoffset     = 0;
    info.activate    = FB_ACTIVATE_NOW;
    if (info.bits_per_pixel == 32)
    {
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
    } else
    {
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
    if (ioctl(fd_fb, FBIOPUT_VSCREENINFO, &info) < 0)
    {
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

    if ((fd_fb = open(fb_device, O_RDWR)) < 0)
    {
        ALOGE("Unable to open frame buffer\n");
        return TFAIL;
    }
    if (fb0_state == 0)
    {
        init_graphics_fb0(fd_fb);
    }
    if ((fd_capture_v4l = open(v4l_capture_dev, O_RDWR, 0)) < 0)
    {
        ALOGE("Unable to open %s\n", v4l_capture_dev);
        return TFAIL;
    }

    if ((fd_output_v4l = open(v4l_output_dev, O_RDWR, 0)) < 0)
    {
        ALOGE("Unable to open %s\n", v4l_output_dev);
        return TFAIL;
    }

    if (v4l_capture_setup() < 0)
    {
        ALOGE("Setup v4l capture failed.\n");
        return TFAIL;
    }

    if (v4l_output_setup() < 0)
    {
        ALOGE("Setup v4l output failed.\n");
        close(fd_capture_v4l);
        return TFAIL;
    }

    /* Overlay setting */
    alpha.alpha  = 0.5;
    alpha.enable = 1;
    // if (ioctl(fd_fb, MXCFB_SET_GBL_ALPHA, &alpha) < 0)
    // {
    // ALOGE("Set global alpha failed\n");
    // close(fd_fb);
    // close(fd_capture_v4l);
    // close(fd_output_v4l);
    // return TFAIL;
    // }

    mxc_v4l_tvin_test();

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(fd_output_v4l, VIDIOC_STREAMOFF, &type);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_capture_v4l, VIDIOC_STREAMOFF, &type);

    for (i = 0; i < g_output_num_buffers; i++)
    {
        munmap(output_buffers[i].start, output_buffers[i].length);
    }
    for (i = 0; i < g_capture_num_buffers; i++)
    {
        munmap(capture_buffers[i].start, capture_buffers[i].length);
    }

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
    for (i = 0; i < 60; i++)
    {
        hasSignal = hasVideoSignal();
        ALOGI("hasSignal = %d,rvs_state = %d\n", hasSignal, rvs_state);
        if ((hasSignal == 1) || (rvs_state == 0))
        { //有视频信号,或者退出倒车
            break;
        } else
        {                            //无视频信号时不打开摄像头，延时再判断
            usleep(1 * 1000 * 1000); // 0.5s
        }
    }
    if (rvs_state == 1)
    {
        start_priview(); //这里会操作摄像头，直到退出倒车才结束次函数
        goto retry;
    }
    ALOGI("==>quick_camera_thread_func end<====\n");
    pthread_exit(NULL);

    return NULL;
}

void* cmdSocket(void* param)
{
    int msocket = -1;
    int ret     = -1;
    int cFd     = -1;

    struct sockaddr_un paddr;
    socklen_t          socklen = sizeof(paddr);
    char               buff[50];
    // char*              buffer = (char*)malloc(16);

    msocket = android_get_control_socket("backmodecar");
    if (msocket < 0)
    {
        ALOGE("can not get local socket=%d %s", msocket, strerror(errno));
        return NULL;
    }
    ret = listen(msocket, 4);
    if (ret < 0)
    {
        ALOGE("listen socket error %s", strerror(errno));
    }
    cFd = accept(msocket, (struct sockaddr*)&paddr, &socklen);
    if (cFd < 0)
    {
        ALOGE("accept socket error %s", strerror(errno));
    }
    struct timeval timeout;
    fd_set         read_fds;

    FD_ZERO(&read_fds);
    for (;;)
    {
        timeout.tv_sec  = 0;
        timeout.tv_usec = 100;
        FD_SET(cFd, &read_fds);

        ret = select(cFd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret <= 0)
        {
            continue;
        }

        if (FD_ISSET(cFd, &read_fds))
        {
            ret = recv(cFd, buff, sizeof(buff) - 1, 0);
            if (ret < 0)
            {
                ALOGE("receive record data error. (%s:%d)", __FILE__, __LINE__);
                continue;
            }
            if (ret > 0)
            {
                if (!strcmp(buff, "1"))
                {
                    ALOGI(" set radar value 1");
                    property_set("debug.backcar.radar", "1");
                }
                if (!strcmp(buff, "2"))
                {
                    ALOGI(" set radar value 2\n");
                    property_set("debug.backcar.radar", "2");
                }
                if (!strcmp(buff, "3"))
                {
                    ALOGI(" set radar value 3");
                    property_set("debug.backcar.radar", "3");
                } else
                {
                    ALOGI("receive from client %s ", buff);
                    continue;
                }
            }
        }
        return 0;
    }
}
int BackupVideo::StartBackupVideo()
{
    ALOGI("Start Back Video");
    int  ret           = 0;
    int  data          = 0;
    int  camera_opened = 0; //摄像头是否已打开
    char value[PROP_VALUE_MAX];
    qs_camera_fd  = open(camera_dev, O_RDWR);
    camera_sgn_fd = open(camera_sgn_dev, O_RDWR);
    ALOGI("==>open qs_camera_io = %d,camera_sgn_fd = %d<==\n", qs_camera_fd, camera_sgn_fd);
    if (qs_camera_fd < 0)
    {
        ALOGE("open /dev/car_rvs error %s ", strerror(errno));
        return -1;
    }
    pthread_t pThreadcmdSocket;
    pthread_create(&pThreadcmdSocket, NULL, cmdSocket, NULL);
    int stopBoot = 0;
    int bootin   = 1;
    while (1)
    {
        // rvs_state = 1;
        fb0_state = (data >> 1) & 0x01;
        property_get("debug.backcar.start", value, "0");
        int bc = atoi(value);
        property_get("service.bkcar.exit", value, "0");
        int bk = atoi(value);
        if (bc == 1)
        { //开始倒车,有AVM时以AVM请求显示信号为准
            property_set("ctl.stop", "bootanim");
            rvs_state = 1;
            if (camera_opened == 0)
            { //摄像头未打开过
                camera_opened = 1;
                if (pthread_create(&state_thread_id, NULL, qc_thread_func, NULL) != 0)
                {
                    ALOGI("Create thread error!\n");
                }
            }
        }
        if (bk == 1)
        { //退出倒车,有AVM时以AVM退出请求信号为准
            rvs_state = 0;
            usleep(500000); //  0.5s
            camera_opened = 0;
            rvs_state     = 0;
            pthread_join(state_thread_id, NULL);
            property_set("service.bkcar.exit", 0);
            property_get("service.bootanim.exit", value, "0");
            int bm = atoi(value);
            if (!bm)
            {
                ALOGI("continue bottanim");
                property_set("ctl.start", "bootanim");
            }
        }
    }
    return 0;
}
};
