#ifndef VEDIO_H
#define VEDIO_H

/*
    device:         "/dev/video0"
    max_frames:     -1 不停止
    filename:       "/home/xiaxianzi/videos/output.mp4"
*/

int getVideo(const char *device, int max_frames, const char *filename);
#endif
