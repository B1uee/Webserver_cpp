#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem // 信号量类
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0) // 参数：信号量、信号量用于线程间同步（1则是进程间）、信号量初始值，返回值：成功返回0，失败返回-1
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0; // 信号量减1，如果信号量的值小于0，则阻塞等待，返回值：成功返回0，失败返回-1
    }
    bool post()
    {
        return sem_post(&m_sem) == 0; // 信号量加1，如果有进程阻塞在这个信号量上，则唤醒它，返回值：成功返回0，失败返回-1
    }

private:
    sem_t m_sem;  // sem_t是一个union类型，包含：char __size[32]; long int __align，作用分别为：信号量的内存大小和信号量的对齐，用户无需关心m_sem的值具体是如何修改的
};


class locker  // 互斥锁类
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) // 参数：互斥锁、互斥锁属性：NULL表示默认属性
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0; // 互斥锁加锁
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;  
    }

private:
    pthread_mutex_t m_mutex;
};


class cond  // 条件变量类
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)  // 参数：条件变量、条件变量属性：NULL表示默认属性
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex) 
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);  // 上锁和解锁的责任在调用这些函数的代码中，而不是在条件变量类本身中
        // 避免在调用wait前加一次锁进入wait又加一次锁的双重锁死锁问题，所以作者把里面的锁给注释了
        ret = pthread_cond_wait(&m_cond, m_mutex); // 阻塞等待条件变量，参数：条件变量、互斥变量，MUTEX is assumed to be locked before.
        // pthread_cond_wait()会将线程放入条件变量的等待队列，然后在内部解锁互斥锁，然后阻塞等待条件变量，当条件变量被唤醒时，会重新加锁互斥锁
        // 解锁是为了让其他线程可以访问共享资源，而阻塞是为了等待条件变量的变化，加锁是因为又要访问共享资源
        
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) // 等待一段时间
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t); // 阻塞等待条件变量，参数：条件变量、互斥变量、绝对时间
        
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0; // 唤醒一个等待条件变量的线程，post和signal的区别是：post是信号量的操作，signal是条件变量的操作
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0; // 唤醒所有等待条件变量的线程
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
