#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static const int CONTROL_LEN = CMSG_LEN(sizeof(int)); // 传递辅助数据的缓冲区长度


void send_fd(int fd, int fd_to_send)
{
    struct iovec iov[1]; // 用于描述通信的缓冲区
    struct msghdr msg; // 用于实现通信的消息
    char buf[0]; // 用于实现通信的辅助数据
    iov[0].iov_base = buf; // 设置缓冲区
    iov[0].iov_len = 1; // 设置缓冲区长度
    msg.msg_name = NULL; // 设置目标socket地址
    msg.msg_namelen = 0; // 设置目标socket地址长度
    msg.msg_iov = iov; // 设置通信缓冲区
    msg.msg_iovlen = 1; // 设置通信缓冲区长度

    cmsghdr cm;
    cm.cmsg_len = CONTROL_LEN; // 设置辅助数据缓冲区长度
    cm.cmsg_level = SOL_SOCKET; // 设置socket API级别
    cm.cmsg_type = SCM_RIGHTS; // 设置辅助数据类型
    *(int*)CMSG_DATA(&cm) = fd_to_send; // 设置辅助数据
    msg.msg_control = &cm; // 设置辅助数据缓冲区
    msg.msg_controllen = CONTROL_LEN; // 设置辅助数据缓冲区长度

    sendmsg(fd, &msg, 0); // 发送数据
}

int recv_fd(int fd)
{
    struct iovec iov[1]; // 用于描述通信的缓冲区
    struct msghdr msg; // 用于实现通信的消息
    char buf[0]; // 用于实现通信的辅助数据
    iov[0].iov_base = buf; // 设置缓冲区
    iov[0].iov_len = 1; // 设置缓冲区长度
    msg.msg_name = NULL; // 设置目标socket地址
    msg.msg_namelen = 0; // 设置目标socket地址长度
    msg.msg_iov = iov; // 设置通信缓冲区
    msg.msg_iovlen = 1; // 设置通信缓冲区长度

    cmsghdr cm;
    msg.msg_control = &cm; // 设置辅助数据缓冲区
    msg.msg_controllen = CONTROL_LEN; // 设置辅助数据缓冲区长度

    recvmsg(fd, &msg, 0); // 接收数据
    int fd_to_read = *(int*)CMSG_DATA(&cm); // 获取辅助数据
    return fd_to_read;
}

int main()
{
    int pipefd[2];
    int fd_to_pass = 0;
    int ret = socketpair(PF_UNIX, SOCK_DGRAM, 0, pipefd); // 创建socketpair
    assert(ret != -1);

    pid_t pid = fork();
    assert(pid >= 0);

    if(pid == 0)
    {
        close(pipefd[0]);
        fd_to_pass = open("test.txt", O_RDWR, 0666); // 打开文件
        send_fd(pipefd[1], (fd_to_pass > 0) ? fd_to_pass : 0); // 发送文件描述符
        close(fd_to_pass);
        exit(0);
    }

    close(pipefd[1]);
    fd_to_pass = recv_fd(pipefd[0]); // 接收文件描述符
    char buf[1024];
    memset(buf, '\0', 1024);
    read(fd_to_pass, buf, 1024); // 读取文件内容
    printf("I got fd %d and data %s\n", fd_to_pass, buf);
    close(fd_to_pass);
}