#pragma once
#include <string>
#include <map>
#include <functional>
#include <boost/beast/http.hpp>

namespace gb{
// ---------------------------------------------------------------------------
// HttpRequest — 解析后的HTTP请求，传递给注册的路由处理器
// ---------------------------------------------------------------------------
struct HttpRequest
{
    boost::beast::http::verb method = boost::beast::http::verb::unknown;
    std::string              target;
    std::string              body;
    std::string              content_type;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params; // 从target解码的查询参数

    std::string GetHeader(const std::string& key) const
    {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : std::string();
    }

    std::string GetParam(const std::string& key) const
    {
        auto it = params.find(key);
        return it != params.end() ? it->second : std::string();
    }
};

// ---------------------------------------------------------------------------
// HttpResponse — 由处理器构造，由服务器序列化
// ---------------------------------------------------------------------------
struct HttpResponse
{
    int    status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;

    void SetHeader(const std::string& key, const std::string& value)
    {
        headers[key] = value;
    }

    void SetJsonBody(const std::string& json_body)
    {
        body         = json_body;
        content_type = "application/json";
    }

    void SetTextBody(const std::string& text_body,
                     const std::string& type = "text/plain")
    {
        body         = text_body;
        content_type = type;
    }
};

using HttpRequestHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

}
