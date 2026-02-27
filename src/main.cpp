#include <iostream>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "rtsp_service.h"

int main() {
    std::string server_address("0.0.0.0:50051");
    RTSPServiceImpl service;

    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(10 * 1024 * 1024);
    builder.SetMaxSendMessageSize(10 * 1024 * 1024);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << ">>> 🚀 C++ RTSP gRPC Server Listening on " << server_address << " <<<\n";
    server->Wait();

    return 0;
}