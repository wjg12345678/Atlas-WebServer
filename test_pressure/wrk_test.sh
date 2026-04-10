#!/bin/bash

# wrk 压力测试脚本
# 用法: ./wrk_test.sh [URL] [并发数] [持续时间] [线程数] [lua脚本]
# 示例: ./wrk_test.sh http://127.0.0.1:9006/healthz 1000 5s 4
# 示例: TOKEN=xxx ./wrk_test.sh http://127.0.0.1:9006/api/private/ping 200 10s 4 ./private_ping.lua

URL=${1:-http://127.0.0.1:9006/}
CONNECTIONS=${2:-1000}
DURATION=${3:-5s}
THREADS=${4:-4}
SCRIPT_PATH=${5:-}

echo "=========================================="
echo "wrk 压力测试"
echo "=========================================="
echo "URL: $URL"
echo "并发连接数: $CONNECTIONS"
echo "测试持续时间: $DURATION"
echo "线程数: $THREADS"
if [ -n "$SCRIPT_PATH" ]; then
echo "Lua脚本: $SCRIPT_PATH"
fi
echo "=========================================="
echo ""

if [ -n "$SCRIPT_PATH" ]; then
    wrk -c $CONNECTIONS -d $DURATION -t $THREADS -s "$SCRIPT_PATH" "$URL"
else
    wrk -c $CONNECTIONS -d $DURATION -t $THREADS "$URL"
fi
