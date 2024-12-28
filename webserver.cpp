#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200); // 获取当前工作目录，200为最大长度
    char root[6] = "/root"; // 字符串末尾有一个'\0'
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1); // strlen不包括'\0'，分配内存时需要额外的一个字节来存储\0
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD]; 
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
    // LT：水平触发，含义是只要这个文件描述符还有数据可读，内核就不断通知你
    // ET：边缘触发，含义是只有当这个文件描述符从无数据变为有数据时，内核才通知你

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
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800); // 异步日志模式，max_queue_size为800
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0); // 同步日志模式
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log); // 3306为mysql默认端口号

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num); // 默认max_request = 10000
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0); // 参数含义：IPv4协议族、流式套接字、默认协议为TCP
    assert(m_listenfd >= 0); // 确保socket创建成功

    //优雅关闭连接
    if (0 == m_OPT_LINGER) 
    {
        struct linger tmp = {0, 1}; // linger：逗留，参数含义：l_onoff=0，l_linger=1，其中l_onoff为0表示禁用linger，l_linger为1表示等待1s（未启用）
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp)); // SOL_SOCKET：通用套接字选项 
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1}; // l_onoff=1，l_linger=1，其中l_onoff为1表示启用linger，l_linger为1表示等待1s（启用）
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address)); // 将address中前sizeof(address)个字节置为0，可被memset(&address, 0, sizeof(address))替代
    address.sin_family = AF_INET; // 地址族：IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY); // INADDR_ANY：0，表示本机的任意IP地址, htonl：将主机字节序转换为网络字节序, nl: network long
    address.sin_port = htons(m_port); // htons：将主机字节序转换为网络字节序, ns: network short

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); // 允许重用本地地址和端口
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)); 
    assert(ret >= 0);
    ret = listen(m_listenfd, 5); // 参数含义：监听套接字、最大连接数
    assert(ret >= 0);

    utils.init(TIMESLOT); 

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);  // 预计会与 epoll 实例关联的文件描述符数量
    assert(m_epollfd != -1);

    // 添加监听文件描述符到epoll
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode); //
    http_conn::m_epollfd = m_epollfd; 

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd); // 创建一对连接的套接字，类似于管道两端
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]); // 将文件描述符设置为非阻塞模式
    utils.addfd(m_epollfd, m_pipefd[0], false, 0); // 将管道读端添加到epoll事件表中

    utils.addsig(SIGPIPE, SIG_IGN); 
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;   // 类的静态成员变量，可以在没有类实例的情况下访问
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    /*
        timer:创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中。
        调用定时器的回调函数关闭连接。
        从定时器链表中删除定时器。
        记录日志。
    */
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd]; // 绑定用户数据
    timer->cb_func = cb_func; // 绑定了回调函数
    time_t cur = time(NULL); // 获取当前时间
    timer->expire = cur + 3 * TIMESLOT; // 设置超时时间 15s
    users_timer[connfd].timer = timer; // 绑定定时器
    utils.m_timer_lst.add_timer(timer); // 将定时器添加到链表中
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer); 

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]); // 调用定时器的回调函数,关闭连接
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);  // 从链表中删除定时器
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata() // 处理新到的客户连接
{
    struct sockaddr_in client_address; // 客户端地址，在调用accept函数后，会将客户端的地址信息保存在这个结构体中
    socklen_t client_addrlength = sizeof(client_address); 
    if (0 == m_LISTENTrigmode) // LT，只处理一个客户连接
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength); // accept返回一个新的套接字描述符，用于与客户端通信
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
        timer(connfd, client_address); // 创建定时器
    }

    else
    {
        while (1) // ET，循环处理客户连接，只通知一次，需要一次性将数据读完
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
    return true; // 只有LT模式下才会返回true
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    // 从管道读端读取信号，根据信号值判断是否超时或需要停止服务器
    int ret = 0;
    int sig;
    char signals[1024]; 
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);  // 参数：管道读端，接收缓冲区，缓冲区大小，阻塞接收，ret为接收到的字节数 
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0) // 读取到的数据为空
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i]) // 根据信号值判断是否超时或需要停止服务器
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
    /*
        reactor和proactor的区别：
        reactor：主线程只负责监听事件，当事件发生时，将事件分发给工作线程进行处理，适合复杂的事件处理逻辑
        proactor：主线程不仅负责监听事件，还负责完成事件的初步处理（如读取数据），然后将处理后的数据交给工作线程进行进一步处理，系统或库完成事件处理，应用程序只处理结果，适合高效 I/O 操作。

    */
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

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
        if (users[sockfd].read_once())
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
            deal_timer(timer, sockfd);
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

        m_pool->append(users + sockfd, 1);

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
        if (users[sockfd].write())
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

void WebServer::eventLoop()
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
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
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
