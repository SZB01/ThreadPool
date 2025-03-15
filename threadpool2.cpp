// threadpool2.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//
#include "threadpool.h"

#include <iostream>
#include <chrono>

using namespace std;


int sum(int a, int b) {
    this_thread::sleep_for(chrono::seconds(3));
    return a + b;
}

int main()
{
    {
        ThreadPool pool;
        //pool.setMode(PoolMode::MODE_CACHED);
        pool.start(2);
        future<int> a = pool.submitTask(sum, 10, 20);
        future<int> a1 = pool.submitTask(sum, 10, 20);
        future<int> a2 = pool.submitTask(sum, 10, 20);
        future<int> a3 = pool.submitTask(sum, 10, 20);
        future<int> a4 = pool.submitTask(sum, 10, 20);
        future<int> a5 = pool.submitTask(sum, 10, 20);
        cout << a.get() << endl;
        cout << a1.get() << endl;
        cout << a2.get() << endl;
        cout << a3.get() << endl;
        cout << a4.get() << endl;
        cout << a5.get() << endl;
    }
}

