/*********************************************************
*
*  Copyright (C) 2014 by Vitaliy Vitsentiy
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
*
*********************************************************/

/*********************************************************
* 线程池实现 (基于STL)
*
* 这个文件实现了一个完全基于C++标准库的线程池，不依赖第三方库。
* 与ctpl.h中基于Boost.Lockfree的实现相比，此版本使用标准库的互斥锁保护队列，
* 更加便携，但在高并发情况下可能性能略低。
*
* 主要功能和特点：
* 1. 提交任务到线程池：支持函数、函数对象、lambda表达式等多种可调用对象
* 2. 动态调整线程池大小：可以根据负载情况增加或减少工作线程数量
* 3. 等待所有任务完成：可以选择等待或立即停止
* 4. 线程安全：使用互斥锁和条件变量保证多线程环境下的安全性
* 5. 支持获取任务返回值：通过std::future机制
* 6. 异常处理：任务中的异常可以通过future传递给调用者
*
* 线程安全考量：
* - 使用互斥锁保护队列的所有操作，确保线程安全
* - 使用std::atomic<bool>标志控制线程的停止，保证线程安全的终止
* - 使用std::condition_variable实现线程同步和通知机制
* - 使用智能指针管理资源，避免内存泄漏
*
* 性能考量：
* - 使用条件变量使空闲线程进入等待状态，减少CPU资源消耗
* - 线程池避免了频繁创建和销毁线程的开销
* - 相比无锁队列实现，此版本在高并发情况下可能有更多的线程竞争
*********************************************************/

#ifndef __ctpl_stl_thread_pool_H__
#define __ctpl_stl_thread_pool_H__

#include <functional>  // 用于std::function和std::bind
#include <thread>      // 用于std::thread线程管理
#include <atomic>      // 用于std::atomic原子操作，保证线程安全
#include <vector>      // 用于存储线程和标志
#include <memory>      // 用于智能指针管理资源
#include <exception>   // 用于异常处理
#include <future>      // 用于std::future和std::packaged_task
#include <mutex>       // 用于互斥锁和条件变量
#include <queue>       // 用于标准库队列

/**
 * 线程池，用于运行用户的函数对象，函数签名为：
 *      ret func(int id, other_params)
 * 其中：
 * - id 是运行该函数的线程索引，可用于标识不同线程
 * - other_params 是用户传递的其他参数
 * - ret 是函数的返回类型，可以通过std::future获取
 */

namespace ctpl {

    namespace detail {
        /**
         * @brief 线程安全的队列实现
         *
         * @tparam T 队列中存储的元素类型
         *
         * 这是一个简单的线程安全队列实现，使用互斥锁保护所有操作。
         * 与Boost.Lockfree队列不同，此队列使用标准库的互斥锁实现线程安全，
         * 在高并发情况下可能会有更多的线程竞争和等待。
         *
         * 线程安全考量：
         * - 所有队列操作都使用互斥锁保护，确保线程安全
         * - 每个公共方法都获取锁，操作完成后自动释放锁
         * - 使用RAII风格的锁管理，确保异常安全
         */
        template <typename T>
        class Queue {
        public:
            /**
             * @brief 将元素推入队列
             *
             * @param value 要推入的元素
             * @return bool 操作是否成功，总是返回true
             *
             * 线程安全：使用互斥锁保护队列操作
             */
            bool push(T const & value) {
                std::unique_lock<std::mutex> lock(this->mutex);  // 获取互斥锁，保护队列操作
                this->q.push(value);  // 将元素添加到队列末尾
                return true;  // 操作总是成功
            }

            /**
             * @brief 从队列中弹出元素
             *
             * @param v 用于存储弹出元素的引用
             * @return bool 如果队列非空并成功弹出则返回true，否则返回false
             *
             * 线程安全：使用互斥锁保护队列操作
             */
            bool pop(T & v) {
                std::unique_lock<std::mutex> lock(this->mutex);  // 获取互斥锁，保护队列操作
                if (this->q.empty())  // 检查队列是否为空
                    return false;  // 队列为空，无法弹出元素
                v = this->q.front();  // 获取队列头部元素
                this->q.pop();  // 移除队列头部元素
                return true;  // 成功弹出元素
            }

            /**
             * @brief 检查队列是否为空
             *
             * @return bool 如果队列为空则返回true，否则返回false
             *
             * 线程安全：使用互斥锁保护队列操作
             */
            bool empty() {
                std::unique_lock<std::mutex> lock(this->mutex);  // 获取互斥锁，保护队列操作
                return this->q.empty();  // 返回队列是否为空
            }

        private:
            std::queue<T> q;  // 实际存储元素的标准库队列
            std::mutex mutex; // 用于保护队列操作的互斥锁
        };
    }

    /**
     * @brief 线程池类，管理一组工作线程
     *
     * 此类实现了一个完整的线程池，可以动态调整大小，提交任务，等待任务完成等。
     * 使用标准库组件实现，不依赖第三方库，具有良好的可移植性。
     */
    class thread_pool {

    public:

        /**
         * @brief 默认构造函数，初始化线程池
         *
         * 创建一个空的线程池，需要调用resize()方法来添加工作线程
         */
        thread_pool() { this->init(); }

        /**
         * @brief 构造函数，指定线程数量
         *
         * @param nThreads 线程池中的线程数量
         *
         * 创建指定数量线程的线程池，并立即启动这些线程等待任务
         */
        thread_pool(int nThreads) { this->init(); this->resize(nThreads); }

        /**
         * @brief 析构函数，等待所有任务完成并停止线程池
         *
         * 调用stop(true)确保所有已提交的任务都被执行完毕
         * 然后释放所有线程资源
         */
        ~thread_pool() {
            this->stop(true);
        }

        /**
         * @brief 获取线程池中线程的数量
         *
         * @return int 线程数量
         */
        int size() { return static_cast<int>(this->threads.size()); }

        /**
         * @brief 获取当前空闲（等待任务）的线程数量
         *
         * @return int 空闲线程数量
         *
         * 线程安全：此方法返回原子变量，可以安全地从多个线程调用
         */
        int n_idle() { return this->nWaiting; }

        /**
         * @brief 获取指定索引的线程引用
         *
         * @param i 线程索引
         * @return std::thread& 线程引用
         *
         * 注意：调用者必须确保索引有效，否则会导致未定义行为
         */
        std::thread & get_thread(int i) { return *this->threads[i]; }

        /**
         * @brief 动态调整线程池大小
         *
         * @param nThreads 新的线程数量，必须 >= 0
         *
         * 此方法可以在运行时增加或减少线程池中的线程数量：
         * 1. 如果 nThreads > 当前线程数，则创建新线程
         * 2. 如果 nThreads < 当前线程数，则停止多余的线程
         *
         * 线程安全考量：
         * - 应该从单个线程调用，否则需要小心避免与其他resize()或stop()调用交错
         * - 增加线程是安全的，可以并发执行
         * - 减少线程时使用了线程分离(detach)而非终止(terminate)，确保线程可以安全完成当前任务
         * - 使用原子标志通知线程停止，避免了强制终止可能导致的资源泄漏
         */
        void resize(int nThreads) {
            if (!this->isStop && !this->isDone) {  // 只有线程池未停止且未完成时才允许调整线程数
                int oldNThreads = static_cast<int>(this->threads.size());  // 当前线程数

                if (oldNThreads <= nThreads) {  // 增加线程数量
                    this->threads.resize(nThreads);  // 扩展线程容器
                    this->flags.resize(nThreads);    // 扩展标志容器

                    // 创建新线程
                    for (int i = oldNThreads; i < nThreads; ++i) {
                        this->flags[i] = std::make_shared<std::atomic<bool>>(false);  // 新线程的停止标志设为false
                        this->set_thread(i);  // 启动新线程，绑定工作函数
                    }
                }
                else {  // 减少线程数量
                    // 标记多余线程的停止标志，并分离这些线程
                    for (int i = oldNThreads - 1; i >= nThreads; --i) {
                        *this->flags[i] = true;  // 设置该线程的停止标志为true，通知其退出
                        this->threads[i]->detach();  // 分离线程，使其在后台运行，主线程不再等待它
                    }
                    {
                        // 唤醒所有可能在等待任务的分离线程，让它们检测到停止标志后退出
                        std::unique_lock<std::mutex> lock(this->mutex);
                        this->cv.notify_all();
                    }
                    // 缩小线程和标志容器，安全删除多余元素
                    this->threads.resize(nThreads);  // 只保留需要的线程对象
                    this->flags.resize(nThreads);    // 只保留需要的标志对象
                }
            }
        }

        /**
         * @brief 清空任务队列
         *
         * 从队列中移除所有待处理的任务并释放资源
         * 注意：此操作会丢弃所有未执行的任务
         *
         * 线程安全：
         * - 使用互斥锁保护的队列操作是线程安全的
         * - 但整体方法不是原子的，调用时应确保没有其他线程同时访问队列
         */
        void clear_queue() {
            std::function<void(int id)> * _f;
            while (this->q.pop(_f))  // 循环弹出队列中的所有任务
                delete _f;  // 释放任务对象的内存
        }

        /**
         * @brief 从任务队列中弹出一个任务
         *
         * @return std::function<void(int)> 弹出的任务，如果队列为空则返回空函数
         *
         * 此方法允许手动从队列中取出任务并执行，主要用于特殊情况下的任务处理
         *
         * 线程安全：
         * - 使用互斥锁保护的队列操作是线程安全的
         * - 使用智能指针确保即使发生异常也能正确释放资源
         */
        std::function<void(int)> pop() {
            std::function<void(int id)> * _f = nullptr;
            this->q.pop(_f);  // 从队列中弹出任务
            std::unique_ptr<std::function<void(int id)>> func(_f);  // 使用智能指针管理资源，确保任务在返回时被删除

            std::function<void(int)> f;
            if (_f)
                f = *_f;  // 如果成功弹出任务，则复制任务内容
            return f;  // 返回任务副本
        }

        /**
         * @brief 等待所有计算线程完成并停止所有线程
         *
         * @param isWait 是否等待队列中的所有任务完成，默认为false
         *
         * 此方法有两种工作模式：
         * 1. isWait=true: 等待模式 - 执行队列中的所有任务，然后停止线程池
         * 2. isWait=false: 立即停止模式 - 立即停止线程池，丢弃队列中未执行的任务
         *
         * 可以异步调用此方法，以便在等待时不阻塞调用线程
         *
         * 线程安全考量：
         * - 使用原子变量isStop和isDone标记线程池状态，确保状态变更对所有线程可见
         * - 使用条件变量通知所有等待的线程检查停止标志
         * - 使用join等待所有线程安全退出，确保资源正确释放
         * - 最后清理队列和容器，防止内存泄漏
         */
        void stop(bool isWait = false) {
            if (!isWait) {  // 不等待任务完成，直接停止
                if (this->isStop)
                    return;  // 已经停止则直接返回，避免重复操作
                this->isStop = true;  // 设置停止标志，所有线程看到此标志后会退出

                // 通知所有线程停止
                for (int i = 0, n = this->size(); i < n; ++i) {
                    *this->flags[i] = true;  // 设置每个线程的停止标志
                }
                this->clear_queue();  // 立即清空任务队列，未执行的任务会被丢弃
            }
            else {  // 等待所有任务完成后再停止
                if (this->isDone || this->isStop)
                    return;  // 已经完成或已停止则直接返回，避免重复操作
                this->isDone = true;  // 设置完成标志，通知线程完成所有任务后退出
            }
            {
                // 唤醒所有等待中的线程，让它们检测到停止/完成标志
                std::unique_lock<std::mutex> lock(this->mutex);
                this->cv.notify_all();  // 唤醒所有等待线程
            }

            // 等待所有线程执行完毕
            for (int i = 0; i < static_cast<int>(this->threads.size()); ++i) {
                if (this->threads[i]->joinable())
                    this->threads[i]->join();  // 等待线程结束，确保资源正确释放
            }

            // 如果线程池中没有线程但队列中还有任务，需要手动清理这些任务
            this->clear_queue();
            this->threads.clear();  // 清空线程容器
            this->flags.clear();    // 清空标志容器
        }

        /**
         * @brief 提交带参数的任务到线程池
         *
         * @tparam F 函数类型
         * @tparam Rest 参数类型包
         * @param f 函数对象（函数指针、函数对象、lambda表达式等）
         * @param rest 传递给函数的参数
         * @return std::future<decltype(f(0, rest...))> 用于获取任务结果的future对象
         *
         * 此方法允许提交带有额外参数的任务到线程池，并返回一个future对象用于获取结果
         *
         * 实现细节：
         * 1. 使用std::bind将函数和参数绑定，第一个参数为线程ID
         * 2. 使用std::packaged_task包装任务，以便获取返回值
         * 3. 使用std::function包装packaged_task，统一任务接口
         * 4. 将任务推入队列
         * 5. 通知一个等待的线程处理新任务
         *
         * 线程安全考量：
         * - 使用互斥锁保护的队列操作是线程安全的
         * - 使用智能指针管理packaged_task的生命周期，确保即使线程池销毁也能正确获取结果
         * - 使用完美转发(std::forward)保留参数的值类别，提高效率
         */
        template<typename F, typename... Rest>
        auto push(F && f, Rest&&... rest) ->std::future<decltype(f(0, rest...))> {
            // 1. 创建 packaged_task，将任务和参数绑定，返回值类型为 f(0, rest...)
            auto pck = std::make_shared<std::packaged_task<decltype(f(0, rest...))(int)>>(
                std::bind(std::forward<F>(f), std::placeholders::_1, std::forward<Rest>(rest)...)
            );

            // 2. 创建 function<void(int)>，包装 packaged_task，便于线程池统一调用
            auto _f = new std::function<void(int id)>([pck](int id) {
                (*pck)(id);  // 执行 packaged_task
            });

            // 3. 将任务指针推入队列
            this->q.push(_f);

            // 4. 唤醒一个等待中的线程来执行新任务
            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();

            // 5. 返回 future，用户可通过 get() 获取任务结果
            return pck->get_future();
        }

        /**
         * @brief 提交无额外参数的任务到线程池
         *
         * @tparam F 函数类型
         * @param f 函数对象（函数指针、函数对象、lambda表达式等）
         * @return std::future<decltype(f(0))> 用于获取任务结果的future对象
         *
         * 此方法是push的简化版本，用于提交只接受线程ID参数的任务
         *
         * 实现细节：
         * 1. 使用std::packaged_task包装任务，以便获取返回值
         * 2. 使用std::function包装packaged_task，统一任务接口
         * 3. 将任务推入队列
         * 4. 通知一个等待的线程处理新任务
         *
         * 线程安全考量：
         * - 与带参数版本相同，确保线程安全的任务提交
         * - 使用完美转发(std::forward)保留函数对象的值类别，提高效率
         *
         * 异常处理：
         * - 任务中抛出的异常会被packaged_task捕获，并通过future传递给调用者
         * - 调用者可以通过future.get()重新抛出异常
         */
        template<typename F>
        auto push(F && f) ->std::future<decltype(f(0))> {
            // 1. 创建 packaged_task，任务类型为 f(0)
            auto pck = std::make_shared<std::packaged_task<decltype(f(0))(int)>>(std::forward<F>(f));

            // 2. 创建 function<void(int)>，包装 packaged_task
            auto _f = new std::function<void(int id)>([pck](int id) {
                (*pck)(id);  // 执行 packaged_task
            });

            // 3. 推入队列
            this->q.push(_f);

            // 4. 唤醒一个等待线程
            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();

            // 5. 返回 future
            return pck->get_future();
        }


    private:

        /**
         * @brief 删除的拷贝和移动构造/赋值函数
         *
         * 线程池不支持拷贝和移动操作，因为这些操作可能导致线程资源管理混乱
         * 使用注释而非C++11的=delete语法是为了兼容旧编译器
         */
        thread_pool(const thread_pool &);// = delete;
        thread_pool(thread_pool &&);// = delete;
        thread_pool & operator=(const thread_pool &);// = delete;
        thread_pool & operator=(thread_pool &&);// = delete;

        /**
         * @brief 设置线程的工作函数
         *
         * @param i 线程索引
         *
         * 此方法为每个线程创建工作函数，并启动线程
         * 工作函数的主要逻辑：
         * 1. 不断从队列中获取任务并执行
         * 2. 队列为空时等待条件变量通知
         * 3. 收到停止信号时安全退出
         *
         * 线程安全考量：
         * - 使用共享指针复制线程停止标志，确保即使线程池被销毁，线程也能安全访问标志
         * - 使用智能指针管理任务对象，确保即使发生异常也能正确释放资源
         * - 使用条件变量和互斥锁实现线程等待和唤醒机制，减少CPU资源消耗
         * - 使用原子操作更新等待线程计数，确保计数的线程安全
         */
        void set_thread(int i) {
            // 复制线程停止标志的共享指针，确保线程可以安全访问标志，即使线程池被销毁
            std::shared_ptr<std::atomic<bool>> flag(this->flags[i]);

            // 定义线程的工作函数
            auto f = [this, i, flag]() {
                std::atomic<bool> & _flag = *flag;  // 线程停止标志的引用
                std::function<void(int id)> * _f;   // 任务指针
                bool isPop = this->q.pop(_f);       // 尝试从队列中弹出一个任务

                while (true) {
                    while (isPop) {  // 如果队列中有任务
                        // 使用智能指针管理任务对象，确保即使发生异常也能正确释放资源
                        std::unique_ptr<std::function<void(int id)>> func(_f);
                        (*_f)(i);  // 执行任务，传入线程索引

                        if (_flag)
                            return;  // 如果线程被标记为停止，则立即退出，即使队列不为空
                        else
                            isPop = this->q.pop(_f);  // 继续尝试获取下一个任务
                    }

                    // 队列为空，等待新任务或停止信号
                    std::unique_lock<std::mutex> lock(this->mutex);
                    ++this->nWaiting;  // 增加等待线程计数

                    // 等待条件变量通知，同时检查三个条件：有新任务、线程池完成标志、线程停止标志
                    this->cv.wait(lock, [this, &_f, &isPop, &_flag](){
                        isPop = this->q.pop(_f);  // 再次尝试获取任务
                        return isPop || this->isDone || _flag;  // 如果有任务或需要停止，则退出等待
                    });

                    --this->nWaiting;  // 减少等待线程计数

                    if (!isPop)  // 如果没有获取到任务，说明是因为停止信号而退出等待
                        return;  // 退出线程
                }
            };

            // 创建并启动线程，使用reset而非make_unique是为了兼容旧编译器
            this->threads[i].reset(new std::thread(f));
        }

        /**
         * @brief 初始化线程池状态
         *
         * 重置所有状态变量为初始值
         */
        void init() {
            this->nWaiting = 0;    // 初始化等待线程数为0
            this->isStop = false;  // 初始化停止标志为false
            this->isDone = false;  // 初始化完成标志为false
        }

        // 成员变量
        std::vector<std::unique_ptr<std::thread>> threads;  // 线程容器，使用智能指针管理线程生命周期
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;  // 线程停止标志，每个线程一个
        detail::Queue<std::function<void(int id)> *> q;  // 线程安全的任务队列
        std::atomic<bool> isDone;  // 是否完成标志，true表示需要完成所有任务后停止
        std::atomic<bool> isStop;  // 是否停止标志，true表示立即停止，不处理剩余任务
        std::atomic<int> nWaiting;  // 等待中的线程数量，用于监控线程池状态

        std::mutex mutex;  // 互斥锁，用于保护条件变量
        std::condition_variable cv;  // 条件变量，用于线程等待和通知
    };

}

#endif // __ctpl_stl_thread_pool_H__
