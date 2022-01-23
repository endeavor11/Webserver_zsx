#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

// has read
// 需要再仔细看一下调用的形式，是单独一个线程还是信号处理函数还是啥
// 答：单独一个线程哦！一直等待队列里有消息可记录



Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
// split_lines: 最大行数
// 函数的作用是，如果异步，就创建一个队列和线程，如果是同步，就初始化一些文件的名称，打开文件这样，没有什么写文件操作
// void WebServer::log_write() 这里调用
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    // file_name 默认 "./ServerLog"
    // split_lines 默认 800000
    //如果设置了max_queue_size,则设置为异步
    // 异步了，单独创建一个线程，那如果是同步呢
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size); // 基于数组的一个队列 队列里面有锁！
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);

    }

    //        static void *flush_log_thread(void *args)
    //        {
    //            Log::get_instance()->async_write_log();
    //        }


    /*
            void *async_write_log() // 只执行这一个函数，就可以一直保持日志录入吗？当然，为什么呢，当然是pop函数有阻塞！
            // 即使当前队列没有日志，那就阻塞wait住，直到有信息来了才返回
            {
                string single_log;
                //从阻塞队列中取出一个日志string，写入文件
                while (m_log_queue->pop(single_log))
                {
                m_mutex.lock();
                fputs(single_log.c_str(), m_fp);
                m_mutex.unlock();
                }
            }
    */


    
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // file_name 是init中的参数
    const char *p = strrchr(file_name, '/'); // 找到fime_name 中最后一个匹配/的地方
                                             // strrchr 在file_name 中找最后一次出现某字符的位置
                                             // file_name 默认 "./ServerLog"
    char log_full_name[256] = {0};

    // log_full_name 这个是和当前的时间有关的
    if (p == NULL)
    {
        // 把内容复制到 log_full_name 中
        // snprintf: 格式化把字符串输入到 log_full_name 里面
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1); // p 是指针，注意
                                 // 这句的意思是把名字复制到log_name 里面，这里就初始化log_name 了
                                 // 具体而言，默认把ServerLog复制到log_name 里面
        strncpy(dir_name, file_name, p - file_name + 1); // file_name 默认 "./ServerLog"
                                // p的位置是/的下标
                                // 处理目录名
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)   // FILE *m_fp;   打开log的文件指针
    {
        return false;
    }

    return true;
}

// 这个就是每次想写入日志，都需要调用这个函数的各种变体，在webserver.cpp 里面有这个函数的各种封装的使用
// 如果是异步，那就把消息放入队列中，就不管了，如果是同步模式，那就函数底下就开始写了
void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        // 这个意思就是该分下一个文件记录log了！
        // 天不是今天了，或者m_count == split_lines 了，自然就要换一个文件录入了
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';        // char *m_buf;
    m_buf[n + m + 1] = '\0';
    log_str = m_buf; // 从 char* 转化为 string

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
    {   // 异步，就是先放到队列了！可以立刻返回！
        m_log_queue->push(log_str);
    }
    else
    {
        // 同步，就是得现在就fput进去！
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
