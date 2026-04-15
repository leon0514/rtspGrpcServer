#include "ffmpeg_demuxer.hpp"
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <cstring>
#include "simple-logger.hpp"

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
    // 检查FFmpeg返回值的工具函数
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

    // 判断字符串开头工具函数
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
            // avformat_network_init();
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
            // avformat_network_deinit();
        }

        bool open(const string &uri, bool auto_reboot = true, int64_t timescale = 1000, bool only_key_frames = false)
        {
            close();
            this->uri_opened_ = uri;
            this->time_scale_opened_ = timescale;
            this->auto_reboot_ = auto_reboot;
            this->only_key_frames_ = only_key_frames;
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
                return false;
            if (!flag_is_opened_)
                return false;

            close();
            return this->open(this->uri_opened_, this->auto_reboot_, this->time_scale_opened_, this->only_key_frames_);
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
                m_bsfc = nullptr;
            }

            if (m_fmtc)
            {
                avformat_close_input(&m_fmtc);
                m_fmtc = nullptr;
            }

            if (m_avioc)
            {
                if (m_avioc->buffer)
                {
                    av_freep(&m_avioc->buffer);
                }
                avio_context_free(&m_avioc);
                m_avioc = nullptr;
            }

            // if (m_pDataWithHeader)
            // {
            //     av_free(m_pDataWithHeader);
            //     m_pDataWithHeader = nullptr;
            // }
            m_pDataWithHeader.clear();
            m_pDataWithHeader.shrink_to_fit();
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

            while (true)
            {
                // 1. [修复核心] 首先检查并排空 BSF (位流过滤器) 内部遗留的包
                if (m_bMp4H264 || m_bMp4HEVC)
                {
                    int recv_ret = av_bsf_receive_packet(m_bsfc, m_pktFiltered);
                    if (recv_ret == 0) // 成功从过滤器拿到一个包
                    {
                        *ppVideo = m_pktFiltered->data;
                        *pnVideoBytes = m_pktFiltered->size;
                        local_pts = (int64_t)(m_pktFiltered->pts * m_userTimeScale * m_timeBase);
                        if (pts)
                            *pts = local_pts;
                        if (iskey_frame)
                            *iskey_frame = m_pktFiltered->flags & AV_PKT_FLAG_KEY;
                        m_frameCount++;
                        return true; // 拿到包后立即返回，下次进入依然会先尝试 Receive
                    }
                    else if (recv_ret != AVERROR(EAGAIN) && recv_ret != AVERROR_EOF)
                    {
                        INFOE("Error during bitstream filtering receive");
                        return false;
                    }
                    // 如果返回 EAGAIN，说明 BSF 缓存空了，需要走下方逻辑读取新包送进去
                }

                // 2. 只有当 BSF 没有缓存时，才从 Demuxer 读取新包
                av_packet_unref(m_pkt);
                int e = av_read_frame(m_fmtc, m_pkt);
                if (e >= 0)
                {
                    if (m_pkt->stream_index != m_iVideoStream)
                    {
                        continue; // 丢弃音频等非视频包
                    }
                    if (this->only_key_frames_ && !(m_pkt->flags & AV_PKT_FLAG_KEY))
                    {
                        continue;
                    }
                }
                else
                {
                    av_packet_unref(m_pkt);
                    if (retries < max_retries)
                    {
                        INFOE("Read frame failed. Attempting to reopen...");
                        if (!this->reopen())
                            return false;
                        is_reboot_ = true;
                        retries++;
                        continue;
                    }
                    return false; // EOF 或读取失败
                }

                // 3. 将新读取的包送入 BSF
                if (m_bMp4H264 || m_bMp4HEVC)
                {
                    int send_ret = av_bsf_send_packet(m_bsfc, m_pkt);
                    if (send_ret < 0)
                    {
                        continue; // 忽略错误包，继续读下一个
                    }
                    // 送入成功后，continue 回到循环顶部去 Receive 这个包
                    continue;
                }
                else
                {
                    // MPEG4 等不需要 BSF 处理的分支保持原逻辑
                    if (m_bMp4MPEG4 && (m_frameCount == 0))
                    {
                        int extraDataSize = m_fmtc->streams[m_iVideoStream]->codecpar->extradata_size;
                        if (extraDataSize > 0)
                        {
                            int total_size = extraDataSize + m_pkt->size - 3 * sizeof(uint8_t);
                            m_pDataWithHeader.resize(total_size);

                            memcpy(m_pDataWithHeader.data(), m_fmtc->streams[m_iVideoStream]->codecpar->extradata, extraDataSize);
                            memcpy(m_pDataWithHeader.data() + extraDataSize, m_pkt->data + 3, m_pkt->size - 3 * sizeof(uint8_t));

                            *ppVideo = m_pDataWithHeader.data();
                            *pnVideoBytes = total_size;
                        }
                    }
                    else
                    {
                        *ppVideo = m_pkt->data;
                        *pnVideoBytes = m_pkt->size;
                    }
                    local_pts = (int64_t)(m_pkt->pts * m_userTimeScale * m_timeBase);
                    if (pts)
                        *pts = local_pts;
                    if (iskey_frame)
                        *iskey_frame = m_pkt->flags & AV_PKT_FLAG_KEY;
                    m_frameCount++;
                    return true;
                }
            }
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

        bool open(AVFormatContext *fmtc, int64_t timeScale = 1000)
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
            m_avioc = avio_alloc_context(avioc_buffer, avioc_buffer_size, 0, pDataProvider.get(), &ReadPacket, nullptr, nullptr);
            if (!m_avioc)
            {
                INFOE("FFmpeg error alloc avio context");
                av_free(avioc_buffer);
                avformat_free_context(ctx);
                return nullptr;
            }
            ctx->pb = m_avioc;

            if (avformat_open_input(&ctx, nullptr, nullptr, nullptr) < 0)
            {
                INFOE("FFmpeg avformat_open_input failed");
                av_free(avioc_buffer);
                avio_context_free(&m_avioc);
                avformat_free_context(ctx);
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
                av_dict_set(&options, "buffer_size", "10485760", 0); // 10MB 底层网络防抖缓存
                av_dict_set(&options, "stimeout", "3000000", 0);     // 3秒超时

                // 【权衡后的探测参数】
                // 1MB - 2MB 是高清 HEVC 的安全线，足以容纳一个最大关键帧，又不会导致退流时残留太多
                av_dict_set(&options, "probesize", "2048000", 0);
                av_dict_set(&options, "analyzeduration", "2000000", 0); // 最多探测 2 秒
                // 减少分析过程中的多余丢包等待
                av_dict_set(&options, "flags", "low_delay", 0);
                // 如果你只需要视频不要音频，直接告诉 FFmpeg 别去花时间找音频流了
                av_dict_set(&options, "allowed_media_types", "video", 0);
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
                if (ctx)
                {
                    avformat_free_context(ctx);
                    ctx = nullptr;
                }
                return nullptr;
            }
            return ctx;
        }

    private:
        shared_ptr<DataProvider> m_pDataProvider;
        AVFormatContext *m_fmtc = nullptr;
        AVIOContext *m_avioc = nullptr;
        AVPacket *m_pkt = nullptr;
        AVPacket *m_pktFiltered = nullptr;
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
        // uint8_t *m_pDataWithHeader = nullptr;
        std::vector<uint8_t> m_pDataWithHeader;
        unsigned int m_frameCount = 0;

        string uri_opened_;
        int64_t time_scale_opened_ = 0;
        bool flag_is_opened_ = false;
        bool auto_reboot_ = false;
        bool is_reboot_ = false;
        bool only_key_frames_ = false;
    };

    std::shared_ptr<FFmpegDemuxer> create_ffmpeg_demuxer(const std::string &path, bool auto_reboot, bool only_key_frames)
    {
        std::shared_ptr<FFmpegDemuxerImpl> instance(new FFmpegDemuxerImpl());
        if (!instance->open(path, auto_reboot, 1000, only_key_frames))
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
};