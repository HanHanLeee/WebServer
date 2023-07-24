#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <cassert>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "http_conn.h"
#include "locker.h"
#include "threadpool.h"

#define MAX_FD 65536 // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 最大事件数

extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );

void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler; // 设置信号处理函数
    if( restart )
        sa.sa_flags |= SA_RESTART; // 重新调用被该信号终止的系统调用
    sigfillset( &sa.sa_mask ); // 将所有信号加入信号集
    assert( sigaction( sig, &sa, NULL ) != -1 ); // 注册信号处理函数
}

void show_error( int connfd, const char* info )
{
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 ); // 发送错误信息
    close( connfd );
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

    addsig( SIGPIPE, SIG_IGN ); // 忽略SIGPIPE信号

    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool< http_conn >; // 创建线程池
    }
    catch( ... )
    {
        return 1;
    }
    
    http_conn* users = new http_conn[ MAX_FD ]; // 创建http_conn数组
    assert( users );
    int user_count = 0; // 用户数量

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 ); // 创建监听socket
    assert( listenfd >= 0 );
    struct linger tmp = { 1, 0 }; 
    setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) ); // 设置优雅关闭连接

    int ret = 0;
    struct sockaddr_in address; // 服务器地址
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET; // IPv4
    inet_pton( AF_INET, ip, &address.sin_addr ); // 将ip地址转换为网络字节序
    address.sin_port = htons( port ); // 端口号

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) ); // 绑定地址
    assert( ret >= 0 );

    ret = listen( listenfd, 5 ); // 监听
    assert( ret >= 0 );

    epoll_event events[ MAX_EVENT_NUMBER ]; // 创建epoll_event数组
    int epollfd = epoll_create( 5 ); // 创建epollfd
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false ); // 将监听socket加入epollfd
    http_conn::m_epollfd = epollfd;

    while( true )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 ); // 监听事件
        if ( ( number < 0 ) && ( errno != EINTR ) ) // 出错
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) // 遍历事件
        {
            int sockfd = events[i].data.fd; // 获取事件对应的文件描述符
            if( sockfd == listenfd ) // 监听socket
            {
                struct sockaddr_in client_address; // 客户端地址
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength ); // 接受连接
                if ( connfd < 0 ) // 出错
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD ) // 用户数量达到上限
                {
                    show_error( connfd, "Internal server busy" ); // 显示错误信息
                    continue;
                }
                users[connfd].init( connfd, client_address ); // 初始化http_conn
            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) // 出错
            {
                users[sockfd].close_conn(); // 关闭连接
            }
            else if( events[i].events & EPOLLIN ) // 可读
            {
                if( users[sockfd].read() ) // 读取数据
                {
                    pool->append( users + sockfd ); // 添加任务
                }
                else
                {
                    users[sockfd].close_conn(); // 关闭连接
                }
            }
            else if( events[i].events & EPOLLOUT ) // 可写
            {
                if( !users[sockfd].write() ) // 写入数据
                {
                    users[sockfd].close_conn(); // 关闭连接
                }
            }
            else
            {}
        }
    }
    
    close( epollfd ); // 关闭epollfd
    close( listenfd ); // 关闭监听socket
    delete [] users; // 释放内存
    delete pool; // 释放内存
    return 0;
}