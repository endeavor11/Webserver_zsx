#include "webserver.h"


// has read
// 日志那块，计时那块需要重新看一下
// 各种 dealwith 函数
// 和 utils 有关的部分，因为之前对utils类理解不够透彻


WebServer::WebServer() // 构造函数
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200); // inux命令中可以使用pwd查看当前目录，系统编程中可以通过getcwd获取当前目录。
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD]; // users_timer client_data 类型包含 address，sockfd，util_timer
                                           // util_timer 用作定时器的节点，pre，next，client_data
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log) // close_log 是否关闭日志
    {
        //初始化日志
        if (1 == m_log_write) // LOGWrite 日志写入方式，默认0，默认同步
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表，把所有的用户信息都存到map里面 users 是 http-conn指针类型
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    // 这里就把线程都创建好
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
    // m_actormodel 并发模型,默认是proactor
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0); // 第三个参数是选择TCP，UDP的
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // utils type : Utils
    utils.init(TIMESLOT); // 设置 timeslot lst_timer里面的函数
                          // 最终是这样使用的 alarm(m_TIMESLOT);
                          // 其实最后的效果就是每多少秒发送一个超时信号

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd; // 这个在http-conn里面是一个静态变量

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd); // 创建一对无名的、相互连接的套接字 用户通信
    // 这里完成了对 m_pipefd 的赋值和初始化 是在这里分配了空间吗
    // 这个主要是用作接收超时信号的回调函数发送过来的一些东西的
    // Utils::sig_handler 里面会往这个套接字里面发送信息
    // 这个应该是属于使用套接字进行通信

    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0); // 注册的是读事件，注意是m_pipefd[0]

    utils.addsig(SIGPIPE, SIG_IGN); // 关闭另一端的时候，服务器发信息的时候会收到 SIGPIPE 信号
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 这里是超时的一切的开始，从这里开始了，然后后面的一切才顺序执行的，这里是第一个超时设置
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    // 注意，m_pipefd 被赋值给其他文件引入的东西了，也是可以使用的，因为在运行的时候，全局变量区好像就那么一块吧
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}
// dealclinetdata 里面会调用这个函数
// 是链接最开始来了就会调用这个
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    // users 的类型是 http_conn
    // 下面这个就是设置一些基础的属性

    // 注意需要初始化，因为之前过期连接的数据还没清理呢
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 监控这个服务器分配 connfd的规律！

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中

    // user_timer client_data类型

    /*struct client_data{
        sockaddr_in address;
        int sockfd;
        util_timer *timer;
    };*/

    /*class util_timer{ // 这个类型是节点
    public:
        util_timer() : prev(NULL), next(NULL) {}

    public:
        time_t expire; // 超时时间，这个一定注意

        void (* cb_func)(client_data *);
        client_data *user_data;
        util_timer *prev;
        util_timer *next;
    };*/

    // client_data 类型
    // 下面这个 users_timer 和 users 两个结构一一对应，下标都是connfd
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func; // cb_func : 删除对应sockfd的监听事件 超时了就执行这个

    /*
    void cb_func(client_data *user_data) // 超时的节点，就会调用这个函数，就是直接关闭，不再监控这个socket的事件了
    {
        epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); // 把这个连接的sock监督事件删除
        assert(user_data);
        close(user_data->sockfd);
        http_conn::m_user_count--;
    }
    */


    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer); // timer已经在队列中了，看需不需要往后调

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    // 就是移出这个timer，就这样
    timer->cb_func(&users_timer[sockfd]); // 移出监听事件

    /*
    void cb_func(client_data *user_data) // 超时的节点，就会调用这个函数，就是直接关闭，不再监控这个socket的事件了
    {
        epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); // 把这个连接的sock监督事件删除
        assert(user_data);
        close(user_data->sockfd);
        http_conn::m_user_count--;
    }
    */


    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata()
{
    // 处理新到来的连接
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode) // 1 是 ET， 0 是 LT
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    // m_pipefd[0] 可读

    // 根据读出的信号，决定是处理超时连接还是关闭服务器

    // lst_timer.cpp 里面有如下调用 会往m_pipefd[1] 发送数据

//    void Utils::sig_handler(int sig)
//    {
//        //为保证函数的可重入性，保留原来的errno
//        int save_errno = errno;
//        int msg = sig;
//        send(u_pipefd[1], (char *)&msg, 1, 0); // 意思就是把sig信号，就是这个int类型，发送给u_pipefd[1] ，这是个套接字描述符
//        errno = save_errno;
//    }

    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i) // 可以有ret个信号的吗
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    // 思考，下面是等待improv了，那是否必要呢
    //
    // 监听到提示了，第一个执行的就是这个！
    // 最可能需要我们处理的事件，可读数据来了，用户的请求
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer); // 看需不需要往后调这个定时器
                                 // 主要是重新设置超时时间，然后往后调
        }

        //若监测到读事件，将该事件放入请求队列
        // 这个只是线程池的工作队列，还没有到拿出来工作的时候

        // 和下面的比一下，这里是放到队列里，下面的是直接 read_once
        m_pool->append(users + sockfd, 0);

        while (true) // 注意底下的break，也就是说 必须等待 improv == 1，才能继续下去处理其他东西，那么improv是什么意思呢
                     // 破案了， improv 是 是否已经完整地读入了一次请求数据，或者写了一次
                     // 读写，只是针对当前缓冲区，是否意义上的完整不知道

                     // request->read_once()  void threadpool<T>::run() 里面调用了这个函数

                     // 不管是读还是写，都会设置这个improv
                     // 注意 这个while 什么时候会退出呢？break的时候，而break只有在improv==1，之后才会执行
                     // 所以，主循环eventLoop，会一直在卡这里，读，写完成了，或者里面超时了
                     // timer_flag 和 这个 improv 基本会一起设置
        {
            if (1 == users[sockfd].improv) // 最开始，默认是0,
            {
                if (1 == users[sockfd].timer_flag) // 最开始也是默认0
                {
                    deal_timer(timer, sockfd); // 移出监听事件 代表关闭这个连接了
                    users[sockfd].timer_flag = 0; // 准备给下一个分配这个端口号用，所以要清零0
                }
                users[sockfd].improv = 0;   // 注意，当检测到当前轮次的读或者写完毕了，需要重置improv！
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())  // 只是读取，没有处理
                                        // 读到不能读，不是只读一行
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd); // // 移出监听事件
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1); // read state=0， write state=1

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write()) // 注意，这里write为什么要判断true，false呢，
                                   // true的话，继续监听端口，如果不是长链接，就返回false，然后移除这个socket
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop() // 最后调用的就是这个
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata(); // 一系列起步动作
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) // 客户端关闭了连接就会有这个事件
            {
                //服务器端关闭连接，移除对应的定时器
                // 取消socket上监听事件
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
                // EPOLLIN 有数据来了，可读
                // 最可能来得是 超时信号，也有可能是暂停服务器信号
                // 注意 m_pipefd[0] 来数据，不是说超时信号来了发送到这个套接字里面，而是，系统自动回调处理函数
                // 这个处理函数里面，我们手动往 m_pipefd[1] 里面发送了数据，所以这个套接字就可读了

                // 过一段时间会发来这个信号，间隔固定，执行dealwithsignal，但不一定真有超时连接
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }

            // 可以写数据了
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}