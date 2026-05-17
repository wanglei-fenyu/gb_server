#pragma once

#include <stdexcept>

template <typename T>
class Singleton
{
public:
    static T* Instance()
    {
        if (m_pInstance == nullptr)
        {
            m_pInstance = new T();
        }
        return m_pInstance;
    }
    
    template <typename... Args>
    static T* Instance(Args&&... args)
    {
        if (m_pInstance == nullptr)
            m_pInstance = new T(std::forward<Args>(args)...);
        return m_pInstance;
    }

    static void DestroyInstance()
    {
        delete m_pInstance;
        m_pInstance = nullptr;
    }

protected:
	protected:
    Singleton() = default;
    virtual ~Singleton() = default;
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

private:
    inline static T* m_pInstance;
};
