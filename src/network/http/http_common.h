#pragma once
#include <string>
#include <map>
#include <functional>
#include <boost/algorithm/string/case_conv.hpp>

namespace gb{

// ---------------------------------------------------------------------------
// HttpMethod — HTTP 方法枚举（替代 boost::beast::http::verb）
// ---------------------------------------------------------------------------
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
    UNKNOWN,
};

inline HttpMethod ParseHttpMethod(const std::string& s)
{
    std::string upper = s;
    boost::algorithm::to_upper(upper);
    if (upper == "GET")     return HttpMethod::GET;
    if (upper == "POST")    return HttpMethod::POST;
    if (upper == "PUT")     return HttpMethod::PUT;
    if (upper == "DELETE")  return HttpMethod::DELETE;
    if (upper == "PATCH")   return HttpMethod::PATCH;
    if (upper == "HEAD")    return HttpMethod::HEAD;
    if (upper == "OPTIONS") return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
}

inline std::string ToHttpMethod(HttpMethod m)
{
    switch (m)
    {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        default:                  return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// HttpRequest — 解析后的HTTP请求，传递给注册的路由处理器
// ---------------------------------------------------------------------------
struct HttpRequest
{
    HttpMethod                    method = HttpMethod::UNKNOWN;
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
