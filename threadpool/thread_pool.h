
#ifndef _THREAD_POOL_
#define _THREAD_POOL_

#include <iostream>

#include <chrono>
#include <memory>
#include <atomic>         // std::atomic, std::atomic_flag, ATOMIC_FLAG_INIT

#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <future>

#include <string>
#include <list>
#include <queue>

//#include <tuple>
//#include <utility>



using std::cout;
using std::endl;

class ThreadPool
{
public:
	using PoolSeconds = std::chrono::seconds;

	struct ThreadPoolConfig
	{
		int core_threads;
		int max_threads;
		int max_task_size; //unused
		PoolSeconds time_out; //Cache thread time out
	};

	enum class ThreadState
	{
		Init = 0,
		Waiting,
		Running,
		Stop
	};

	enum class ThreadFlag
	{
		Init = 0,
		Core,
		Cache
	};

	std::string toFlagString(ThreadFlag state);

	using ThreadPtr = std::shared_ptr<std::thread>;
	struct ThreadWrapper
	{
		ThreadPtr thread_ptr;
		std::atomic<int> id;
		std::atomic<ThreadState> state;
		std::atomic<ThreadFlag> flag;

		ThreadWrapper()
		{
			thread_ptr = nullptr;
			id = 0;
			state = ThreadState::Init;
			flag = ThreadFlag::Init;
		}
	};

	using ThreadWrapperPtr = std::shared_ptr<ThreadWrapper>;


public:
	ThreadPool(ThreadPoolConfig config);
	~ThreadPool();

	template <typename F, typename... Args>
	auto addTask(F &&f, Args &&... args) -> std::shared_ptr<std::future<typename std::result_of<F(Args...)>::type>>
	{
		if (this->is_shutdown.load() || this->is_shutdown_now || !isAvailable())
		{
			throw std::runtime_error("enqueue on stopped or unavailed ThreadPool");
			//return nullptr;
		}
		if (getWaitingThreadSize() == 0 && getTatalThreadSize() < config.max_threads)
		{
			addThread(getNextThreadId(), ThreadFlag::Cache);
		}

		//using return_type = std::result_of_t(F(Args...));
		using return_type = std::result_of_t<F(Args...)>;

		
		auto task = std::make_shared<std::packaged_task<return_type()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...)
			);

		total_function_num++;

		std::future<return_type> res = task->get_future();

		{
			std::unique_lock<std::mutex> lock(tasks_mutex);
			tasks.emplace([task]() { (*task)(); });
		}
		
		this->tasks_cv.notify_one();
		return std::make_shared<std::future<return_type>>(std::move(res));
	}


	bool reset(ThreadPoolConfig config);
	void shutdown();
	void shutdownNow();

	int getWaitingThreadSize() { return this->waiting_thread_num.load(); }
	int getTatalThreadSize() { return this->threads.size(); }
	int getRunnedFuncNum() { return this->total_function_num.load(); }

	// 当前线程池是否可用,是否有必要，一旦构造初始化完毕即是可用的
	// 不可用的话，构造函数抛出异常
	bool isAvailable() { return this->is_available.load(); }

private:
	void addThread(int id, ThreadFlag flag = ThreadFlag::Core);

	bool start();
	void shutdown(bool is_now);
	void resize(int thread_num);

private:
	int getNextThreadId() { return this->thread_id++; }
	bool isVaildConfig(const ThreadPoolConfig& config);


private:
	ThreadPoolConfig config;

	std::list<ThreadWrapperPtr> threads;

	std::queue<std::function<void()>> tasks;
	std::mutex tasks_mutex;
	std::condition_variable tasks_cv;

	std::atomic<int> total_function_num;
	std::atomic<int> waiting_thread_num;
	std::atomic<int> thread_id;

	std::atomic<bool> is_shutdown_now;
	std::atomic<bool> is_shutdown;
	std::atomic<bool> is_available;

};


#endif // _THREAD_POOL_
