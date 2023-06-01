#define _GUN_SOURCE 1
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h> 
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>

#define BUFFER_SIZE 64

int main(int argc, char* argv[])
{
    if(argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    struct sockaddr_in server_address;
    bzero(&server_address, sizeof(server_address)); // 将server_address的前sizeof(server_address)个字节置为0
    server_address.sin_family = AF_INET; // 指定协议族为IPv4
    inet_pton(AF_INET, ip, &server_address.sin_addr); // 将ip地址转换为网络字节序
    server_address.sin_port = htons(port); // 将端口号转换为网络字节序

    int sockfd = socket(PF_INET, SOCK_STREAM, 0); // 创建socket
    assert(sockfd >= 0);
    if(connect(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) // 尝试连接
    {
        printf("connection failed\n");
        close(sockfd);
        return 1;
    }
    pollfd fds[2];
    fds[0].fd = 0; // 标准输入
    fds[0].events = POLLIN; // 标准输入可读
    fds[0].revents = 0;
    fds[1].fd = sockfd; // sockfd
    fds[1].events = POLLIN | POLLRDHUP; // sockfd可读或者对方关闭连接
    fds[1].revents = 0;
    char read_buf[BUFFER_SIZE];
    int pipefd[2];
    int ret = pipe(pipefd); // 创建管道
    assert(ret != -1);

    while(1)
    {
        ret = poll(fds, 2, -1); // 轮询
        if(ret < 0)
        {
            printf("poll failure\n");
            break;
        }

        if(fds[1].revents & POLLRDHUP) // 对方关闭连接
        {
            printf("server close the connection\n");
            break;
        }
        else if(fds[1].revents & POLLIN) // sockfd可读
        {
            memset(read_buf, '\0', BUFFER_SIZE);
            recv(fds[1].fd, read_buf, BUFFER_SIZE - 1, 0);
            printf("%s\n", read_buf);
        }

        if(fds[0].revents & POLLIN)
        {
            // 使用splice将用户输入的数据直接写到sockfd上（零拷贝）
            // 从标准输入读取数据，写到管道中
            ret = splice(0, NULL, pipefd[1], NULL, 32768, SPLICE_F_MORE | SPLICE_F_MOVE);
            // 从管道中读取数据，写到sockfd上
            ret = splice(pipefd[0], NULL, sockfd, NULL, 32768, SPLICE_F_MORE | SPLICE_F_MOVE);
        }
    }
    close(sockfd);
    return 0;
}