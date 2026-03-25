#include <iostream>          // 标准输入输出流，用于控制台打印
#include <cstring>           // C风格字符串操作，如 memset
#include <unistd.h>          // 提供系统调用如 close, read, write
#include <sys/socket.h>      // socket 相关函数和结构体
#include <netinet/in.h>      // sockaddr_in 结构体定义
#include <arpa/inet.h>       // 网络地址转换函数，如 inet_ntoa
#include <sys/epoll.h>       // epoll 事件驱动机制
#include <fcntl.h>           // fcntl 函数，用于设置非阻塞标志
#include <errno.h>           // errno 全局错误码，用于错误处理

// 设置文件描述符为非阻塞模式
void set_nonblocking(int fd) {
    // 获取当前文件描述符标志
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl get failed");   // 打印错误信息
        return;
    }
    // 将 O_NONBLOCK 标志添加到 flags 中，并设置回去
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl set failed");
    }
}

int main() {
    std::cout << "Server starting..." << std::endl;  // 打印启动信息

    // 1. 创建 TCP 套接字（IPv4，流式）
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket creation failed" << std::endl; // 错误输出
        return 1;
    }
    std::cout << "Socket created successfully, fd = " << listen_fd << std::endl;

    // 2. 准备服务端地址结构体
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));      // 清零
    server_addr.sin_family = AF_INET;                  // IPv4 协议族
    server_addr.sin_port = htons(8080);                // 端口 8080，转为网络字节序
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 监听所有本地接口

    // 3. 绑定地址到 socket
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "bind failed" << std::endl;
        close(listen_fd);   // 失败后关闭套接字
        return 1;
    }

    // 4. 开始监听，设置最大等待连接队列长度为 5
    if (listen(listen_fd, 5) < 0) {
        std::cerr << "listen failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    std::cout << "Server listening on port 8080..." << std::endl;

    // 5. 创建 epoll 实例
    int epoll_fd = epoll_create1(0);   // epoll_create1(0) 等价于 epoll_create(1) 且 flags 为0
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed" << std::endl;
        close(listen_fd);
        return 1;
    }

    // 6. 将监听 socket 加入 epoll 中，关注可读事件（有新连接）
    struct epoll_event ev;
    ev.events = EPOLLIN;          // 监听读事件
    ev.data.fd = listen_fd;       // 事件关联的文件描述符
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl failed");
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }

    const int MAX_EVENTS = 10;          // 每次 epoll_wait 最多返回的事件数
    struct epoll_event events[MAX_EVENTS];  // 存放就绪事件的数组

    // 7. 事件循环
    while (true) {
        // 等待事件发生，-1 表示阻塞直到有事件
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait failed");
            break;   // 出现错误则退出循环
        }

        // 处理每一个就绪事件
        for (int i = 0; i < nfds; ++i) {
            // 如果是监听 socket 的可读事件，表示有新连接
            if (events[i].data.fd == listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                // 接受新连接
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    std::cerr << "accept failed" << std::endl;
                    continue;
                }
                // 打印客户端信息
                std::cout << "New Client connected: " << inet_ntoa(client_addr.sin_addr)
                          << ":" << ntohs(client_addr.sin_port) << std::endl;
                // 设置为非阻塞模式（边缘触发要求非阻塞）
                set_nonblocking(client_fd);
                // 将新客户端 socket 加入 epoll，关注读事件，并使用边缘触发模式（EPOLLET）
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    std::cerr << "epoll_ctl add client failed" << std::endl;
                    close(client_fd);
                    continue;
                }
            } else {
                // 否则是已连接的客户端有数据可读
                int client_fd = events[i].data.fd;
                char buffer[1024];
                ssize_t n;
                // 边缘触发模式下，需要循环读取直到 EAGAIN/EWOULDBLOCK
                while ((n = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
                    buffer[n] = '\0';               // 添加字符串结束符
                    std::cout << "Received from client: " << buffer;
                    // 回显给客户端
                    send(client_fd, buffer, n, 0);
                }
                if (n == 0) {
                    // 客户端主动关闭连接
                    std::cout << "Client closed connection" << std::endl;
                    // 从 epoll 中删除并关闭 socket
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                } else if (n < 0) {
                    // 出错时，检查是否为非阻塞模式下的正常情况（无数据可读）
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // 边缘触发下，数据已经读完，这里只是打印提示
                        std::cout << "No more data to read from client" << std::endl;
                    } else {
                        // 其他错误，打印并关闭连接
                        perror("recv error");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        close(client_fd);
                    }
                }
            }
        }
    }
    return 0;
}