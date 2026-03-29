# Linux C++ 高并发 TCP 服务器学习笔记

本项目是我从零开始学习 Linux 下 C++ 高并发网络编程的完整记录。通过逐步实现单线程阻塞、多线程、I/O 多路复用（epoll 水平触发/边缘触发）、非阻塞 I/O 以及多线程 epoll 模型，深入理解 TCP 服务器的高并发实现原理。最终版本实现了支持 `sendfile` 零拷贝的静态文件服务器，并在虚拟机环境下完成了系统调优与压力测试。

## 📚 技术栈

- **语言**：C++17
- **网络模型**：epoll（边缘触发） + 多线程 Reactor
- **I/O 优化**：非阻塞 I/O、sendfile 零拷贝
- **协议**：HTTP/1.1（静态文件服务）
- **构建**：g++、CMake
- **压测**：wrk、ab（Apache Bench）
- **系统**：Linux（Ubuntu/CentOS）

## 🧱 演进历程

| 阶段 | 模型 | 核心特点 | 局限性 |
|------|------|----------|--------|
| **阶段1** | 单线程阻塞 | 基础 socket 编程，一次仅服务一个连接 | 无法并发，一次只能处理一个客户端 |
| **阶段2** | 多线程（每连接一线程） | 使用 `pthread_create`，每个客户端一个线程 | 线程数随连接数线性增长，资源消耗大 |
| **阶段3** | epoll 水平触发（单线程） | 单线程管理数千连接，事件驱动 | 无法利用多核 CPU |
| **阶段4** | epoll 边缘触发 + 非阻塞 I/O | 循环读写直到 `EAGAIN`，减少事件通知次数 | 仍为单线程，无法利用多核 |
| **阶段5** | 多线程 epoll（Reactor 模式） | 主线程 accept，工作线程独立 epoll 事件循环 | 业务逻辑未分离，不支持 HTTP 协议 |
| **阶段6** | 多线程 epoll HTTP 静态文件服务器 | 解析 HTTP 请求，使用 `sendfile` 零拷贝传输静态文件 | 无持久化、缓存等高级特性 |
| **阶段7** | 单线程 epoll 多种实践 | 多端口监听、简单 HTTP 响应，作为补充对比 | 性能低于多线程版本 |

> 每个阶段的完整代码及详细注释见 [Project Diary/项目经过.md](Project%20Diary/项目经过.md)。

## 📁 目录结构

```
.
├── .vscode/                              # VS Code 配置
├── 01-single-thread-blocking/
│   └── tcp_server.cpp
├── 02-multithread/
│   └── tcp_server.cpp
├── 03-epoll-lt/
│   └── tcp_server.cpp
├── 04-epoll-et/
│   └── tcp_server.cpp
├── 05-multithread-epoll/
│   └── tcp_server.cpp
├── 06-static-file-server-sendfile/
│   ├── tcp_server.cpp
│   └── index.html
├── 07-epoll-single-thread-practices/
│   ├── http_server_simple.cpp          # 单线程简单 HTTP 服务器
│   └── multi_port_server.cpp           # 单线程多端口监听服务器
├── Project Diary/
│   ├── 个人问题解答.md                  # 学习过程中遇到的问题与解答
│   └── 项目经过.md                      # 完整项目演进笔记（含代码）
└── README.md                            # 本文件
```

## 🚀 快速开始

### 1. 编译与运行（以最终版本为例）
```bash
cd 06-static-file-server-sendfile
g++ tcp_server.cpp -o tcp_server -pthread
./tcp_server
```

### 2. 测试
```bash
# 简单访问
curl http://127.0.0.1:8080/

# 压力测试（需安装 wrk 或 ab）
wrk -t4 -c1000 -d30s http://127.0.0.1:8080/
ab -n 10000 -c 1000 http://127.0.0.1:8080/
```

### 3. 系统调优（可选，提高并发上限）
```bash
# 文件描述符上限
ulimit -n 1000000

# 内核参数（临时生效）
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.ipv4.ip_local_port_range="1024 65000"
sysctl -w net.core.somaxconn=4096
```

## 📊 压测结果（虚拟机 2核2G）

| 压测工具 | 并发数 | QPS | 平均延迟 | 备注 |
|----------|--------|-----|----------|------|
| ab | 1000 | 约 8000 | < 10ms | 短连接，文件描述符升至 ~1000 |
| wrk | 1000 | 1083 | 35ms | 复用连接，QPS 更真实 |

> 详细压测记录见 [Project Diary/项目经过.md](Project%20Diary/项目经过.md) 第四阶段。

## 🧠 核心难点与解决方案

| 难点 | 解决方案 |
|------|----------|
| epoll 边缘触发数据漏读 | 循环调用 `recv` 直到返回 `EAGAIN`，配合非阻塞 I/O |
| 发送缓冲区满导致部分发送 | 为每个连接维护发送缓冲区，在 `EPOLLOUT` 事件中继续发送 |
| 多线程连接分配与锁竞争 | 主线程轮询分配，每个工作线程独立 epoll 实例，无锁设计 |
| TIME_WAIT 耗尽端口 | 启用 `tcp_tw_reuse`，扩大 `ip_local_port_range` |
| 文件传输 CPU 占用高 | 使用 `sendfile` 零拷贝替代 `read+write` |

## 🔮 下一步计划

- 解析 HTTP 请求路径，支持返回不同静态文件（如 CSS、JS、图片）
- 引入线程池分离业务逻辑（如文件 I/O 与 HTTP 解析）
- 支持 HTTP/1.1 长连接（keep-alive），实现简单缓存
- 添加日志记录和错误页面定制
- 探索 `io_uring` 异步 I/O 模型

## 🤝 贡献与反馈

本项目为个人学习记录，欢迎 Star ⭐ 和 Fork。如有问题或建议，请通过 GitHub Issues 交流。

---

**项目地址**：[https://github.com/gu-reni/tcp-server-learning](https://github.com/gu-reni/tcp-server-learning)  
**详细演进笔记**：见 [Project Diary/项目经过.md](Project%20Diary/项目经过.md)  
**问题与解答**：见 [Project Diary/个人问题解答.md](Project%20Diary/个人问题解答.md)