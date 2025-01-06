#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD]; // 创建MAX_FD个http类对象

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
    
    // 当服务器调用 listen 函数后，内核会为该套接字维护一个队列，用于存放已完成三次握手但尚未被 accept 处理的连接。
    ret = listen(m_listenfd, 5); // 参数含义：监听套接字、监听队列的最大长度
    assert(ret >= 0);

    utils.init(TIMESLOT); 

    /*
        服务器接收http请求:
        浏览器端发出http连接请求，主线程创建http对象接收请求并将所有数据读入对应buffer，
        将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
    */
    // epoll创建内核事件表
    // epollfd 是一个文件描述符，代表一个 epoll 实例。这个实例管理一个事件表，用于监控多个文件描述符上的事件
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);  // 预计会与 epoll 实例关联的文件描述符数量，现在已经不起作用
    assert(m_epollfd != -1);

    // 添加监听文件描述符到epoll(将 listenfd 添加到 epollfd 所管理的事件表中)
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
    timer->cb_func(&users_timer[sockfd]); // 调用定时器的回调函数,从epollfd中删除sockfd，并关闭sockfd
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
            case SIGALRM:  // 超时发送的信号
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
        reactor：非阻塞同步网络模式，感知的是就绪可读写事件，来了事件操作系统通知应用进程，让应用进程来处理，比如应用进程主动调用 read 方法来完成数据的读取

        proactor：异步网络模式，感知的是已完成的读写事件。在发起异步读写请求时，需要传入数据缓冲区的地址（用来存放结果数据）等信息，
        这样系统内核才可以自动帮我们把数据的读写工作完成，这里的读写工作全程由操作系统来做，并不需要像 Reactor 那样还需要应用进程主动发起 read/write 来读写数据，应用进程直接处理数据。
        
        「事件」就是有新连接、有数据可读、有数据可写的这些 I/O 事件
        「处理」包含从驱动读取到内核以及从内核读取到用户空间

        而由于linux的异步IO不成熟，在本函数中采用了同步IO模拟的proactor，先完成读取（同步的，会阻塞），然后将请求加入请求队列，工作线程直接处理已经读取好数据的请求；
        而reactor则是把读取数据的操作也交给工作线程处理，读取完再处理。
    */
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)   // 主线程等待事件（读、写等），并将其分派给处理程序（这里是线程池）。实际的读操作也由线程池中的工作线程执行，读取完再处理。
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true) // 轮询，确保工作线程的读操作完成，可以理解为同步非阻塞中的同步过程（等待数据传输完成）
        {
            if (1 == users[sockfd].improv) // 数据读操作完成（无论成功与否）
            {
                if (1 == users[sockfd].timer_flag) // 定时器超时，读失败时会改
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
        if (users[sockfd].read_once())  // 主线程由于是同步IO模拟的，先读取数据（阻塞在这），然后将请求加入请求队列，工作线程直接处理已经读取好数据的请求
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr)); // inet_ntoa：将网络地址转换为点分十进制的字符串

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
        /*
            epoll_wait 会返回所有就绪事件的数量。
            int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
            timeout：是超时时间，单位是毫秒，-1表示一直阻塞（至少有一个事件发生），0表示立即返回，>0表示等待指定时间
        */
       //等待所监控文件描述符上有事件的产生
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
         
        
        for (int i = 0; i < number; i++) 
        {
            int sockfd = events[i].data.fd; // 获取就绪事件的文件描述符

            //处理新到的客户连接
            if (sockfd == m_listenfd) // listenfd是监听套接字，处理多个客户连接
            {
                bool flag = dealclientdata();  // 只有LT模式返回true，因为会一直唤醒
                if (false == flag)
                    continue; // ET一口气读完，直接跳到下一个事件
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) // 客户端关闭连接一半、挂起事件（连接断开/出错）、错误事件（fd上发生错误）
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd); // 删除定时器、sockfd，并从epollfd中删除sockfd
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) // 管道读取端且事件为可读，说明是定时器或终止信号
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)  // read
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT) // write
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
