#define _GNU_SOURCE 1
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h> // close
#include <string.h> // bzero
#include <stdlib.h> // atoi
#include <fcntl.h> // fcntl
#include <poll.h> // poll
#include <errno.h> // errno

#define USER_LIMIT 5 // 最大用户数量
#define BUFFER_SIZE 64 // 读缓冲区大小
#define FD_LIMIT 65535 // 文件描述符数量限制

struct client_data
{
    sockaddr_in address; // 客户端socket地址
    char* write_buf; // 待写到客户端的数据的位置
    char buf[BUFFER_SIZE]; // 从客户端读入的数据
};

int setnonblocking(int fd) // 将文件描述符设置为非阻塞
{
    int old_option = fcntl(fd, F_GETFL); // 获取文件描述符的旧状态
    int new_option = old_option | O_NONBLOCK; // 设置为非阻塞
    fcntl(fd, F_SETFL, new_option); // 设置文件描述符的新状态
    return old_option; // 返回旧状态
}

int main(int argc, char* argv[])
{
    if(argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0])); // basename()返回路径中的文件名部分
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]); // 将字符串转换为整数

    int ret = 0;
    struct sockaddr_in address; // 服务器socket地址
    bzero(&address, sizeof(address)); // 将address的前sizeof(address)个字节置为0
    address.sin_family = AF_INET; // 指定协议族为IPv4
    inet_pton(AF_INET, ip, &address.sin_addr); // 将ip地址转换为网络字节序
    address.sin_port = htons(port); // 将端口号转换为网络字节序

    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // 创建socket
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address)); // 绑定socket和socket地址
    assert(ret != -1);

    ret = listen(listenfd, 5); // 监听socket
    assert(ret != -1);

    client_data* users = new client_data[FD_LIMIT]; // 保存所有客户端数据
    pollfd fds[USER_LIMIT + 1]; // 保存所有文件描述符
    int user_counter = 0; // 客户端数量
    for(int i = 1; i <= USER_LIMIT; ++i)
    {
        fds[i].fd = -1; // 初始化
        fds[i].events = 0;
    }
    fds[0].fd = listenfd; // 监听socket
    fds[0].events = POLLIN | POLLERR; // 监听socket可读或者出错
    fds[0].revents = 0;

    while(1)
    {
        ret = poll(fds, user_counter + 1, -1); // 监听所有文件描述符
        if(ret < 0)
        {
            printf("poll failure\n");
            break;
        }

        for(int i = 0; i < user_counter+1; ++i)
        {
            if((fds[i].fd == listenfd) && (fds[i].revents & POLLIN))
            {
                struct sockaddr_in client_address; // 客户端socket地址
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength); // 接受客户端连接
                if(connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if(user_counter >= USER_LIMIT)
                {
                    const char* info = "too many users\n";
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0); // 向客户端发送信息
                    close(connfd);
                    continue;
                }
                user_counter++;
                users[connfd].address = client_address; // 保存客户端socket地址
                setnonblocking(connfd); // 设置为非阻塞
                fds[user_counter].fd = connfd; // 保存客户端文件描述符
                fds[user_counter].events = POLLIN | POLLRDHUP | POLLERR; // 监听客户端文件描述符可读、客户端关闭连接或者出错
                fds[user_counter].revents = 0;
                printf("comes a new user, now have %d users\n", user_counter);
            }
            else if(fds[i].revents & POLLERR)
            {
                printf("get an error from %d\n", fds[i].fd);
                char errors[100];
                memset(errors, '\0', 100);
                socklen_t length = sizeof(errors);
                if(getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &errors, &length) < 0) // 获取并清除socket错误
                {
                    printf("get socket option failed\n");
                }
                continue;
            }
            else if(fds[i].revents & POLLRDHUP)
            {
                users[fds[i].fd] = users[fds[user_counter].fd]; // 将最后一个客户端数据复制到当前客户端
                close(fds[i].fd); // 关闭当前客户端
                fds[i] = fds[user_counter]; // 将最后一个文件描述符复制到当前文件描述符
                i--; // 重新检查当前文件描述符
                user_counter--; // 客户端数量减一
                printf("a client left\n");
            }
            else if(fds[i].revents & POLLIN)
            {
                int connfd = fds[i].fd;
                memset(users[connfd].buf, '\0', BUFFER_SIZE);
                ret = recv(connfd, users[connfd].buf, BUFFER_SIZE-1, 0); // 从客户端读入数据
                printf("get %d bytes of client data %s from %d\n", ret, users[connfd].buf, connfd);
                if(ret < 0)
                {
                    if(errno != EAGAIN)
                    {
                        close(connfd);
                        users[fds[i].fd] = users[fds[user_counter].fd]; // 将最后一个客户端数据复制到当前客户端
                        fds[i] = fds[user_counter]; // 将最后一个文件描述符复制到当前文件描述符
                        i--; // 重新检查当前文件描述符
                        user_counter--; // 客户端数量减一
                    }
                }
                else if(ret == 0)
                {
                }
                else
                {
                    for(int j = 1; j <= user_counter; ++j)
                    {
                        if(fds[j].fd == connfd)
                        {
                            continue;
                        }
                        fds[j].events |= ~POLLIN; // 将客户端文件描述符可读事件取消
                        fds[j].events |= POLLOUT; // 将客户端文件描述符可写事件加入
                        users[fds[j].fd].write_buf = users[connfd].buf; // 将客户端数据保存到客户端写缓冲区
                    }
                }
            }
            else if(fds[i].revents & POLLOUT)
            {
                int connfd = fds[i].fd;
                if(!users[connfd].write_buf)
                {
                    continue;
                }
                ret = send(connfd, users[connfd].write_buf, strlen(users[connfd].write_buf), 0); // 向客户端发送数据
                users[connfd].write_buf = NULL; // 清空客户端写缓冲区
                fds[i].events |= ~POLLOUT; // 将客户端文件描述符可写事件取消
                fds[i].events |= POLLIN; // 将客户端文件描述符可读事件加入
            }
        }
    }
    delete [] users;
    close(listenfd);
    return 0;
}