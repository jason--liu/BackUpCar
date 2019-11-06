/*
 * video_capture.h
 *
 *  Created on: Oct 24, 2019
 *      Author: liuqianyi
 */

#ifndef VIDEO_CAPTURE_H_
#define VIDEO_CAPTURE_H_

#ifdef __cplusplus
extern "C" {
#endif	// __cplusplus
/* check the supported webcam resolutions using $v4l2-ctl --list-formats-ext */
#define IM_WIDTH 720 
#define IM_HEIGHT 480
#define CLEAR(x) memset (&(x), 0, sizeof (x))

struct buffer {
	void * start;
	size_t length;
};

void init_video_capture();
char video_capture(unsigned char* dst);
void free_video_capture();
#ifdef __cplusplus
}
#endif	// __cplusplus

#endif /* VIDEO_CAPTURE_H_ */
