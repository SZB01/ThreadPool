#ifndef THREADPOOL_H
#define THREADPOLL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <thread>
#include <future>
#include <unordered_map>

//const unsigned int TASK_MAX_THRESHHOLD = INT32_MAX;
const unsigned int TASK_MAX_THRESHHOLD = 2;
const unsigned int THREAD_MAX_THRESHHOLD = 100;
const unsigned int THREAD_MAX_IDLE_TIME = 60; //单位seconds


//线程池支持的模式
enum PoolMode {
	MODE_FIXED,
	MODE_CACHED,
};

//线程类型
class Thread {
public:
	//线程函数对象类型
	using ThreadFunc = std::function<void(unsigned int)>;

	//启动线程
	void start() {
		if (this == nullptr) {
			std::cout << "错误!" << std::endl;
		}
		//创建一个线程来执行线程函数
		std::thread t(func_, threadId_); //c++11 线程对象t和线程函数func_
		t.detach();  //设置分离线程
	}

	//线程构造
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{
	}
	//线程析构
	~Thread() = default;

	// 获取线程ID
	unsigned int getId() const {
		return threadId_;
	}

private:
	ThreadFunc func_;
	static unsigned int generateId_;
	unsigned int threadId_; //保存线程ID
};

unsigned int Thread::generateId_ = 0;

// 线程池类型
class ThreadPool {
public:
	// 构造函数
	ThreadPool()
		: initThreadSize_(0)
		, idleThreadSize_(0)
		, taskSize_(0)
		, curThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)

	{
	}

	// 析构函数
	~ThreadPool() {
		isPoolRunning_ = false;
		// 等待线程池所有的线程返回
		// 两种状态 一种是阻塞，一种是正在执行任务中


		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&] {return threads_.size() == 0; });

	}

	// 设置线程池工作模式
	void setMode(PoolMode mode) {
		if (checkRunningState()) {
			return;
		}
		poolMode_ = mode;
	}

	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(size_t threshhold) {
		if (checkRunningState()) {
			return;
		}
		taskQueMaxThreshHold_ = threshhold;
	}
	// 设置cached模式下线程上限阈值 
	void setThreadMaxThreshHold(size_t threshhold) {
		if (checkRunningState()) {
			return;
		}
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshhold;
		}
		return;
	}

	// 给线程池提交任务
	// 使用可变参数模板，让submitTask可以接受任意函数和任意类型的参数
	// pool.submitTask(sum1, 10, 20);

	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// 打包任务，放入任务队列
		using Rtype = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<Rtype()>>(
			std::bind(std::forward<Func>(func),std::forward<Args>(args)...));
		std::future<Rtype> result = task->get_future();

		// 获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 线程通信 等待任务队列有空余 不能阻塞超过1s，否则判断提交任务失败,返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&] {return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			std::cerr << "task queue is full, submit task fail." << std::endl << std::endl;
			
			// 任务提交失败，返回一个要求类型的默认构造
			auto task = std::make_shared<std::packaged_task<Rtype()>>(
				[]()->Rtype {return Rtype(); }
			);
			(*task)();
			return task->get_future();
		}

		//如果有空余 把任务放到任务队列里
		//taskQue_.push(sp);
		taskQue_.emplace([task]() {
			(*task)();
			});
		++taskSize_;

		//因为放了任务，队列肯定不空，在notEmpty_上通知
		notEmpty_.notify_all();

		// cached模式 任务处理比较紧急，适合任务小而多的场景
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << "create new thread..." << std::endl;
			//创建新线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			unsigned int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			threads_[threadId]->start();

			++idleThreadSize_;  // 空闲线程的数量
			++curThreadSize_;
		}


		return result;
	}



	//开启线程池
	void start(size_t initThreadSize = std::thread::hardware_concurrency()) {
		isPoolRunning_ = true;

		//记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//创建线程对象
		for (int i = 0; i < initThreadSize_; ++i) {
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			unsigned int threadId = ptr->getId();
			//std::cout << threadId << std::endl;
			threads_.emplace(threadId, std::move(ptr));
		}

		//启动所有线程
		for (unsigned int i = 0; i < initThreadSize_; ++i) {
			threads_[i]->start(); // 执行一个线程函数
			++idleThreadSize_;  // 记录初始空闲线程的数量
		}
	}

	//不允许拷贝线程池
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool* operator=(const ThreadPool&) = delete;

private:
	//定义线程函数
	void threadFunc(unsigned int threadId) {
		auto lastTime = std::chrono::high_resolution_clock().now();
		while (true) {
			Task task;
			{
				//先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::unique_lock<std::mutex> clock(cmtx_);
				std::cout << "tid:" << std::this_thread::get_id() << std::endl;
				std::cout << "尝试获取任务..." << std::endl;
				clock.unlock();

				// cached模式下，有可能创建了很多线程，空闲时间超过60s
				// 就结束该线程(回收超过initThreadSize_数量的线程)
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_) {
						threads_.erase(threadId);
						exitCond_.notify_all();
						std::cout << "回收threadid:" << std::this_thread::get_id() << std::endl;
						return;
					}
					// 每一秒中返回一次 
					// 区分超时返回还是有任务待执行返回
					if (poolMode_ == PoolMode::MODE_CACHED) {
						// 条件变量超时返回
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_) {
								// 开始回收当前线程
								// 记录线程数量的相关变量的值修改

								// 把线程对象从线程列表容器中删除 
								threads_.erase(threadId);
								--curThreadSize_;
								--idleThreadSize_;

								std::cout << "回收threadid:" << std::this_thread::get_id() << std::endl;
								return;
							}
						}
					}
					else {
						// 等待notEmpty条件
						notEmpty_.wait(lock);
					}

				}

				--idleThreadSize_;

				clock.lock();
				std::cout << "tid:" << std::this_thread::get_id() << std::endl;
				std::cout << "成功获取任务" << std::endl;
				clock.unlock();

				// 取一个任务出来执行
				task = taskQue_.front();
				taskQue_.pop();
				--taskSize_;

				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				//取出一个任务，进行通知
				notFull_.notify_all();
			}
			//释放掉这个锁

			//当前线程负责执行这个任务
			if (task != nullptr) {
				task();
			}

			++idleThreadSize_;
			lastTime = std::chrono::high_resolution_clock().now(); // 更新线程调度的时间
		}

	}

	//检查pool的运行状态
	bool checkRunningState() const {
		return isPoolRunning_;
	}

private:
	//std::vector<std::unique_ptr<Thread>> threads_;	// 线程列表
	std::unordered_map<unsigned, std::unique_ptr<Thread>> threads_;
	size_t initThreadSize_;								// 初始线程数量
	std::atomic_int curThreadSize_;						// 当前线程数量
	std::atomic_int idleThreadSize_;					// 记录空闲线程的数量

	size_t taskQueMaxThreshHold_;	// 任务数量上限
	size_t threadSizeThreshHold_;	// 线程数量上限

	using Task = std::function<void()>;
	std::queue<Task> taskQue_;					// 任务队列
	std::atomic_uint taskSize_;					// 任务数量

	std::mutex taskQueMtx_;				// 保证任务队列的线程安全
	std::condition_variable notFull_;	// 任务队列不满
	std::condition_variable notEmpty_;	// 任务队列不空
	std::condition_variable exitCond_;	// 等待线程资源全部回收
	std::mutex cmtx_;					// 保证输出有序

	PoolMode poolMode_; // 当前工作模式

	std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态

};

#endif // !THREADPOOL_H
