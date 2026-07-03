#!/bin/bash
set -e

# 1. 基础环境配置
PLATFORM="linux-x86_64" # 如果是 Mac 请改为 darwin-x86_64 或 darwin-arm64
VERSION="v2.28.0"
PLUGIN_NAME="protoc-gen-openapiv2"
PROTO_DIR="./proto"

# 2. 下载插件二进制文件 (如果本地不存在)
if ! command -v $PLUGIN_NAME &> /dev/null; then
    if [ ! -f "./$PLUGIN_NAME" ]; then
        URL="https://github.com/grpc-ecosystem/grpc-gateway/releases/download/${VERSION}/protoc-gen-openapiv2-${VERSION}-${PLATFORM}"
        echo "正在下载 $PLUGIN_NAME 插件..., url is ${URL}"
        curl -sSL -o "$PLUGIN_NAME" "$URL"
        chmod +x "$PLUGIN_NAME"
    fi
    # 将当前目录加入 PATH，方便 protoc 找到插件
    export PATH=$PATH:$(pwd)
fi

# 3. 下载 Google API 依赖
mkdir -p "${PROTO_DIR}/google/api"
if [ ! -f "${PROTO_DIR}/google/api/annotations.proto" ]; then
    echo "正在下载依赖协议文件..."
    curl -sSL -o "${PROTO_DIR}/google/api/annotations.proto" https://raw.githubusercontent.com/googleapis/googleapis/master/google/api/annotations.proto
    curl -sSL -o "${PROTO_DIR}/google/api/http.proto" https://raw.githubusercontent.com/googleapis/googleapis/master/google/api/http.proto
fi

# 4. 生成 Envoy 使用的 .pb 文件
protoc -I "${PROTO_DIR}" \
       --include_imports \
       --include_source_info \
       --descriptor_set_out="${PROTO_DIR}/stream.pb" \
       "${PROTO_DIR}/stream.proto"

echo "✅ stream.pb 生成成功！"

# 5. 生成 Swagger JSON
# 注意：我们显式指定使用下载的插件
protoc -I "${PROTO_DIR}" \
       --plugin=protoc-gen-openapiv2="./$PLUGIN_NAME" \
       --openapiv2_out=. \
       --openapiv2_opt=allow_merge=true,merge_file_name=stream \
       "${PROTO_DIR}/stream.proto"

echo "✅ stream.swagger.json 生成成功！"