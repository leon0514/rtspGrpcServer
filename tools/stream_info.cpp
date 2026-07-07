#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "stream_service.grpc.pb.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

static void printUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -s, --server <addr:port>  gRPC server address (default: 127.0.0.1:50051)\n"
              << "  -o, --output <file>       Write output to JSON file instead of stdout\n"
              << "  -h, --help                Show this help message\n";
}

int main(int argc, char **argv)
{
    std::string server_address = "127.0.0.1:50051";
    std::string output_file;
    bool write_json = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--server")
        {
            if (i + 1 < argc)
            {
                server_address = argv[++i];
            }
            else
            {
                std::cerr << "Error: " << arg << " requires a value\n";
                return 1;
            }
        }
        else if (arg == "-o" || arg == "--output")
        {
            if (i + 1 < argc)
            {
                output_file = argv[++i];
                write_json = true;
            }
            else
            {
                std::cerr << "Error: " << arg << " requires a value\n";
                return 1;
            }
        }
        else if (arg == "-h" || arg == "--help")
        {
            printUsage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Error: unknown argument " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    auto stub = streamingservice::RTSPStreamService::NewStub(channel);

    json result;
    result["server"] = server_address;

    // 1. 获取所有流信息
    {
        streamingservice::ListStreamsRequest list_req;
        streamingservice::ListStreamsResponse list_resp;
        grpc::ClientContext ctx;
        auto status = stub->ListStreams(&ctx, list_req, &list_resp);

        if (!status.ok())
        {
            result["success"] = false;
            result["error"] = status.error_message();
        }
        else
        {
            result["success"] = true;
            result["total_count"] = list_resp.total_count();

            json streams = json::array();
            for (const auto &s : list_resp.streams())
            {
                json j;
                j["stream_id"] = s.stream_id();
                j["rtsp_url"] = s.rtsp_url();
                j["status"] = s.status();
                j["status_name"] = streamingservice::StreamStatus_Name(s.status());
                j["decoder_type"] = s.decoder_type();
                j["decoder_type_name"] = streamingservice::DecoderType_Name(s.decoder_type());
                j["width"] = s.width();
                j["height"] = s.height();
                j["decode_interval_ms"] = s.decode_interval_ms();
                j["heartbeat_timeout_ms"] = s.heartbeat_timeout_ms();
                j["keep_on_failure"] = s.keep_on_failure();
                j["only_key_frames"] = s.only_key_frames();
                j["use_shared_mem"] = s.use_shared_mem();
                streams.push_back(j);
            }
            result["streams"] = streams;
        }
    }

    // 2. 获取共享内存布局（即使 ListStreams 失败也尝试，方便调试）
    {
        streamingservice::ShmLayoutRequest shm_req;
        streamingservice::ShmLayoutResponse shm_resp;
        grpc::ClientContext shm_ctx;
        auto shm_status = stub->GetShmLayout(&shm_ctx, shm_req, &shm_resp);

        json layout;
        layout["success"] = false;
        if (shm_status.ok() && shm_resp.success())
        {
            layout["success"] = true;
            const auto &l = shm_resp.layout();
            layout["slot_count"] = l.slot_count();
            layout["max_frame_bytes"] = l.max_frame_bytes();
            layout["alignment"] = l.alignment();
            layout["slot_size"] = l.slot_size();
            layout["seq_offset"] = l.seq_offset();
            layout["meta_offset"] = l.meta_offset();
            layout["payload_offset"] = l.payload_offset();
            layout["meta_data_size"] = l.meta_data_size();
            layout["head_idx_offset"] = l.head_idx_offset();
            layout["total_size"] = l.total_size();
        }
        else if (!shm_status.ok())
        {
            layout["error"] = shm_status.error_message();
        }
        else
        {
            layout["error"] = shm_resp.message();
        }
        result["shm_layout"] = layout;
    }

    // 3. 输出
    std::string json_str = result.dump(2);
    if (write_json)
    {
        std::ofstream ofs(output_file);
        if (!ofs)
        {
            std::cerr << "Error: failed to open output file: " << output_file << "\n";
            return 1;
        }
        ofs << json_str << "\n";
        std::cout << "Written to " << output_file << "\n";
    }
    else
    {
        std::cout << json_str << "\n";
    }

    return 0;
}
