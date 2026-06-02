#pragma once
/// 测试报告工具 — 将测试结果写入文件 + 输出到控制台。
///
/// 文件格式 (log4/db_test_report.txt):
///   === 测试名称 ===
///   [PASS] | 标题 | 结果
///   [FAIL] | 标题 | 结果

#include <string>
#include <fstream>
#include <mutex>
#include <iostream>
#include <chrono>
#include <ctime>

class TestReporter
{
public:
    TestReporter(const std::string& filepath)
    {
        file_.open(filepath, std::ios::trunc);
        if (file_.is_open())
        {
            auto now = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());

            // Use localtime_s for safe thread-local conversion
            std::tm tm_buf;
#if defined(_WIN32)
            localtime_s(&tm_buf, &now);
#else
            localtime_r(&now, &tm_buf);
#endif
            char time_str[64];
            std::strftime(time_str, sizeof(time_str),
                          "%Y-%m-%d %H:%M:%S", &tm_buf);

            file_ << "################################################################################\n"
                  << "# DB/Redis 集成测试报告\n"
                  << "# 时间: " << time_str << "\n"
                  << "################################################################################\n";
            file_.flush();
        }
        else
        {
            std::cerr << "[WARN] 无法打开报告文件: " << filepath << std::endl;
        }
    }

    ~TestReporter()
    {
        if (total_ > 0)
        {
            file_ << "\n--- 总计 ---\n"
                  << "通过: " << passed_ << "\n"
                  << "失败: " << failed_ << "\n"
                  << "总计: " << total_ << "\n";
            file_.flush();
        }
    }

    void Section(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        file_ << "\n=== " << name << " ===\n";
        file_.flush();
        section_started_ = true;
    }

    /// 写入一条测试结果。
    /// @param title  测试标题（描述被测操作）
    /// @param result 测试结果（实际值、返回值等）
    /// @param passed 是否通过
    void Write(const std::string& title, const std::string& result, bool passed)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        file_ << (passed ? "[PASS]" : "[FAIL]")
              << " | " << title
              << " | " << result << "\n";
        file_.flush();
        ++total_;
        if (passed) ++passed_; else ++failed_;
    }

    int Passed() const { return passed_; }
    int Failed() const { return failed_; }
    int Total()  const { return total_; }

    /// 是否已写入过 Section（用于检查文件是否为空）
    bool HasSection() const { return section_started_; }

private:
    std::ofstream file_;
    std::mutex    mtx_;
    int           passed_          = 0;
    int           failed_          = 0;
    int           total_           = 0;
    bool          section_started_ = false;
};

// ── 全局报告实例（在 main.cpp 中定义） ──
extern TestReporter g_reporter;

// ── 便捷工具：控制台输出 + 文件写入 ──
inline void TestResult(const std::string& label, bool ok,
                        const std::string& detail = "")
{
    std::cout << (ok ? "[PASS]" : "[FAIL]") << " " << label;
    if (!detail.empty()) std::cout << " -- " << detail;
    std::cout << std::endl;
    g_reporter.Write(label, detail.empty() ? (ok ? "ok" : "FAILED") : detail, ok);
}

inline void TestSection(const std::string& name)
{
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << name << "\n";
    std::cout << std::string(60, '=') << "\n" << std::endl;
    g_reporter.Section(name);
}
