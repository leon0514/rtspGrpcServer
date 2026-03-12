#include <iostream>
#include <memory>
#include <filesystem>
#include <thread> // for hardware_concurrency
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include "rtsp_service.hpp"
#include "task_scheduler.hpp"
#include "timer_scheduler.hpp"
#include <cuda_runtime.h>

// spdlog headers
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

extern "C"
{
#include <libavformat/avformat.h>
}

void display_banner()
{
    // ANSI color codes for bright cyan and reset
    const char *CYAN = "\033[1;36m";
    const char *RESET = "\033[0m";
    printf("%s", CYAN);
    printf("%s\n", " ██████╗ ██████╗ ██████╗  ██████╗    ██████╗ ████████╗███████╗██████╗ ");
    printf("%s\n", "██╔════╝ ██╔══██╗██╔══██╗██╔════╝    ██╔══██╗╚══██╔══╝██╔════╝██╔══██╗");
    printf("%s\n", "██║  ███╗██████╔╝██████╔╝██║         ██████╔╝   ██║   ███████╗██████╔╝");
    printf("%s\n", "██║   ██║██╔══██╗██╔═══╝ ██║         ██╔══██╗   ██║   ╚════██║██╔═══╝ ");
    printf("%s\n", "╚██████╔╝██║  ██║██║     ╚██████╗    ██║  ██║   ██║   ███████║██║     ");
    printf("%s\n", " ╚═════╝ ╚═╝  ╚═╝╚═╝      ╚═════╝    ╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝     ");
    printf("%s\n", "                                 v1.2                                 ");
    printf("%s\n", RESET);
    printf("%s\n", "                                                                      ");
}

// 配置日志系统
void setupLogging()
{
    try
    {
        if (!std::filesystem::exists("logs"))
        {
            std::filesystem::create_directories("logs");
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Could not create logs directory: " << e.what() << std::endl;
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/server.log", true);

    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("rtsp_logger", sinks.begin(), sinks.end());

    // 设置日志级别和格式
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v"); // 添加 [%t] 显示线程ID

    spdlog::set_default_logger(logger);
}

// 【修改】初始化所有 CUDA 设备，而不仅仅是设备 0
void initCudaDevices()
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0)
    {
        spdlog::warn("No CUDA devices found. GPU decoding will fallback to CPU or fail.");
        return;
    }

    // 遍历系统中所有的 GPU 进行初始化
    for (int i = 0; i < device_count; ++i)
    {
        cudaSetDevice(i);

        // 设置同步模式为 BlockingSync，避免 CPU 100% 自旋等待
        // 这对于多流高并发场景至关重要，能显著降低 CPU 负载
        err = cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
        if (err != cudaSuccess)
        {
            if (err == cudaErrorSetOnActiveProcess)
            {
                // 如果设备此前已被隐式初始化，尝试重置并重新设置标志
                spdlog::warn("GPU {} already initialized, resetting to apply BlockingSync...", i);
                cudaDeviceReset();
                cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
            }
        }

        // 打印显卡信息
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);
        spdlog::info("CUDA GPU [{}] initialized: {} (Compute Capability {}.{})", i, prop.name, prop.major, prop.minor);
    }

    spdlog::info("CUDA Sync Mode: BlockingSync (Low CPU Usage) applied to all {} GPUs.", device_count);

    // 防御性编程：回到默认设备0
    cudaSetDevice(0);
}

int main(int argc, char **argv)
{
    // 1. 先初始化日志，确保后续步骤可以打印日志
    setupLogging();
    display_banner();

    avformat_network_init();

    // 2. 解析参数
    std::string server_address("0.0.0.0:50051");
    if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))
    {
        std::cout << "Usage: " << argv[0] << " [address:port]\n"
                  << "Default address is 0.0.0.0:50051" << std::endl;
        return 0;
    }
    if (argc > 1)
    {
        server_address = argv[1];
    }

    // 3. 【解除注释并修改】初始化所有 CUDA 卡的低 CPU 占用标志
    // 必须在启动任何解码器之前调用
    // initCudaDevices();

    // 4. 初始化全局线程池 (TaskScheduler)
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0)
        hardware_threads = 4; // 兜底

    // 环境变量辅助解析器
    auto envUInt = [](const char *name, unsigned int def) -> unsigned int
    {
        if (const char *val = std::getenv(name))
        {
            try
            {
                return static_cast<unsigned int>(std::stoul(val));
            }
            catch (...)
            {
            }
        }
        return def;
    };

    // ① IO 线程池：用于网络阻塞操作 (Demux/Download/重连)，IO密集型可适当多配
    size_t io_threads = envUInt("RTSP_IO_THREADS", hardware_threads * 2);
    if (io_threads == 0)
        io_threads = 2;
    if (io_threads > 64)
        io_threads = 64; // 限制上限

    // ② GPU 专属计算线程池 (单卡)：因为硬件解码(NVDEC)和编码(nvJPEG)都是扔给硬件做的，
    // C++ 线程只是负责“提交任务+等待”，不需要跟 CPU 核心数绑定。一般 4-8 个线程就能喂饱单张显卡。
    size_t gpu_threads_per_card = envUInt("RTSP_GPU_THREADS_PER_CARD", 6);
    if (gpu_threads_per_card == 0)
        gpu_threads_per_card = 2;
    if (gpu_threads_per_card > 16)
        gpu_threads_per_card = 16;

    // ③ CPU 回退计算线程池：用于 FFmpeg CPU 解码和 OpenCV 图像处理，高度消耗 CPU。
    size_t cpu_compute_threads = envUInt("RTSP_CPU_COMPUTE_THREADS", hardware_threads);
    if (cpu_compute_threads == 0)
        cpu_compute_threads = 2;
    if (cpu_compute_threads > 64)
        cpu_compute_threads = 64;

    TaskScheduler::instance().init(io_threads, gpu_threads_per_card, cpu_compute_threads);
    TimerScheduler::instance().start();

    // 5. 启动 gRPC 服务
    // grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    RTSPServiceImpl service;

    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(20 * 1024 * 1024); // 增大一点缓冲
    builder.SetMaxSendMessageSize(20 * 1024 * 1024);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    spdlog::info(">>> 🚀 C++ RTSP gRPC Server Listening on {} <<<", server_address);

    // 阻塞等待
    server->Wait();

    return 0;
}