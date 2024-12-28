#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "cyan";
    string passwd = "admin123";
    string databasename = "webserver";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化 
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,  // 端口号、数据库信息、日志写入方式
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, // 优雅关闭链接、触发组合模式、数据库连接池数量、线程池内的线程数量
                config.close_log, config.actor_model); // 是否关闭日志、并发模型选择
    

    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}