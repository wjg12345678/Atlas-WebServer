# 文件业务模块

本文档描述当前文件业务的接口边界、数据模型、权限控制、存储方式和测试入口。当前实现面向小文件稳定上传与下载场景。

## 接口

- `GET /healthz`
  - 健康检查
- `POST /api/login`
  - 登录成功后返回会话 `token`
- `POST /api/private/logout`
  - 使当前 Bearer Token 失效
- `POST /api/private/files`
  - 需要 `Authorization: Bearer <token>`
  - 当前主路径使用 JSON 请求体
  - 文件内容通过 `content_base64` 字段传输
  - 当前上传大小限制为 `64 KB`
- `GET /api/private/files`
  - 返回当前登录用户的文件列表
- `GET /api/private/files/:id/download`
  - 仅允许下载自己的文件
- `DELETE /api/private/files/:id`
  - 仅允许删除自己的文件
- `GET /api/private/operations`
  - 返回当前用户最近 50 条操作记录

## 数据表

- `user`
  - 用户名和密码
- `files`
  - 文件归属、磁盘文件名、原始文件名、类型、大小、创建时间
- `operation_logs`
  - 用户行为日志，记录登录、上传、下载、删除、鉴权失败等事件

## 权限模型

- 登录后由服务端生成会话 token，并持久化到 `user_sessions`
- `/api/private/*` 路由统一走 Bearer Token 鉴权
- 文件下载、删除按 `owner_username` 校验资源归属

## 安全改造

- 密码不再以明文形式写库，而是存成 `SHA-256(salt + password)` 的十六进制摘要
- 为兼容旧数据，若用户记录没有 `passwd_salt`，登录时仍允许按旧明文校验，并在成功后自动升级为哈希存储
- 登录态不再只存在进程内存里，服务重启后只要 `user_sessions` 未过期，Bearer Token 仍可继续使用

## 页面流程

- 登录成功后进入欢迎页
- 欢迎页提供 `看照片`、`看视频`、`文件管理台` 三个入口
- 文件管理页直接复用当前登录态，不再内嵌二次登录表单

## 存储说明

- 文件内容写入 `root/uploads/`
- 文件元数据写入 `files`
- 用户操作审计写入 `operation_logs`
- `docker-compose.yml` 已将 `./root/uploads` 挂载到容器内 `/app/root/uploads`
- 这样容器重建后，上传文件不会因为容器层重置而丢失

## 测试

- 总入口: [scripts/run_smoke_suite.sh](../scripts/run_smoke_suite.sh)
- 认证链路: [scripts/test_auth.sh](../scripts/test_auth.sh)
- 私有接口链路: [scripts/test_private_api.sh](../scripts/test_private_api.sh)
- 文件链路: [scripts/test_files.sh](../scripts/test_files.sh)
- 兼容旧入口: [scripts/test_file_workflow.sh](../scripts/test_file_workflow.sh)

## 当前限制

- 当前前端仅保留 `64 KB` 以内的小文件上传
- 前端页面与后端接口都按同一限制拦截大文件
- 当前实现不是大文件传输方案
- 会话有效期当前固定为 7 天，未做刷新机制
