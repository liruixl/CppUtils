#ifndef __THREAD_POOL_OLD__
#define __THREAD_POOL_OLD__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using std::cout;
using std::endl;

	class ThreadPool {
	public:
		using PoolSeconds = std::chrono::seconds;

		/** �̳߳ص�����
		 * core_threads: �����̸߳������̳߳�������ӵ�е��̸߳�������ʼ���ͻᴴ���õ��̣߳���פ���̳߳�
		 *
		 * max_threads: >=core_threads��������ĸ���̫���̳߳�ִ�в�����ʱ��
		 * �ڲ��ͻᴴ��������߳�����ִ�и���������ڲ��߳������ᳬ��max_threads
		 *
		 * max_task_size: �ڲ�����洢����������������ʱû��ʹ��
		 *
		 * time_out: Cache�̵߳ĳ�ʱʱ�䣬Cache�߳�ָ����max_threads-core_threads���߳�,
		 * ��time_outʱ����û��ִ�����񣬴��߳̾ͻᱻ�Զ�����
		 */
		struct ThreadPoolConfig {
			int core_threads;
			int max_threads;
			int max_task_size;
			PoolSeconds time_out;
		};

		/**
		 * �̵߳�״̬���еȴ������С�ֹͣ
		 */
		enum class ThreadState { kInit = 0, kWaiting = 1, kRunning = 2, kStop = 3 };

		/**
		 * �̵߳������ʶ����־���߳��Ǻ����̻߳���Cache�̣߳�Cache���ڲ�Ϊ��ִ�и���������ʱ����������
		 */
		enum class ThreadFlag { kInit = 0, kCore = 1, kCache = 2 };

		using ThreadPtr = std::shared_ptr<std::thread>;
		using ThreadId = std::atomic<int>;
		using ThreadStateAtomic = std::atomic<ThreadState>;
		using ThreadFlagAtomic = std::atomic<ThreadFlag>;

		/**
		 * �̳߳����̴߳��ڵĻ�����λ��ÿ���̶߳��и��Զ����ID�����߳������ʶ��״̬
		 */
		struct ThreadWrapper {
			ThreadPtr ptr;
			ThreadId id;
			ThreadFlagAtomic flag;
			ThreadStateAtomic state;

			ThreadWrapper() {
				ptr = nullptr;
				id = 0;
				state.store(ThreadState::kInit);
			}
		};
		using ThreadWrapperPtr = std::shared_ptr<ThreadWrapper>;
		using ThreadPoolLock = std::unique_lock<std::mutex>;

		ThreadPool(ThreadPoolConfig config) : config_(config) {
			this->total_function_num_.store(0);
			this->waiting_thread_num_.store(0);

			this->thread_id_.store(0);
			this->is_shutdown_.store(false);
			this->is_shutdown_now_.store(false);

			if (IsValidConfig(config_)) {
				is_available_.store(true);
			}
			else {
				is_available_.store(false);
			}
		}

		~ThreadPool() { 
			ShutDown(); 
		}

		bool Reset(ThreadPoolConfig config) {
			if (!IsValidConfig(config)) {
				return false;
			}
			if (config_.core_threads != config.core_threads) {
				return false;
			}
			config_ = config;
			return true;
		}

		// �����̳߳ع���
		bool Start() {
			if (!IsAvailable()) {
				return false;
			}
			int core_thread_num = config_.core_threads;
			cout << "Init thread num " << core_thread_num << endl;
			while (core_thread_num-- > 0) {
				AddThread(GetNextThreadId());
			}
			cout << "Init thread end" << endl;
			return true;
		}

		// ��ȡ���ڴ��ڵȴ�״̬���̵߳ĸ���
		int GetWaitingThreadSize() { return this->waiting_thread_num_.load(); }

		// ��ȡ�̳߳��е�ǰ�̵߳��ܸ���
		int GetTotalThreadSize() { return this->worker_threads_.size(); }

		// �����̳߳���ִ�к���
		template <typename F, typename... Args>
		auto Run(F &&f, Args &&... args) -> std::shared_ptr<std::future<std::result_of_t<F(Args...)>>> {
			if (this->is_shutdown_.load() || this->is_shutdown_now_.load() || !IsAvailable()) {
				return nullptr;
			}
			if (GetWaitingThreadSize() == 0 && GetTotalThreadSize() < config_.max_threads) {
				AddThread(GetNextThreadId(), ThreadFlag::kCache);
			}

			using return_type = std::result_of_t<F(Args...)>;
			auto task = std::make_shared<std::packaged_task<return_type()>>(
				std::bind(std::forward<F>(f), std::forward<Args>(args)...));
			total_function_num_++;

			std::future<return_type> res = task->get_future();
			{
				ThreadPoolLock lock(this->task_mutex_);
				this->tasks_.emplace([task]() { (*task)(); });
			}
			this->task_cv_.notify_one();
			return std::make_shared<std::future<std::result_of_t<F(Args...)>>>(std::move(res));
		}

		// ��ȡ��ǰ�̳߳��Ѿ�ִ�й��ĺ�������
		int GetRunnedFuncNum() { return total_function_num_.load(); }

		// �ص��̳߳أ��ڲ���û��ִ�е���������ִ��
		void ShutDown() {
			ShutDown(false);
			cout << "shutdown" << endl;
			for (auto t : worker_threads_)
			{
				t->ptr->join();
			}
		}

		// ִ�йص��̳߳أ��ڲ���û��ִ�е�����ֱ��ȡ����������ִ��
		void ShutDownNow() {
			ShutDown(true);
			cout << "shutdown now" << endl;
		}

		// ��ǰ�̳߳��Ƿ����
		bool IsAvailable() { return is_available_.load(); }

	private:
		void ShutDown(bool is_now) {
			if (is_available_.load()) {
				if (is_now) {
					this->is_shutdown_now_.store(true);
				}
				else {
					this->is_shutdown_.store(true);
				}
				this->task_cv_.notify_all();
				is_available_.store(false);
			}
		}

		void AddThread(int id) { AddThread(id, ThreadFlag::kCore); }

		void AddThread(int id, ThreadFlag thread_flag) {
			cout << "AddThread " << id << " flag " << static_cast<int>(thread_flag) << endl;
			ThreadWrapperPtr thread_ptr = std::make_shared<ThreadWrapper>();
			thread_ptr->id.store(id);
			thread_ptr->flag.store(thread_flag);
			auto func = [this, thread_ptr]() {
				for (;;) {
					std::function<void()> task;
					{
						ThreadPoolLock lock(this->task_mutex_);
						if (thread_ptr->state.load() == ThreadState::kStop) {
							break;
						}
						cout << "thread id " << thread_ptr->id.load() << " running start" << endl;
						thread_ptr->state.store(ThreadState::kWaiting);
						++this->waiting_thread_num_;
						bool is_timeout = false;
						if (thread_ptr->flag.load() == ThreadFlag::kCore) {
							this->task_cv_.wait(lock, [this, thread_ptr] {
								return (this->is_shutdown_ || this->is_shutdown_now_ || !this->tasks_.empty() ||
									thread_ptr->state.load() == ThreadState::kStop);
							});
						}
						else {
							this->task_cv_.wait_for(lock, this->config_.time_out, [this, thread_ptr] {
								return (this->is_shutdown_ || this->is_shutdown_now_ || !this->tasks_.empty() ||
									thread_ptr->state.load() == ThreadState::kStop);
							});
							is_timeout = !(this->is_shutdown_ || this->is_shutdown_now_ || !this->tasks_.empty() ||
								thread_ptr->state.load() == ThreadState::kStop);
						}
						--this->waiting_thread_num_;
						cout << "thread id " << thread_ptr->id.load() << " running wait end" << endl;

						if (is_timeout) {
							thread_ptr->state.store(ThreadState::kStop);
						}

						if (thread_ptr->state.load() == ThreadState::kStop) {
							cout << "thread id " << thread_ptr->id.load() << " state stop" << endl;
							break;
						}
						if (this->is_shutdown_ && this->tasks_.empty()) {
							cout << "thread id " << thread_ptr->id.load() << " shutdown" << endl;
							break;
						}
						if (this->is_shutdown_now_) {
							cout << "thread id " << thread_ptr->id.load() << " shutdown now" << endl;
							break;
						}
						thread_ptr->state.store(ThreadState::kRunning);
						task = std::move(this->tasks_.front());
						this->tasks_.pop();
					}
					task();
				}
				cout << "thread id " << thread_ptr->id.load() << " running end" << endl;
			};
			thread_ptr->ptr = std::make_shared<std::thread>(std::move(func));

			/*if (thread_ptr->ptr->joinable()) {
				thread_ptr->ptr->detach();
			}*/

			this->worker_threads_.emplace_back(std::move(thread_ptr));
		}

		void Resize(int thread_num) {
			if (thread_num < config_.core_threads) return;
			int old_thread_num = worker_threads_.size();
			cout << "old num " << old_thread_num << " resize " << thread_num << endl;
			if (thread_num > old_thread_num) {
				while (thread_num-- > old_thread_num) {
					AddThread(GetNextThreadId());
				}
			}
			else {
				int diff = old_thread_num - thread_num;
				auto iter = worker_threads_.begin();
				while (iter != worker_threads_.end()) {
					if (diff == 0) {
						break;
					}
					auto thread_ptr = *iter;
					if (thread_ptr->flag.load() == ThreadFlag::kCache &&
						thread_ptr->state.load() == ThreadState::kWaiting) {  // wait
						thread_ptr->state.store(ThreadState::kStop);          // stop;
						--diff;
						iter = worker_threads_.erase(iter);
					}
					else {
						++iter;
					}
				}
				this->task_cv_.notify_all();
			}
		}

		int GetNextThreadId() { return this->thread_id_++; }

		bool IsValidConfig(ThreadPoolConfig config) {
			if (config.core_threads < 1 || config.max_threads < config.core_threads || config.time_out.count() < 1) {
				return false;
			}
			return true;
		}

	private:
		ThreadPoolConfig config_;

		std::list<ThreadWrapperPtr> worker_threads_;

		std::queue<std::function<void()>> tasks_;
		std::mutex task_mutex_;
		std::condition_variable task_cv_;

		std::atomic<int> total_function_num_;
		std::atomic<int> waiting_thread_num_;
		std::atomic<int> thread_id_;

		std::atomic<bool> is_shutdown_now_;
		std::atomic<bool> is_shutdown_;
		std::atomic<bool> is_available_;
	};

#endif