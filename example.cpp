#include <ctpl.h>      // 包含线程池头文件，使用基于Boost.Lockfree的实现
#include <iostream>    // 用于标准输入输出
#include <string>      // 用于字符串处理

/**
 * @brief 简单的测试函数 - 打印线程ID
 *
 * @param id 执行此函数的线程ID
 *
 * 这是最基本的线程池任务函数，只接受线程ID参数，
 * 用于测试线程池的基本功能。
 */
void first(int id) {
    std::cout << "hello from " << id << ", function\n";
}

/**
 * @brief 带整数参数的测试函数
 *
 * @param id 执行此函数的线程ID
 * @param par 用户传入的整数参数
 *
 * 此函数演示如何向线程池任务传递额外参数。
 */
void aga(int id, int par) {
    std::cout << "hello from " << id << ", function with parameter " << par <<'\n';
}

/**
 * @brief 测试用的类，用于演示对象在线程池中的生命周期
 *
 * 此类通过打印构造函数和析构函数的调用，
 * 帮助我们理解对象如何在线程池中传递和管理。
 */
struct Third {
    /**
     * @brief 构造函数
     * @param v 整数值
     */
    Third(int v) {
        this->v = v;
        std::cout << "Third ctor " << this->v << '\n';  // 打印构造信息
    }

    /**
     * @brief 移动构造函数
     * @param c 被移动的对象
     */
    Third(Third && c) {
        this->v = c.v;
        std::cout<<"Third move ctor\n";  // 打印移动构造信息
    }

    /**
     * @brief 拷贝构造函数
     * @param c 被拷贝的对象
     */
    Third(const Third & c) {
        this->v = c.v;
        std::cout<<"Third copy ctor\n";  // 打印拷贝构造信息
    }

    /**
     * @brief 析构函数
     */
    ~Third() {
        std::cout << "Third dtor\n";  // 打印析构信息
    }

    int v;  // 存储的整数值
};

/**
 * @brief 测试字符串参数传递
 *
 * @param id 执行此函数的线程ID
 * @param s 字符串参数，通过常量引用传递以避免拷贝
 *
 * 此函数演示如何向线程池任务传递字符串参数。
 */
void mmm(int id, const std::string & s) {
    std::cout << "mmm function " << id << ' ' << s << '\n';
}

/**
 * @brief 测试对象引用传递和延迟执行
 *
 * @param id 执行此函数的线程ID
 * @param t Third对象的引用
 *
 * 此函数演示：
 * 1. 如何向线程池任务传递对象引用
 * 2. 如何在任务中执行耗时操作（通过sleep模拟）
 *
 * 注意：传递引用时需要确保对象的生命周期长于任务执行时间，
 * 否则可能导致悬空引用问题。
 */
void ugu(int id, Third & t) {
    // 模拟耗时操作，睡眠2秒
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::cout << "hello from " << id << ", function with parameter Third " << t.v <<'\n';
}

/**
 * @brief 主函数，演示线程池的各种用法
 *
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return int 程序退出码
 *
 * 此函数全面展示了线程池的各种功能和用法，包括：
 * - 不同类型任务的提交
 * - 参数传递方式
 * - 对象生命周期管理
 * - 返回值获取
 * - 异常处理
 * - 线程池管理
 */
int main(int argc, char **argv) {
    // 创建一个包含2个线程的线程池
    // 线程池会立即启动这些线程并等待任务
    ctpl::thread_pool p(2);

    //==================================================================
    // 1. 函数指针任务提交测试
    //==================================================================

    // 使用std::ref传递函数引用，获取future用于同步
    std::future<void> qw = p.push(std::ref(first));

    // 直接传递函数指针，不关心返回值
    p.push(first);

    // 传递带参数的函数，第一个参数是函数指针，后面是额外参数
    p.push(aga, 7);

    //==================================================================
    // 2. 函数对象（仿函数）测试
    //==================================================================
    {
        // 定义一个函数对象类，用于测试仿函数在线程池中的使用
        struct Second {
            // 构造函数，接收字符串参数
            Second(const std::string & s) {
                std::cout << "Second ctor\n";
                this->s = s;
            }

            // 移动构造函数
            Second(Second && c) {
                std::cout << "Second move ctor\n";
                s = std::move(c.s);
            }

            // 拷贝构造函数
            Second(const Second & c) {
                this->s = c.s;
                std::cout << "Second copy ctor\n";
            }

            // 析构函数
            ~Second() {
                std::cout << "Second dtor\n";
            }

            // 函数调用运算符，使对象可以像函数一样被调用
            void operator()(int id) const {
                std::cout << "hello from " << id << ' ' << this->s << '\n';
            }

        private:
            std::string s;  // 存储的字符串
        } second(", functor");  // 创建一个函数对象实例

        // 传递仿函数的引用，避免拷贝
        // 注意：确保对象生命周期长于任务执行时间
        p.push(std::ref(second));

        // 等待2秒，确保上一个任务有时间执行
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        // 传递仿函数的常量引用，会触发拷贝构造
        p.push(const_cast<const Second &>(second));

        // 传递仿函数的移动对象，会触发移动构造
        p.push(std::move(second));

        // 再次传递second，此时second已被移动，是空壳
        p.push(second);

        // 直接传递临时对象，避免了命名对象的构造和析构
        p.push(Second(", functor"));
    }  // 作用域结束，second对象被析构

    //==================================================================
    // 3. 对象生命周期测试
    //==================================================================
    {
        // 创建一个Third对象
        Third t(100);

        // 传递对象引用，对象在主线程中，任务只使用引用
        // 注意：确保对象生命周期长于任务执行时间
        p.push(ugu, std::ref(t));

        // 传递对象的拷贝，会触发拷贝构造，任务使用对象的副本
        p.push(ugu, t);

        // 传递对象的移动版本，会触发移动构造，原对象变为空壳
        p.push(ugu, std::move(t));
    }  // 作用域结束，t对象被析构，但之前已创建的副本仍在线程池中使用

    // 直接传递临时对象，避免了命名对象的构造和析构
    p.push(ugu, Third(200));

    //==================================================================
    // 4. Lambda表达式测试
    //==================================================================
    std::string s = ", lambda";

    // 传递捕获变量的lambda表达式
    // lambda会捕获s的副本，即使s后续被修改也不影响
    p.push([s](int id){
        // 模拟耗时操作
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        std::cout << "hello from " << id << ' ' << s << '\n';
    });

    // 再次提交相同的lambda，会在另一个线程执行
    p.push([s](int id){
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        std::cout << "hello from " << id << ' ' << s << '\n';
    });

    //==================================================================
    // 5. 带参数的函数测试
    //==================================================================
    // 传递字符串字面量作为参数
    p.push(mmm, "worked");

    //==================================================================
    // 6. 弹出任务测试
    //==================================================================
    // 从队列中弹出一个任务（如果有的话）
    auto f = p.pop();
    if (f) {
        // 如果成功弹出任务，手动执行它
        // 传入0作为线程ID参数
        std::cout << "poped function from the pool ";
        f(0);
    }

    //==================================================================
    // 7. 动态调整线程池大小
    //==================================================================
    // 将线程池大小从2减少到1
    // 多余的线程会在完成当前任务后安全退出
    p.resize(1);

    //==================================================================
    // 8. 返回值测试
    //==================================================================
    std::string s2 = "result";

    // 提交一个返回字符串的lambda表达式
    auto f1 = p.push([s2](int){
        return s2;  // 返回捕获的字符串
    });

    // 通过future获取任务的返回值
    // get()会等待任务完成并返回结果
    std::cout << "returned " << f1.get() << '\n';

    //==================================================================
    // 9. 异常处理测试
    //==================================================================
    // 提交一个会抛出异常的lambda表达式
    auto f2 = p.push([](int){
        throw std::exception();  // 抛出标准异常
    });

    try {
        // 尝试获取结果，这会重新抛出任务中的异常
        f2.get();
    } catch (std::exception & e) {
        // 捕获并处理异常
        std::cout << "Exception caught: " << e.what() << '\n';
    }

    //==================================================================
    // 10. 获取线程引用测试
    //==================================================================
    // 获取线程池中特定线程的引用
    // 这可以用于设置线程属性等高级操作
    auto & th = p.get_thread(0);

    // 程序结束时，线程池析构函数会等待所有任务完成并释放资源
    return 0;
}
