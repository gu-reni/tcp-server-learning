#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

int main()
{
    std::cout << "Server starting..." << std::endl;

    // ==================== 1. 创建监听 socket ====================
    // socket(): 创建一个 IPv4 TCP socket，返回文件描述符
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        std::cerr << "socket creation failed" << std::endl;
        return 1;
    }
    std::cout << "Socket created successfully, fd = " << listen_fd << std::endl;

    // ==================== 2. 准备服务器地址结构体 ====================
    // sockaddr_in: IPv4 地址结构
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));          // 清零结构体，避免残留数据
    server_addr.sin_family = AF_INET;                      // IPv4
    server_addr.sin_port = htons(8080);                    // 端口号，网络字节序（大端）
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);       // 监听所有本地接口，网络字节序

    // ==================== 3. 绑定地址 ====================
    // bind(): 将 socket 与地址和端口关联
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        std::cerr << "bind failed" << std::endl;
        close(listen_fd);
        return 1;
    }

    // ==================== 4. 开始监听 ====================
    // listen(): 使 socket 进入被动监听状态，backlog=5 表示已完成连接队列的最大长度
    if (listen(listen_fd, 5) < 0)
    {
        std::cerr << "listen failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    std::cout << "Server listening on port 8080..." << std::endl;

    // ==================== 5. 创建 epoll 实例 ====================
    // epoll_create1(): 创建 epoll 实例，返回 epoll 文件描述符
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        std::cerr << "epoll_create1 failed" << std::endl;
        close(listen_fd);
        return 1;
    }

    // ==================== 6. 将监听 socket 加入 epoll 监听 ====================
    struct epoll_event ev;
    ev.events = EPOLLIN;        // 监听可读事件（有新连接时触发）
    ev.data.fd = listen_fd;     // 将文件描述符存储在 data 中，方便识别
    // epoll_ctl(): 向 epoll 实例添加要监视的文件描述符
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0)
    {
        perror("epoll_ctl failed");
        close(listen_fd);
        close(epoll_fd);
        return 1;
    }

    // ==================== 7. 事件循环 ====================
    const int MAX_EVENTS = 10;                 // 一次最多处理的事件数
    struct epoll_event events[MAX_EVENTS];     // 存放就绪事件的数组

    while (true)
    {
        // epoll_wait(): 等待事件发生，-1 表示无限等待
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0)
        {
            perror("epoll_wait failed");
            break;
        }

        // 遍历所有就绪的事件
        for (int i = 0; i < nfds; ++i)
        {
            // 情况1：监听 socket 就绪（有新连接）
            if (events[i].data.fd == listen_fd)
            {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                // accept(): 从已完成连接队列中取出一个连接，返回新的 socket fd
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0)
                {
                    std::cerr << "accept failed" << std::endl;
                    continue;
                }
                std::cout << "New Client connected: "
                          << inet_ntoa(client_addr.sin_addr) << ":"
                          << ntohs(client_addr.sin_port) << std::endl;

                // 将新客户端 socket 也加入 epoll 监听，等待其可读事件
                ev.events = EPOLLIN;
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
                {
                    std::cerr << "epoll_ctl add client failed" << std::endl;
                    close(client_fd);
                    continue;
                }
            }
            else
            {
                // 情况2：已连接的客户端 socket 就绪（有数据可读）
                int client_fd = events[i].data.fd;
                char buffer[1024];
                // recv(): 从客户端接收数据，flags=0 为阻塞模式
                ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                if (n <= 0)
                {
                    if (n == 0)
                    {
                        std::cout << "Client closed connection" << std::endl;   // 对方正常关闭
                    }
                    else
                    {
                        perror("recv failed");   // 出错
                    }
                    // 从 epoll 中删除该 fd，并关闭 socket
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                }
                else
                {
                    // 正常收到数据，回显给客户端
                    buffer[n] = '\0';   // 添加字符串结束符，方便打印
                    std::cout << "Received from client: " << buffer;
                    // send(): 将相同数据发回客户端
                    send(client_fd, buffer, n, 0);
                }
            }
        }
    }

    // 程序正常情况下不会退出（while 无限循环），但为了完整，关闭资源
    close(listen_fd);
    close(epoll_fd);
    return 0;
}