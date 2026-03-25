#include <iostream>      // 标准输入输出流，用于打印信息
#include <cstring>       // 字符串操作函数，如 memset
#include <unistd.h>      // 系统调用：close、read、write 等
#include <sys/socket.h>  // socket 相关核心函数：socket, bind, listen, accept, send, recv
#include <netinet/in.h>  // IPv4 地址结构（sockaddr_in）和字节序转换函数
#include <arpa/inet.h>   // IP 地址转换函数，如 inet_ntoa

int main() {
    std::cout << "Server starting..." << std::endl;

    // ==================== 1. 创建监听 socket ====================
    // socket(): 创建 IPv4 TCP 套接字，返回文件描述符
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket creation failed" << std::endl;
        return 1;
    }
    std::cout << "Socket created successfully, fd = " << listen_fd << std::endl;

    // ==================== 2. 准备服务器地址结构体 ====================
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));          // 清零结构体，避免残留数据
    server_addr.sin_family = AF_INET;                      // IPv4 地址族
    server_addr.sin_port = htons(8080);                    // 端口号，网络字节序（大端）
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);       // 监听所有本地接口，网络字节序

    // ==================== 3. 绑定地址 ====================
    // bind(): 将 socket 与 IP 地址和端口关联
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "bind failed" << std::endl;
        close(listen_fd);
        return 1;
    }

    // ==================== 4. 开始监听 ====================
    // listen(): 使 socket 进入被动监听状态，backlog=5 表示已完成连接队列的最大长度
    if (listen(listen_fd, 5) < 0) {
        std::cerr << "listen failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    std::cout << "Server listening on port 8080..." << std::endl;

    // ==================== 5. 接受一个客户端连接 ====================
    struct sockaddr_in client_addr;      // 用于存放客户端地址信息
    socklen_t client_len = sizeof(client_addr);  // 地址结构长度（输入输出参数）
    // accept(): 阻塞等待客户端连接，返回一个新的 socket 用于与该客户端通信
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        std::cerr << "accept failed" << std::endl;
        close(listen_fd);
        return 1;
    }
    // 打印客户端 IP 和端口（inet_ntoa 将二进制 IP 转为点分十进制字符串，ntohs 将网络字节序端口转为主机字节序）
    std::cout << "Client connected: "
              << inet_ntoa(client_addr.sin_addr)
              << ":"
              << ntohs(client_addr.sin_port)
              << std::endl;

    // ==================== 6. 接收一次数据并回显 ====================
    char buffer[1024];          // 数据缓冲区
    // recv(): 从客户端接收数据，flags=0 表示阻塞读取
    // 注意：这里只调用一次 recv，因此只能处理客户端发送的第一批数据，之后会退出
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';               // 添加字符串结束符，方便打印
        std::cout << "Received: " << buffer;  // 打印接收到的内容
        send(client_fd, buffer, n, 0);        // 将相同数据回显给客户端
    } else if (n == 0) {
        // recv 返回 0 表示对方正常关闭连接
        std::cout << "Client closed connection" << std::endl;
    } else {
        // recv 返回 -1 表示出错
        std::cerr << "recv error" << std::endl;
    }

    // ==================== 7. 关闭连接 ====================
    close(client_fd);   // 关闭与客户端的连接
    close(listen_fd);   // 关闭监听 socket

    return 0;
}