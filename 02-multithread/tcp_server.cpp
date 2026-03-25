#include <iostream>      // 标准输入输出流，用于打印信息
#include <cstring>       // 字符串操作，如 memset
#include <unistd.h>      // 系统调用，如 close、read、write
#include <sys/socket.h>  // socket 相关函数：socket, bind, listen, accept, send, recv
#include <netinet/in.h>  // IPv4 地址结构（sockaddr_in）和字节序转换函数
#include <arpa/inet.h>   // IP 地址转换函数，如 inet_ntoa
#include <pthread.h>     // 多线程库：pthread_create, pthread_detach

// 线程函数：处理与单个客户端的通信
// 参数 arg 是一个指向 int 的指针，存放客户端 socket 文件描述符
void *handle_client(void *arg)
{
    // 从参数中取出客户端 socket 文件描述符
    int client_fd = *(int *)arg;
    // 释放动态分配的内存（在 main 中用 new int(client_fd) 分配）
    delete (int *)arg;

    char buffer[1024];          // 数据缓冲区
    ssize_t n;                  // recv 返回值，实际接收的字节数

    // 循环接收数据，直到客户端关闭连接或出错
    while ((n = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0)
    {
        buffer[n] = '\0';                       // 添加字符串结束符，方便打印
        std::cout << "Received:" << buffer;     // 打印接收到的内容
        send(client_fd, buffer, n, 0);          // 将相同数据回显给客户端
    }

    // recv 返回 0 表示客户端正常关闭连接
    if (n == 0)
    {
        std::cout << "Client closed connection" << std::endl;
    }
    // recv 返回 -1 表示出错
    else if (n < 0)
    {
        std::cerr << "recv error" << std::endl;
    }

    // 关闭与客户端的连接
    close(client_fd);
    return nullptr;
}

int main()
{
    std::cout << "Server starting..." << std::endl;

    // ==================== 1. 创建监听 socket ====================
    // socket(): 创建 IPv4 TCP 套接字，返回文件描述符
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        std::cerr << "socket creation failed" << std::endl;
        return 1;
    }
    std::cout << "Socket created successfully, fd = " << listen_fd << std::endl;

    // ==================== 2. 准备服务器地址结构体 ====================
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));          // 清零结构体
    server_addr.sin_family = AF_INET;                      // IPv4
    server_addr.sin_port = htons(8080);                    // 端口号，网络字节序
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);       // 监听所有本地接口，网络字节序

    // ==================== 3. 绑定地址 ====================
    // bind(): 将 socket 与 IP 地址和端口关联
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

    // ==================== 5. 主循环：接受客户端连接 ====================
    while (true)
    {
        struct sockaddr_in client_addr;     // 用于存放客户端地址信息
        socklen_t client_len = sizeof(client_addr);

        // accept(): 阻塞等待客户端连接，返回一个新的 socket 用于与该客户端通信
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            std::cerr << "accept failed" << std::endl;
            continue;   // 出错时继续等待下一个连接
        }

        // 打印客户端 IP 和端口
        std::cout << "Client connected: "
                  << inet_ntoa(client_addr.sin_addr)
                  << ":"
                  << ntohs(client_addr.sin_port)
                  << std::endl;

        // 动态分配内存，将 client_fd 包装成指针，以便传递给线程
        int *pclient = new int(client_fd);
        pthread_t tid;   // 线程 ID

        // 创建新线程，线程函数为 handle_client，参数为 pclient
        if (pthread_create(&tid, nullptr, handle_client, pclient) != 0)
        {
            std::cerr << "pthread_create failed" << std::endl;
            delete pclient;       // 创建失败时释放内存
            close(client_fd);     // 关闭客户端连接
        }
        else
        {
            // 分离线程，使其结束后自动回收资源，无需主线程 join
            pthread_detach(tid);
        }
    }

    // 理论上程序不会执行到这里（因为 while 无限循环），但为了规范，关闭监听 socket
    close(listen_fd);
    return 0;
}