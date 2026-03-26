#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string>
#include <vector>

// 常量定义
const int MAX_EVENTS = 10;          // epoll_wait 一次最多处理的事件数
const int BACKLOG = 4096;           // listen 的 backlog（需配合系统 somaxconn）
const int THREAD_COUNT = 4;         // 工作线程数量，可根据 CPU 核数调整

// 每个客户端连接的信息
struct client_info {
    int fd;                         // 客户端 socket 文件描述符
    std::string send_buffer;        // 待发送的数据队列
};

// 设置文件描述符为非阻塞
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
    }
}

// 尝试发送缓冲区中的数据，返回 true 表示全部发完，false 表示还有剩余
bool try_send(int fd, std::string& buffer) {
    const char* data = buffer.data();
    size_t remaining = buffer.size();
    while (remaining > 0) {
        ssize_t sent = send(fd, data, remaining, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满，保留未发送部分
                buffer = std::string(data, remaining);
                return false;
            } else {
                perror("send");
                return false;   // 错误，上层会关闭连接
            }
        }
        data += sent;
        remaining -= sent;
    }
    buffer.clear();   // 全部发完
    return true;
}

// 工作线程函数：每个线程独立运行一个 epoll 事件循环
void* worker_thread(void* arg) {
    int epoll_fd = *(int*)arg;               // 该线程的 epoll 文件描述符
    struct epoll_event events[MAX_EVENTS];   // 存放就绪事件的数组

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            client_info* info = static_cast<client_info*>(events[i].data.ptr);
            int client_fd = info->fd;

            // 可读事件
            if (events[i].events & EPOLLIN) {
                char buffer[1024];
                ssize_t n;
                while ((n = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
                    buffer[n] = '\0';
                    std::cout << "Received: " << buffer;
                    info->send_buffer.append(buffer, n);   // 加入发送队列

                    // 尝试立即发送
                    if (try_send(client_fd, info->send_buffer)) {
                        // 全部发完，修改事件只监听可读
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.ptr = info;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                    } else {
                        // 有未发完数据，同时监听可读可写
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                        ev.data.ptr = info;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                    }
                }
                if (n == 0) {
                    // 客户端关闭连接
                    std::cout << "Client closed connection" << std::endl;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                    delete info;
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // 真正的错误
                    perror("recv");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                    delete info;
                }
            }

            // 可写事件（发送剩余数据）
            if (events[i].events & EPOLLOUT) {
                if (try_send(client_fd, info->send_buffer)) {
                    // 发送完毕，改回只监听可读
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.ptr = info;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                } else {
                    // 还未发完，保持监听可写（无需操作）
                }
            }
        }
    }
    return nullptr;
}

int main() {
    std::cout << "Server starting..." << std::endl;

    // 1. 创建监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    // 2. 设置地址重用，避免 bind 失败
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }

    // 3. 准备服务器地址结构体
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // 4. 绑定
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    // 5. 监听
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    std::cout << "Server listening on port 8080..." << std::endl;

    // 6. 创建线程池
    std::vector<int> epoll_fds(THREAD_COUNT);      // 每个工作线程的 epoll fd
    std::vector<pthread_t> threads(THREAD_COUNT);  // 线程 ID

    for (int i = 0; i < THREAD_COUNT; ++i) {
        epoll_fds[i] = epoll_create1(0);
        if (epoll_fds[i] < 0) {
            perror("epoll_create1");
            // 清理已创建的 epoll
            for (int j = 0; j < i; ++j) close(epoll_fds[j]);
            close(listen_fd);
            return 1;
        }

        // 创建线程，将 epoll fd 的地址作为参数
        if (pthread_create(&threads[i], nullptr, worker_thread, &epoll_fds[i]) != 0) {
            perror("pthread_create");
            for (int j = 0; j <= i; ++j) close(epoll_fds[j]);
            close(listen_fd);
            return 1;
        }
        // 分离线程，自动回收资源
        pthread_detach(threads[i]);
    }
    std::cout << "Created " << THREAD_COUNT << " worker threads." << std::endl;

    // 7. 主循环：accept 新连接并分配给工作线程
    int next_worker = 0;   // 轮询索引
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }
        std::cout << "New client connected: "
                  << inet_ntoa(client_addr.sin_addr) << ":"
                  << ntohs(client_addr.sin_port) << std::endl;

        // 设置为非阻塞
        set_nonblocking(client_fd);

        // 创建客户端信息对象
        client_info* info = new client_info{client_fd, ""};

        // 轮询选择一个工作线程
        int target = next_worker % THREAD_COUNT;
        next_worker++;

        // 将新连接添加到目标工作线程的 epoll 中
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;   // 边缘触发，初始只监听可读
        ev.data.ptr = info;
        if (epoll_ctl(epoll_fds[target], EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            perror("epoll_ctl add");
            delete info;
            close(client_fd);
        }
    }

    // 程序不会到达这里，但为了完整，关闭资源（实际需等待线程结束）
    close(listen_fd);
    for (int fd : epoll_fds) close(fd);
    return 0;
}