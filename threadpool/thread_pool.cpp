
#include "thread_pool.h"

ThreadPool::ThreadPool(ThreadPoolConfig config)
{
	if (!isVaildConfig(config))
	{
		this->is_available.store(false);
		throw std::exception();
	}

	this->config = config;

	this->total_function_num.store(0);
	this->waiting_thread_num.store(0);

	this->thread_id.store(0);
	this->is_shutdown.store(false);
	this->is_shutdown_now.store(false);

	this->is_available.store(true); //在此情况下一定为true

	start(); //init
}

ThreadPool::~ThreadPool()
{
	shutdown();
}


bool ThreadPool::start()
{
	
	if (!isAvailable()) { return false; }

	int core_thread_num = config.core_threads;
	cout << "Init thread num " << core_thread_num << endl;

	for (int i = 0; i < core_thread_num; i++)
	{
		addThread(getNextThreadId());
	}

	cout << "Init thread pool end." << endl;

	return true;
}


void ThreadPool::addThread(int id, ThreadFlag flag)
{
	cout << "addThread " << id << " flag:" << toFlagString(flag)  <<endl;
	//init ThreadWrapperPtr
	ThreadWrapperPtr t = std::make_shared<ThreadWrapper>();
	t->id.store(id);
	t->flag.store(flag);

	auto worker_func = [this, t]() {
		while (true)
		{
			std::function<void()> task;

			{
				std::unique_lock<std::mutex> lock(this->tasks_mutex);

				if (t->state.load() == ThreadState::Stop)
				{
					break;
				}

				cout << "thread id " << t->id.load() << " running start" << endl;
				t->state.store(ThreadState::Waiting);

				//condition_variable
				++this->waiting_thread_num;
				bool is_timeout = false;

				if (t->flag.load() == ThreadFlag::Core)
				{
					this->tasks_cv.wait(lock, [this, t]() {
						return (this->is_shutdown || this->is_shutdown_now ||
							!this->tasks.empty() ||
							t->state.load() == ThreadState::Stop);
					});
				}
				else
				{
					this->tasks_cv.wait_for(lock, this->config.time_out, [this, t]() {
						return (this->is_shutdown || this->is_shutdown_now ||
							!this->tasks.empty() ||
							t->state.load() == ThreadState::Stop);
					});

					//right?
					is_timeout = !(this->is_shutdown || this->is_shutdown_now || 
						!this->tasks.empty() ||
						t->state.load() == ThreadState::Stop);
				}
				--this->waiting_thread_num;
				cout << "thread id " << t->id.load() << " running wait end" << endl;

				if (is_timeout)
				{
					//不从list里面移除线程吗
					t->state.store(ThreadState::Stop);
				}


				if (t->state.load() == ThreadState::Stop)
				{
					cout << "thread id " << t->id.load() << " state stop" << endl;
					break;
				}
				if (this->is_shutdown && this->tasks.empty()) {
					cout << "thread id " << t->id.load() << " shutdown" << endl;
					break;
				}
				if (this->is_shutdown_now) {
					cout << "thread id " << t->id.load() << " shutdown now" << endl;
					break;
				}

				t->state.store(ThreadState::Running);

				//pop
				task = std::move(this->tasks.front());
				this->tasks.pop();

			} //unlock

			task(); //run task
		}
	};

	t->thread_ptr = std::make_shared<std::thread>(worker_func); //std::move?

	if (t->thread_ptr->joinable())
	{
		t->thread_ptr->detach();
	}

	this->threads.emplace_back(std::move(t));
}

void ThreadPool::shutdown()
{
	shutdown(false);
	cout << "shutdown" << endl;
}

void ThreadPool::shutdownNow()
{
	shutdown(true);
	cout << "shutdown now" << endl;
}

void ThreadPool::shutdown(bool is_now)
{
	if (!is_available.load())
	{
		return;
	}

	if (is_now)
	{
		is_shutdown_now.store(true);
	}
	else
	{
		is_shutdown.store(true);
	}

	this->tasks_cv.notify_all();
	this->is_available.store(false);
}

bool ThreadPool::reset(ThreadPoolConfig config)
{
	if (!isVaildConfig(config)) {
		return false;
	}
	if (this->config.core_threads != config.core_threads) {
		return false;
	}

	this->config = config;
	return true;
}


void ThreadPool::resize(int thread_num)
{
	if (thread_num < config.core_threads) return;
	int old_thread_num = threads.size();
	cout << "old num " << old_thread_num << " resize " << thread_num << endl;
	if (thread_num > old_thread_num) {
		while (thread_num-- > old_thread_num) {
			addThread(getNextThreadId());
		}
	}
	else {
		int diff = old_thread_num - thread_num;
		auto iter = threads.begin();
		while (iter != threads.end()) {
			if (diff == 0) {
				break;
			}
			auto thread_ptr = *iter;
			if (thread_ptr->flag.load() == ThreadFlag::Cache &&
				thread_ptr->state.load() == ThreadState::Waiting) {  // wait
				thread_ptr->state.store(ThreadState::Stop);          // stop;
				--diff;
				iter = threads.erase(iter);
			}
			else {
				++iter;
			}
		}
		this->tasks_cv.notify_all();
	}
}

bool ThreadPool::isVaildConfig(const ThreadPoolConfig & config)
{
	if (config.core_threads < 1 || config.max_threads < config.core_threads || config.time_out.count() < 1) {
		return false;
	}
	return true;
}

std::string ThreadPool::toFlagString(ThreadFlag state)
{
	std::string res = "Unknow";

	switch (state)
	{
#define XX(name) \
	case ThreadPool::ThreadFlag::name:\
		res = #name;\
		break;

		XX(Init);
		XX(Core);
		XX(Cache);
#undef XX
	default:
		break;
	}

	return res;
}