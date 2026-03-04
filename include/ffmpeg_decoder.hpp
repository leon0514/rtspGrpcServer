#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace FFHDDecoder {

    class FFmpegDecoder {
    public:
        virtual ~FFmpegDecoder() = default;

        // 发送来自 Demuxer 的压缩数据包到解码器
        // 如果传入 pData = nullptr 且 nSize = 0，则代表清空(Flush)解码器内部缓存的剩余帧
        virtual bool send_packet(const uint8_t* pData, int nSize, int64_t pts = 0) = 0;

        // 接收解码后的原始帧 (调用一次 send_packet 可能需要循环调用多次 receive_frame)
        // 成功获取到一帧返回 true，需要更多数据或结束则返回 false
        virtual bool receive_frame(AVFrame** pOutFrame) = 0;

        // 获取解码后的视频信息
        virtual int get_width() = 0;
        virtual int get_height() = 0;
        virtual AVPixelFormat get_pix_fmt() = 0;

        // 辅助功能：将解码后的 AVFrame 转换为 BGR24 数据（常用于 OpenCV Mat）
        virtual bool convert_to_bgr(AVFrame* frame, uint8_t* bgr_buffer, int bgr_linesize) = 0;
    };

    // 工厂函数：创建解码器
    // codec_id 对应 Demuxer 中的 get_video_codec()
    // extradata 和 extradata_size 对于某些格式（如未过 BSF 滤镜的 MP4）是必需的
    std::shared_ptr<FFmpegDecoder> create_ffmpeg_decoder(AVCodecID codec_id, 
                                                         uint8_t* extradata = nullptr, 
                                                         int extradata_size = 0);
}