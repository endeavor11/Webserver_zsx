#include "config.h"


// has read partial


int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "qgydb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化 简单写几个属性
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    

    //日志 初始化日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    // 这里服务器就创建好了这么多线程了！ 默认8个线程
    server.thread_pool();

    //触发模式 简单设置 m_TRIGMode
    server.trig_mode();

    //监听 创建监听套接字，最重要！
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}