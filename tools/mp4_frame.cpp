/**
 * mp4_frame.cpp  —  Decode frames from an MP4 to BMP using FFmpeg (Linux port)
 *
 * Usage:
 *   mp4_frame <in.mp4> <out-prefix> [count] [skip]
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

#pragma pack(push, 1)
struct BmpFileHeader
{
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};
struct BmpInfoHeader
{
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
#pragma pack(pop)

static bool SaveBmp(const char* path, const uint8_t* pixels,
                    uint32_t width, uint32_t height, int32_t stride)
{
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    const uint32_t rowBytes = width * 4;
    BmpFileHeader fh = {};
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.bfSize    = fh.bfOffBits + rowBytes * height;

    BmpInfoHeader ih = {};
    ih.biSize        = sizeof(ih);
    ih.biWidth       = static_cast<int32_t>(width);
    ih.biHeight      = static_cast<int32_t>(height); // bottom-up
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = 0;
    ih.biSizeImage   = rowBytes * height;

    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y)
        std::fwrite(pixels + static_cast<ptrdiff_t>(y) * stride, rowBytes, 1, f);

    std::fclose(f);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::printf("Usage: mp4_frame <in.mp4> <out-prefix> [count] [skip]\n");
        return 1;
    }
    const char* inPath = argv[1];
    const char* prefix = argv[2];
    const int   count  = (argc > 3) ? std::atoi(argv[3]) : 1;
    const int   skip   = (argc > 4) ? std::atoi(argv[4]) : 0;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, inPath, nullptr, nullptr) < 0)
    {
        std::printf("avformat_open_input failed: %s\n", inPath);
        return 1;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) return 1;

    int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIdx < 0) { std::printf("No video stream found\n"); return 1; }

    AVStream* stream = fmtCtx->streams[streamIdx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, stream->codecpar);
    if (avcodec_open2(ctx, codec, nullptr) < 0) return 1;

    std::printf("[mp4] %dx%d\n", ctx->width, ctx->height);

    SwsContext* sws = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
                                     ctx->width, ctx->height, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    std::vector<uint8_t> bgra(static_cast<size_t>(ctx->width) * ctx->height * 4);
    uint8_t* dstSlice[1] = { bgra.data() };
    int dstStride[1]     = { ctx->width * 4 };

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    int emitted = 0, index = 0;

    while (emitted < count && av_read_frame(fmtCtx, pkt) >= 0)
    {
        if (pkt->stream_index == streamIdx && avcodec_send_packet(ctx, pkt) == 0)
        {
            while (emitted < count && avcodec_receive_frame(ctx, frame) == 0)
            {
                if (index++ >= skip)
                {
                    sws_scale(sws, frame->data, frame->linesize, 0, ctx->height, dstSlice, dstStride);
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s_%03d.bmp", prefix, emitted);
                    if (SaveBmp(path, bgra.data(), static_cast<uint32_t>(ctx->width),
                                static_cast<uint32_t>(ctx->height), ctx->width * 4))
                    {
                        std::printf("[mp4] %s\n", path);
                    }
                    ++emitted;
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmtCtx);
    return 0;
}
