
/*
 *  系统关于CUDA的功能函数
 */

#include "cuda_tools.hpp"
#include <string>

namespace CUDATools
{
    bool check_driver(CUresult e, const char *call, int line, const char *file)
    {
        if (e != CUDA_SUCCESS)
        {

            const char *message = nullptr;
            const char *name = nullptr;
            cuGetErrorString(e, &message);
            cuGetErrorName(e, &name);
            // cuGetErrorString / cuGetErrorName 在未知错误码时可能返回 nullptr
            INFOE("CUDA Driver error %s # %s, code = %s [ %d ] in file %s:%d",
                  call,
                  message ? message : "<unknown>",
                  name ? name : "<unknown>",
                  e, file, line);
            return false;
        }
        return true;
    }

    bool check_runtime(cudaError_t e, const char *call, int line, const char *file)
    {
        if (e != cudaSuccess)
        {
            INFOE("CUDA Runtime error %s # %s, code = %s [ %d ] in file %s:%d", call, cudaGetErrorString(e), cudaGetErrorName(e), e, file, line);
            return false;
        }
        return true;
    }

    bool check_device_id(int device_id)
    {
        int device_count = -1;
        checkCudaRuntime(cudaGetDeviceCount(&device_count));
        if (device_id < 0 || device_id >= device_count)
        {
            INFOE("Invalid device id: %d, count = %d", device_id, device_count);
            return false;
        }
        return true;
    }

    int current_device_id()
    {
        int device_id = 0;
        checkCudaRuntime(cudaGetDevice(&device_id));
        return device_id;
    }

    dim3 grid_dims(int numJobs)
    {
        if (numJobs <= 0)
        {
            return dim3(0);
        }
        int numBlockThreads = numJobs < GPU_BLOCK_THREADS ? numJobs : GPU_BLOCK_THREADS;
        return dim3(((numJobs + numBlockThreads - 1) / (float)numBlockThreads));
    }

    dim3 block_dims(int numJobs)
    {
        return numJobs < GPU_BLOCK_THREADS ? numJobs : GPU_BLOCK_THREADS;
    }

    AutoDevice::AutoDevice(int device_id)
    {
        // 保存旧设备，失败时标记为无效，避免析构时设置非法设备
        has_old_ = (cudaGetDevice(&old_) == cudaSuccess);
        checkCudaRuntime(cudaSetDevice(device_id));
    }

    AutoDevice::~AutoDevice()
    {
        if (has_old_)
        {
            checkCudaRuntime(cudaSetDevice(old_));
        }
    }
}
