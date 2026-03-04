#include <iostream>
#include <memory>
#include <filesystem> // for log directory creation
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "rtsp_service.hpp"
#include <cuda_runtime.h>

// spdlog headers
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

void display_banner() {
    printf("%s\n"," ██████╗ ██████╗ ██████╗  ██████╗    ██████╗ ████████╗███████╗██████╗ ");
    printf("%s\n","██╔════╝ ██╔══██╗██╔══██╗██╔════╝    ██╔══██╗╚══██╔══╝██╔════╝██╔══██╗");
    printf("%s\n","██║  ███╗██████╔╝██████╔╝██║         ██████╔╝   ██║   ███████╗██████╔╝");
    printf("%s\n","██║   ██║██╔══██╗██╔═══╝ ██║         ██╔══██╗   ██║   ╚════██║██╔═══╝ ");
    printf("%s\n","╚██████╔╝██║  ██║██║     ╚██████╗    ██║  ██║   ██║   ███████║██║     ");
    printf("%s\n"," ╚═════╝ ╚═╝  ╚═╝╚═╝      ╚═════╝    ╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝     ");
    printf("%s\n","                                 v1.0                                 ");
    printf("%s\n","                                                                      ");
}

// 初始化 CUDA 设备，设置同步模式为阻塞同步，避免 CPU 自旋等待
void initCudaDevice()
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0)
    {
        spdlog::warn("No CUDA devices found, GPU decoding will not be available");
        return;
    }

    // 设置同步模式为 BlockingSync，避免默认的自旋等待浪费 CPU
    // cudaDeviceScheduleBlockingSync: 线程将阻塞等待 GPU，不消耗 CPU
    // cudaDeviceScheduleSpin: 默认值，自旋等待，高 CPU 占用
    // cudaDeviceScheduleYield: 让出 CPU，但延迟稍高
    err = cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
    if (err != cudaSuccess)
    {
        // 如果设备已经初始化，需要先重置
        if (err == cudaErrorSetOnActiveProcess)
        {
            cudaDeviceReset();
            cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
        }
    }

    // 选择默认设备
    cudaSetDevice(0);
    spdlog::info("CUDA initialized with {} device(s), using BlockingSync mode", device_count);
}

int main(int argc, char **argv)
{
    // 初始化 CUDA 设备，设置同步模式减少 CPU 占用
    // initCudaDevice();

    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    // 默认监听地址/端口，可通过命令行传参覆盖
    // 使用示例： ./rtsp_server 0.0.0.0:60000
    std::string server_address("0.0.0.0:50051");
    // 先检查帮助选项
    if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))
    {
        std::cout << "Usage: " << argv[0] << " [address:port]\n"
                << "Default address is 0.0.0.0:50051" << std::endl;
        return 0;
    }

    // 然后处理普通地址参数
    if (argc > 1)
    {
        server_address = argv[1];
    }

    RTSPServiceImpl service;

    // create logs directory if needed
    try
    {
        std::filesystem::create_directories("logs");
    }
    catch (const std::exception &e)
    {
        // failure to create directory is non-fatal, just log
        spdlog::warn("Could not create logs directory: {}", e.what());
    }

    // initialize multi-sink logger (console + file)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/server.log", true);
    
    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("rtsp_logger", sinks.begin(), sinks.end());
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(10 * 1024 * 1024);
    builder.SetMaxSendMessageSize(10 * 1024 * 1024);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    display_banner();
    spdlog::info(">>> 🚀 C++ RTSP gRPC Server Listening on {} <<<", server_address);
    server->Wait();

    return 0;
}