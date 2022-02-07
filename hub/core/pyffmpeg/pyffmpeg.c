#include "avcodec.h"
#include "avformat.h"
#include "swscale.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>

static void logging(const char *fmt, ...);
static int decode_video_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame, unsigned char **decompressed, struct SwsContext **sws_context, int *bufpos, int64_t *ts);
int readFunc(void *opaque, uint8_t *buf, int buf_size);

struct buffer_data
{
    uint8_t *ptr;
    size_t size;
};

int getVideoShape(unsigned char *file, int size, int ioBufferSize, int *shape, int isBytes)
{
    av_log_set_level(AV_LOG_QUIET); // Some warning messages are being spammed even though it does not affect decompression.
    AVFormatContext *pFormatContext = NULL;
    AVIOContext *pioContext = NULL;
    AVDictionary *d = NULL;
    unsigned char *ioBuffer;
    pFormatContext = avformat_alloc_context();
    struct buffer_data bd = {0};

    int ret;

    if (!pFormatContext)
    {
        logging("ERROR could not allocate memory for Format Context");
        return -1;
    }

    av_dict_set(&d, "protocol_whitelist", "file,http,https,tcp,tls,subfile", 0);

    if (isBytes == 1)
    {
        bd.ptr = file;
        bd.size = size;
        ioBuffer = av_malloc(ioBufferSize);
        pioContext = avio_alloc_context(ioBuffer, ioBufferSize, 0, &bd, &readFunc, NULL, NULL);
        pFormatContext->pb = pioContext;
        ret = avformat_open_input(&pFormatContext, NULL, NULL, &d);
    }
    else
    {
        ret = avformat_open_input(&pFormatContext, (const char *)file, NULL, &d);
    }

    if (ret != 0)
    {
        logging("ERROR could not open the file");
        return -1;
    }

    if (avformat_find_stream_info(pFormatContext, NULL) < 0)
    {
        logging("ERROR could not get the stream info");
        return -1;
    }

    for (unsigned int i = 0; i < pFormatContext->nb_streams; i++)
    {

        AVCodecParameters *pLocalCodecParameters = NULL;
        pLocalCodecParameters = pFormatContext->streams[i]->codecpar;

        if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            double fps = av_q2d(pFormatContext->streams[i]->avg_frame_rate);
            double timebase = av_q2d(pFormatContext->streams[i]->time_base);
            double duration = pFormatContext->streams[i]->duration * timebase;
            if (duration < 0)
            {
                duration = (double)pFormatContext->duration / AV_TIME_BASE;
            }
            int n_frames = (int)(duration * fps);
            int width = pLocalCodecParameters->width;
            int height = pLocalCodecParameters->height;
            shape[0] = n_frames;
            shape[1] = height;
            shape[2] = width;
            break;
        }
    }
    avformat_close_input(&pFormatContext);
    if (isBytes == 1)
    {
        if (pioContext)
        {
            av_freep(&pioContext->buffer);
            av_freep(&pioContext);
        }
    }
    return 0;
}

int decompressVideo(unsigned char *file, int size, int ioBufferSize, int start_frame, int step, unsigned char *decompressed, int isBytes, int nbytes)
{
    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext *pFormatContext = NULL;
    AVIOContext *pioContext = NULL;
    AVDictionary *d = NULL;
    unsigned char *ioBuffer;
    pFormatContext = avformat_alloc_context();
    struct buffer_data bd = {0};

    int ret;

    if (!pFormatContext)
    {
        logging("ERROR could not allocate memory for Format Context");
        return -1;
    }

    av_dict_set(&d, "protocol_whitelist", "file,http,https,tcp,tls,subfile", 0);

    if (isBytes == 1)
    {
        bd.ptr = file;
        bd.size = size;
        ioBuffer = av_malloc(ioBufferSize);
        pioContext = avio_alloc_context(ioBuffer, ioBufferSize, 0, &bd, &readFunc, NULL, NULL);
        pFormatContext->pb = pioContext;
        ret = avformat_open_input(&pFormatContext, NULL, NULL, &d);
    }
    else
    {
        ret = avformat_open_input(&pFormatContext, (const char *)file, NULL, &d);
    }

    if (ret != 0)
    {
        logging("ERROR could not open the file");
        return -1;
    }

    if (avformat_find_stream_info(pFormatContext, NULL) < 0)
    {
        logging("ERROR could not get the stream info");
        return -1;
    }

    int video_stream_index = -1;
    AVCodec *pCodec = NULL;
    AVCodecParameters *pCodecParameters = NULL;

    for (unsigned int i = 0; i < pFormatContext->nb_streams; i++)
    {

        AVCodecParameters *pLocalCodecParameters = NULL;
        pLocalCodecParameters = pFormatContext->streams[i]->codecpar;
        AVCodec *pLocalCodec = NULL;
        pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

        if (pLocalCodec == NULL)
        {
            logging("ERROR unsupported codec!");
            continue;
        }

        if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_index = i;
            pCodec = pLocalCodec;
            pCodecParameters = pLocalCodecParameters;
            break;
        }
    }

    if (video_stream_index == -1)
    {
        logging("Input does not contain a video stream!");
        return -1;
    }

    float fps = (float)pFormatContext->streams[video_stream_index]->avg_frame_rate.num / (float)pFormatContext->streams[video_stream_index]->avg_frame_rate.den;
    float start_time = start_frame / fps;
    int64_t seek_target = (int64_t)(start_time * AV_TIME_BASE);
    int64_t step_time = (int64_t)((step / fps) * AV_TIME_BASE);

    seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q, pFormatContext->streams[video_stream_index]->time_base);
    step_time = av_rescale_q(step_time, AV_TIME_BASE_Q, pFormatContext->streams[video_stream_index]->time_base);

    AVCodecContext *pCodecContext = avcodec_alloc_context3(pCodec);

    if (!pCodecContext)
    {
        logging("failed to allocated memory for AVCodecContext");
        return -1;
    }

    if (avcodec_parameters_to_context(pCodecContext, pCodecParameters) < 0)
    {
        logging("failed to copy codec params to codec context");
        return -1;
    }

    if (avcodec_open2(pCodecContext, pCodec, NULL) < 0)
    {
        logging("failed to open codec through avcodec_open2");
        return -1;
    }

    AVFrame *pFrame = av_frame_alloc();

    if (!pFrame)
    {
        logging("failed to allocated memory for AVFrame");
        return -1;
    }

    AVPacket *pPacket = av_packet_alloc();

    if (!pPacket)
    {
        logging("failed to allocated memory for AVPacket");
        return -1;
    }

    struct SwsContext *sws_context = NULL;

    av_seek_frame(pFormatContext, video_stream_index, seek_target, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(pCodecContext);

    int response = 0;
    int bufpos = 0;
    unsigned char *start = decompressed;
    while (av_read_frame(pFormatContext, pPacket) >= 0)
    {
        if (pPacket->stream_index == video_stream_index)
        {
            response = decode_video_packet(pPacket, pCodecContext, pFrame, &decompressed, &sws_context, &bufpos, &seek_target);
            decompressed = start + bufpos;
            if (response < 0)
                break;
            if (bufpos >= nbytes)
                break;
            if (response == 1) // if frame written
            {
                if (step > 1)
                {
                    seek_target += step_time;
                    av_seek_frame(pFormatContext, video_stream_index, seek_target, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(pCodecContext);
                }
            }
        }
        av_packet_unref(pPacket);
    }
    avformat_close_input(&pFormatContext);
    av_packet_free(&pPacket);
    av_frame_free(&pFrame);
    avcodec_free_context(&pCodecContext);
    if (isBytes == 1)
    {
        if (pioContext)
        {
            av_freep(&pioContext->buffer);
            av_freep(&pioContext);
        }
    }
    return 0;
}

static int decode_video_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame, unsigned char **decompressed, struct SwsContext **sws_context, int *bufpos, int64_t *ts)
{
    int response = avcodec_send_packet(pCodecContext, pPacket);
    while (response >= 0)
    {
        response = avcodec_receive_frame(pCodecContext, pFrame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
        {
            break;
        }
        else if (response < 0)
        {
            return response;
        }

        if (response >= 0)
        {
            if (pFrame->pts >= *ts)
            {
                int height = pFrame->height;
                int width = pFrame->width;
                const int out_linesize[1] = {3 * width};
                (*sws_context) = sws_getCachedContext((*sws_context), width, height, pFrame->format, width, height, AV_PIX_FMT_RGB24, 0, 0, 0, 0);
                sws_scale((*sws_context), (const uint8_t *const *)&pFrame->data, pFrame->linesize, 0, height, (uint8_t *const *)decompressed, out_linesize);
                *bufpos += height * width * 3;
                return 1;
            }
        }
        break;
    }
    return 0;
}

int readFunc(void *opaque, uint8_t *buf, int buf_size)
{
    struct buffer_data *bd = (struct buffer_data *)opaque;
    buf_size = FFMIN(buf_size, (int)bd->size);
    memmove(buf, bd->ptr, buf_size);
    bd->ptr += buf_size;
    bd->size -= buf_size;

    return buf_size;
}

static void logging(const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "LOG: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
