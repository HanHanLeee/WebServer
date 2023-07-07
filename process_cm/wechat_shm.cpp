#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define USER_LIMIT 5 // 最大用户数量
#define BUFFER_SIZE 1024  // 读缓冲区大小
#define FD_LIMIT 65535  // 文件描述符数量限制
#define MAX_EVENT_NUMBER 1024  // 最大事件数量
#define PROCESS_LIMIT 65535  // 进程数量限制

struct client_data
{
    sockaddr_in address;  // 客户端socket地址
    int connfd;  // socket文件描述符
    pid_t pid;  // 处理这个连接的子进程的PID
    int pipefd[2];  // 和父进程通信用的管道
};

static const char* shm_name = "/my_shm";  // 共享内存的名字
int sig_pipefd[2];  // 信号管道，以实现统一事件源
int epollfd;  // epoll文件描述符
int listenfd;  // 监听socket文件描述符
int shmfd;  // 共享内存文件描述符
char* share_mem = 0;  // 共享内存的起始地址
client_data* users = 0;  // 客户端连接数组，进程间共享
int* sub_process = 0;  // 子进程和客户连接的映射关系表，用进程的PID索引这个数组，即可取得该进程所处理的客户连接的编号
int user_count = 0;  // 当前客户数量
bool stop_child = false;  // 子进程是否停止运行

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);  // 获取文件描述符旧的状态标志
    int new_option = old_option | O_NONBLOCK;  // 设置为非阻塞
    fcntl(fd, F_SETFL, new_option);  // 设置文件描述符新的状态标志
    return old_option;  // 返回旧的状态标志
}

// 将文件描述符fd上的EPOLLIN注册到epollfd指示的epoll内核事件表中，参数enable_et指定是否对fd启用ET模式
void addfd(int epollfd, int fd, bool enable_et = true)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN;  // 只关注可读事件
    if(enable_et)
    {
        event.events |= EPOLLET;  // 启用ET模式
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  // 注册事件
    setnonblocking(fd);  // 设置非阻塞
}

// 信号处理函数
void sig_handler(int sig)
{
    int save_errno = errno;  // 保存原来的errno，以保证函数的可重入性
    int msg = sig;
    send(sig_pipefd[1], (char*)&msg, 1, 0);  // 将信号写入管道，以通知主循环
    errno = save_errno;
}

// 设置信号处理函数
void addsig(int sig, void(*handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;  // 设置信号处理函数
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;  // 重新调用被该信号终止的系统调用
    }
    sigfillset(&sa.sa_mask);  // 将所有信号加入信号集
    assert(sigaction(sig, &sa, NULL) != -1);  // 注册信号处理函数
}

void del_resource()
{
    close(sig_pipefd[0]);
    close(sig_pipefd[1]);
    close(listenfd);
    close(epollfd);
    shm_unlink(shm_name);  // 删除共享内存
    delete [] users;
    delete [] sub_process;
}

// 停止一个子进程
void child_term_handler(int sig)
{
    stop_child = true;
}

// 子进程运行的函数，参数idx指出该子进程处理的客户连接的编号，users是保存所有客户连接数据的数组，参数share_mem指出共享内存的起始地址
int run_child(int idx, client_data* users, char* share_mem)
{
    epoll_event events[MAX_EVENT_NUMBER];
    // 子进程使用I/O复用技术来同时监听两个文件描述符：客户连接socket、与父进程通信的管道文件描述符
    int child_epollfd = epoll_create(5);
    assert(child_epollfd != -1);
    int connfd = users[idx].connfd;  // 该子进程所处理的客户连接
    addfd(child_epollfd, connfd);  // 添加客户连接socket
    int pipefd = users[idx].pipefd[1];  // 与父进程通信的管道
    addfd(child_epollfd, pipefd);  // 添加管道文件描述符
    int ret;
    // 添加信号处理函数
    addsig(SIGTERM, child_term_handler, false);
    while(!stop_child)
    {
        int number = epoll_wait(child_epollfd, events, MAX_EVENT_NUMBER, -1);  // 监听事件
        if((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            // 如果就绪的文件描述符是客户连接socket，即有客户端发来了消息
            if((sockfd == connfd) && (events[i].events & EPOLLIN))
            {
                memset(share_mem + idx * BUFFER_SIZE, '\0', BUFFER_SIZE);  // 读取客户数据到共享内存
                // 将客户数据读取到共享内存的第idx个客户连接的位置，这里是为了简化问题，实际上应该用信号量加锁
                ret = recv(connfd, share_mem + idx * BUFFER_SIZE, BUFFER_SIZE - 1, 0);
                if(ret < 0)
                {
                    if(errno != EAGAIN)
                    {
                        stop_child = true;
                    }
                }
                else if(ret == 0)
                {
                    stop_child = true;
                }
                else
                {
                    // 成功读取客户
                    send(pipefd, (char*)&idx, sizeof(idx), 0);  // 通知主进程（父进程）来处理
                }
            }
            else if((sockfd == pipefd) && (events[i].events & EPOLLIN))  // 如果就绪的文件描述符是管道，即父进程通知本进程（子进程）来处理客户数据
            {
                int client = 0;
                // 从管道读取数据，即读取父进程通知的客户连接编号
                ret = recv(sockfd, (char*)&client, sizeof(client), 0);
                if(ret < 0)
                {
                    if(errno != EAGAIN)
                    {
                        stop_child = true;
                    }
                }
                else if(ret == 0)
                {
                    stop_child = true;
                }
                else
                {
                    send(connfd, share_mem + client * BUFFER_SIZE, BUFFER_SIZE, 0);  // 将共享内存的数据发送到客户端
                }
            }
            else
            {
                continue;
            }
        }
    }
    close(connfd);  // 关闭客户连接
    close(pipefd);  // 关闭管道
    close(child_epollfd);  // 关闭子进程的epoll文件描述符
    return 0;
}

int main(int argc, char* argv[])
{
    if(argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);  // 将ip地址转换为网络字节序
    address.sin_port = htons(port);  // 将端口号转换为网络字节序

    listenfd = socket(PF_INET, SOCK_STREAM, 0);  // 创建监听socket
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));  // 绑定监听socket
    assert(ret != -1);

    ret = listen(listenfd, 5);  // 监听
    assert(ret != -1);

    user_count = 0;
    users = new client_data [USER_LIMIT + 1];  // 分配用户数组
    sub_process = new int [PROCESS_LIMIT];  // 分配进程数组
    for(int i = 0; i < PROCESS_LIMIT; ++i)
    {
        sub_process[i] = -1;
    }

    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);  // 创建epoll文件描述符
    assert(epollfd != -1);
    addfd(epollfd, listenfd);  // 添加监听socket

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);  // 创建双向管道
    assert(ret != -1);
    setnonblocking(sig_pipefd[1]);  // 设置管道写端为非阻塞
    addfd(epollfd, sig_pipefd[0]);  // 添加管道读端

    addsig(SIGCHLD, sig_handler);  // 添加信号处理函数
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);  // 忽略SIGPIPE信号

    bool stop_server = false;
    bool terminate = false;

    shmfd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);  // 创建共享内存
    assert(shmfd != -1);
    ret = ftruncate(shmfd, USER_LIMIT * BUFFER_SIZE);  // 设置共享内存大小
    assert(ret != -1);

    share_mem = (char*)mmap(NULL, USER_LIMIT * BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);  // 将共享内存映射到进程的虚拟内存空间
    assert(share_mem != MAP_FAILED);
    close(shmfd);

    while(!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);  // 监听事件
        if((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            // 如果就绪的文件描述符是监听socket，即有客户端连接
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);  // 接受客户连接
                if(connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if(user_count >= USER_LIMIT)  // 如果连接数超过最大用户数
                {
                    const char* info = "too many users\n";
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0);  // 发送错误信息
                    close(connfd);
                    continue;
                }
                // 保存第user_count个客户连接的相关数据
                users[user_count].address = client_address;
                users[user_count].connfd = connfd;
                // 在主进程和子进程之间建立管道，以传递必要的数据
                ret = socketpair(PF_UNIX, SOCK_STREAM, 0, users[user_count].pipefd);
                assert(ret != -1);
                pid_t pid = fork();  // 创建子进程
                if(pid < 0)
                {
                    close(connfd);
                    continue;
                }
                else if(pid == 0)  // 子进程
                {
                    close(epollfd);  // 关闭子进程的epoll文件描述符
                    close(listenfd);  // 关闭子进程的监听socket
                    close(users[user_count].pipefd[0]);  // 关闭子进程的管道读端
                    close(sig_pipefd[0]);  // 关闭子进程的信号管道读端
                    close(sig_pipefd[1]);  // 关闭子进程的信号管道写端
                    run_child(user_count, users, share_mem);  // 运行子进程
                    munmap((void*)share_mem, USER_LIMIT * BUFFER_SIZE);  // 解除共享内存映射
                    exit(0);
                }
                else
                {
                    close(connfd);  // 关闭父进程的客户连接socket
                    close(users[user_count].pipefd[1]);  // 关闭父进程的管道写端
                    addfd(epollfd, users[user_count].pipefd[0]);  // 添加父进程的管道读端
                    users[user_count].pid = pid;  // 保存第user_count个客户连接的子进程pid
                    sub_process[pid] = user_count;  // 保存第user_count个客户连接的子进程在进程数组中的下标
                    user_count++;  // 客户连接数加1
                }
            }
            else if((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);  // 接收信号
                if(ret == -1)
                {
                    continue;
                }
                else if(ret == 0)
                {
                    continue;
                }
                else
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                            case SIGCHLD:  // 子进程退出
                            {
                                pid_t pid;
                                int stat;
                                while((pid = waitpid(-1, &stat, WNOHANG)) > 0)  // 回收子进程
                                {
                                    int del_user = sub_process[pid];
                                    sub_process[pid] = -1;
                                    if((del_user < 0) || (del_user > USER_LIMIT))
                                    {
                                        continue;
                                    }
                                    epoll_ctl(epollfd, EPOLL_CTL_DEL, users[del_user].pipefd[0], 0);  // 删除父进程的管道读端
                                    close(users[del_user].pipefd[0]);  // 关闭父进程的管道读端
                                    users[del_user] = users[--user_count];  // 将最后一个客户连接的数据复制到第del_user个客户连接
                                    sub_process[users[del_user].pid] = del_user;  // 更新进程数组
                                }
                                if(terminate && user_count == 0)  // 如果服务器终止且所有客户连接都关闭
                                {
                                    stop_server = true;
                                }
                                break;
                            }
                            case SIGTERM:  // 终止服务器
                            case SIGINT:
                            {
                                printf("kill all the child now\n");
                                if(user_count == 0)  // 如果没有客户连接
                                {
                                    stop_server = true;
                                    break;
                                }
                                for(int i = 0; i < user_count; ++i)
                                {
                                    int pid = users[i].pid;
                                    kill(pid, SIGTERM);  // 终止子进程
                                }
                                terminate = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }

            else if(events[i].events & EPOLLIN)
            {
                int child = 0;
                ret = recv(sockfd, (char*)&child, sizeof(child), 0);  // 接收子进程发送的数据
                printf("read data from child accross pipe\n");
                if(ret == -1)
                {
                    continue;
                }
                else if(ret == 0)
                {
                    continue;
                }
                else
                {
                    for(int j = 0; j < user_count; ++j)
                    {
                        if(users[j].pipefd[0] != sockfd)  // 找到发送数据的子进程
                        {
                            printf("send data to child accross pipe\n");
                            send(users[j].pipefd[0], (char*)&child, sizeof(child), 0);  // 将数据发送给其他子进程
                        }
                    }
                }
            }
        }
    }

    del_resource();  // 释放资源
    return 0;
}