# TinyWebServer

基于 C++ 的轻量级 Web 服务器项目，在原始 TinyWebServer 的基础上完成了面向工程化场景的一轮升级，重点补强了并发模型、连接管理、日志系统、后台运行能力和 HTTPS 支持。

当前版本已经从经典的“半同步半反应堆”演进为更接近生产场景的 `Main-Reactor + Multi-SubReactor` 架构，并补充了 ET、Keep-Alive、超时管理、动态线程池、异步日志、守护进程与 OpenSSL TLS。

## 项目亮点

- `Main-Reactor + Multi-SubReactor` 多反应堆模型
- `epoll` 边缘触发 `ET`，支持 ET 模式下一次性读满
- HTTP/1.1 基础解析与 `Keep-Alive`
- 静态文件传输支持 `sendfile`
- HTTPS 支持，集成 OpenSSL
- 动态线程池，支持扩容、空闲线程回收、任务调度优化
- MySQL 连接池复用、空闲检测、自动重连
- 最小堆连接超时管理
- 环形缓冲区读写，减少拷贝
- 异步日志、日志分级、日志滚动
- 配置文件驱动运行参数
- 守护进程模式、后台运行、信号处理、异常重启

## 架构概览

当前版本的主要执行路径如下：

1. `main.cpp` 负责配置加载、环境变量覆盖、守护进程/信号处理、服务启动
2. 主 Reactor 负责监听 `listenfd` 并接收新连接
3. 新连接按轮询分发到多个 SubReactor
4. SubReactor 负责连接读写事件、连接超时扫描
5. 业务解析与响应处理由线程池协同完成
6. HTTP 明文连接保留 `sendfile` 零拷贝发送
7. HTTPS 连接自动切换为 `SSL_read / SSL_write`

## 已完成的增强项

- 从半同步半反应堆升级为主从 Reactor / Multi-Reactor
- 线程池优化：动态扩容、空闲线程回收
- `LT` 切换为 `ET`
- 增加 ET 模式下的一次性读满处理
- 修复 `EAGAIN` 处理不完整问题
- 支持长连接 `Keep-Alive`
- 增加最小堆定时器处理超时连接
- HTTP/1.1 基础解析增强
- MySQL 连接池复用、超时检测、自动重连
- 引入内存池与环形缓冲区
- 减少内存拷贝
- 日志系统异步化、日志分级、日志滚动
- 配置文件读取运行参数
- 增加守护进程模式、后台运行
- 完善异常处理、崩溃重启、信号处理
- 支持 HTTPS（OpenSSL）

## 目录结构

```text
.
├── CGImysql/          # MySQL 连接池
├── http/              # HTTP 连接、请求解析、响应发送
├── lock/              # 同步原语封装
├── log/               # 日志系统
├── memorypool/        # 内存池
├── root/              # 静态资源
├── threadpool/        # 动态线程池
├── timer/             # 超时管理
├── config.cpp
├── config.h
├── main.cpp
├── server.conf        # 运行配置
├── server_ctl.sh      # start/stop/restart/reload/status
├── webserver.cpp
└── webserver.h
```

## 编译环境

- Linux
- `g++`
- `epoll`
- MySQL Client 开发库
- OpenSSL 开发库

Docker 构建环境已经在仓库内配置完成。

## 快速开始

### 1. 使用 Docker Compose

```bash
docker compose up --build
```

默认会启动：

- MySQL 8.0
- Web 服务

访问地址：

```text
http://127.0.0.1:9006/
```

### 2. 本地编译

确保本机已经安装：

- `g++`
- `libmysqlclient`
- `openssl`

然后执行：

```bash
make server
./server -f server.conf
```

## 配置文件

项目使用 [server.conf](/Users/mac/Desktop/TinyWebServer-master/server.conf) 作为默认配置入口。

当前支持的核心配置项：

```ini
port=9006
log_write=1
log_level=1
log_split_lines=800000
log_queue_size=800
trig_mode=3
opt_linger=0
sql_num=8
thread_num=8
threadpool_max_threads=16
threadpool_idle_timeout=30
mysql_idle_timeout=60
conn_timeout=15
close_log=0
actor_model=0
daemon_mode=0
pid_file=./TinyWebServer.pid
https_enable=0
https_cert_file=./certs/server.crt
https_key_file=./certs/server.key
db_host=127.0.0.1
db_port=3306
db_user=root
db_password=root
db_name=qgydb
```

### 配置说明

- `port`：监听端口
- `log_write`：日志模式，`0` 同步，`1` 异步
- `log_level`：日志级别
- `trig_mode`：触发模式，`3` 表示 `ET + ET`
- `sql_num`：MySQL 连接池大小
- `thread_num`：初始线程数
- `threadpool_max_threads`：线程池最大线程数
- `threadpool_idle_timeout`：线程空闲回收时间
- `mysql_idle_timeout`：MySQL 空闲连接检测时间
- `conn_timeout`：连接超时时间
- `actor_model`：并发模型选择
- `daemon_mode`：是否启用守护进程
- `pid_file`：守护进程 PID 文件
- `https_enable`：是否启用 HTTPS
- `https_cert_file`：证书路径
- `https_key_file`：私钥路径

## 命令行参数

```bash
./server -f server.conf
./server -p 9006 -l 1 -m 3 -o 0 -s 8 -t 8 -c 0 -a 1 -d 1
```

支持的命令行参数：

- `-f`：配置文件路径
- `-p`：端口
- `-l`：日志写入模式
- `-m`：触发模式
- `-o`：`linger` 配置
- `-s`：数据库连接数量
- `-t`：线程数量
- `-c`：是否关闭日志
- `-a`：并发模型
- `-d`：是否启用守护进程

说明：数据库相关配置优先读取环境变量 `TWS_DB_*`。

## 守护进程与后台运行

启用方式：

```ini
daemon_mode=1
pid_file=./TinyWebServer.pid
```

控制脚本：

```bash
./server_ctl.sh start
./server_ctl.sh stop
./server_ctl.sh restart
./server_ctl.sh reload
./server_ctl.sh status
```

已支持：

- PID 文件写入与清理
- 防重复启动
- `SIGTERM / SIGINT / SIGHUP` 处理
- 守护模式下 worker 异常退出自动拉起

## HTTPS 使用说明

先准备证书：

```bash
mkdir -p certs
openssl req -x509 -nodes -newkey rsa:2048 \
  -keyout certs/server.key \
  -out certs/server.crt \
  -days 365 \
  -subj "/CN=localhost"
```

修改配置：

```ini
https_enable=1
https_cert_file=./certs/server.crt
https_key_file=./certs/server.key
```

启动后访问：

```bash
curl -k https://127.0.0.1:9006/
```

说明：

- HTTP 明文静态文件发送仍走 `sendfile`
- HTTPS 连接因 TLS 加密限制，自动回退到 `SSL_write` 分块发送

## MySQL 初始化

Docker Compose 已经内置初始化 SQL。

如果手动初始化，可参考：

```sql
CREATE DATABASE qgydb;
USE qgydb;

CREATE TABLE user(
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;

INSERT INTO user(username, passwd) VALUES('name', 'passwd');
```

## 页面资源

静态资源位于 [root/](/Users/mac/Desktop/TinyWebServer-master/root) 目录，包括：

- 登录
- 注册
- 图片展示
- 视频展示

## GitHub 提交建议

仓库已经补充 `.gitignore`，建议提交前确认以下内容不要进入版本库：

- `server`
- `ServerLog/`
- `*.pid`
- `certs/`
- MySQL 数据目录

## 后续可继续扩展的方向

- HTTP/1.1 更完整语义支持
- HTTPS 热加载证书
- HTTP/2
- 更细粒度的监控与指标
- 更严格的配置校验
- 单元测试与压测脚本整理

## License

本项目保留原始 TinyWebServer 学习项目的实践属性，适合用于：

- 网络编程学习
- Linux 服务器项目练手
- C++ 后端工程能力展示

如用于简历或公开展示，建议明确说明你在原始项目基础上的增强部分。
