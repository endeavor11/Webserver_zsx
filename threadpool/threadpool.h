#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"


// has read
// 太精髓了

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) // 注意这里参数放的是this
                                                                    // this是给worker的参数
                                                                    // 最重要的：create之后线程随时可以执行了！！！
                                                                    // 然后最开始肯定是阻塞了，因为队列里面没有任务
                                                                    // 一旦有任务了，就会有信号量post，然后去执行了！
                                                                    // 所以线程池只有一个队列是任务队列，线程是数组的形式
                                                                    // 不用人工去查找哪个线程空闲，去执行哪个任务
                                                                    // 创建线程的时候，就开始等待任务了！！
                                                                    // 一直等待队列中的任务！
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T *request, int state) // reactor 调用这个 // read state=0， write state=1
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state; // 和 append_p 区别在这里
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); // m_queuestat 是信号量，表示是否有任务需要处理
                        // 注意这个操作会让wait的线程唤醒
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request) // proactor 调用这个
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run() // 其实最主要的就是这个函数，各个线程会不断地调用这个函数，从请求队列里面拿出请求
{
    while (true)
    {
        m_queuestat.wait(); // 注意这个可不是条件变量，wait不一定wait的，还得看里面任务队列有没有任务了
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        if (1 == m_actor_model)
        {
            if (0 == request->m_state) // 这个state什么含义 好像是读为0，写为1
            {
                if (request->read_once())   // http_conn 里面单纯读数据的函数，读到 m_read_buf 里面
                                            // 只是把当前缓存里面的东西都读出来了，但是也不一定是一个完整的请求
                                            // 但是还是决定 把 improv设置为1，代表这里要读的都读完了
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    // 这个局部对象对自动调用构造函数和析构函数，完成资源的获取和释放
                    request->process(); // 如果请求不完全，还没接收完数据，那就直接返回了。继续一下轮看请求队列里空不空
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else // 写 state = 1
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else // proactor 是不用读了吗？什么时候读好了
        {
            // 这里没有像上面reactor那样，有stat区别是因为，proactor调用这个只有一个读数据，写的话会调用另个函数，不使用队列了
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
