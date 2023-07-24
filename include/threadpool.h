#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list> 
#include <cstdio> 
#include <exception> 
#include <pthread.h> 
#include "locker.h"

template< typename T >
class threadpool
{
public:
    threadpool( int thread_number = 8, int max_requests = 10000 );
    ~threadpool();
    bool append( T* request );

private:
    static void* worker( void* arg );  
    void run();

private:
    int m_thread_number; // 线程池中的线程数
    int m_max_requests; // 请求队列中允许的最大请求数
    pthread_t* m_threads; // 描述线程池的数组，其大小为m_thread_number
    std::list< T* > m_workqueue; // 请求队列
    locker m_queuelocker; // 保护请求队列的互斥锁
    sem m_queuestat; // 是否有任务需要处理
    bool m_stop; // 是否结束线程
};

template< typename T >
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL)
{
    if( (thread_number <= 0) || (max_requests <= 0) )
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number]; // 创建线程数组
    if( !m_threads )
    {
        throw std::exception();
    }

    for( int i = 0; i < thread_number; ++i )
    {
        printf( "create the %dth thread\n", i );
        if( pthread_create( m_threads + i, NULL, worker, this ) != 0 ) // 创建线程
        {
            delete [] m_threads;
            throw std::exception();
        }
        if( pthread_detach( m_threads[i] ) ) // 将线程设置为脱离线程
        {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T >
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

template< typename T >
bool threadpool<T>::append( T* request )
{
    m_queuelocker.lock(); // 加锁
    if( m_workqueue.size() > m_max_requests ) // 请求队列中的请求数大于最大请求数
    {
        m_queuelocker.unlock(); // 解锁
        return false;
    }
    m_workqueue.push_back( request ); // 将任务添加到请求队列中
    m_queuelocker.unlock(); // 解锁
    m_queuestat.post(); // 增加信号量
    return true;
}

template< typename T >
void* threadpool<T>::worker( void* arg )
{
    threadpool* pool = (threadpool*)arg;
    pool->run(); // 执行任务
    return pool;
}

template< typename T >
void threadpool<T>::run()
{
    while( !m_stop )
    {
        m_queuestat.wait(); // 等待信号量
        m_queuelocker.lock(); // 加锁
        if( m_workqueue.empty() ) // 请求队列为空
        {
            m_queuelocker.unlock(); // 解锁
            continue;
        }
        T* request = m_workqueue.front(); // 获取请求队列的第一个任务
        m_workqueue.pop_front(); // 删除请求队列的第一个任务
        m_queuelocker.unlock(); // 解锁
        if( !request ) // 任务为空
        {
            continue;
        }
        request->process(); // 执行任务
    }
}
#endif