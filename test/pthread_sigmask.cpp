#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>


#define handle_error_en(en, msg) \
    do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0) // 错误处理宏

static void *sig_thread(void *arg)
{
    sigset_t *set = (sigset_t *)arg;
    int s, sig;

    for (;;) {
        s = sigwait(set, &sig);
        if (s != 0)
            handle_error_en(s, "sigwait");
        printf("Signal handling thread got signal %d\n", sig);
    }
}

int main(int argc, char* argv[])
{
    pthread_t thread;
    sigset_t set;
    int s;

    sigemptyset(&set); // 初始化信号集
    sigaddset(&set, SIGQUIT); // 将SIGQUIT信号加入信号集
    sigaddset(&set, SIGUSR1); // 将SIGUSR1信号加入信号集
    s = pthread_sigmask(SIG_BLOCK, &set, NULL); // 设置线程的信号屏蔽字
    if (s != 0)
        handle_error_en(s, "pthread_sigmask");

    s = pthread_create(&thread, NULL, &sig_thread, (void *)&set); // 创建线程
    if (s != 0)
        handle_error_en(s, "pthread_create");

    pause(); // 等待信号
}