#pragma once
#include "connection.h"
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <memory>
#include <functional>
#include <condition_variable>

using namespace std;

class ConnectionPool{
    public:
        //获取连接池对象实例（懒汉单例模式，在获取实例时才实例化对象）
        static ConnectionPool *getConnectionPool();

        //给外部提供接口，从连接池中获取一个可用的空闲连接
       shared_ptr<Connection> getConnection();
    private:
     //单例模式-构造函数私有化
        ConnectionPool();
        //从配置文件中加载配置项
        bool loadConfigFile();
        //运行在独立线程中，专门负责生产新连接
        void produceConnectionTask();
        //扫描超过maxIdTIme时间的空闲连接，进行对于连接的回收
        void scannerConnectionTask();

        string _ip;             //Mysql的IP地址
        unsigned short port;    //mysql的端口号默认3306
        string _username;       //mysql的登录用户名
        string _password;       //mysql的密码
        string _dbname;         //连接的数据库名称
        int _initSize;          //连接池最大的初始连接数量
        int _maxSize;           //连接池的最大连接量
        int _maxIdleTime;       //连接池的最大空闲时间
        int _connectionTimeout; //连接池获取连接的超时时间

        //存储Mysql连接的队列
        queue<Connection *> _connectionQue;
        //维护连接队列的线程安全互斥锁
        mutex _queueMutex;

        //记录connection的总数量
        atomic_int _connectionCnt;

        //设置条件变量，用于连接生产线程和连接消费线程的通信
        condition_variable cv;
}