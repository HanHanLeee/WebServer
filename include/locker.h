#ifndef LOCKER_H
#define LOCKER_H

#include <exception>  
#include <pthread.h>  
#include <semaphore.h>

class sem
{
public:
    sem()
    {
        if(sem_init(&m_sem, 0, 0) != 0) // 初始化信号量
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if(sem_init(&m_sem, 0, num) != 0) // 初始化信号量
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem); // 销毁信号量
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0; // 等待信号量
    }
    bool post()
    {
        return sem_post(&m_sem) == 0; // 增加信号量
    }
private:
    sem_t m_sem;
};

class locker
{
public:
    locker()
    {
        if(pthread_mutex_init(&m_mutex, NULL) != 0) // 初始化互斥锁
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex); // 销毁互斥锁
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0; // 加锁
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0; // 解锁
    }
    pthread_mutex_t* get()
    {
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;
};

class cond
{
public:
    cond()
    {
        if(pthread_mutex_init(&m_mutex, NULL) != 0) // 初始化互斥锁
        {
            throw std::exception();
        }
        if(pthread_cond_init(&m_cond, NULL) != 0) // 初始化条件变量
        {
            pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_mutex_destroy(&m_mutex); // 销毁互斥锁
        pthread_cond_destroy(&m_cond); // 销毁条件变量
    }
    bool wait()
    {
        int ret = 0;
        pthread_mutex_lock(&m_mutex); // 加锁
        ret = pthread_cond_wait(&m_cond, &m_mutex); // 等待条件变量
        pthread_mutex_unlock(&m_mutex); // 解锁
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0; // 唤醒等待条件变量的线程
    }

private:
    pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};

#endif