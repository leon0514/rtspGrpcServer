#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include "stream_service.grpc.pb.h"
#include <opencv2/opencv.hpp>

// 与 zero_copy_channel.hpp 中 ShmMeta 完全一致
struct alignas(64) ShmMeta
{
    uint64_t actual_size;
    uint64_t width;
    uint64_t height;
    uint64_t timestamp;
    uint32_t channels;
    uint32_t depth;
    uint32_t step;
    uint32_t reserved;
};

// 与 zero_copy_channel.hpp 中 ShmLayoutInfo 字段一致
struct ShmLayout
{
    uint64_t slot_count = 0;
    uint64_t max_frame_bytes = 0;
    uint64_t alignment = 0;
    uint64_t slot_size = 0;
    uint64_t seq_offset = 0;
    uint64_t meta_offset = 0;
    uint64_t payload_offset = 0;
    uint64_t meta_data_size = 0;
    uint64_t head_idx_offset = 0;
    uint64_t total_size = 0;
};

struct StreamSnapshot
{
    std::string stream_id;
    std::string rtsp_url;
    bool use_shared_mem = false;
    bool connected = false;
};

static void printUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -s, --server <addr:port>  gRPC server address (default: 127.0.0.1:50051)\n"
              << "  -o, --output-dir <dir>    Output directory (default: ./saved_frames)\n"
              << "  -n, --count <n>           Frames per stream (default: 1)\n"
              << "  -i, --interval <sec>      Interval between frames in seconds (default: 0)\n"
              << "  -t, --timeout <ms>        Read timeout in milliseconds (default: 3000)\n"
              << "  -h, --help                Show this help message\n";
}

static bool parseArgs(int argc, char **argv,
                      std::string &server,
                      std::string &output_dir,
                      int &count,
                      double &interval_ms,
                      int &timeout_ms)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--server")
        {
            if (i + 1 >= argc) return false;
            server = argv[++i];
        }
        else if (arg == "-o" || arg == "--output-dir")
        {
            if (i + 1 >= argc) return false;
            output_dir = argv[++i];
        }
        else if (arg == "-n" || arg == "--count")
        {
            if (i + 1 >= argc) return false;
            count = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "-i" || arg == "--interval")
        {
            if (i + 1 >= argc) return false;
            interval_ms = std::max(0.0, std::atof(argv[++i]) * 1000.0);
        }
        else if (arg == "-t" || arg == "--timeout")
        {
            if (i + 1 >= argc) return false;
            timeout_ms = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "-h" || arg == "--help")
        {
            printUsage(argv[0]);
            std::exit(0);
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

static bool fetchShmLayout(streamingservice::RTSPStreamService::Stub &stub, ShmLayout &out)
{
    streamingservice::ShmLayoutRequest req;
    streamingservice::ShmLayoutResponse resp;
    grpc::ClientContext ctx;
    auto status = stub.GetShmLayout(&ctx, req, &resp);
    if (!status.ok() || !resp.success())
    {
        std::cerr << "Failed to get SHM layout: "
                  << (status.ok() ? resp.message() : status.error_message()) << "\n";
        return false;
    }

    const auto &l = resp.layout();
    out.slot_count = l.slot_count();
    out.max_frame_bytes = l.max_frame_bytes();
    out.alignment = l.alignment();
    out.slot_size = l.slot_size();
    out.seq_offset = l.seq_offset();
    out.meta_offset = l.meta_offset();
    out.payload_offset = l.payload_offset();
    out.meta_data_size = l.meta_data_size();
    out.head_idx_offset = l.head_idx_offset();
    out.total_size = l.total_size();
    return true;
}

static bool listStreams(streamingservice::RTSPStreamService::Stub &stub,
                        std::vector<StreamSnapshot> &out)
{
    streamingservice::ListStreamsRequest req;
    streamingservice::ListStreamsResponse resp;
    grpc::ClientContext ctx;
    auto status = stub.ListStreams(&ctx, req, &resp);
    if (!status.ok())
    {
        std::cerr << "ListStreams failed: " << status.error_message() << "\n";
        return false;
    }

    out.clear();
    for (const auto &s : resp.streams())
    {
        StreamSnapshot info;
        info.stream_id = s.stream_id();
        info.rtsp_url = s.rtsp_url();
        info.use_shared_mem = s.use_shared_mem();
        info.connected = (s.status() == streamingservice::STATUS_CONNECTED);
        out.push_back(info);
    }
    return true;
}

static bool fetchJpegFrame(streamingservice::RTSPStreamService::Stub &stub,
                           const std::string &stream_id,
                           cv::Mat &frame,
                           int64_t &timestamp)
{
    streamingservice::FrameRequest req;
    req.set_stream_id(stream_id);
    streamingservice::FrameResponse resp;
    grpc::ClientContext ctx;
    auto status = stub.GetLatestFrame(&ctx, req, &resp);
    if (!status.ok())
    {
        std::cerr << "GetLatestFrame failed: " << status.error_message() << "\n";
        return false;
    }
    if (!resp.success() || resp.image_data().empty())
    {
        std::cerr << "No JPEG frame available: " << resp.message() << "\n";
        return false;
    }

    std::vector<uint8_t> buf(resp.image_data().begin(), resp.image_data().end());
    frame = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (frame.empty())
    {
        std::cerr << "Failed to decode JPEG frame\n";
        return false;
    }
    timestamp = resp.frame_seq();
    return true;
}

class ShmReader
{
public:
    ShmReader(const std::string &stream_id, const ShmLayout &layout)
        : stream_id_(stream_id), layout_(layout), base_(nullptr), last_idx_(0)
    {
        std::string path = "/dev/shm/" + stream_id;
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0)
        {
            std::cerr << "open SHM failed: " << path << " errno=" << errno << "\n";
            return;
        }

        base_ = static_cast<uint8_t *>(::mmap(nullptr, layout_.total_size, PROT_READ, MAP_SHARED, fd_, 0));
        if (base_ == MAP_FAILED)
        {
            std::cerr << "mmap SHM failed: " << path << " errno=" << errno << "\n";
            base_ = nullptr;
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~ShmReader()
    {
        if (base_)
        {
            ::munmap(base_, layout_.total_size);
            base_ = nullptr;
        }
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool isValid() const { return base_ != nullptr; }

    bool read(cv::Mat &frame, uint64_t &timestamp, int timeout_ms, std::string &reason)
    {
        reason.clear();
        if (!base_)
        {
            reason = "SHM not mapped";
            return false;
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (tryRead(frame, timestamp, reason))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (reason.empty()) reason = "timeout";
        return false;
    }

private:
    bool tryRead(cv::Mat &frame, uint64_t &timestamp, std::string &reason)
    {
        reason.clear();

        // 使用编译器内置原子操作，避免 std::atomic reinterpret_cast 的 UB 和 segfault
        uint64_t head = __atomic_load_n(
            reinterpret_cast<uint64_t *>(base_ + layout_.head_idx_offset),
            __ATOMIC_ACQUIRE);
        if (head == last_idx_)
        {
            reason = "no new frame (head=" + std::to_string(head) + ")";
            return false;
        }

        size_t idx = head % layout_.slot_count;
        uint8_t *slot = base_ + idx * layout_.slot_size;

        uint64_t seq1 = __atomic_load_n(
            reinterpret_cast<uint64_t *>(slot + layout_.seq_offset),
            __ATOMIC_ACQUIRE);
        if (seq1 & 1)
        {
            reason = "slot writing (seq=" + std::to_string(seq1) + ")";
            return false;
        }

        ShmMeta meta;
        std::memcpy(&meta, slot + layout_.meta_offset, sizeof(ShmMeta));

        uint64_t seq2 = __atomic_load_n(
            reinterpret_cast<uint64_t *>(slot + layout_.seq_offset),
            __ATOMIC_ACQUIRE);
        if (seq1 != seq2)
        {
            reason = "sequence changed during read";
            return false;
        }

        if (meta.actual_size == 0 || meta.actual_size > layout_.max_frame_bytes)
        {
            reason = "invalid actual_size=" + std::to_string(meta.actual_size)
                     + " max=" + std::to_string(layout_.max_frame_bytes);
            return false;
        }

        if (meta.width == 0 || meta.height == 0 || meta.channels == 0)
        {
            reason = "invalid meta: w=" + std::to_string(meta.width)
                     + " h=" + std::to_string(meta.height)
                     + " c=" + std::to_string(meta.channels);
            return false;
        }

        if (meta.channels != 1 && meta.channels != 3 && meta.channels != 4)
        {
            reason = "invalid channels=" + std::to_string(meta.channels);
            return false;
        }

        if (meta.depth > CV_64F)
        {
            reason = "invalid depth=" + std::to_string(meta.depth);
            return false;
        }

        int cv_type = CV_MAKETYPE(meta.depth, meta.channels);
        cv::Mat img(static_cast<int>(meta.height), static_cast<int>(meta.width), cv_type);
        if (img.empty())
        {
            reason = "cv::Mat create failed";
            return false;
        }

        size_t img_bytes = img.total() * img.elemSize();
        if (meta.actual_size > img_bytes)
        {
            reason = "actual_size > img_bytes: " + std::to_string(meta.actual_size)
                     + " > " + std::to_string(img_bytes);
            return false;
        }

        size_t elem_size = img.elemSize1();                    // 单个通道字节数（与 Python 端 itemsize 对应）
        size_t expected_step = meta.width * meta.channels * elem_size;
        uint8_t *payload = slot + layout_.payload_offset;

        if (meta.step > 0 && meta.step != expected_step)
        {
            size_t src_stride = meta.step;
            size_t row_bytes = meta.width * meta.channels * elem_size;
            for (uint64_t y = 0; y < meta.height; ++y)
            {
                std::memcpy(img.ptr(static_cast<int>(y)), payload + y * src_stride, row_bytes);
            }
        }
        else
        {
            std::memcpy(img.data, payload, meta.actual_size);
        }

        frame = img;
        timestamp = meta.timestamp;
        last_idx_ = head;
        return true;
    }

    std::string stream_id_;
    ShmLayout layout_;
    int fd_ = -1;
    uint8_t *base_ = nullptr;
    uint64_t last_idx_ = 0;
};

static std::string sanitizeFilename(const std::string &name)
{
    std::string out;
    for (char c : name)
    {
        if (std::isalnum(c) || c == '-' || c == '_')
        {
            out.push_back(c);
        }
        else
        {
            out.push_back('_');
        }
    }
    return out;
}

int main(int argc, char **argv)
{
    std::string server = "127.0.0.1:50051";
    std::string output_dir = "./saved_frames";
    int count = 1;
    double interval_ms = 0.0;
    int timeout_ms = 3000;

    if (!parseArgs(argc, argv, server, output_dir, count, interval_ms, timeout_ms))
    {
        printUsage(argv[0]);
        return 1;
    }

    if (::mkdir(output_dir.c_str(), 0755) != 0 && errno != EEXIST)
    {
        std::cerr << "Failed to create output directory: " << output_dir << " errno=" << errno << "\n";
        return 1;
    }

    auto channel = grpc::CreateChannel(server, grpc::InsecureChannelCredentials());
    auto stub = streamingservice::RTSPStreamService::NewStub(channel);

    // 1. 获取 SHM 布局（给 SHM 模式用）
    ShmLayout layout;
    bool has_layout = fetchShmLayout(*stub, layout);

    // 2. 获取所有流
    std::vector<StreamSnapshot> streams;
    if (!listStreams(*stub, streams))
    {
        return 1;
    }
    if (streams.empty())
    {
        std::cout << "No active streams\n";
        return 0;
    }

    std::cout << "Found " << streams.size() << " streams, saving to " << output_dir << "\n\n";

    int saved = 0;
    int failed = 0;

    for (const auto &info : streams)
    {
        std::string mode = info.use_shared_mem ? "SHM" : "JPEG";
        std::cout << "[" << info.stream_id << "] mode=" << mode
                  << ", status=" << (info.connected ? "CONNECTED" : "NOT_CONNECTED")
                  << ", url=" << info.rtsp_url << "\n";

        if (!info.connected)
        {
            std::cout << "  -> skip: not connected\n\n";
            failed++;
            continue;
        }

        std::unique_ptr<ShmReader> shm_reader;
        if (info.use_shared_mem)
        {
            if (!has_layout)
            {
                std::cout << "  -> skip: SHM layout unavailable\n\n";
                failed++;
                continue;
            }
            shm_reader = std::make_unique<ShmReader>(info.stream_id, layout);
            if (!shm_reader->isValid())
            {
                std::cout << "  -> skip: SHM open failed\n\n";
                failed++;
                continue;
            }
        }

        for (int idx = 0; idx < count; ++idx)
        {
            cv::Mat frame;
            int64_t timestamp = 0;
            bool ok = false;
            std::string reason;

            if (info.use_shared_mem)
            {
                uint64_t ts = 0;
                ok = shm_reader->read(frame, ts, timeout_ms, reason);
                timestamp = static_cast<int64_t>(ts);
            }
            else
            {
                ok = fetchJpegFrame(*stub, info.stream_id, frame, timestamp);
                if (!ok) reason = "gRPC GetLatestFrame failed";
            }

            if (!ok || frame.empty())
            {
                std::cout << "  -> frame " << (idx + 1) << "/" << count
                          << " read failed: " << reason << "\n";
                failed++;
                continue;
            }

            std::ostringstream oss;
            oss << output_dir << "/" << sanitizeFilename(info.stream_id)
                << "_" << timestamp << "_" << idx << ".jpg";
            std::string path = oss.str();

            if (!cv::imwrite(path, frame))
            {
                std::cout << "  -> failed to write " << path << "\n";
                failed++;
                continue;
            }

            std::cout << "  -> saved " << path
                      << " shape=" << frame.rows << "x" << frame.cols
                      << " ts=" << timestamp << " mode=" << mode << "\n";
            saved++;

            if (idx < count - 1 && interval_ms > 0)
            {
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(interval_ms));
            }
        }
        std::cout << "\n";
    }

    std::cout << "Done: saved=" << saved << ", failed/skipped=" << failed << "\n";
    return 0;
}
