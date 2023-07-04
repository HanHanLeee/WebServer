#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <signal.h>
#include "sort_timer.h"

#define FD_LIMIT 65535 // 文件描述符数量限制
#define MAX_EVENT_NUMBER 1024  // 最大事件数量
#define TIMESLOT 5  // 最小超时单位

static int pipefd[2]; // 信号处理函数使用的管道，以实现统一事件源
static sort_timer_lst timer_lst; // 定时器升序链表
static int epollfd = 0; // epoll文件描述符

// 设置文件描述符为非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;

    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void sig_handler(int sig) {
    // 保留原来的errno，在函数最后恢复，以保证函数的可重入性
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0); // 将信号值写入管道，以通知主循环
    errno = save_errno;
}

// 设置信号的处理函数
void addsig(int sig) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART; // 重新调用被该信号终止的系统调用
    sigfillset(&sa.sa_mask); // 将所有信号加入信号集

    assert(sigaction(sig, &sa, NULL) != -1);
}

void timer_handler() {
    // 定时处理任务，实际上就是调用tick函数
    timer_lst.tick();
    // 因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时，以不断触发SIGALRM信号
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之
void cb_func(client_data* user_data) {
    // 从epollfd中删除该文件描述符
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    printf("close fd %d\n", user_data->sockfd);
}

int main(int argc, char* argv[])
{
    if(argc <= 2) {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi(argv[2]);

    // 创建监听socket
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr); // 将ip地址转换为网络字节序
    address.sin_port = htons(port); // 将端口号转换为网络字节序

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    // 创建epoll对象和事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd);

    // 使用socketpair创建管道，注册pipefd[0]上的可读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0]);

    // 设置信号处理函数
    addsig(SIGALRM);
    addsig(SIGTERM);
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT];
    bool timeout = false;
    alarm(TIMESLOT); // 定时

    while(!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        for(int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);

                addfd(epollfd, connfd); // 添加新的文件描述符到epoll中
                users[connfd].address = client_address; // 初始化客户连接数据
                users[connfd].sockfd = connfd;

                // 创建定时器，设置其回调函数和超时时间，然后绑定定时器和用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer(timer);
            }
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) // 处理信号
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1) {
                    // handle the error
                    continue;
                }
                else if(ret == 0) {
                    continue;
                }
                else {
                    // 因为每个信号值占1字节，所以按字节来逐个接收信号
                    for(int i = 0; i < ret; i++) {
                        switch(signals[i]) {
                            case SIGALRM: // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务，这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM: // 接收到SIGTERM信号，表示服务器要终止了，此时我们将stop_server变量置为true，以实现服务器的平滑终止
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if(events[i].events & EPOLLIN) // 处理客户连接上接收到的数据
            {
                memset(users[sockfd].buf, '\0', BUFFER_SIZE);
                ret = recv(sockfd, users[sockfd].buf, BUFFER_SIZE - 1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd);

                util_timer* timer = users[sockfd].timer;
                if(ret < 0) {
                    // 如果发生读错误，则关闭连接，并移除其对应的定时器
                    if(errno != EAGAIN) {
                        cb_func(&users[sockfd]);
                        if(timer) {
                            timer_lst.del_timer(timer);
                        }
                    }
                }
                else if(ret == 0) {
                    // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器
                    cb_func(&users[sockfd]);
                    if(timer) {
                        timer_lst.del_timer(timer);
                    }
                }
                else {
                    // 如果某个客户连接上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间
                    if(timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("adjust timer once\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
            else {
                // 其他事件
            }
        }
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    return 0;
}
