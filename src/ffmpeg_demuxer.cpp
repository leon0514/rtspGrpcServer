#include "ffmpeg_demuxer.hpp"
#include <iostream>
#include <string>
#include <memory>
#include "simple-logger.hpp" // 假设你有对应的头文件

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
};

using namespace std;

namespace FFHDDemuxer
{
    static inline bool check_ffmpeg_retvalue(int e, const char *call, int iLine, const char *szFile)
    {
        if (e < 0)
        {
            std::cout << "FFMPEGDemuxer error " << call << ", code = " << e << " in file " << szFile << ":" << iLine << std::endl;
            return false;
        }
        return true;
    }

#define checkFFMPEG(call) check_ffmpeg_retvalue(call, #call, __LINE__, __FILE__)

    static bool string_begin_with(const string &str, const string &with)
    {
        if (str.size() < with.size())
            return false;
        if (with.empty())
            return true;
        return memcmp(str.c_str(), with.c_str(), with.size()) == 0;
    }

    class FFmpegDemuxerImpl : public FFmpegDemuxer
    {
    public:
        FFmpegDemuxerImpl()
        {
            avformat_network_init();
            // 兼容 FFmpeg 新版本：必须动态分配 AVPacket
            m_pkt = av_packet_alloc();
            m_pktFiltered = av_packet_alloc();
        }

        ~FFmpegDemuxerImpl()
        {
            close();
            if (m_pkt)
                av_packet_free(&m_pkt);
            if (m_pktFiltered)
                av_packet_free(&m_pktFiltered);
            avformat_network_deinit();
        }

        bool open(const string &uri, bool auto_reboot = true, int64_t timescale = 1000 /*Hz*/)
        {
            close();
            this->uri_opened_ = uri;
            this->time_scale_opened_ = timescale;
            this->auto_reboot_ = auto_reboot;
            return this->open(this->CreateFormatContext(uri), timescale);
        }

        bool open(shared_ptr<DataProvider> pDataProvider)
        {
            close();
            bool ok = this->open(this->CreateFormatContext(pDataProvider));
            if (ok)
            {
                m_avioc = m_fmtc->pb;
            }
            return ok;
        }

        bool reopen() override
        {
            if (m_pDataProvider)
                return false; // 内存流不支持自动重新打开
            if (!flag_is_opened_)
                return false;

            close();
            return this->open(this->uri_opened_, this->auto_reboot_, this->time_scale_opened_);
        }

        void close()
        {
            if (!flag_is_opened_ && !m_fmtc)
                return;

            if (m_pkt)
                av_packet_unref(m_pkt);
            if (m_pktFiltered)
                av_packet_unref(m_pktFiltered);

            if (m_bsfc)
            {
                av_bsf_free(&m_bsfc);
            }

            if (m_fmtc)
            {
                avformat_close_input(&m_fmtc); // 内部会自动将 m_fmtc 置 nullptr
            }

            if (m_avioc)
            {
                if (m_avioc->buffer)
                {
                    av_freep(&m_avioc->buffer);
                }
                avio_context_free(&m_avioc);
            }

            if (m_pDataWithHeader)
            {
                av_free(m_pDataWithHeader);
                m_pDataWithHeader = nullptr;
            }
            flag_is_opened_ = false;
        }

        IAVCodecID get_video_codec() override { return m_eVideoCodec; }
        IAVPixelFormat get_chroma_format() override { return m_eChromaFormat; }
        int get_fps() override { return m_fps; }
        int get_total_frames() override { return m_total_frames; }
        int get_width() override { return m_nWidth; }
        int get_height() override { return m_nHeight; }
        int get_bit_depth() override { return m_nBitDepth; }
        int get_frame_size() { return m_nWidth * (m_nHeight + m_nChromaHeight) * m_nBPP; }

        void get_extra_data(uint8_t **ppData, int *bytes) override
        {
            if (m_fmtc && m_iVideoStream >= 0)
            {
                // 修复：废弃的 codec 替换为 codecpar
                *ppData = m_fmtc->streams[m_iVideoStream]->codecpar->extradata;
                *bytes = m_fmtc->streams[m_iVideoStream]->codecpar->extradata_size;
            }
            else
            {
                *ppData = nullptr;
                *bytes = 0;
            }
        }

        bool demux(uint8_t **ppVideo, int *pnVideoBytes, int64_t *pts = nullptr, bool *iskey_frame = nullptr) override
        {
            *pnVideoBytes = 0;
            *ppVideo = nullptr;

            if (!m_fmtc)
                return false;

            int max_retries = auto_reboot_ ? 3 : 0;
            int retries = 0;
            int64_t local_pts = 0;

            // 外层循环：为了处理断线重连 以及 BSF 需要多帧 (EAGAIN) 的情况
            while (true)
            {
                av_packet_unref(m_pkt); // 清理旧包

                int e = av_read_frame(m_fmtc, m_pkt);
                if (e >= 0)
                {
                    if (m_pkt->stream_index != m_iVideoStream)
                    {
                        continue; // 非视频帧，继续读取
                    }
                }
                else
                {
                    // 读取失败（EOF或网络断开）
                    av_packet_unref(m_pkt);
                    if (retries < max_retries)
                    {
                        INFOE("Read frame failed. Attempting to reopen...");
                        if (!this->reopen())
                        {
                            INFOE("Reopen failed.");
                            return false;
                        }
                        is_reboot_ = true;
                        retries++;
                        continue; // 重连成功，重新尝试读取
                    }
                    return false; // 重试次数耗尽或不启用重连
                }

                if (iskey_frame)
                {
                    *iskey_frame = m_pkt->flags & AV_PKT_FLAG_KEY;
                }

                if (m_bMp4H264 || m_bMp4HEVC)
                {
                    av_packet_unref(m_pktFiltered); // 确保干净

                    int send_ret = av_bsf_send_packet(m_bsfc, m_pkt);
                    if (send_ret < 0)
                    {
                        av_packet_unref(m_pkt);
                        continue; // 送入滤镜失败，尝试读取下一帧
                    }

                    int recv_ret = av_bsf_receive_packet(m_bsfc, m_pktFiltered);
                    if (recv_ret == 0)
                    {
                        // 成功过滤出一帧
                        *ppVideo = m_pktFiltered->data;
                        *pnVideoBytes = m_pktFiltered->size;
                        local_pts = (int64_t)(m_pktFiltered->pts * m_userTimeScale * m_timeBase);
                        break; // 成功获取数据，跳出大循环
                    }
                    else if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF)
                    {
                        // 滤镜需要更多输入，继续拉取下一帧送入
                        continue;
                    }
                    else
                    {
                        INFOE("Error during bitstream filtering");
                        return false;
                    }
                }
                else
                {
                    if (m_bMp4MPEG4 && (m_frameCount == 0))
                    {
                        int extraDataSize = m_fmtc->streams[m_iVideoStream]->codecpar->extradata_size;
                        if (extraDataSize > 0)
                        {
                            m_pDataWithHeader = (uint8_t *)av_malloc(extraDataSize + m_pkt->size - 3 * sizeof(uint8_t));
                            if (!m_pDataWithHeader)
                            {
                                INFOE("FFmpeg error, m_pDataWithHeader alloc failed");
                                return false;
                            }
                            memcpy(m_pDataWithHeader, m_fmtc->streams[m_iVideoStream]->codecpar->extradata, extraDataSize);
                            memcpy(m_pDataWithHeader + extraDataSize, m_pkt->data + 3, m_pkt->size - 3 * sizeof(uint8_t));

                            *ppVideo = m_pDataWithHeader;
                            *pnVideoBytes = extraDataSize + m_pkt->size - 3 * sizeof(uint8_t);
                        }
                    }
                    else
                    {
                        *ppVideo = m_pkt->data;
                        *pnVideoBytes = m_pkt->size;
                    }
                    local_pts = (int64_t)(m_pkt->pts * m_userTimeScale * m_timeBase);
                    break; // 成功获取数据，跳出大循环
                }
            } // while(true) end

            if (pts)
                *pts = local_pts;
            m_frameCount++;
            return true;
        }

        virtual bool isreboot() override { return is_reboot_; }
        virtual void reset_reboot_flag() override { is_reboot_ = false; }

        static int ReadPacket(void *opaque, uint8_t *pBuf, int nBuf)
        {
            return ((DataProvider *)opaque)->get_data(pBuf, nBuf);
        }

    private:
        double r2d(AVRational r) const
        {
            return r.num == 0 || r.den == 0 ? 0. : (double)r.num / (double)r.den;
        }

        bool open(AVFormatContext *fmtc, int64_t timeScale = 1000 /*Hz*/)
        {
            if (!fmtc)
            {
                INFOE("No AVFormatContext provided.");
                return false;
            }

            this->m_fmtc = fmtc;

            if (!checkFFMPEG(avformat_find_stream_info(fmtc, nullptr)))
                return false;

            m_iVideoStream = av_find_best_stream(fmtc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (m_iVideoStream < 0)
            {
                INFOE("FFmpeg error: Could not find video stream in input file");
                return false;
            }

            m_frameCount = 0;
            m_eVideoCodec = fmtc->streams[m_iVideoStream]->codecpar->codec_id;
            m_nWidth = fmtc->streams[m_iVideoStream]->codecpar->width;
            m_nHeight = fmtc->streams[m_iVideoStream]->codecpar->height;
            m_eChromaFormat = (AVPixelFormat)fmtc->streams[m_iVideoStream]->codecpar->format;

            AVRational rTimeBase = fmtc->streams[m_iVideoStream]->time_base;
            m_timeBase = av_q2d(rTimeBase);
            m_userTimeScale = timeScale;
            m_fps = r2d(fmtc->streams[m_iVideoStream]->avg_frame_rate);
            m_total_frames = fmtc->streams[m_iVideoStream]->nb_frames;

            switch (m_eChromaFormat)
            {
            case AV_PIX_FMT_YUV420P10LE:
                m_nBitDepth = 10;
                m_nChromaHeight = (m_nHeight + 1) >> 1;
                m_nBPP = 2;
                break;
            case AV_PIX_FMT_YUV420P12LE:
                m_nBitDepth = 12;
                m_nChromaHeight = (m_nHeight + 1) >> 1;
                m_nBPP = 2;
                break;
            case AV_PIX_FMT_YUV444P10LE:
                m_nBitDepth = 10;
                m_nChromaHeight = m_nHeight << 1;
                m_nBPP = 2;
                break;
            case AV_PIX_FMT_YUV444P12LE:
                m_nBitDepth = 12;
                m_nChromaHeight = m_nHeight << 1;
                m_nBPP = 2;
                break;
            case AV_PIX_FMT_YUV444P:
                m_nBitDepth = 8;
                m_nChromaHeight = m_nHeight << 1;
                m_nBPP = 1;
                break;
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
            case AV_PIX_FMT_YUVJ422P:
            case AV_PIX_FMT_YUVJ444P:
                m_nBitDepth = 8;
                m_nChromaHeight = (m_nHeight + 1) >> 1;
                m_nBPP = 1;
                break;
            default:
                INFOW("ChromaFormat not recognized. Assuming 420");
                m_nBitDepth = 8;
                m_nChromaHeight = (m_nHeight + 1) >> 1;
                m_nBPP = 1;
            }

            m_bMp4H264 = m_eVideoCodec == AV_CODEC_ID_H264 && (!strcmp(fmtc->iformat->long_name, "QuickTime / MOV") || !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") || !strcmp(fmtc->iformat->long_name, "Matroska / WebM"));
            m_bMp4HEVC = m_eVideoCodec == AV_CODEC_ID_HEVC && (!strcmp(fmtc->iformat->long_name, "QuickTime / MOV") || !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") || !strcmp(fmtc->iformat->long_name, "Matroska / WebM"));
            m_bMp4MPEG4 = m_eVideoCodec == AV_CODEC_ID_MPEG4 && (!strcmp(fmtc->iformat->long_name, "QuickTime / MOV") || !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") || !strcmp(fmtc->iformat->long_name, "Matroska / WebM"));

            if (m_bMp4H264 || m_bMp4HEVC)
            {
                const char *filter_name = m_bMp4H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb";
                const AVBitStreamFilter *bsf = av_bsf_get_by_name(filter_name);
                if (!bsf)
                {
                    INFOE("FFmpeg error: av_bsf_get_by_name() failed");
                    return false;
                }
                if (!checkFFMPEG(av_bsf_alloc(bsf, &m_bsfc)))
                    return false;

                avcodec_parameters_copy(m_bsfc->par_in, fmtc->streams[m_iVideoStream]->codecpar);

                if (!checkFFMPEG(av_bsf_init(m_bsfc)))
                    return false;
            }

            this->flag_is_opened_ = true;
            return true;
        }

        AVFormatContext *CreateFormatContext(shared_ptr<DataProvider> pDataProvider)
        {
            AVFormatContext *ctx = avformat_alloc_context();
            if (!ctx)
            {
                INFOE("FFmpeg error alloc context");
                return nullptr;
            }

            int avioc_buffer_size = 8 * 1024 * 1024;
            uint8_t *avioc_buffer = (uint8_t *)av_malloc(avioc_buffer_size);
            if (!avioc_buffer)
            {
                INFOE("FFmpeg error malloc buffer");
                avformat_free_context(ctx);
                return nullptr;
            }

            m_pDataProvider = pDataProvider;
            m_avioc = avio_alloc_context(avioc_buffer, avioc_buffer_size,
                                         0, pDataProvider.get(), &ReadPacket, nullptr, nullptr);
            if (!m_avioc)
            {
                INFOE("FFmpeg error alloc avio context");
                av_free(avioc_buffer); // 修复泄漏1
                avformat_free_context(ctx);
                return nullptr;
            }
            ctx->pb = m_avioc;

            if (avformat_open_input(&ctx, nullptr, nullptr, nullptr) < 0)
            {
                INFOE("FFmpeg avformat_open_input failed");
                // 修复泄漏2: ctx 内部会处理自身，但我们在外部分配的 buffer/avio 需要手动释放
                av_free(avioc_buffer);
                avio_context_free(&m_avioc);
                return nullptr;
            }

            return ctx;
        }

        AVFormatContext *CreateFormatContext(const string &uri)
        {
            AVDictionary *options = nullptr;
            if (string_begin_with(uri, "rtsp://"))
            {
                av_dict_set(&options, "rtsp_transport", "tcp", 0);
                av_dict_set(&options, "buffer_size", "1024000", 0);
                av_dict_set(&options, "stimeout", "2000000", 0);
                av_dict_set(&options, "max_delay", "1000000", 0);
            }

            AVFormatContext *ctx = nullptr;
            int ret = avformat_open_input(&ctx, uri.c_str(), nullptr, &options);

            if (options)
            {
                av_dict_free(&options);
            }

            if (ret < 0)
            {
                INFOE("FFmpeg avformat_open_input failed");
                return nullptr;
            }
            return ctx;
        }

    private:
        shared_ptr<DataProvider> m_pDataProvider;
        AVFormatContext *m_fmtc = nullptr;
        AVIOContext *m_avioc = nullptr;
        AVPacket *m_pkt = nullptr;         // 替换为指针类型
        AVPacket *m_pktFiltered = nullptr; // 替换为指针类型
        AVBSFContext *m_bsfc = nullptr;

        int m_fps = 0;
        int m_total_frames = 0;
        int m_iVideoStream = -1;
        bool m_bMp4H264 = false, m_bMp4HEVC = false, m_bMp4MPEG4 = false;
        AVCodecID m_eVideoCodec = AV_CODEC_ID_NONE;
        AVPixelFormat m_eChromaFormat = AV_PIX_FMT_NONE;
        int m_nWidth = 0, m_nHeight = 0, m_nBitDepth = 0, m_nBPP = 0, m_nChromaHeight = 0;
        double m_timeBase = 0.0;
        int64_t m_userTimeScale = 0;
        uint8_t *m_pDataWithHeader = nullptr;
        unsigned int m_frameCount = 0;

        string uri_opened_;
        int64_t time_scale_opened_ = 0;
        bool flag_is_opened_ = false;
        bool auto_reboot_ = false;
        bool is_reboot_ = false;
    };

    std::shared_ptr<FFmpegDemuxer> create_ffmpeg_demuxer(const std::string &path, bool auto_reboot)
    {
        std::shared_ptr<FFmpegDemuxerImpl> instance(new FFmpegDemuxerImpl());
        if (!instance->open(path, auto_reboot))
            instance.reset();
        return instance;
    }

    std::shared_ptr<FFmpegDemuxer> create_ffmpeg_demuxer(std::shared_ptr<DataProvider> provider)
    {
        std::shared_ptr<FFmpegDemuxerImpl> instance(new FFmpegDemuxerImpl());
        if (!instance->open(provider))
            instance.reset();
        return instance;
    }
}; // namespace FFHDDemuxer