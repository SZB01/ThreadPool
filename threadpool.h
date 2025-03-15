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
const unsigned int THREAD_MAX_IDLE_TIME = 60; //��λseconds


//�̳߳�֧�ֵ�ģʽ
enum PoolMode {
	MODE_FIXED,
	MODE_CACHED,
};

//�߳�����
class Thread {
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(unsigned int)>;

	//�����߳�
	void start() {
		if (this == nullptr) {
			std::cout << "����!" << std::endl;
		}
		//����һ���߳���ִ���̺߳���
		std::thread t(func_, threadId_); //c++11 �̶߳���t���̺߳���func_
		t.detach();  //���÷����߳�
	}

	//�̹߳���
	Thread(ThreadFunc func)
		: func_(func)
		, threadId_(generateId_++)
	{
	}
	//�߳�����
	~Thread() = default;

	// ��ȡ�߳�ID
	unsigned int getId() const {
		return threadId_;
	}

private:
	ThreadFunc func_;
	static unsigned int generateId_;
	unsigned int threadId_; //�����߳�ID
};

unsigned int Thread::generateId_ = 0;

// �̳߳�����
class ThreadPool {
public:
	// ���캯��
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

	// ��������
	~ThreadPool() {
		isPoolRunning_ = false;
		// �ȴ��̳߳����е��̷߳���
		// ����״̬ һ����������һ��������ִ��������


		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&] {return threads_.size() == 0; });

	}

	// �����̳߳ع���ģʽ
	void setMode(PoolMode mode) {
		if (checkRunningState()) {
			return;
		}
		poolMode_ = mode;
	}

	// ����task�������������ֵ
	void setTaskQueMaxThreshHold(size_t threshhold) {
		if (checkRunningState()) {
			return;
		}
		taskQueMaxThreshHold_ = threshhold;
	}
	// ����cachedģʽ���߳�������ֵ 
	void setThreadMaxThreshHold(size_t threshhold) {
		if (checkRunningState()) {
			return;
		}
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshhold;
		}
		return;
	}

	// ���̳߳��ύ����
	// ʹ�ÿɱ����ģ�壬��submitTask���Խ������⺯�����������͵Ĳ���
	// pool.submitTask(sum1, 10, 20);

	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// ������񣬷����������
		using Rtype = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<Rtype()>>(
			std::bind(std::forward<Func>(func),std::forward<Args>(args)...));
		std::future<Rtype> result = task->get_future();

		// ��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// �߳�ͨ�� �ȴ���������п��� ������������1s�������ж��ύ����ʧ��,����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&] {return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			std::cerr << "task queue is full, submit task fail." << std::endl << std::endl;
			
			// �����ύʧ�ܣ�����һ��Ҫ�����͵�Ĭ�Ϲ���
			auto task = std::make_shared<std::packaged_task<Rtype()>>(
				[]()->Rtype {return Rtype(); }
			);
			(*task)();
			return task->get_future();
		}

		//����п��� ������ŵ����������
		//taskQue_.push(sp);
		taskQue_.emplace([task]() {
			(*task)();
			});
		++taskSize_;

		//��Ϊ�������񣬶��п϶����գ���notEmpty_��֪ͨ
		notEmpty_.notify_all();

		// cachedģʽ ������ȽϽ������ʺ�����С����ĳ���
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_)
		{
			std::cout << "create new thread..." << std::endl;
			//�������߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			unsigned int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			threads_[threadId]->start();

			++idleThreadSize_;  // �����̵߳�����
			++curThreadSize_;
		}


		return result;
	}



	//�����̳߳�
	void start(size_t initThreadSize = std::thread::hardware_concurrency()) {
		isPoolRunning_ = true;

		//��¼��ʼ�̸߳���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		//�����̶߳���
		for (int i = 0; i < initThreadSize_; ++i) {
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			unsigned int threadId = ptr->getId();
			//std::cout << threadId << std::endl;
			threads_.emplace(threadId, std::move(ptr));
		}

		//���������߳�
		for (unsigned int i = 0; i < initThreadSize_; ++i) {
			threads_[i]->start(); // ִ��һ���̺߳���
			++idleThreadSize_;  // ��¼��ʼ�����̵߳�����
		}
	}

	//���������̳߳�
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool* operator=(const ThreadPool&) = delete;

private:
	//�����̺߳���
	void threadFunc(unsigned int threadId) {
		auto lastTime = std::chrono::high_resolution_clock().now();
		while (true) {
			Task task;
			{
				//�Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::unique_lock<std::mutex> clock(cmtx_);
				std::cout << "tid:" << std::this_thread::get_id() << std::endl;
				std::cout << "���Ի�ȡ����..." << std::endl;
				clock.unlock();

				// cachedģʽ�£��п��ܴ����˺ܶ��̣߳�����ʱ�䳬��60s
				// �ͽ������߳�(���ճ���initThreadSize_�������߳�)
				while (taskQue_.size() == 0)
				{
					if (!isPoolRunning_) {
						threads_.erase(threadId);
						exitCond_.notify_all();
						std::cout << "����threadid:" << std::this_thread::get_id() << std::endl;
						return;
					}
					// ÿһ���з���һ�� 
					// ���ֳ�ʱ���ػ����������ִ�з���
					if (poolMode_ == PoolMode::MODE_CACHED) {
						// ����������ʱ����
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_) {
								// ��ʼ���յ�ǰ�߳�
								// ��¼�߳���������ر�����ֵ�޸�

								// ���̶߳�����߳��б�������ɾ�� 
								threads_.erase(threadId);
								--curThreadSize_;
								--idleThreadSize_;

								std::cout << "����threadid:" << std::this_thread::get_id() << std::endl;
								return;
							}
						}
					}
					else {
						// �ȴ�notEmpty����
						notEmpty_.wait(lock);
					}

				}

				--idleThreadSize_;

				clock.lock();
				std::cout << "tid:" << std::this_thread::get_id() << std::endl;
				std::cout << "�ɹ���ȡ����" << std::endl;
				clock.unlock();

				// ȡһ���������ִ��
				task = taskQue_.front();
				taskQue_.pop();
				--taskSize_;

				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				//ȡ��һ�����񣬽���֪ͨ
				notFull_.notify_all();
			}
			//�ͷŵ������

			//��ǰ�̸߳���ִ���������
			if (task != nullptr) {
				task();
			}

			++idleThreadSize_;
			lastTime = std::chrono::high_resolution_clock().now(); // �����̵߳��ȵ�ʱ��
		}

	}

	//���pool������״̬
	bool checkRunningState() const {
		return isPoolRunning_;
	}

private:
	//std::vector<std::unique_ptr<Thread>> threads_;	// �߳��б�
	std::unordered_map<unsigned, std::unique_ptr<Thread>> threads_;
	size_t initThreadSize_;								// ��ʼ�߳�����
	std::atomic_int curThreadSize_;						// ��ǰ�߳�����
	std::atomic_int idleThreadSize_;					// ��¼�����̵߳�����

	size_t taskQueMaxThreshHold_;	// ������������
	size_t threadSizeThreshHold_;	// �߳���������

	using Task = std::function<void()>;
	std::queue<Task> taskQue_;					// �������
	std::atomic_uint taskSize_;					// ��������

	std::mutex taskQueMtx_;				// ��֤������е��̰߳�ȫ
	std::condition_variable notFull_;	// ������в���
	std::condition_variable notEmpty_;	// ������в���
	std::condition_variable exitCond_;	// �ȴ��߳���Դȫ������
	std::mutex cmtx_;					// ��֤�������

	PoolMode poolMode_; // ��ǰ����ģʽ

	std::atomic_bool isPoolRunning_; // ��ʾ��ǰ�̳߳ص�����״̬

};

#endif // !THREADPOOL_H
