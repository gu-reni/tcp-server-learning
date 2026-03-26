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
#include <sys/sendfile.h>
#include <sys/stat.h>

// 常量定义
const int MAX_EVENTS = 10;          // epoll_wait 一次最多处理的事件数
const int BACKLOG = 4096;           // listen 的 backlog（需配合系统 somaxconn）
const int THREAD_COUNT = 4;         // 工作线程数量，可根据 CPU 核数调整

// 每个客户端连接的信息
struct client_info {
    int fd;                         // 客户端 socket 文件描述符
    int file_fd;                    // 要发送的文件描述符（-1 表示无文件）
    off_t file_offset;              // 当前文件发送偏移量
    off_t file_size;                // 文件总大小
    std::string send_buffer;        // 响应头缓冲区（尚未发送的部分）
    bool header_sent;               // 是否已发送完响应头
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

// 尝试发送文件内容，返回 true 表示全部发完，false 表示还有剩余
bool try_sendfile(int client_fd, int file_fd, off_t& offset, off_t count) {
    while (count > 0) {
        ssize_t sent = sendfile(client_fd, file_fd, &offset, count);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            } else {
                perror("sendfile");
                return false;
            }
        }
        count -= sent;
    }
    return true;
}

// 工作线程函数：每个线程独立运行一个 epoll 事件循环
void* worker_thread(void* arg) {
    int epoll_fd = *(int*)arg;
    struct epoll_event events[MAX_EVENTS];

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
                ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                if (n > 0) {
                    // 忽略请求内容，打开静态文件
                    info->file_fd = open("index.html", O_RDONLY);
                    if (info->file_fd < 0) {
                        // 文件不存在，返回 404
                        const char* not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                        info->send_buffer = not_found;
                        info->header_sent = false;
                        // 尝试发送 404 响应
                        if (try_send(client_fd, info->send_buffer)) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                            close(client_fd);
                            delete info;
                        } else {
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                            ev.data.ptr = info;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                        }
                    } else {
                        // 获取文件大小
                        struct stat st;
                        fstat(info->file_fd, &st);
                        info->file_size = st.st_size;
                        info->file_offset = 0;

                        // 构造响应头
                        std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                             std::to_string(info->file_size) + "\r\n"
                                             "Connection: close\r\n\r\n";
                        info->send_buffer = header;
                        info->header_sent = false;

                        // 尝试发送响应头
                        if (try_send(client_fd, info->send_buffer)) {
                            info->header_sent = true;
                            if (try_sendfile(client_fd, info->file_fd, info->file_offset, info->file_size)) {
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                                close(info->file_fd);
                                close(client_fd);
                                delete info;
                            } else {
                                struct epoll_event ev;
                                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                                ev.data.ptr = info;
                                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                            }
                        } else {
                            struct epoll_event ev;
                            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                            ev.data.ptr = info;
                            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                        }
                    }
                } else if (n == 0) {
                    // 客户端关闭连接
                    std::cout << "Client closed connection" << std::endl;
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    if (info->file_fd != -1) close(info->file_fd);
                    close(client_fd);
                    delete info;
                } else {
                    // recv 出错
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("recv");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        if (info->file_fd != -1) close(info->file_fd);
                        close(client_fd);
                        delete info;
                    }
                }
            }

            // 可写事件
            if (events[i].events & EPOLLOUT) {
                if (!info->header_sent) {
                    // 继续发送响应头
                    if (try_send(client_fd, info->send_buffer)) {
                        info->header_sent = true;
                        if (info->file_fd != -1) {
                            if (try_sendfile(client_fd, info->file_fd, info->file_offset, info->file_size)) {
                                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                                close(info->file_fd);
                                close(client_fd);
                                delete info;
                            } else {
                                // 文件未发完，保持监听可写
                            }
                        } else {
                            // 没有文件（404 情况），关闭连接
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                            close(client_fd);
                            delete info;
                        }
                    } else {
                        // 头部仍未发完，保持监听可写
                    }
                } else {
                    // 头部已发，继续发送文件
                    if (info->file_fd != -1) {
                        if (try_sendfile(client_fd, info->file_fd, info->file_offset, info->file_size)) {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                            close(info->file_fd);
                            close(client_fd);
                            delete info;
                        } else {
                            // 文件未发完，保持监听可写
                        }
                    } else {
                        // 理论上不会发生
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        close(client_fd);
                        delete info;
                    }
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
    std::vector<int> epoll_fds(THREAD_COUNT);
    std::vector<pthread_t> threads(THREAD_COUNT);

    for (int i = 0; i < THREAD_COUNT; ++i) {
        epoll_fds[i] = epoll_create1(0);
        if (epoll_fds[i] < 0) {
            perror("epoll_create1");
            for (int j = 0; j < i; ++j) close(epoll_fds[j]);
            close(listen_fd);
            return 1;
        }

        if (pthread_create(&threads[i], nullptr, worker_thread, &epoll_fds[i]) != 0) {
            perror("pthread_create");
            for (int j = 0; j <= i; ++j) close(epoll_fds[j]);
            close(listen_fd);
            return 1;
        }
        pthread_detach(threads[i]);
    }
    std::cout << "Created " << THREAD_COUNT << " worker threads." << std::endl;

    // 7. 主循环：accept 新连接并分配给工作线程
    int next_worker = 0;
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

        set_nonblocking(client_fd);

        // 初始化 client_info，file_fd 初始为 -1
        client_info* info = new client_info{client_fd, -1, 0, 0, "", false};

        int target = next_worker % THREAD_COUNT;
        next_worker++;

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = info;
        if (epoll_ctl(epoll_fds[target], EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            perror("epoll_ctl add");
            delete info;
            close(client_fd);
        }
    }

    close(listen_fd);
    for (int fd : epoll_fds) close(fd);
    return 0;
}