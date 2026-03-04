#include "ffmpeg_decoder.hpp"
#include <iostream>
#include "simple-logger.hpp"

using namespace std;

namespace FFHDDecoder {

    static inline bool check_ffmpeg_retvalue(int e, const char* call, int iLine, const char* szFile) {
        if (e < 0) {
            char errbuf[128];
            av_strerror(e, errbuf, sizeof(errbuf));
            std::cout << "FFMPEGDecoder error " << call << ", code = " << e 
                      << " (" << errbuf << ") in file " << szFile << ":" << iLine << std::endl;
            return false;
        }
        return true;
    }

#define checkFFMPEG(call) check_ffmpeg_retvalue(call, #call, __LINE__, __FILE__)

    class FFmpegDecoderImpl : public FFmpegDecoder {
    public:
        FFmpegDecoderImpl() {
            m_packet = av_packet_alloc();
            m_frame = av_frame_alloc();
        }

        ~FFmpegDecoderImpl() override {
            close();
        }

        bool open(AVCodecID codec_id, uint8_t* extradata, int extradata_size) {
            // 1. 查找解码器
            const AVCodec* codec = avcodec_find_decoder(codec_id);
            if (!codec) {
                INFOE("FFmpeg error: Unsupported codec ID");
                return false;
            }

            // 2. 分配解码器上下文
            m_ctx = avcodec_alloc_context3(codec);
            if (!m_ctx) {
                INFOE("FFmpeg error: Could not allocate video codec context");
                return false;
            }

            // 3. 复制 extradata (SPS/PPS 等头部信息)
            if (extradata && extradata_size > 0) {
                m_ctx->extradata = (uint8_t*)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (m_ctx->extradata) {
                    memcpy(m_ctx->extradata, extradata, extradata_size);
                    m_ctx->extradata_size = extradata_size;
                }
            }

            // 4. 打开解码器
            if (!checkFFMPEG(avcodec_open2(m_ctx, codec, nullptr))) {
                return false;
            }

            return true;
        }

        void close() {
            if (m_sws_ctx) {
                sws_freeContext(m_sws_ctx);
                m_sws_ctx = nullptr;
            }
            if (m_ctx) {
                avcodec_free_context(&m_ctx);
            }
            if (m_frame) {
                av_frame_free(&m_frame);
            }
            if (m_packet) {
                av_packet_free(&m_packet);
            }
        }

        bool send_packet(const uint8_t* pData, int nSize, int64_t pts) override {
            if (!m_ctx) return false;

            // 如果 pData 为 nullptr 且 nSize == 0，代表 Flush，送入 nullptr packet 即可
            if (pData == nullptr || nSize == 0) {
                return checkFFMPEG(avcodec_send_packet(m_ctx, nullptr));
            }

            m_packet->data = (uint8_t*)pData;
            m_packet->size = nSize;
            m_packet->pts = pts;

            // 发送数据包到解码器
            int ret = avcodec_send_packet(m_ctx, m_packet);
            
            // 清理 packet 状态，避免内存泄漏警告
            av_packet_unref(m_packet);

            return checkFFMPEG(ret);
        }

        bool receive_frame(AVFrame** pOutFrame) override {
            if (!m_ctx || !m_frame) return false;

            // 尝试从解码器接收解码后的原始帧
            int ret = avcodec_receive_frame(m_ctx, m_frame);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                // EAGAIN: 需要更多输入包才能解码出一帧
                // EOF: 已经到达流的末尾（Flush完毕）
                return false;
            } else if (ret < 0) {
                INFOE("Error during decoding");
                return false;
            }

            *pOutFrame = m_frame;
            return true;
        }

        int get_width() override { return m_ctx ? m_ctx->width : 0; }
        int get_height() override { return m_ctx ? m_ctx->height : 0; }
        AVPixelFormat get_pix_fmt() override { return m_ctx ? m_ctx->pix_fmt : AV_PIX_FMT_NONE; }

        // 辅助工具：将解码出的 YUV 转换为 BGR (方便送给 OpenCV 或显示)
        bool convert_to_bgr(AVFrame* frame, uint8_t* bgr_buffer, int bgr_linesize) override {
            if (!frame || !bgr_buffer) return false;

            AVPixelFormat src_fmt = (AVPixelFormat)frame->format;
            switch (src_fmt) {
                case AV_PIX_FMT_YUVJ420P: src_fmt = AV_PIX_FMT_YUV420P; break;
                case AV_PIX_FMT_YUVJ422P: src_fmt = AV_PIX_FMT_YUV422P; break;
                case AV_PIX_FMT_YUVJ444P: src_fmt = AV_PIX_FMT_YUV444P; break;
                case AV_PIX_FMT_YUVJ440P: src_fmt = AV_PIX_FMT_YUV440P; break;
                default: break;
            }

            // 2. 创建或获取复用的 swscale 上下文
            m_sws_ctx = sws_getCachedContext(m_sws_ctx,
                frame->width, frame->height, src_fmt,
                frame->width, frame->height, AV_PIX_FMT_BGR24,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

            if (!m_sws_ctx) return false;

            // 3. 将 color range 设置给 swscale（可选，但这能保证色彩的绝对准确，避免发灰）
            // 如果原本是全色域，这一步能保证转换成 BGR 时色彩对比度不受损
            int* inv_table = nullptr;
            int* table = nullptr;
            int src_range, dst_range, brightness, contrast, saturation;
            if (sws_getColorspaceDetails(m_sws_ctx, &inv_table, &src_range, &table, &dst_range, 
                                         &brightness, &contrast, &saturation) >= 0) {
                // 如果原始帧明确标记了 JPEG 范围，或者是废弃的 J 格式，强制设置为全色域 1
                src_range = (frame->color_range == AVCOL_RANGE_JPEG || 
                             frame->format == AV_PIX_FMT_YUVJ420P) ? 1 : 0;
                sws_setColorspaceDetails(m_sws_ctx, inv_table, src_range, table, dst_range, 
                                         brightness, contrast, saturation);
            }

            // 4. 执行格式转换
            uint8_t* dest_data[4] = { bgr_buffer, nullptr, nullptr, nullptr };
            int dest_linesize[4] = { bgr_linesize, 0, 0, 0 };

            int ret = sws_scale(m_sws_ctx, frame->data, frame->linesize, 0, frame->height,
                                dest_data, dest_linesize);
            return ret > 0;
        }

    private:
        AVCodecContext* m_ctx = nullptr;
        AVPacket* m_packet = nullptr;
        AVFrame* m_frame = nullptr;
        SwsContext* m_sws_ctx = nullptr;
    };

    std::shared_ptr<FFmpegDecoder> create_ffmpeg_decoder(AVCodecID codec_id, uint8_t* extradata, int extradata_size) {
        std::shared_ptr<FFmpegDecoderImpl> instance(new FFmpegDecoderImpl());
        if (!instance->open(codec_id, extradata, extradata_size)) {
            instance.reset();
        }
        return instance;
    }
}