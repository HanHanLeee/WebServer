#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>

#define MAX_EVENT_NUMBER 1024 // 最大事件数
static int pipefd[2]; // 管道

/* 设置文件描述符为非阻塞 */
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL ); // 获取文件描述符的旧状态
    int new_option = old_option | O_NONBLOCK; // 设置文件描述符为非阻塞
    fcntl( fd, F_SETFL, new_option ); // 设置文件描述符的新状态
    return old_option; // 返回旧状态
}

/* 将文件描述符fd上的EPOLLIN注册到epollfd指示的epoll内核事件表中，参数enable_et指定是否对fd启用ET模式 */
void addfd( int epollfd, int fd, bool enable_et )
{
    struct epoll_event event; // epoll事件
    event.data.fd = fd; // 设置事件的文件描述符
    event.events = EPOLLIN; // 设置事件的类型为可读事件
    if( enable_et )
    {
        event.events |= EPOLLET; // 设置事件的类型为边缘触发
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event ); // 注册事件
    setnonblocking( fd ); // 设置文件描述符为非阻塞
}

/* 信号处理函数 */
void sig_handler( int sig )
{
    /* 保留原来的errno，在函数最后恢复，以保证函数的可重入性 */
    int save_errno = errno;
    int msg = sig; // 信号值
    send( pipefd[1], ( char* )&msg, 1, 0 ); // 将信号值写入管道
    errno = save_errno;
}

/* 设置信号的处理函数 */
void addsig( int sig )
{
    struct sigaction sa; // 信号处理结构体
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler; // 设置信号处理函数
    sa.sa_flags |= SA_RESTART; // 设置信号处理函数的行为
    sigfillset( &sa.sa_mask ); // 初始化信号集
    assert( sigaction( sig, &sa, NULL ) != -1 ); // 设置信号处理函数
}

int main(int argc, char* argv[])
{
    if(argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0])); // 打印命令行参数
        return 1;
    }
    const char* ip = argv[1]; // ip地址
    int port = atoi(argv[2]); // 端口号

    int ret = 0;
    struct sockaddr_in address; // ipv4地址
    bzero(&address, sizeof(address)); // 初始化地址
    address.sin_family = AF_INET; // ipv4协议族
    inet_pton(AF_INET, ip, &address.sin_addr); // 将ip地址转换为网络字节序
    address.sin_port = htons(port); // 将端口号转换为网络字节序

    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // 创建socket
    assert(listenfd >= 0); // 断言socket创建成功

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address)); // 绑定socket
    assert(ret != -1); // 断言socket绑定成功

    ret = listen(listenfd, 5); // 监听socket
    assert(ret != -1); // 断言socket监听成功

    struct epoll_event events[MAX_EVENT_NUMBER]; // epoll事件数组
    int epollfd = epoll_create(5); // 创建epoll内核事件表
    assert(epollfd != -1); // 断言epoll内核事件表创建成功

    addfd(epollfd, listenfd, true); // 将listenfd上的EPOLLIN注册到epollfd指示的epoll内核事件表中，启用ET模式

    /* 使用socketpair创建管道，注册pipefd[0]上的可读事件 */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd); // 创建管道
    assert(ret != -1); // 断言管道创建成功
    setnonblocking(pipefd[1]); // 设置管道写端为非阻塞
    addfd(epollfd, pipefd[0], true); // 将pipefd[0]上的EPOLLIN注册到epollfd指示的epoll内核事件表中，启用ET模式

    /* 设置一些信号的处理函数 */
    addsig(SIGHUP); // 1
    addsig(SIGCHLD); // 17
    addsig(SIGTERM); // 15
    addsig(SIGINT); // 2
    bool stop_server = false; // 是否停止服务器

    while(!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); // 等待事件发生
        if((number < 0) && (errno != EINTR)) // epoll_wait返回错误
        {
            printf("epoll failure\n"); // 打印错误信息
            break;
        }

        for(int i = 0; i < number; i++) // 遍历事件数组
        {
            int sockfd = events[i].data.fd; // 获取事件的文件描述符
            if(sockfd == listenfd) // 事件的文件描述符为listenfd
            {
                struct sockaddr_in client_address; // 客户端地址
                socklen_t client_addrlength = sizeof(client_address); // 客户端地址长度
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength); // 接受连接
                addfd(epollfd, connfd, true); // 将connfd上的EPOLLIN注册到epollfd指示的epoll内核事件表中，启用ET模式
            }
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) // 事件的文件描述符为pipefd[0]，且事件类型为可读事件
            {
                int sig;
                char signals[1024]; // 信号值数组
                ret = recv(pipefd[0], signals, sizeof(signals), 0); // 接收信号值
                if(ret == -1) // 接收失败
                {
                    continue;
                }
                else if(ret == 0) // 接收成功
                {
                    continue;
                }
                else // 接收成功
                {
                    for(int i = 0; i < ret; i++) // 遍历信号值数组
                    {
                        switch(signals[i]) // 根据信号值
                        {
                            case SIGCHLD: // 子进程状态发生变化
                            case SIGHUP: // 连接断开
                            {
                                continue;
                            }
                            case SIGTERM: // 终止进程
                            case SIGINT: // 键盘输入中
                            {
                                stop_server = true; // 停止服务器
                            }
                        }
                    }
                }
            }
            else // 其他事件
            {
                /* 其他处理 */
            }
        }
    }

    printf("close fds\n"); // 打印信息
    close(listenfd); // 关闭listenfd
    close(pipefd[1]); // 关闭pipefd[1]
    close(pipefd[0]); // 关闭pipefd[0]
    return 0;
}