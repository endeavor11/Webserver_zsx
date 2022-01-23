#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// has read

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users; // 注意一下，现在在这里存储了所有的用户，之后主程序还是main，但是这里定义的东西还能用，因为是用
                           // 这里定义的函数调用的
                           // 这个存的是用户名和密码！

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user") // 查询完，不拿结果，用底下的store把结果存到RES*中
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result); // 获取列数

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result); // 返回一个所有字段结构的数组
    // 对于结果集，返回所有MYSQL_FIELD结构的数组。每个结构提供了结果集中1列的字段定义

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
// 注意，新连接来的时候，只注册读事件，有明确的写的任务之后，才会注册写事件
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode) // TRIGMode == 1 是ET，0是LT
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
// 修改，只是修改，没有添加也没有删除
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    // 当有新连接的饿时候，就调用这个
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode); // 就是监听这个读事件 写是等有需求了之后才会监听写事件的
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str()); // sql_user 这些都是conn的成员变量
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    // 需要注意的是，每个用户都有一个http_conn

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
// 其实就是把 \r\n 改成 \0\0
// 更改的内容是 m_read_buf，是存储数据的数组

// 注意，虽然for循环里面是直到m-read-idx，但是，碰到一行的末尾，也返回了，没有读到m-read-idx！
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) // m_checked_idx < m_read_idx 条件成立说明读入到了read_idx，但是只处理到了checkerd这里
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)  // 已经到末尾了，说明这行的数据没读完整 按理 m_read_idx 这个地方应该还没数据
                                                    // 是啊，因为m_read_idx初始化为0，这个时候一个数据都没有
                return LINE_OPEN;                   //
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN; // 这里返回line-open很关键，看到了吗？如果请求头的某一行都没发送完整，那么一般而言会碰到这里，
                      // 然后返回line-open
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完

// 这里光读了，没有处理
// 表现为 m_read_idx 一直增加
bool http_conn::read_once() // 注意，可不是从头读数据的！因为有可能之前有遗留数据，这次数据来了请求才完整，所以还要判断read-idx超没超的
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    // char m_read_buf[READ_BUFFER_SIZE];
    // m_read_buf 是个数组
    // m_read_buf + m_read_idx 就是进来的数据应该放的地方
    // READ_BUFFER_SIZE - m_read_idx 是可读的大小
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    // 一直读，直到没有数据,一般都用这个
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t"); // 在 text中寻找最先含有空格或者\t的字符的位置并返回
                                  // 所以 m_url 应该是url开始的下标
                                  // char *m_url;
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; // 空格或者\t 改成 \0, 这样直接从text读，那么就会只读到方法
    char *method = text; // method 在text 的最开始就是
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t"); // 找 m_url 中第一个不是\t的字符串
    m_version = strpbrk(m_url, " \t"); // 在 m_url 中寻找最先含有空格或者\t的字符的位置并返回 1.1
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0'; // 因为第一个字符可能是 \t 空格，这里改成\0
    m_version += strspn(m_version, " \t"); // 找 m_version 中第一个不是\t的字符串
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); // 查找第一个出现 / 的下标
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; // 行检查完 检查头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0') // 说明读到空行了呀！结束了！
    {
        if (m_content_length != 0) // 这个时候判断内容长度，如果不是0，说明是post，要先解析content，如果是0，就get-request
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text); // 设置 content 内容
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)) // m-read-idx 是所有读取的内容 content-length是请求体的长度
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // parse_line: 更改 m_read_buf 里面的内容，把 \r\n 改成 \0\0
    // parse_line: 从 m_check_idx 开始，一直处理到 m-read-idx
    // m_start_line: 新一行开始的地方

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line(); // 返回新一行开始的下标
        // char *get_line() { return m_read_buf + m_start_line; };
        m_start_line = m_checked_idx; // 从已经读取的下一个地方开始
        LOG_INFO("%s", text);
        switch (m_check_state) // 进入状态机的节奏
        {
        case CHECK_STATE_REQUESTLINE: // 解析请求行
        {
            ret = parse_request_line(text); // m_check_state = CHECK_STATE_HEADER ret = NO_REQUEST

            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text); // 这个函数一次只解析一个头，因为使用的是状态机，所以需要反复执行这个好几次才能获取完成的头部信息
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST) // 只有text[0] 是 \0 的时候，才返回GET_REQUEST 说明遇到空行了
                                         // 如果是post，检测到 content 长度不为0， 就返回 NO_REQUEST
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text); // 把content都存储到了 m_string 里面去
            if (ret == GET_REQUEST)    // 即使是 post 请求，最后也是 GET_REQUEST
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/'); // 查找第一个匹配的 /


//    //当url为/时，显示判断界面
//    if (strlen(m_url) == 1)
//        strcat(m_url, "judge.html");  就变成了 /judge.html

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1]; // 2 或者 3

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/"); // 就是吧第一个字符设置成 /
        strcat(m_url_real, m_url + 2); // 应该是把前面的数字去掉了
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); // 应该是获取文件的完整路径了
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3') // flag 没用上，寄！
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据

            // 没有java方便呀...难受
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) // 说明没有重名的
            {
                // 底下的 mysql 是每个conn都有的
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else // 有重名的
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0') // post 跳转到注册
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1') // post 跳转到登录
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5') // post 图片请求
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6') // 视频请求
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7') // 关注页面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); // 都不是的话，直接把url拼接过去
                                                                   // char m_real_file[FILENAME_LEN]; FILENAME_LEN=200

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);

    // m_file_address char* 类型
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write() // 好像只有 proactor 写数据的时候会调用这个，错了，reactor也会调用这个，不过是先放进队列里，然后再调用的
// 调用write的函数 ： void WebServer::dealwithwrite
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); // 这个是改变！ 没有要写的，设置为监听读
        init(); // 把所有东西都初始化 read_idx 之类的
        return true;
    }

    while (1) // 注意这个循环，因为有可能一次写不完的，待定，初次查没查到
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0) // 出错了 其实是缓冲区满了
        {
            if (errno == EAGAIN) // 提示再试一次 有可能是发送完了，没什么要发送的了
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);   // 写失败了，再次监听写
                return true;
            }
            unmap(); // 取消映射
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len) // 说明头部信息已经写得差不多了
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap(); // 取消内存映射而已
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); // 没发送的了，监听写

            if (m_linger) // 如果发来的请求有keep-alive的话，就是会true
            {
                init();
                return true;
            }
            else
            {
                return false; // 不是长链接，看到返回false，会关闭连接！ void WebServer::dealwithwrite 会执行这个函数，
                              // 然后可能关闭连接1
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...) // response 每一行都要调用这个
{
    // m_write_idx ： 已经写到了哪里了
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title); // 我寻思这返回的true false结果也没人要啊
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form)) // 噢，这里要了 一般都是返回true
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else // 没东西，返回个空页
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process() // 不管是reactor还是proactor，都会调用这个
{
    HTTP_CODE read_ret = process_read(); // m_file_address 之类的请求的数据已经设置好了
    if (read_ret == NO_REQUEST) // 应该是没接收完整数据，待定 // 注意还有bad_request
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);    // 还没读完数据，监听写
        return;
    }
    bool write_ret = process_write(read_ret); // 如果请求出错，比如没有资源，这里就返回false 错错错，没资源啥的都不要紧，都是true
                                              // 因为就算没资源，也是要正确返回文档的
                                              // 这里处理了 bad_request 上面处理了no_request
                                              // 注意，bad-request也是会发送消息的！
                                              // 但是no-request就不发送了，继续等待
                                              // 正常的话，这里返回true

                                              // 这个是往 m_wirte_buf 里面写数据，没有真正发送
                                              // 什么时候返回false呢，add_content 都没成功这种

                                              // 这个函数的作用是把之后要写的文件向量准备好，还没开始写呐
    if (!write_ret)
    {
        close_conn(); // 这里面会删除监听！
    }

    // 监控socket的写事件，如果可写的话，就可以把buf里面的数据写进去了
    // 这个写不是一直可以写的吗？会有什么触发的契机吗
    // 之前好像没注册
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

// 1.请求数据不全：如果请求数据没全，返回no-request，然后继续监控可读数据，之后会重新开始执行dealwithread，这个函数进入等待improv改变的循环中
// 什么时候会删除epoll监控呢 close_conn 里面会真删除，就是write-ret返回false的时候
// 正常写完数据是不删除的

// 第一次初始化连接 init 的时候 addfd
// 写失败了，才 close_conn -> removefd （那超时怎么处理）
// 无数据可写了，监听读  process里面还没有请求呢，监听读  modfd
// 2.超时了怎么办 直接删除对应的读写监听，关闭套接字。就没了！不会手动清零一些东西的。所以其他链接来了才会覆盖http-conn里面的那些元素，init
// 3.连接关闭就只有超时这一种方法吗？有没有一种主动关闭的方法
// m_user_count 只有在超时，close_conn 函数里面会 -1
// 4.写完了，会不会清空内存，方便下次写 init() 会的！ 看write函数最后，如果是长链接的话，就会有init()函数