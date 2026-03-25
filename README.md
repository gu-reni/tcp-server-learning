# Linux C++ 高并发 TCP 服务器学习笔记

本项目是我从零开始学习 Linux 下 C++ 高并发网络编程的完整记录。通过逐步实现单线程阻塞、多线程、I/O 多路复用（epoll 水平触发/边缘触发）、非阻塞 I/O 以及多线程 epoll 模型，深入理解 TCP 服务器的高并发实现原理。

## 学习路线

1. **单线程阻塞服务器**  
   - 基础 socket 编程：`socket`, `bind`, `listen`, `accept`, `recv`, `send`  
   - 一次只处理一个客户端，接收一次数据即退出

2. **多线程服务器**  
   - 每个客户端一个线程（`pthread_create`）  
   - 实现简单并发，但线程数随连接数线性增长，资源消耗大

3. **epoll 水平触发（LT）单线程服务器**  
   - 使用 `epoll_create1`, `epoll_ctl`, `epoll_wait` 在一个线程内监听多个 fd  
   - 高效处理大量连接，无需多线程开销

4. **epoll 边缘触发（ET）+ 非阻塞 I/O**  
   - 设置非阻塞，循环读写直到 `EAGAIN`  
   - 减少事件触发次数，提高性能

5. **多线程 epoll（Reactor 模式）**  
   - 主线程 accept 连接，按轮询分配给多个工作线程  
   - 每个工作线程独立运行 epoll 事件循环，充分利用多核 CPU

6. **系统调优与压测**  
   - 调整文件描述符限制（`ulimit -n`）  
   - 内核参数调优（`tcp_tw_reuse`, `ip_local_port_range`, `somaxconn`）  
   - 使用 `ab`, `wrk` 进行压力测试，分析瓶颈

## 目录结构
.
├── 01-single-thread-blocking/
│ └── tcp_server.cpp # 单线程阻塞版本
├── 02-multithread/
│ └── tcp_server.cpp # 多线程版本（每连接一线程）
├── 03-epoll-lt/
│ └── tcp_server.cpp # epoll 水平触发
├── 04-epoll-et/
│ └── tcp_server.cpp # epoll 边缘触发 + 非阻塞
├── 05-multithread-epoll/
│ └── tcp_server.cpp # 多线程 epoll（Reactor）
└── Project Diary
└──README.md



text

## 编译与运行

每个阶段代码均可独立编译运行：

```bash
cd 03-epoll-lt
g++ tcp_server.cpp -o tcp_server
./tcp_server
```
测试连接：

```bash
telnet 127.0.0.1 8080
压测示例（需安装 wrk 或 ab）：
```
```bash
# 使用 ab 发送 10000 个请求，并发 1000
ab -n 10000 -c 1000 http://127.0.0.1:8080/

# 使用 wrk 4 线程，1000 并发，持续 30 秒
wrk -t4 -c1000 -d30s http://127.0.0.1:8080/
```
关键系统调优参数
```bash
ulimit -n 65535
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.ipv4.ip_local_port_range="1024 65000"
sysctl -w net.core.somaxconn=4096
```
收获与总结
理解 TCP 三次握手、四次挥手在代码中的体现

掌握 socket 编程的核心函数及其参数含义

多线程模型与 epoll 模型的优缺点对比

边缘触发必须配合非阻塞 I/O，并循环处理直至 EAGAIN

系统参数调整对高并发的重要性

下一步计划
实现 Reactor 模式的多线程 epoll 服务器

添加简单的 HTTP 协议解析，实现 HTTP 服务器

引入 sendfile 实现零拷贝文件传输

线程池优化业务处理

欢迎 Star ⭐ 或 Fork 本仓库，一起交流学习！
