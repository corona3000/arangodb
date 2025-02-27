////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "H1Connection.h"

#include <fuerte/helper.h>
#include <fuerte/loop.h>
#include <fuerte/types.h>
#include <velocypack/Parser.h>

#include <atomic>
#include <cassert>

#include "debugging.h"

namespace arangodb { namespace fuerte { inline namespace v1 { namespace http {

namespace fu = ::arangodb::fuerte::v1;
using namespace arangodb::fuerte::detail;
using namespace arangodb::fuerte::v1;
using namespace arangodb::fuerte::v1::http;

template <SocketType ST>
int H1Connection<ST>::on_message_begin(http_parser* p) {
  H1Connection<ST>* self = static_cast<H1Connection<ST>*>(p->data);
  self->_lastHeaderField.clear();
  self->_lastHeaderValue.clear();
  self->_lastHeaderWasValue = false;
  self->_shouldKeepAlive = false;
  self->_messageComplete = false;
  self->_response.reset(new Response());
  return 0;
}

template <SocketType ST>
int H1Connection<ST>::on_status(http_parser* parser, const char* at,
                                size_t len) {
  H1Connection<ST>* self = static_cast<H1Connection<ST>*>(parser->data);
  // required for some arango shenanigans
  self->_response->header.addMeta(std::string("http/") +
                                      std::to_string(parser->http_major) + '.' +
                                      std::to_string(parser->http_minor),
                                  std::string(at, len));
  return 0;
}

template <SocketType ST>
int H1Connection<ST>::on_header_field(http_parser* parser, const char* at,
                                      size_t len) {
  H1Connection<ST>* self = static_cast<H1Connection<ST>*>(parser->data);
  if (self->_lastHeaderWasValue) {
    toLowerInPlace(self->_lastHeaderField);  // in-place
    self->_response->header.addMeta(std::move(self->_lastHeaderField),
                                    std::move(self->_lastHeaderValue));
    self->_lastHeaderField.assign(at, len);
  } else {
    self->_lastHeaderField.append(at, len);
  }
  self->_lastHeaderWasValue = false;
  return 0;
}

template <SocketType ST>
int H1Connection<ST>::on_header_value(http_parser* parser, const char* at,
                                      size_t len) {
  H1Connection<ST>* self = static_cast<H1Connection<ST>*>(parser->data);
  if (self->_lastHeaderWasValue) {
    self->_lastHeaderValue.append(at, len);
  } else {
    self->_lastHeaderValue.assign(at, len);
  }
  self->_lastHeaderWasValue = true;
  return 0;
}

template <SocketType ST>
int H1Connection<ST>::on_header_complete(http_parser* parser) {
  H1Connection<ST>* self = static_cast<H1Connection<ST>*>(parser->data);
  self->_response->header.responseCode =
      static_cast<StatusCode>(parser->status_code);
  if (!self->_lastHeaderField.empty()) {
    toLowerInPlace(self->_lastHeaderField);  // in-place
    self->_response->header.addMeta(std::move(self->_lastHeaderField),
                                    std::move(self->_lastHeaderValue));
  }
  // Adjust idle timeout if necessary
  self->_shouldKeepAlive = http_should_keep_alive(parser);

  // head has no body, but may have a Content-Length
  if (self->_item->request->header.restVerb == RestVerb::Head) {
    return 1;  // tells the parser it should not expect a body
  } else if (parser->content_length > 0 &&
             parser->content_length < ULLONG_MAX) {
    uint64_t maxReserve = std::min<uint64_t>(2 << 24, parser->content_length);
    self->_responseBuffer.reserve(maxReserve);
  }

  return 0;
}

template <SocketType ST>
int H1Connection<ST>::on_body(http_parser* parser, const char* at, size_t len) {
  static_cast<H1Connection<ST>*>(parser->data)->_responseBuffer.append(at, len);
  return 0;
}

template <SocketType ST>
int H1Connection<ST>::on_message_complete(http_parser* parser) {
  static_cast<H1Connection<ST>*>(parser->data)->_messageComplete = true;
  return 0;
}

template <SocketType ST>
H1Connection<ST>::H1Connection(EventLoopService& loop,
                               ConnectionConfiguration const& config)
    : GeneralConnection<ST>(loop, config),
      _queue(),
      _active(false),
      _lastHeaderWasValue(false),
      _shouldKeepAlive(false),
      _messageComplete(false) {
  // initialize http parsing code
  http_parser_settings_init(&_parserSettings);
  _parserSettings.on_message_begin = &on_message_begin;
  _parserSettings.on_status = &on_status;
  _parserSettings.on_header_field = &on_header_field;
  _parserSettings.on_header_value = &on_header_value;
  _parserSettings.on_headers_complete = &on_header_complete;
  _parserSettings.on_body = &on_body;
  _parserSettings.on_message_complete = &on_message_complete;
  http_parser_init(&_parser, HTTP_RESPONSE);

  _parser.data = static_cast<void*>(this);

  // preemtively cache
  if (this->_config._authenticationType == AuthenticationType::Basic) {
    _authHeader.append("Authorization: Basic ");
    _authHeader.append(
        fu::encodeBase64(this->_config._user + ":" + this->_config._password, true));
    _authHeader.append("\r\n");
  } else if (this->_config._authenticationType == AuthenticationType::Jwt) {
    if (this->_config._jwtToken.empty()) {
      throw std::logic_error("JWT token is not set");
    }
    _authHeader.append("Authorization: bearer ");
    _authHeader.append(this->_config._jwtToken);
    _authHeader.append("\r\n");
  }

  FUERTE_LOG_TRACE << "creating http connection: this=" << this << "\n";
}

template <SocketType ST>
H1Connection<ST>::~H1Connection() try {
  drainQueue(Error::Canceled);
  abortOngoingRequests(Error::Canceled);
} catch (...) {
}

// Start an asynchronous request.
template <SocketType ST>
void H1Connection<ST>::sendRequest(std::unique_ptr<Request> req,
                                   RequestCallback cb) {
  // construct RequestItem
  auto item = std::make_unique<RequestItem>();
  // requestItem->_response later
  item->requestHeader = buildRequestBody(*req);
  item->callback = std::move(cb);
  item->request = std::move(req);

  // Prepare a new request
  this->_numQueued.fetch_add(1, std::memory_order_relaxed);
  if (!_queue.push(item.get())) {
    FUERTE_LOG_ERROR << "connection queue capacity exceeded\n";
    uint32_t q = this->_numQueued.fetch_sub(1, std::memory_order_relaxed);
    FUERTE_ASSERT(q > 0);
    item->invokeOnError(Error::QueueCapacityExceeded);
    return;
  }
  item.release();  // queue owns this now

  FUERTE_LOG_HTTPTRACE << "queued item: this=" << this << "\n";

  // _state.load() after queuing request, to prevent race with connect
  Connection::State state = this->_state.load();
  if (state == Connection::State::Connected) {
    startWriting();
  } else if (state == Connection::State::Disconnected) {
    FUERTE_LOG_HTTPTRACE << "sendRequest: not connected\n";
    this->start();  // <- thread-safe connection start
  } else if (state == Connection::State::Failed) {
    FUERTE_LOG_ERROR << "queued request on failed connection\n";
    drainQueue(fuerte::Error::ConnectionClosed);
  }
}

template <SocketType ST>
size_t H1Connection<ST>::requestsLeft() const {
  size_t q = this->_numQueued.load(std::memory_order_relaxed);
  if (this->_active.load(std::memory_order_relaxed)) {
    q++;
  }
  return q;
}

template <SocketType ST>
void H1Connection<ST>::finishConnect() {
  auto exp = Connection::State::Connecting;
  if (this->_state.compare_exchange_strong(exp, Connection::State::Connected)) {
    startWriting();  // starts writing queue if non-empty
  }
}

// Thread-Safe: activate the combined write-read loop
template <SocketType ST>
void H1Connection<ST>::startWriting() {
  FUERTE_LOG_HTTPTRACE << "startWriting: this=" << this << "\n";
  if (!_active) {
    this->_io_context->post([self(Connection::shared_from_this())] {
      auto& me = static_cast<H1Connection<ST>&>(*self);
      FUERTE_LOG_HTTPTRACE << "startWriting: active=true, this=" << &me << "\n";
      if (!me._active.exchange(true)) {  // we are the only ones here now
        // we might get in a race with shutdownConnection()
        Connection::State state = me._state.load();
        if (state != Connection::State::Connected) {
          me._active.store(false);
          me.startConnection();
        } else {
          me.asyncWriteNextRequest();
        }
      }
    });
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

template <SocketType ST>
std::string H1Connection<ST>::buildRequestBody(Request const& req) {
  // build the request header
  FUERTE_ASSERT(req.header.restVerb != RestVerb::Illegal);

  std::string header;
  header.reserve(256);  // TODO is there a meaningful size ?
  header.append(fu::to_string(req.header.restVerb));
  header.push_back(' ');

  http::appendPath(req, /*target*/header);

  header.append(" HTTP/1.1\r\n")
      .append("Host: ")
      .append(this->_config._host)
      .append("\r\n");
  if (this->_config._idleTimeout.count() >
      0) {  // technically not required for http 1.1
    header.append("Connection: Keep-Alive\r\n");
  } else {
    header.append("Connection: Close\r\n");
  }

  if (req.header.restVerb != RestVerb::Get &&
      req.contentType() != ContentType::Custom) {
    header.append("Content-Type: ")
        .append(to_string(req.contentType()))
        .append("\r\n");
  }
  if (req.acceptType() != ContentType::Custom) {
    header.append("Accept: ")
        .append(to_string(req.acceptType()))
        .append("\r\n");
  }

  bool haveAuth = false;
  for (auto const& pair : req.header.meta()) {
    if (pair.first == fu_content_length_key) {
      continue;  // skip content-length header
    }

    if (pair.first == fu_authorization_key) {
      haveAuth = true;
    }

    header.append(pair.first);
    header.append(": ");
    header.append(pair.second);
    header.append("\r\n");
  }

  if (!haveAuth && !_authHeader.empty()) {
    header.append(_authHeader);
  }

  if (req.header.restVerb != RestVerb::Get &&
      req.header.restVerb != RestVerb::Head) {
    header.append("Content-Length: ");
    header.append(std::to_string(req.payloadSize()));
    header.append("\r\n\r\n");
  } else {
    header.append("\r\n");
  }
  // body will be appended seperately
  return header;
}

// writes data from task queue to network using asio_ns::async_write
template <SocketType ST>
void H1Connection<ST>::asyncWriteNextRequest() {
  FUERTE_LOG_HTTPTRACE << "asyncWriteNextRequest: this=" << this << "\n";
  FUERTE_ASSERT(_active.load());
  FUERTE_ASSERT(_item == nullptr);

  RequestItem* ptr = nullptr;
  if (!_queue.pop(ptr)) {
    _active.store(false);
    if (_queue.empty()) {
      FUERTE_LOG_HTTPTRACE << "asyncWriteNextRequest: stopped writing, this="
                           << this << "\n";
      if (_shouldKeepAlive && this->_config._idleTimeout.count() > 0) {
        FUERTE_LOG_HTTPTRACE << "setting idle keep alive timer, this=" << this
                             << "\n";
        setTimeout(this->_config._idleTimeout);
      } else {
        this->shutdownConnection(Error::CloseRequested);
      }
      return;
    }
    if (_active.exchange(true)) {
      return; // someone else restarted
    }
    bool success = _queue.pop(ptr);
    FUERTE_ASSERT(success);
  }
  uint32_t q = this->_numQueued.fetch_sub(1, std::memory_order_relaxed);
  FUERTE_ASSERT(_item.get() == nullptr);
  FUERTE_ASSERT(ptr != nullptr);
  FUERTE_ASSERT(q > 0);
  
  _item.reset(ptr);
  setTimeout(_item->request->timeout());

  std::array<asio_ns::const_buffer, 2> buffers;
  buffers[0] =
      asio_ns::buffer(_item->requestHeader.data(), _item->requestHeader.size());
  // GET and HEAD have no payload
  if (_item->request->header.restVerb != RestVerb::Get &&
      _item->request->header.restVerb != RestVerb::Head) {
    buffers[1] = _item->request->payload();
  }

  asio_ns::async_write(
      this->_proto.socket, std::move(buffers),
      [self(Connection::shared_from_this())](
          asio_ns::error_code const& ec, std::size_t nwrite) {
        static_cast<H1Connection<ST>&>(*self).asyncWriteCallback(ec, nwrite);
      });
  FUERTE_LOG_HTTPTRACE << "asyncWriteNextRequest: done, this=" << this << "\n";
}

// called by the async_write handler (called from IO thread)
template <SocketType ST>
void H1Connection<ST>::asyncWriteCallback(asio_ns::error_code const& ec,
                                          size_t nwrite) {
  if (ec || _item == nullptr) {
    // Send failed
    FUERTE_LOG_DEBUG << "asyncWriteCallback (http): error '" << ec.message()
                     << "', this=" << this << "\n";
    auto item = std::move(_item);

    // keepalive timeout may have expired
    auto err = translateError(ec, Error::WriteError);
    if (item) { // may be null if connection was canceled
      if (ec == asio_ns::error::broken_pipe && nwrite == 0) {  // re-queue
        sendRequest(std::move(item->request), item->callback);
      } else {
        // let user know that this request caused the error
        item->callback(err, std::move(item->request), nullptr);
      }
    } else {
      err = Error::Canceled;
    }

    // Stop current connection and try to restart a new one.
    this->restartConnection(err);
    return;
  }
  FUERTE_ASSERT(_item != nullptr);

  // Send succeeded
  FUERTE_LOG_HTTPTRACE << "asyncWriteCallback: send succeeded "
                       << "this=" << this << "\n";

  // request is written we no longer need data for that
  _item->requestHeader.clear();

  setTimeout(_item->request->timeout());      // extend timeout

  this->asyncReadSome();  // listen for the response
}

// ------------------------------------
// Reading data
// ------------------------------------

// called by the async_read handler (called from IO thread)
template <SocketType ST>
void H1Connection<ST>::asyncReadCallback(asio_ns::error_code const& ec) {
  if (ec) {
    FUERTE_LOG_DEBUG << "asyncReadCallback: Error while reading from socket: '"
                     << ec.message() << "' , this=" << this << "\n";

    // Restart connection, will invoke _item cb
    this->restartConnection(translateError(ec, Error::ReadError));
    return;
  }
  FUERTE_ASSERT(_item != nullptr);
  
  // Inspect the data we've received so far.
  size_t nparsed = 0;
  auto buffers = this->_receiveBuffer.data();  // no copy
  for (auto const& buffer : buffers) {
    const char* data = reinterpret_cast<const char*>(buffer.data());
    size_t n = http_parser_execute(&_parser, &_parserSettings, data, buffer.size());
    if (n != buffer.size()) {
      /* Handle error. Usually just close the connection. */
      std::string msg = "Invalid HTTP response in parser: '";
      msg.append(http_errno_description(HTTP_PARSER_ERRNO(&_parser))).append("'");
      FUERTE_LOG_ERROR << msg << ", this=" << this << "\n";
      this->shutdownConnection(Error::ProtocolError, msg);  // will cleanup _item
      return;
    }
    nparsed += n;
  }

  // Remove consumed data from receive buffer.
  this->_receiveBuffer.consume(nparsed);
  
  if (_messageComplete) {
    this->_proto.timer.cancel();  // got response in time

    // thread-safe access on IO-Thread
    if (!_responseBuffer.empty()) {
      _response->setPayload(std::move(_responseBuffer), 0);
    }

    try {
      _item->callback(Error::NoError, std::move(_item->request),
                      std::move(_response));
    } catch (...) {
      FUERTE_LOG_ERROR << "unhandled exception in fuerte callback\n";
    }

    _item.reset();
    FUERTE_LOG_HTTPTRACE << "asyncReadCallback: completed parsing "
                            "response this="
                         << this << "\n";

    asyncWriteNextRequest();  // send next request
    return;
  }

  FUERTE_LOG_HTTPTRACE << "asyncReadCallback: response not complete yet\n";
  this->asyncReadSome();  // keep reading from socket
}

/// Set timeout accordingly
template <SocketType ST>
void H1Connection<ST>::setTimeout(std::chrono::milliseconds millis) {
  if (millis.count() == 0) {
    this->_proto.timer.cancel();
    return;
  }

  // expires_after cancels pending ops
  this->_proto.timer.expires_after(millis);
  this->_proto.timer.async_wait(
      [self = Connection::weak_from_this()](auto const& ec) {
        std::shared_ptr<Connection> s;
        if (ec || !(s = self.lock())) {  // was canceled / deallocated
          return;
        }
        auto* me = static_cast<H1Connection<ST>*>(s.get());

        FUERTE_LOG_DEBUG << "HTTP-Request timeout\n";
        if (me->_active) {
          me->restartConnection(Error::Timeout);
        } else {  // close an idle connection
          me->shutdownConnection(Error::CloseRequested);
        }
      });
}

/// abort ongoing / unfinished requests
template <SocketType ST>
void H1Connection<ST>::abortOngoingRequests(const fuerte::Error ec) {
  // simon: thread-safe, only called from IO-Thread
  // (which holds shared_ptr) and destructors
  if (_item) {
    // Item has failed, remove from message store
    _item->invokeOnError(ec);
    _item.reset();
  }
  _active.store(false);  // no IO operations running
}

/// abort all requests lingering in the queue
template <SocketType ST>
void H1Connection<ST>::drainQueue(const fuerte::Error ec) {
  RequestItem* item = nullptr;
  while (_queue.pop(item)) {
    FUERTE_ASSERT(item);
    std::unique_ptr<RequestItem> guard(item);
    uint32_t q = this->_numQueued.fetch_sub(1, std::memory_order_relaxed);
    FUERTE_ASSERT(q > 0);
    guard->invokeOnError(ec);
  }
}

template class arangodb::fuerte::v1::http::H1Connection<SocketType::Tcp>;
template class arangodb::fuerte::v1::http::H1Connection<SocketType::Ssl>;
#ifdef ASIO_HAS_LOCAL_SOCKETS
template class arangodb::fuerte::v1::http::H1Connection<SocketType::Unix>;
#endif

}}}}  // namespace arangodb::fuerte::v1::http
