#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

/*
    http报文的处理流程：
    1. 浏览器端发出http连接请求，主线程创建http对象接收请求并将所有数据读入对应buffer，
       将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
    2. 工作线程取出任务后，调用process_read函数，通过主、从状态机对请求报文进行解析
    3. 解析完之后，跳转do_request函数生成响应报文，通过process_write写入buffer，
       返回给浏览器端。
*/


class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD // 报文的请求方法
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE // 主状态机的状态
    {
        CHECK_STATE_REQUESTLINE = 0, // 解析请求行
        CHECK_STATE_HEADER,  // 解析请求头
        CHECK_STATE_CONTENT  // 解析消息体，仅用于解析POST请求
    };
    enum HTTP_CODE // 
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS // 从状态机的状态
    {
        LINE_OK = 0,    // 完整读取一行
        LINE_BAD,  // 报文语法有误
        LINE_OPEN  // 读取的行不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    // 初始化套接字
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true); // 关闭连接
    void process();
    bool read_once(); // 读取浏览器端发来的全部数据
    bool write(); // 响应报文写入函数
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool); // 同步线程初始化数据库读取表
    int timer_flag; // 是否启动定时器
    int improv;


private:
    void init();
    HTTP_CODE process_read(); // 从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret); // 向m_write_buf写入响应报文
    HTTP_CODE parse_request_line(char *text); // 主状态机解析请求行
    HTTP_CODE parse_headers(char *text); // 主状态机解析请求头
    HTTP_CODE parse_content(char *text);    // 主状态机解析请求内容
    HTTP_CODE do_request(); // 生成响应报文
    char *get_line() { return m_read_buf + m_start_line; }; //m_start_line是已经解析的字符，get_line用于将指针向后偏移，指向未处理的字符
    LINE_STATUS parse_line(); //从状态机读取一行，分析是请求报文的哪一部分
    void unmap();

    // 根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;

    // 存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx; //指向缓冲区m_read_buf的数据末尾的下一个字节
    long m_checked_idx; // 从状态机在m_read_buf中读取的位置
    int m_start_line;   // 每一个数据行在m_read_buf中的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE]; // 存储发出的响应报文数据
    int m_write_idx; // 写缓冲区中待发送的字节数
    CHECK_STATE m_check_state; // 主状态机当前所处的状态
    METHOD m_method;

    //以下为解析请求报文中对应的6个变量
    //存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;


    char *m_file_address;  //读取服务器上的文件地址
    struct stat m_file_stat;
    struct iovec m_iv[2];  //io向量机制iovec
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send; 
    int bytes_have_send;
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
