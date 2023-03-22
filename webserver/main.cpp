#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <cassert>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "ls_time.h"
#include"log/log.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5      //最小超时单位


//#define SYNLOG  //同步写日志
#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//添加文件描述符到epoll中
extern void addfd( int epollfd, int fd, bool one_shot );
//从epoll中移除监听的文件描述符
extern void removefd( int epollfd, int fd );

extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;


//信号处理函数
void sig_handler(int sig){
    //保证函数可重入性，保留errno，表示中断后再次进入此函数，不会丢失数据
    int save_errno=errno;
    int msg=sig;

    //将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1],(char *)&msg,1,0);
    //将原来的errno赋值为当前的errno
    errno =save_errno;
}

//添加信号捕捉,设置信号函数
void addsig(int sig, void( handler )(int), bool restart = true){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    //信号函数仅仅发送信号值，不做对应逻辑处理

    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    //将所有信号添加
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd,const char* info){
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );         //获取端口号
    addsig( SIGPIPE, SIG_IGN );         //对SIGPIE信号进行处理

    //创建线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ];     //创建数组用于保存所有的客户端信息
    assert(users);
    int user_count=0;

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );//监听文件描述符
    assert(listenfd>=0);  

    struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_addr.s_addr =htonl(INADDR_ANY) ;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert(ret >= 0);
    ret = listen( listenfd, 5 );
    assert(ret >= 0);

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    epollfd = epoll_create( 5 );
    assert(epollfd != -1);
   
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    //创建管道套接字
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    //设置管道写端非阻塞
    setnonblocking(pipefd[1]);

    //设置管道读端为ET非阻塞
    addfd(epollfd, pipefd[0], false);

    //传递给主循环的信号值，这里只关注SIGALRM和SIGTERM  
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    
    //用户定时器数组
    client_data *users_timer = new client_data[MAX_FD];

    //超时标志
    bool timeout = false;

    //每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);

    //循环条件
    bool stop_server = false;
    while(!stop_server) {
        //监测发生事件的文件描述符
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }
        //轮询文件描述符
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            //处理新到的客户连接
            if( sockfd == listenfd ) {
                
                //初始化客户端连接地址
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
#ifdef listenfdLT
                //该连接分配的文件描述符
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if (connfd < 0)
                {
                    printf("errno is:%d",errno);
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    printf("Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);
                
                
                //初始化该连接对应的连接资源(client_data数据)
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                //创建定时器临时变量
                util_timer *timer = new util_timer;
                //设置定时器对应的连接资源
                timer->user_data = &users_timer[connfd];
                //设置回调函数
                timer->cb_func = cb_func;

                time_t cur = time(NULL);

                //设置绝对超时时间
                timer->expire = cur + 3 * TIMESLOT;
                //创建该连接对应的定时器，初始化为前述临时变量
                users_timer[connfd].timer = timer;
                //将该定时器添加到链表中
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                while(1){
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        printf( "errno is: %d\n", errno );
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {

                        show_error(connfd, "Internal server busy");
                        //LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);
                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);

                }
                continue;
#endif               
              /* if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {//目前连接数满了//发送客户端，服务器正忙。
                    close(connfd);
                    continue;
                }
                users[connfd].init( connfd, client_address);
                */

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {

                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
                
                //users[sockfd].close_conn();

            } 
                //管道读端对应文件描述符发生读事件
            else if((sockfd == pipefd[0]) &&events[i].events & EPOLLIN) {

                /*if(users[sockfd].read()) {
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }*/

                int sig;
                char signals[1024];
                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)  //没信号
                {
                    continue;
                }
                else
                {
                    //处理信号对应逻辑
                    for (int i = 0; i < ret; ++i)
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
                        }
                        }
                    }
                }

            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client()");
                    Log::get_instance()->flush();

                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if( events[i].events & EPOLLOUT ) {

                /*if( !users[sockfd].write() ) {
                    users[sockfd].close_conn();
                }*/

                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client()");
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    
    close( epollfd );
    close( listenfd );
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
