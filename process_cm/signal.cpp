#include <sys/sem.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

union semun
{
    int val;  // Value for SETVAL
    struct semid_ds *buf;  // Buffer for IPC_STAT, IPC_SET
    unsigned short *array; // Array for GETALL, SETALL
    struct seminfo *__buf; // Buffer for IPC_INFO (Linux-specific)
};

void pv(int sem_id, int op)
{
    struct sembuf sem_b;
    sem_b.sem_num = 0;
    sem_b.sem_op = op; // P/V操作
    sem_b.sem_flg = SEM_UNDO;
    semop(sem_id, &sem_b, 1);
}

int main(int argc, char* argv[])
{
    int sem_id = semget(IPC_PRIVATE, 1, 0666);
    union semun sem_un;
    sem_un.val = 1;
    semctl(sem_id, 0, SETVAL, sem_un); // 信号量初始化为1

    pid_t id = fork();
    if(id < 0)
    {
        return 1;
    }
    else if(id == 0)
    {
        printf("child try to get binary sem\n");
        pv(sem_id, -1); // 获取信号量
        printf("child get the sem and would release it after 5 seconds\n");
        sleep(5);
        pv(sem_id, 1); // 释放信号量
        exit(0);
    }
    else
    {
        printf("parent try to get binary sem\n");
        pv(sem_id, -1); // 获取信号量
        printf("parent get the sem and would release it after 5 seconds\n");
        sleep(5);
        pv(sem_id, 1); // 释放信号量
    }

    waitpid(id, NULL, 0);
    semctl(sem_id, 0, IPC_RMID, sem_un); // 删除信号量
    return 0;
}
