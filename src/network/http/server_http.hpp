#pragma once
// Ported from Simple-Web-Server (https://github.com/eidheim/Simple-Web-Server)
// MIT License - Copyright (c) 2014-2018 Ole Christian Eidheim
//
// 已适配新版 Asio（Boost.Asio 1.90 / standalone Asio 1.30）：
//   - asio::io_service            -> asio::io_context
//   - socket->get_io_service()    -> asio::query(get_executor(), execution::context)
//   - resolver::query 已移除      -> async_resolve(host, service, handler) 配合 results_type
//   - ssl::rfc2818_verification   -> ssl::host_name_verification

#include "utility.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

#ifdef USE_STANDALONE_ASIO
#include <asio.hpp>
#include <asio/steady_timer.hpp>
namespace gb::http {
  using error_code = std::error_code;
  using errc = std::errc;
  namespace make_error_code = std;
} // namespace gb::http
#else
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/query.hpp>
#include <boost/asio/execution/context.hpp>
namespace gb::http {
  namespace asio = boost::asio;
  using error_code = boost::system::error_code;
  namespace errc = boost::system::errc;
  namespace make_error_code = boost::system::errc;
} // namespace gb::http
#endif

// 2017 年末 TODO：移除以下检查，始终使用 std::regex
#include <regex>
namespace gb::http {
  namespace regex = std;
}

namespace gb::http {
  template <class socket_type>
  class Server;

  template <class socket_type>
  class ServerBase {
  protected:
    class Session;

  public:
    class Response : public std::enable_shared_from_this<Response>, public std::ostream {
      friend class ServerBase<socket_type>;
      friend class Server<socket_type>;

      asio::streambuf streambuf;

      std::shared_ptr<Session> session;
      long timeout_content;

      Response(std::shared_ptr<Session> session, long timeout_content) noexcept : std::ostream(&streambuf), session(std::move(session)), timeout_content(timeout_content) {}

      template <typename size_type>
      void write_header(const CaseInsensitiveMultimap &header, size_type size) {
        bool content_length_written = false;
        bool chunked_transfer_encoding = false;
        for(auto &field : header) {
          if(!content_length_written && case_insensitive_equal(field.first, "content-length"))
            content_length_written = true;
          else if(!chunked_transfer_encoding && case_insensitive_equal(field.first, "transfer-encoding") && case_insensitive_equal(field.second, "chunked"))
            chunked_transfer_encoding = true;

          *this << field.first << ": " << field.second << "\r\n";
        }
        if(!content_length_written && !chunked_transfer_encoding && !close_connection_after_response)
          *this << "Content-Length: " << size << "\r\n\r\n";
        else
          *this << "\r\n";
      }

    public:
      std::size_t size() noexcept {
        return streambuf.size();
      }

      /// 如果需要递归发送长消息的各个部分，请使用此函数
      void send(const std::function<void(const error_code &)> &callback = nullptr) noexcept {
        session->connection->set_timeout(timeout_content);
        auto self = this->shared_from_this(); // 保持 Response 实例在后续 async_write 期间存活
        asio::async_write(*session->connection->socket, streambuf, [self, callback](const error_code &ec, std::size_t /*bytes_transferred*/) {
          self->session->connection->cancel_timeout();
          auto lock = self->session->connection->handler_runner->continue_lock();
          if(!lock)
            return;
          if(callback)
            callback(ec);
        });
      }

      /// 使用 std::ostream::write 直接写入流缓冲区
      void write(const char_type *ptr, std::streamsize n) {
        std::ostream::write(ptr, n);
      }

      /// 便捷函数：写入状态行、可选的响应头字段及空内容
      void write(StatusCode status_code = StatusCode::success_ok, const CaseInsensitiveMultimap &header = CaseInsensitiveMultimap()) {
        *this << "HTTP/1.1 " << gb::http::status_code(status_code) << "\r\n";
        write_header(header, 0);
      }

      /// 便捷函数：写入状态行、响应头字段及内容
      void write(StatusCode status_code, const std::string &content, const CaseInsensitiveMultimap &header = CaseInsensitiveMultimap()) {
        *this << "HTTP/1.1 " << gb::http::status_code(status_code) << "\r\n";
        write_header(header, content.size());
        if(!content.empty())
          *this << content;
      }

      /// 便捷函数：写入状态行、响应头字段及内容
      void write(StatusCode status_code, std::istream &content, const CaseInsensitiveMultimap &header = CaseInsensitiveMultimap()) {
        *this << "HTTP/1.1 " << gb::http::status_code(status_code) << "\r\n";
        content.seekg(0, std::ios::end);
        auto size = content.tellg();
        content.seekg(0, std::ios::beg);
        write_header(header, size);
        if(size)
          *this << content.rdbuf();
      }

      /// 便捷函数：写入成功状态行、响应头字段及内容
      void write(const std::string &content, const CaseInsensitiveMultimap &header = CaseInsensitiveMultimap()) {
        write(StatusCode::success_ok, content, header);
      }

      /// 便捷函数：写入成功状态行、响应头字段及内容
      void write(std::istream &content, const CaseInsensitiveMultimap &header = CaseInsensitiveMultimap()) {
        write(StatusCode::success_ok, content, header);
      }

      /// 便捷函数：写入成功状态行及响应头字段
      void write(const CaseInsensitiveMultimap &header) {
        write(StatusCode::success_ok, std::string(), header);
      }

      /// 若为 true，则在响应发送完毕后强制服务器关闭连接。
      ///
      /// 实现未指定内容长度的 HTTP/1.0 服务器时很有用。
      bool close_connection_after_response = false;
    };

    class Content : public std::istream {
      friend class ServerBase<socket_type>;

    public:
      std::size_t size() noexcept {
        return streambuf.size();
      }
      /// 便捷函数，返回 std::string。流缓冲区将被消耗。
      std::string string() noexcept {
        try {
          std::string str;
          auto size = streambuf.size();
          str.resize(size);
          read(&str[0], static_cast<std::streamsize>(size));
          return str;
        }
        catch(...) {
          return std::string();
        }
      }

    private:
      asio::streambuf &streambuf;
      Content(asio::streambuf &streambuf) noexcept : std::istream(&streambuf), streambuf(streambuf) {}
    };

    class Request {
      friend class ServerBase<socket_type>;
      friend class Server<socket_type>;
      friend class Session;

      asio::streambuf streambuf;

      Request(std::size_t max_request_streambuf_size, std::shared_ptr<asio::ip::tcp::endpoint> remote_endpoint) noexcept
          : streambuf(max_request_streambuf_size), content(streambuf), remote_endpoint(std::move(remote_endpoint)) {}

    public:
      std::string method, path, query_string, http_version;

      Content content;

      CaseInsensitiveMultimap header;

      regex::smatch path_match;

      std::shared_ptr<asio::ip::tcp::endpoint> remote_endpoint;

      /// 请求头被完整读取的时间点。
      std::chrono::system_clock::time_point header_read_time;

      std::string remote_endpoint_address() noexcept {
        try {
          return remote_endpoint->address().to_string();
        }
        catch(...) {
          return std::string();
        }
      }

      unsigned short remote_endpoint_port() noexcept {
        return remote_endpoint->port();
      }

      /// 返回查询键及其百分号解码后的值。
      CaseInsensitiveMultimap parse_query_string() noexcept {
        return gb::http::QueryString::parse(query_string);
      }
    };

  protected:
    class Connection : public std::enable_shared_from_this<Connection> {
    public:
      template <typename... Args>
      Connection(std::shared_ptr<ScopeRunner> handler_runner, Args &&... args) noexcept : handler_runner(std::move(handler_runner)), socket(new socket_type(std::forward<Args>(args)...)) {}

      std::shared_ptr<ScopeRunner> handler_runner;

      std::unique_ptr<socket_type> socket; // Socket 必须用 unique_ptr，因为 asio::ssl::stream<asio::ip::tcp::socket> 不可移动
      std::mutex socket_close_mutex;

      std::unique_ptr<asio::steady_timer> timer;

      std::shared_ptr<asio::ip::tcp::endpoint> remote_endpoint;

      void close() noexcept {
        error_code ec;
        std::unique_lock<std::mutex> lock(socket_close_mutex); // 以下操作需要按顺序执行
        socket->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket->lowest_layer().close(ec);
      }

      void set_timeout(long seconds) noexcept {
        if(seconds == 0) {
          timer = nullptr;
          return;
        }

        auto &io_context = static_cast<asio::io_context &>(asio::query(socket->get_executor(), asio::execution::context));
        timer = std::unique_ptr<asio::steady_timer>(new asio::steady_timer(io_context));
        timer->expires_after(std::chrono::seconds(seconds));
        auto self = this->shared_from_this();
        timer->async_wait([self](const error_code &ec) {
          if(!ec)
            self->close();
        });
      }

      void cancel_timeout() noexcept {
        if(timer) {
          timer->cancel();
        }
      }
    };

    class Session {
    public:
      Session(std::size_t max_request_streambuf_size, std::shared_ptr<Connection> connection) noexcept : connection(std::move(connection)) {
        if(!this->connection->remote_endpoint) {
          error_code ec;
          this->connection->remote_endpoint = std::make_shared<asio::ip::tcp::endpoint>(this->connection->socket->lowest_layer().remote_endpoint(ec));
        }
        request = std::shared_ptr<Request>(new Request(max_request_streambuf_size, this->connection->remote_endpoint));
      }

      std::shared_ptr<Connection> connection;
      std::shared_ptr<Request> request;
    };

  public:
    class Config {
      friend class ServerBase<socket_type>;

      Config(unsigned short port) noexcept : port(port) {}

    public:
      /// 使用的端口号。HTTP 默认为 80，HTTPS 默认为 443。设为 0 表示获取自动分配的端口。
      unsigned short port;
    /// 若未设置 io_context，则指定调用 start() 时服务器使用的线程数。
    /// 默认为 1 个线程。
    std::size_t thread_pool_size = 1;
      /// 请求处理超时时间。默认 5 秒。
      long timeout_request = 5;
      /// 内容处理超时时间。默认 300 秒。
      long timeout_content = 300;
      /// 请求流缓冲区的最大大小。默认为架构最大值。
      /// 达到该限制将返回 message_size 错误码。
      std::size_t max_request_streambuf_size = std::numeric_limits<std::size_t>::max();
      /// IPv4 点分十进制地址或 IPv6 十六进制表示法地址。
      /// 若为空，则监听所有地址。
      std::string address;
      /// 设为 false 可避免绑定到已被占用的地址。默认为 true。
      bool reuse_address = true;
    };
    /// 在调用 start() 前设置。
    Config config;

  private:
    class regex_orderable : public regex::regex {
      std::string str;

    public:
      regex_orderable(const char *regex_cstr) : regex::regex(regex_cstr), str(regex_cstr) {}
      regex_orderable(std::string regex_str) : regex::regex(regex_str), str(std::move(regex_str)) {}
      bool operator<(const regex_orderable &rhs) const noexcept {
        return str < rhs.str;
      }
    };

  public:
    /// 警告：调用 start() 后不要添加或移除资源
    std::map<regex_orderable, std::map<std::string, std::function<void(std::shared_ptr<typename ServerBase<socket_type>::Response>, std::shared_ptr<typename ServerBase<socket_type>::Request>)>>> resource;

    std::map<std::string, std::function<void(std::shared_ptr<typename ServerBase<socket_type>::Response>, std::shared_ptr<typename ServerBase<socket_type>::Request>)>> default_resource;

    std::function<void(std::shared_ptr<typename ServerBase<socket_type>::Request>, const error_code &)> on_error;

    std::function<void(std::unique_ptr<socket_type> &, std::shared_ptr<typename ServerBase<socket_type>::Request>)> on_upgrade;

    /// 如果你有自己的 asio::io_context，在运行 start() 前把指针存到这里。
    std::shared_ptr<asio::io_context> io_context;

    /// 可选：外部提供的 io_context（非拥有，通常来自 IoServicePool 的 IoWorker）。
    /// 设置后 bind() 不再自建 io_context；accept_and_run() 只做 listen + accept 即返回，
    /// 不阻塞、不启动任何线程（io_context 由外部线程运行，生命周期与停止由外部负责）。
    asio::io_context *external_io_context = nullptr;

    /// 连接 io_context 选择器：把每个新连接分布到池中的不同 io_context
    /// （与 TCP Listener 的 IoServicePool::GetIoService() 轮询一致）。
    /// 为空时连接使用 acceptor 的 io_context。返回的 io_context 必须正被某个线程运行。
    std::function<asio::io_context &()> io_context_selector;

    /// 如果预先知道服务器端口，请改用 start()。
    /// 返回分配的端口。若未设置 io_context，则改为创建内部 io_context。
    /// 在 accept_and_run() 之前调用。
    unsigned short bind() {
      asio::ip::tcp::endpoint endpoint;
      if(config.address.size() > 0)
        endpoint = asio::ip::tcp::endpoint(asio::ip::make_address(config.address), config.port);
      else
        endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), config.port);

      if(!io_context) {
        if(external_io_context) {
          // 外部 io_context（不拥有）：空删除器别名，accept_and_run()/stop() 不会运行/停止它
          io_context = std::shared_ptr<asio::io_context>(external_io_context, [](asio::io_context *) {});
        }
        else {
          io_context = std::make_shared<asio::io_context>();
          internal_io_context = true;
        }
      }

      if(!acceptor)
        acceptor = std::unique_ptr<asio::ip::tcp::acceptor>(new asio::ip::tcp::acceptor(*io_context));
      acceptor->open(endpoint.protocol());
      acceptor->set_option(asio::socket_base::reuse_address(config.reuse_address));
      acceptor->bind(endpoint);

      after_bind();

      return acceptor->local_endpoint().port();
    }

    /// 如果预先知道服务器端口，请改用 start()。
    /// 接受请求；若调用 bind() 前未设置 io_context，则运行内部 io_context。
    /// 在 bind() 之后调用。
    void accept_and_run() {
      acceptor->listen();
      accept();

      if(internal_io_context) {
        if(io_context->stopped())
          io_context->restart();

        // 若 thread_pool_size>1，在 (thread_pool_size-1) 个线程中运行 io_context.run() 实现线程池
        threads.clear();
        for(std::size_t c = 1; c < config.thread_pool_size; c++) {
          threads.emplace_back([this]() {
            this->io_context->run();
          });
        }

        // 主线程
        if(config.thread_pool_size > 0)
          io_context->run();

        // 等待其余线程（如果有）结束
        for(auto &t : threads)
          t.join();
      }
    }

    /// 通过调用 bind() 和 accept_and_run() 启动服务器
    void start() {
      bind();
      accept_and_run();
    }

    /// 停止接受新请求，并关闭当前连接。
    void stop() noexcept {
      if(acceptor) {
        error_code ec;
        acceptor->close(ec);

        {
          std::unique_lock<std::mutex> lock(*connections_mutex);
          for(auto &connection : *connections)
            connection->close();
          connections->clear();
        }

        if(internal_io_context)
          io_context->stop();
      }
    }

    virtual ~ServerBase() noexcept {
      handler_runner->stop();
      stop();
    }

  protected:
    bool internal_io_context = false;

    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    std::vector<std::thread> threads;

    std::shared_ptr<std::unordered_set<Connection *>> connections;
    std::shared_ptr<std::mutex> connections_mutex;

    std::shared_ptr<ScopeRunner> handler_runner;

    ServerBase(unsigned short port) noexcept : config(port), connections(new std::unordered_set<Connection *>()), connections_mutex(new std::mutex()), handler_runner(new ScopeRunner()) {}

    virtual void after_bind() {}
    virtual void accept() = 0;

    /// 连接应使用的 io_context：有选择器则用选择器，否则用 acceptor 的 io_context。
    asio::io_context &connection_io_context() noexcept {
      if(io_context_selector)
        return io_context_selector();
      return *io_context;
    }

    template <typename... Args>
    std::shared_ptr<Connection> create_connection(Args &&... args) noexcept {
      auto connections = this->connections;
      auto connections_mutex = this->connections_mutex;
      auto connection = std::shared_ptr<Connection>(new Connection(handler_runner, std::forward<Args>(args)...), [connections, connections_mutex](Connection *connection) {
        {
          std::unique_lock<std::mutex> lock(*connections_mutex);
          auto it = connections->find(connection);
          if(it != connections->end())
            connections->erase(it);
        }
        delete connection;
      });
      {
        std::unique_lock<std::mutex> lock(*connections_mutex);
        connections->emplace(connection.get());
      }
      return connection;
    }

    void read(const std::shared_ptr<Session> &session) {
      session->connection->set_timeout(config.timeout_request);
      asio::async_read_until(*session->connection->socket, session->request->streambuf, "\r\n\r\n", [this, session](const error_code &ec, std::size_t bytes_transferred) {
        session->connection->cancel_timeout();
        auto lock = session->connection->handler_runner->continue_lock();
        if(!lock)
          return;
        session->request->header_read_time = std::chrono::system_clock::now();
        if((!ec || ec == asio::error::not_found) && session->request->streambuf.size() == session->request->streambuf.max_size()) {
          auto response = std::shared_ptr<Response>(new Response(session, this->config.timeout_content));
          response->write(StatusCode::client_error_payload_too_large);
          response->send();
          if(this->on_error)
            this->on_error(session->request, make_error_code::make_error_code(errc::message_size));
          return;
        }
        if(!ec) {
          // request->streambuf.size() 不一定等于 bytes_transferred，参见 Boost 文档：
          // "async_read_until 操作成功后，streambuf 可能包含分隔符之外的额外数据"
          // 因此解析请求头时直接从流中逐行提取。streambuf 中剩余的
          // 内容字节将在下面的 async_read 函数中继续读取（用于获取请求内容）。
          std::size_t num_additional_bytes = session->request->streambuf.size() - bytes_transferred;

          if(!RequestMessage::parse(session->request->content, session->request->method, session->request->path,
                                    session->request->query_string, session->request->http_version, session->request->header)) {
            if(this->on_error)
              this->on_error(session->request, make_error_code::make_error_code(errc::protocol_error));
            return;
          }

          // 如果有内容，则继续读取
          auto header_it = session->request->header.find("Content-Length");
          if(header_it != session->request->header.end()) {
            unsigned long long content_length = 0;
            try {
              content_length = stoull(header_it->second);
            }
            catch(const std::exception &) {
              if(this->on_error)
                this->on_error(session->request, make_error_code::make_error_code(errc::protocol_error));
              return;
            }
            if(content_length > num_additional_bytes) {
              session->connection->set_timeout(config.timeout_content);
              asio::async_read(*session->connection->socket, session->request->streambuf, asio::transfer_exactly(content_length - num_additional_bytes), [this, session](const error_code &ec, std::size_t /*bytes_transferred*/) {
                session->connection->cancel_timeout();
                auto lock = session->connection->handler_runner->continue_lock();
                if(!lock)
                  return;
                if(!ec) {
                  if(session->request->streambuf.size() == session->request->streambuf.max_size()) {
                    auto response = std::shared_ptr<Response>(new Response(session, this->config.timeout_content));
                    response->write(StatusCode::client_error_payload_too_large);
                    response->send();
                    if(this->on_error)
                      this->on_error(session->request, make_error_code::make_error_code(errc::message_size));
                    return;
                  }
                  this->find_resource(session);
                }
                else if(this->on_error)
                  this->on_error(session->request, ec);
              });
            }
            else
              this->find_resource(session);
          }
          else if((header_it = session->request->header.find("Transfer-Encoding")) != session->request->header.end() && header_it->second == "chunked") {
            auto chunks_streambuf = std::make_shared<asio::streambuf>(this->config.max_request_streambuf_size);
            this->read_chunked_transfer_encoded(session, chunks_streambuf);
          }
          else
            this->find_resource(session);
        }
        else if(this->on_error)
          this->on_error(session->request, ec);
      });
    }

    void read_chunked_transfer_encoded(const std::shared_ptr<Session> &session, const std::shared_ptr<asio::streambuf> &chunks_streambuf) {
      session->connection->set_timeout(config.timeout_content);
      asio::async_read_until(*session->connection->socket, session->request->streambuf, "\r\n", [this, session, chunks_streambuf](const error_code &ec, size_t bytes_transferred) {
        session->connection->cancel_timeout();
        auto lock = session->connection->handler_runner->continue_lock();
        if(!lock)
          return;
        if((!ec || ec == asio::error::not_found) && session->request->streambuf.size() == session->request->streambuf.max_size()) {
          auto response = std::shared_ptr<Response>(new Response(session, this->config.timeout_content));
          response->write(StatusCode::client_error_payload_too_large);
          response->send();
          if(this->on_error)
            this->on_error(session->request, make_error_code::make_error_code(errc::message_size));
          return;
        }
        if(!ec) {
          std::string line;
          getline(session->request->content, line);
          bytes_transferred -= line.size() + 1;
          line.pop_back();
          unsigned long length = 0;
          try {
            length = stoul(line, 0, 16);
          }
          catch(...) {
            if(this->on_error)
              this->on_error(session->request, make_error_code::make_error_code(errc::protocol_error));
            return;
          }

          auto num_additional_bytes = session->request->streambuf.size() - bytes_transferred;

          if((2 + length) > num_additional_bytes) {
            session->connection->set_timeout(config.timeout_content);
            asio::async_read(*session->connection->socket, session->request->streambuf, asio::transfer_exactly(2 + length - num_additional_bytes), [this, session, chunks_streambuf, length](const error_code &ec, size_t /*bytes_transferred*/) {
              session->connection->cancel_timeout();
              auto lock = session->connection->handler_runner->continue_lock();
              if(!lock)
                return;
              if(!ec) {
                if(session->request->streambuf.size() == session->request->streambuf.max_size()) {
                  auto response = std::shared_ptr<Response>(new Response(session, this->config.timeout_content));
                  response->write(StatusCode::client_error_payload_too_large);
                  response->send();
                  if(this->on_error)
                    this->on_error(session->request, make_error_code::make_error_code(errc::message_size));
                  return;
                }
                this->read_chunked_transfer_encoded_chunk(session, chunks_streambuf, length);
              }
              else if(this->on_error)
                this->on_error(session->request, ec);
            });
          }
          else
            this->read_chunked_transfer_encoded_chunk(session, chunks_streambuf, length);
        }
        else if(this->on_error)
          this->on_error(session->request, ec);
      });
    }

    void read_chunked_transfer_encoded_chunk(const std::shared_ptr<Session> &session, const std::shared_ptr<asio::streambuf> &chunks_streambuf, unsigned long length) {
      std::ostream tmp_stream(chunks_streambuf.get());
      if(length > 0) {
        std::unique_ptr<char[]> buffer(new char[length]);
        session->request->content.read(buffer.get(), static_cast<std::streamsize>(length));
        tmp_stream.write(buffer.get(), static_cast<std::streamsize>(length));
        if(chunks_streambuf->size() == chunks_streambuf->max_size()) {
          auto response = std::shared_ptr<Response>(new Response(session, this->config.timeout_content));
          response->write(StatusCode::client_error_payload_too_large);
          response->send();
          if(this->on_error)
            this->on_error(session->request, make_error_code::make_error_code(errc::message_size));
          return;
        }
      }

      // 移除 "\r\n"
      session->request->content.get();
      session->request->content.get();

      if(length > 0)
        read_chunked_transfer_encoded(session, chunks_streambuf);
      else {
        if(chunks_streambuf->size() > 0) {
          std::ostream ostream(&session->request->streambuf);
          ostream << chunks_streambuf.get();
        }
        this->find_resource(session);
      }
    }

    void find_resource(const std::shared_ptr<Session> &session) {
      // 升级连接
      if(on_upgrade) {
        auto it = session->request->header.find("Upgrade");
        if(it != session->request->header.end()) {
          // 从连接集合中移除该连接
          {
            std::unique_lock<std::mutex> lock(*connections_mutex);
            auto it = connections->find(session->connection.get());
            if(it != connections->end())
              connections->erase(it);
          }

          on_upgrade(session->connection->socket, session->request);
          return;
        }
      }
      // 查找路径与方法的匹配项，并调用 write
      for(auto &regex_method : resource) {
        auto it = regex_method.second.find(session->request->method);
        if(it != regex_method.second.end()) {
          regex::smatch sm_res;
          if(regex::regex_match(session->request->path, sm_res, regex_method.first)) {
            session->request->path_match = std::move(sm_res);
            write(session, it->second);
            return;
          }
        }
      }
      auto it = default_resource.find(session->request->method);
      if(it != default_resource.end())
        write(session, it->second);
    }

    void write(const std::shared_ptr<Session> &session,
               std::function<void(std::shared_ptr<typename ServerBase<socket_type>::Response>, std::shared_ptr<typename ServerBase<socket_type>::Request>)> &resource_function) {
      session->connection->set_timeout(config.timeout_content);
      auto response = std::shared_ptr<Response>(new Response(session, config.timeout_content), [this](Response *response_ptr) {
        auto response = std::shared_ptr<Response>(response_ptr);
        response->send([this, response](const error_code &ec) {
          if(!ec) {
            if(response->close_connection_after_response)
              return;

            auto range = response->session->request->header.equal_range("Connection");
            for(auto it = range.first; it != range.second; it++) {
              if(case_insensitive_equal(it->second, "close"))
                return;
              else if(case_insensitive_equal(it->second, "keep-alive")) {
                auto new_session = std::make_shared<Session>(this->config.max_request_streambuf_size, response->session->connection);
                this->read(new_session);
                return;
              }
            }
            if(response->session->request->http_version >= "1.1") {
              auto new_session = std::make_shared<Session>(this->config.max_request_streambuf_size, response->session->connection);
              this->read(new_session);
              return;
            }
          }
          else if(this->on_error)
            this->on_error(response->session->request, ec);
        });
      });

      try {
        resource_function(response, session->request);
      }
      catch(const std::exception &) {
        if(on_error)
          on_error(session->request, make_error_code::make_error_code(errc::operation_canceled));
        return;
      }
    }
  };

  template <class socket_type>
  class Server : public ServerBase<socket_type> {};

  using HTTP = asio::ip::tcp::socket;

  template <>
  class Server<HTTP> : public ServerBase<HTTP> {
  public:
    Server() noexcept : ServerBase<HTTP>::ServerBase(80) {}

  protected:
    void accept() override {
      auto connection = create_connection(connection_io_context());

      acceptor->async_accept(*connection->socket, [this, connection](const error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if(!lock)
          return;

        // 立即开始接受新连接（除非 io_context 已停止）
        if(ec != asio::error::operation_aborted)
          this->accept();

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if(!ec) {
          asio::ip::tcp::no_delay option(true);
          error_code ec;
          session->connection->socket->set_option(option, ec);

          this->read(session);
        }
        else if(this->on_error)
          this->on_error(session->request, ec);
      });
    }
  };
} // namespace gb::http

