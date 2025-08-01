#include "video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <signal.h>

// #define DEVICE "/dev/video0"
#define WIDTH  640
#define HEIGHT 480
// #define FILENAME "output.mp4"
#define FRAME_RATE 30
#define BIT_RATE 400000
// #define MAX_FRAMES 100

volatile sig_atomic_t stop = 0;

void handle_sigint(int sig) {
    (void) sig;
    stop = 1;
}

void cleanup_resources(AVFormatContext *fmt_ctx, AVCodecContext *codec_ctx, 
                      AVFrame *frame, AVPacket *pkt, struct SwsContext *sws_ctx,
                      void *buffer_start, size_t buffer_length, int fd) {
    // FFmpeg资源清理
    if (fmt_ctx) {
        av_write_trailer(fmt_ctx);
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
    }
    if (codec_ctx)
        avcodec_free_context(&codec_ctx);
    if (frame)
        av_frame_free(&frame);
    if (pkt)
        av_packet_free(&pkt);
    if (sws_ctx)
        sws_freeContext(sws_ctx);
    
    // V4L2资源清理
    if (buffer_start)
        munmap(buffer_start, buffer_length);
    if (fd != -1)
        close(fd);
}

int getVideo(const char * device, int max_frames, const char *filename) {
    // V4L2相关变量
    int fd = -1;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    enum v4l2_buf_type type;
    void *buffer_start = NULL;
    size_t buffer_length = 0;
    
    // FFmpeg相关变量
    AVFormatContext *fmt_ctx = NULL;
    AVStream *video_stream = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws_ctx = NULL;
    int ret;
    
    // 注册信号处理
    signal(SIGINT, handle_sigint);
    
    // 1. 初始化FFmpeg
    avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, filename);
    if (!fmt_ctx) {
        fprintf(stderr, "无法创建输出上下文\n");
        cleanup_resources(NULL, NULL, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    // 2. 查找并配置编码器
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "找不到H.264编码器\n");
        cleanup_resources(fmt_ctx, NULL, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    video_stream = avformat_new_stream(fmt_ctx, codec);
    if (!video_stream) {
        fprintf(stderr, "无法创建视频流\n");
        cleanup_resources(fmt_ctx, NULL, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "无法分配编码器上下文\n");
        cleanup_resources(fmt_ctx, NULL, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    codec_ctx->width = WIDTH;
    codec_ctx->height = HEIGHT;
    codec_ctx->time_base = (AVRational){1, FRAME_RATE};
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx->bit_rate = BIT_RATE;
    codec_ctx->gop_size = 10;
    codec_ctx->max_b_frames = 1;
    
    // 3. 打开编码器
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "无法打开编码器\n");
        cleanup_resources(fmt_ctx, codec_ctx, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    // 4. 配置流参数
    avcodec_parameters_from_context(video_stream->codecpar, codec_ctx);
    video_stream->time_base = codec_ctx->time_base;
    
    // 5. 打开输出文件
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "无法打开输出文件\n");
            cleanup_resources(fmt_ctx, codec_ctx, NULL, NULL, NULL, NULL, 0, -1);
            return 1;
        }
    }
    
    // 6. 写入文件头
    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "写入文件头失败\n");
        cleanup_resources(fmt_ctx, codec_ctx, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    // 7. 分配帧和包
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "无法分配视频帧\n");
        cleanup_resources(fmt_ctx, codec_ctx, NULL, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;
    
    if (av_frame_get_buffer(frame, 0) < 0) {
        fprintf(stderr, "无法分配帧数据\n");
        cleanup_resources(fmt_ctx, codec_ctx, frame, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "无法分配AVPacket\n");
        cleanup_resources(fmt_ctx, codec_ctx, frame, NULL, NULL, NULL, 0, -1);
        return 1;
    }
    
    // 8. 初始化V4L2
    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("打开设备失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, -1);
        return 1;
    }
    
    // 查询设备能力
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("查询设备能力失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, fd);
        return 1;
    }
    
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "设备不支持视频捕获\n");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, fd);
        return 1;
    }
    
    // 设置视频格式
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("设置格式失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, fd);
        return 1;
    }
    
    // 申请缓冲区
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("申请缓冲区失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, fd);
        return 1;
    }
    
    // 映射内存
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        perror("查询缓冲区失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, fd);
        return 1;
    }
    
    buffer_length = buf.length;
    buffer_start = mmap(NULL, buf.length, 
                       PROT_READ | PROT_WRITE, 
                       MAP_SHARED, 
                       fd, 
                       buf.m.offset);
    
    if (buffer_start == MAP_FAILED) {
        perror("内存映射失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, NULL, 0, fd);
        return 1;
    }
    
    // 将缓冲区放入队列
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("放入缓冲区失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, buffer_start, buffer_length, fd);
        return 1;
    }
    
    // 开始捕获
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("开始捕获失败");
        cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, NULL, buffer_start, buffer_length, fd);
        return 1;
    }
    
    printf("开始捕获视频，按Ctrl+C停止...\n");
    
    int frame_count = 0;
    while (!stop) {
        // 取出缓冲区
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            perror("捕获帧失败");
            break;
        }
        
        // 创建像素格式转换上下文
        sws_ctx = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_YUYV422,
                                WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws_ctx) {
            fprintf(stderr, "无法创建转换上下文\n");
            break;
        }
        
        // 准备源数据
        const uint8_t *src_data[1] = {buffer_start};
        int src_linesize[1] = {WIDTH * 2}; // YUYV是2字节/像素
        
        // 转换像素格式
        sws_scale(sws_ctx, src_data, src_linesize, 0, HEIGHT,
                  frame->data, frame->linesize);
        
        frame->pts = frame_count;
        frame_count ++;
        printf("已捕获 %d/%d 帧\r", frame_count, max_frames);
        fflush(stdout);

        // 编码帧
        if ((ret = avcodec_send_frame(codec_ctx, frame)) < 0) {
            fprintf(stderr, "发送帧到编码器失败: %s\n", av_err2str(ret));
            sws_freeContext(sws_ctx);
            break;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_packet(codec_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                fprintf(stderr, "编码错误: %s\n", av_err2str(ret));
                break;
            }
            
            // 调整时间戳并写入文件
            av_packet_rescale_ts(pkt, codec_ctx->time_base, video_stream->time_base);
            pkt->stream_index = video_stream->index;
            
            if (av_interleaved_write_frame(fmt_ctx, pkt) < 0) {
                fprintf(stderr, "写入帧失败\n");
                av_packet_unref(pkt);
                break;
            }
            
            av_packet_unref(pkt);
        }
        
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
        
        // 重新排队缓冲区
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("重新排队缓冲区失败");
            break;
        }

        if (max_frames != -1 && frame_count >= max_frames) {
            printf("捕获完成\n");
            break;
        }
    }
    
    // 刷新编码器缓冲区
    if (!stop) {
        avcodec_send_frame(codec_ctx, NULL); // 发送空帧刷新
        
        while (1) {
            ret = avcodec_receive_packet(codec_ctx, pkt);
            if (ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                fprintf(stderr, "刷新编码器失败: %s\n", av_err2str(ret));
                break;
            }
            
            av_packet_rescale_ts(pkt, codec_ctx->time_base, video_stream->time_base);
            pkt->stream_index = video_stream->index;
            
            if (av_interleaved_write_frame(fmt_ctx, pkt) < 0) {
                fprintf(stderr, "写入刷新帧失败\n");
                break;
            }
            
            av_packet_unref(pkt);
        }
    }
    
    // 清理资源
    cleanup_resources(fmt_ctx, codec_ctx, frame, pkt, sws_ctx, buffer_start, buffer_length, fd);
    printf("视频捕获完成！保存为 %s\n", filename);
    return 0;
}