# Release Notes

## v1.1 Benchmark & Showcase Refresh

这一版聚焦“展示质量”和“可讲述性”升级，重点收口 benchmark 体系、刷新仓库文案，并补齐更适合 GitHub 展示与面试讲解的发布材料。

### Highlights

- 重做 benchmark 方案，形成覆盖轻接口、静态资源、鉴权接口、文件查询和上传写路径的分层压测矩阵
- 新增统一 benchmark runner，固定并发、线程数、时长、请求体大小、数据库命中与容器资源采样
- 补充 `1000` 并发读场景样本，并加入同步 / 异步日志模式对比，给出更完整的性能结论
- 刷新 README 中的压测章节、项目亮点和简历写法，使仓库首页信息更适合展示和投递
- 清理压测生成物管理方式，补充 `.gitignore`，避免将本地 benchmark 临时文件误提交到仓库

### Benchmark Summary

- `GET /healthz` 在当前发布配置下可达到 `7.5k req/s` 量级
- `GET /api/private/ping` 在 Bearer Token 鉴权链路下仍可达到 `7.4k req/s` 量级
- `GET /api/private/files` 在高并发下明显受 MySQL 影响，`500~1000` 并发区间开始出现稳定 `read/timeout` 错误
- `POST /api/login` 与 `POST /api/private/files` 仍是当前最重的两条写路径，后续优化优先级高于继续拉高轻接口吞吐
- 当前机器上 `TWS_LOG_WRITE=0` 明显优于默认异步日志模式，仓库文档已显式说明这一点

### User-Facing Changes

- README 首页信息密度更高，压测方法、结果和结论更完整
- 新增更适合简历和面试复述的项目描述版本
- 压测文档从“单组结果展示”升级为“方法 + 矩阵 + 对比实验 + 结论”的完整材料

### Added Files

- `scripts/run_benchmark_suite.sh`
- `test_pressure/login.lua`
- `test_pressure/private_upload.lua`

### Updated Files

- `README.md`
- `docs/benchmark.md`
- `docs/benchmark.csv`
- `docker-compose.yml`
- `.gitignore`

### Recommended GitHub Release Copy

Title:

`v1.1 Benchmark & Showcase Refresh`

Body:

```text
This release focuses on benchmark quality and project presentation.

Highlights:
- Rebuilt the benchmark suite into a layered matrix covering health checks, static pages, authenticated APIs, file listing, and upload write paths
- Added a reusable benchmark runner with fixed variables and Docker resource sampling
- Added 1000-concurrency read-path samples and an async-vs-sync logging comparison
- Refreshed the README benchmark section, project highlights, and resume-oriented descriptions
- Cleaned up benchmark artifact handling via .gitignore

Key numbers in the current publishable configuration:
- GET /healthz: up to 7.5k req/s
- GET /api/private/ping: up to 7.4k req/s
- GET /api/private/files: ~2.1k to 3.1k req/s depending on concurrency, with MySQL becoming the dominant bottleneck under higher load
- POST /api/login and POST /api/private/files remain the heaviest write paths and the clearest next optimization targets

See README.md and docs/benchmark.md for the full matrix, methodology, and comparison notes.
```

## v1.0 Showcase Build

这一版可以作为 Atlas WebServer 的首个完整展示版本，已经具备“可运行、可演示、可讲解、可验证”的项目交付形态。

### 本版重点

- 完成 HTTP 层职责拆分，降低单文件复杂度
- 形成 `Main-Reactor + Multi-SubReactor + Dynamic Thread Pool` 并发处理模型
- 补齐用户注册、登录、Token 鉴权、文件上传下载、权限控制、操作日志
- 新增统一前端视觉样式，页面不再是零散演示页
- 支持 Docker Compose 启动、健康检查、MySQL 持久化
- 新增 smoke test、压测脚本、压测图表、架构文档、时序文档
- 完成配置体系收敛：默认读取 `server.conf`，支持环境变量和命令行覆盖

### 适合展示的能力

- Linux 网络编程：`socket`、`epoll`、ET/LT、非阻塞 IO
- C++ 服务端架构：Reactor、线程池、连接池、定时器、日志系统
- 工程化落地：鉴权、配置管理、部署、健康检查、压测、测试脚本
- 重构能力：将臃肿模块按职责拆分并验证行为不回归

### 当前建议演示路径

1. 打开注册页和登录页，演示统一前端入口
2. 登录后进入文件管理页，展示小文件上传、列表、下载、删除
3. 展示操作日志，说明业务审计已经接入
4. 结合 README 中的架构图、压测图和时序图讲实现细节

### 当前边界

- 当前版本主打“小文件稳定上传方案”，没有继续保留大文件实验性实现
- 更细粒度 RBAC、监控告警、CI/CD 仍属于后续演进项
- 目前更适合作为秋招项目展示版本，而不是直接对外商用发布版本
