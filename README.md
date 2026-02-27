# RTSP gRPC Server

This repository contains a C++ implementation of an **RTSP streaming server** that exposes a gRPC interface for clients to request streams.

The project consists of a server application (in `src/`) and a Python client (`client/`) which demonstrates how to interact with the service.

---


## 🚀 Features

- Accepts RTSP input streams and forwards frames over gRPC
- Uses OpenCV for encoding/decoding video frames
- Modular design with factory pattern for decoders/encoders
- Example Python client demonstrating remote video capture
- Dockerfile included for containerized deployment

---

## 🛠️ Dependencies

- C++17 compatible compiler (e.g. `g++`/`clang`)
- CMake 3.10+
- [gRPC](https://grpc.io/) and Protobuf
- OpenCV (4.x recommended)
- Python 3 with `grpcio` and `opencv-python` for the client

### Installing (Ubuntu/Debian example)
```bash
sudo apt update
sudo apt install -y build-essential cmake libopencv-dev libgrpc++-dev protobuf-compiler
pip install grpcio grpcio-tools opencv-python
``` 

---

## 🏗️ Building the Server

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

By default, an executable (e.g. `rtsp_grpc_server`) will be produced in `build/`.

### Docker

Build an image using the provided `Dockerfile`:
```bash
docker build -t rtsp-grpc-server .
```
Run the container:
```bash
docker run --rm -p 50051:50051 rtsp-grpc-server
```

---

## 📡 Running the Server

Start the server either from a build or via Docker. It listens on port `50051`

### Command-line options
```text
Usage: ./rtsp_grpc_server
```

---

## 🧩 Client Usage

The Python client in `client/` shows how to call the service and capture frames remotely.

```bash
cd client
python3 client.py
```

The client uses the generated `stream_service_pb2.py` and `stream_service_pb2_grpc.py` files. Regenerate them if the protobuf changes:
```bash
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. stream_service.proto
```

---

## 📌 Protobuf Definition

`stream_service.proto` defines the gRPC service and messages used by server and client. Make sure to keep both copies in sync.
