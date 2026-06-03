#pragma once

/// 线程安全的 Meyer's Singleton。
/// - C++11 起函数局部静态初始化是线程安全的。
/// - 不使用 inline static 成员，避免 MSVC selectany 问题。
/// - 实例在程序退出时自动析构（reverse order）。
template <typename T>
class Singleton
{
public:
    static T* Instance()
    {
        static T instance;
        return &instance;
    }

protected:
    Singleton() = default;
    virtual ~Singleton() = default;
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
};
