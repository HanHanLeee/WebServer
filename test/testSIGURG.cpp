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

#define BUFFER_SIZE 1024 // 缓冲区大小

static int connfd; // 连接描述符

/* SIGURG 信号的处理函数*/
void sig_urg( int sig )
{
    int save_errno = errno;
    char buffer[ BUFFER_SIZE ]; // 缓冲区
    memset( buffer, '\0', BUFFER_SIZE );
    int ret = recv( connfd, buffer, BUFFER_SIZE-1, MSG_OOB ); // 接收带外数据
    printf( "got %d bytes of oob data '%s'\n", ret, buffer );
    errno = save_errno;
}

/* 设置文件描述符fd上的SIGURG信号的处理函数 */
void addsig( int sig, void (*sig_handler)(int) )
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
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1]; // IP地址
    int port = atoi( argv[2] ); // 端口号

    struct sockaddr_in address; // 服务器地址
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET; // IPv4协议
    inet_pton( AF_INET, ip, &address.sin_addr ); // 将ip地址转换为网络字节序
    address.sin_port = htons( port ); // 将端口号转换为网络字节序

    int sock = socket( PF_INET, SOCK_STREAM, 0 ); // 创建socket
    assert( sock >= 0 ); // 断言socket创建成功

    int ret = bind( sock, ( struct sockaddr* )&address, sizeof( address ) ); // 绑定socket
    assert( ret != -1 ); // 断言socket绑定成功

    ret = listen( sock, 5 ); // 监听socket
    assert( ret != -1 ); // 断言socket监听成功

    struct sockaddr_in client; // 客户端地址
    socklen_t client_addrlength = sizeof( client ); // 客户端地址长度
    connfd = accept( sock, ( struct sockaddr* )&client, &client_addrlength ); // 接受客户端连接
    if(connfd < 0)
    {
        printf( "errno is: %d\n", errno );
    }
    else
    {
        addsig( SIGURG, sig_urg ); // 设置SIGURG信号的处理函数
        /* 使用SIGURG信号之前，我们必须设置socket的宿主进程或进程组 */
        fcntl( connfd, F_SETOWN, getpid() ); // 设置socket的宿主进程
        char buffer[ BUFFER_SIZE ]; // 缓冲区
        /* 循环接收普通数据 */
        while( 1 )
        {
            memset( buffer, '\0', BUFFER_SIZE );
            ret = recv( connfd, buffer, BUFFER_SIZE-1, 0 ); // 接收普通数据
            if( ret <= 0 )
            {
                break;
            }
            printf( "got %d bytes of normal data '%s'\n", ret, buffer );
        }
        close( connfd ); // 关闭连接描述符
    }
    close( sock ); // 关闭socket
    return 0;
}