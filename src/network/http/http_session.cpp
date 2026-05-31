#include "http_server_internal.h"
#include "log/log.h"

#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/core/bind_handler.hpp>

namespace gb
{

HttpSession::HttpSession(HttpServer::Impl& owner, beast::tcp_stream&& stream)
    : owner_(owner), stream_(std::move(stream))
{
}

void HttpSession::Run()
{
    DoRead();
}

void HttpSession::DoRead()
{
    parser_.emplace();
    parser_->body_limit(owner_.max_body_size_);

    beast_http::async_read(stream_, buffer_, *parser_,
        beast::bind_front_handler(&HttpSession::OnRead, shared_from_this()));
}

void HttpSession::OnRead(beast::error_code ec, size_t /*bytes*/)
{
    if (ec == beast_http::error::end_of_stream)
        return DoClose();
    if (ec)
    {
        LOG_ERROR("HttpSession read error: {}", ec.message());
        return DoClose();
    }

    // 处理请求
    auto&        beast_req = parser_->get();
    HttpRequest  req       = ToHttpRequest(beast_req);
    HttpResponse http_res;

    auto path = GetPath(req.target);
    owner_.Dispatch(path, beast_req.method(), req, http_res);

    // 直接构建并发送响应（不需要序列化器）
    auto response = std::make_shared<beast_http::response<beast_http::string_body>>(
        ToBeastResponse(http_res, beast_req));

    bool keep_alive = response->keep_alive();
    auto self       = shared_from_this();
    beast_http::async_write(stream_, *response,
        [self, this, response, keep_alive](beast::error_code ec2, size_t) {
            if (ec2)
            {
                LOG_ERROR("HttpSession write error: {}", ec2.message());
                return DoClose();
            }

            if (keep_alive)
            {
                parser_.reset();
                buffer_.consume(buffer_.size());
                DoRead();
            }
            else
            {
                DoClose();
            }
        });
}

void HttpSession::DoClose()
{
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
}

} // namespace gb
