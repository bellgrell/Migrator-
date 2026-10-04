#include "commonconnection.h"
#include "public.h"

ConnectionPool *ConnectionPool::getConnectionPool(){
    //对于静态局部变量的初始化，编译器自动进行lock和unlock
    static ConnectionPool pool;
    return &pool;
}

//从文件中加载配置项
bool ConnectionPool::loadConfigFile(){
    FILE *pf = fopen("mysql.ini", "r");
    return false;
}
//=未完待续