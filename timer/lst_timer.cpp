#include "lst_timer.h"
#include "../http/http_conn.h"

// has read
// 大部分是链表的操作，底下一些增加信号处理函数的需要再去看看，处理非活动连接

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
void sort_timer_lst::tick() // 疑问：什么时候调用这个函数, eventLoop 里面，如果到了定的时间，就执行一次
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire) // 如果当前时间小于规定的超时时间，那么说明没啥事，带伙都没有超时呢，就直接break
        {
            break;
        }
        tmp->cb_func(tmp->user_data); // 如果发现超时了，那么就调用cb_func 函数
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
// 会把这个函数设置成很多信号的处理函数
// 下面这是webserver中的使用
// utils.addsig(SIGPIPE, SIG_IGN);
// utils.addsig(SIGALRM, utils.sig_handler, false);
// utils.addsig(SIGTERM, utils.sig_handler, false);

void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0); // 意思就是把sig信号，就是这个int类型，发送给u_pipefd[1] ，这是个套接字描述符
    errno = save_errno;
}

//设置信号函数
// 在 webserver 中有大量使用
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    // 把 sig信号和handler绑定起来
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler; // 新设置的信号处理函数绑定
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
// 疑问：什么时候调用这个函数

// 答 在主循环里面，webserver.cpp 里面，如果来了定时alarm信号，就会这是timeout参数，然后决定是否执行这个函数
//if (timeout)
//{
//    utils.timer_handler();
//
//    LOG_INFO("%s", "timer tick");
//
//    timeout = false;
//}


// 起步的时候是在 webserver.cpp 里面有一个 alarm(TIMESLOT)
// 有超时的，最终肯定会执行这个
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT); // 调用这个函数的时候，信号就是其来自 alarm，这是消耗掉了，所以现在需要再次设置一下alarm
}

void Utils::show_error(int connfd, const char *info) // 连接超过数量了，就调用这个
                                                     // 疑问 调用的时候，connfd是谁
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 工具类,信号和描述符基础操作
// 注意，m_pipefd 被赋值给其他文件引入的东西了，也是可以使用的，因为在运行的时候，全局变量区好像就那么一块吧
// Utils::u_pipefd = m_pipefd;
// Utils::u_epollfd = m_epollfd;

// 这两个都是在webserver.cpp 中被赋值的
// 这两个定义的时候是static的，全局整个类就这么一个

// 而 m_pipefd 是
// ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd); // 创建一对无名的、相互连接的套接字 用户通信
// 这里完成了对 m_pipefd 的赋值和初始化 是在这里分配了空间吗

// webserver 里面大量使用了Utils类

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data) // 超时的节点，就会调用这个函数，就是直接关闭，不再监控这个socket的事件了
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); // 把这个连接的sock监督事件删除
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
