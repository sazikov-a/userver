#include <storages/postgres/detail/pg_connection_wrapper.hpp>

#include <pg_config.h>
#include <pq_portal_funcs.h>
#include <pq_workaround.h>

#include <engine/task/cancel.hpp>
#include <logging/log.hpp>
#include <tracing/tags.hpp>
#include <utils/assert.hpp>
#include <utils/internal_tag.hpp>
#include <utils/strerror.hpp>

#include <storages/postgres/detail/pg_message_severity.hpp>
#include <storages/postgres/detail/tracing_tags.hpp>
#include <storages/postgres/dsn.hpp>
#include <storages/postgres/exceptions.hpp>
#include <storages/postgres/io/traits.hpp>
#include <storages/postgres/message.hpp>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_TRACE() LOG_TRACE() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_DEBUG() LOG_DEBUG() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_INFO() LOG_INFO() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_WARNING() LOG_WARNING() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_LIMITED_WARNING() LOG_LIMITED_WARNING() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_ERROR() LOG_ERROR() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG_LIMITED_ERROR() LOG_LIMITED_ERROR() << log_extra_
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): uses file/line info
#define PGCW_LOG(level) LOG(level) << log_extra_

namespace storages::postgres::detail {

namespace {

/// Size of error message buffer for PQcancel
/// 256 bytes is recommended in libpq documentation
/// https://www.postgresql.org/docs/12/static/libpq-cancel.html
constexpr int kErrBufferSize = 256;
// TODO move to config
constexpr bool kVerboseErrors = false;

const char* MsgForStatus(ConnStatusType status) {
  switch (status) {
    case CONNECTION_OK:
      return "PQstatus: Connection established";
    case CONNECTION_BAD:
      return "PQstatus: Failed to start a connection";
    case CONNECTION_STARTED:
      return "PQstatus: Waiting for connection to be made";
    case CONNECTION_MADE:
      return "PQstatus: Connection OK; waiting to send";
    case CONNECTION_AWAITING_RESPONSE:
      return "PQstatus: Waiting for a response from the server";
    case CONNECTION_AUTH_OK:
      return "PQstatus: Received authentication; waiting for backend start-up";
    case CONNECTION_SETENV:
      return "PQstatus: Negotiating environment settings";
    case CONNECTION_SSL_STARTUP:
      return "PQstatus: Negotiating SSL";
    case CONNECTION_NEEDED:
      return "PQstatus: Internal state: connect() needed";
    case CONNECTION_CHECK_WRITABLE:
      return "PQstatus: Checking connection to handle writes";
    case CONNECTION_CONSUME:
      return "PQstatus: Consuming remaining response messages on connection";
    case CONNECTION_GSS_STARTUP:
      return "PQstatus: Negotiating GSSAPI";
#if PG_VERSION_NUM >= 130000
    case CONNECTION_CHECK_TARGET:
      return "PQstatus: Checking for a proper target connection";
#endif
  }
}

void NoticeReceiver(void* conn_wrapper_ptr, PGresult const* pg_res) {
  if (!conn_wrapper_ptr || !pg_res) {
    return;
  }

  auto conn_wrapper = static_cast<PGConnectionWrapper*>(conn_wrapper_ptr);
  conn_wrapper->LogNotice(pg_res);
}

}  // namespace

PGConnectionWrapper::PGConnectionWrapper(engine::TaskProcessor& tp, uint32_t id,
                                         SizeGuard&& size_guard)
    : bg_task_processor_{tp},
      log_extra_{{tracing::kDatabaseType, tracing::kDatabasePostgresType},
                 {"pg_conn_id", id}},
      size_guard_{std::move(size_guard)},
      last_use_{std::chrono::steady_clock::now()},
      is_broken_{false} {
  // TODO add SSL initialization
}

PGConnectionWrapper::~PGConnectionWrapper() { Close().Detach(); }

template <typename ExceptionType>
void PGConnectionWrapper::CheckError(const std::string& cmd,
                                     int pg_dispatch_result) {
  static const std::string kCheckConnectionQuota =
      ". It may be useful to check the user's connection quota "
      "(https://nda.ya.ru/t/BqsBhgnS3bU6rV)";

  if (pg_dispatch_result == 0) {
    auto msg = PQerrorMessage(conn_);
    PGCW_LOG_WARNING() << "libpq " << cmd << " error: " << msg
                       << (std::is_base_of_v<ConnectionError, ExceptionType>
                               ? kCheckConnectionQuota
                               : "");
    throw ExceptionType(cmd + " execution error: " + msg);
  }
}

PGTransactionStatusType PGConnectionWrapper::GetTransactionStatus() const {
  return PQtransactionStatus(conn_);
}

ConnectionState PGConnectionWrapper::GetConnectionState() const {
  if (!conn_) {
    return ConnectionState::kOffline;
  }
  switch (GetTransactionStatus()) {
    case PQTRANS_IDLE:
      return ConnectionState::kIdle;
    case PQTRANS_ACTIVE:
      return ConnectionState::kTranActive;
    case PQTRANS_INTRANS:
      return ConnectionState::kTranIdle;
    case PQTRANS_INERROR:
      return ConnectionState::kTranError;
    case PQTRANS_UNKNOWN:
      return ConnectionState::kOffline;
  }
}

int PGConnectionWrapper::GetServerVersion() const {
  return PQserverVersion(conn_);
}

engine::Task PGConnectionWrapper::Close() {
  engine::io::Socket tmp_sock = std::exchange(socket_, {});
  PGconn* tmp_conn = std::exchange(conn_, nullptr);

  // NOLINTNEXTLINE(cppcoreguidelines-slicing)
  return engine::impl::CriticalAsync(
      bg_task_processor_,
      [tmp_conn, socket = std::move(tmp_sock), is_broken = is_broken_,
       sg = std::move(size_guard_)]() mutable {
        int fd = -1;
        if (socket) {
          fd = std::move(socket).Release();
        }

        // We must call `PQfinish` only after we do the `socket.Release()`.
        // Otherwise `PQfinish` closes the file descriptor fd but we keep
        // listening for that fd in the `socket` variable, and if `fd` number is
        // reused we may accidentally receive alien events.

        if (tmp_conn != nullptr) {
          if (is_broken) {
            int pq_fd = PQsocket(tmp_conn);
            if (fd != -1 && pq_fd != -1 && fd != pq_fd) {
              LOG_LIMITED_ERROR() << "fd from socket != fd from PQsocket ("
                                  << fd << " != " << pq_fd << ')';
            }
            if (pq_fd >= 0) {
              int res = shutdown(pq_fd, SHUT_RDWR);
              if (res < 0) {
                auto old_errno = errno;
                LOG_WARNING() << "error while shutdown() socket (" << old_errno
                              << "): " << ::utils::strerror(old_errno);
              }
            }
          }
          PQfinish(tmp_conn);
        }
      });
}

template <typename ExceptionType>
void PGConnectionWrapper::CloseWithError(ExceptionType&& ex) {
  PGCW_LOG_DEBUG() << "Closing connection because of failure: " << ex;
  Close().Wait();
  // NOLINTNEXTLINE(hicpp-exception-baseclass)
  throw std::forward<ExceptionType>(ex);
}

engine::Task PGConnectionWrapper::Cancel() {
  if (!conn_) {
    // NOLINTNEXTLINE(cppcoreguidelines-slicing)
    return engine::impl::Async(bg_task_processor_, [] {});
  }
  PGCW_LOG_DEBUG() << "Cancel current request";
  std::unique_ptr<PGcancel, decltype(&PQfreeCancel)> cancel{PQgetCancel(conn_),
                                                            &PQfreeCancel};
  // NOLINTNEXTLINE(cppcoreguidelines-slicing)
  return engine::impl::Async(
      bg_task_processor_, [this, cancel = std::move(cancel)] {
        std::array<char, kErrBufferSize> buffer{};
        if (!PQcancel(cancel.get(), buffer.data(), buffer.size())) {
          PGCW_LOG_LIMITED_WARNING() << "Failed to cancel current request";
          // TODO Throw exception or not?
        }
      });
}

void PGConnectionWrapper::AsyncConnect(const Dsn& dsn, Deadline deadline,
                                       ScopeTime& scope) {
  PGCW_LOG_DEBUG() << "Connecting to " << DsnCutPassword(dsn);

  auto options = OptionsFromDsn(dsn);
  log_extra_.Extend(tracing::kDatabaseInstance, std::move(options.dbname));
  log_extra_.Extend(tracing::kPeerAddress,
                    std::move(options.host) + ':' + options.port);

  scope.Reset(scopes::kLibpqConnect);
  StartAsyncConnect(dsn);
  scope.Reset(scopes::kLibpqWaitConnectFinish);
  WaitConnectionFinish(deadline, dsn);
  PGCW_LOG_DEBUG() << "Connected to " << DsnCutPassword(dsn);
}

void PGConnectionWrapper::StartAsyncConnect(const Dsn& dsn) {
  if (conn_) {
    PGCW_LOG_LIMITED_ERROR()
        << "Attempt to connect a connection that is already connected"
        << logging::LogExtra::Stacktrace();
    throw ConnectionFailed{dsn, "Already connected"};
  }

  conn_ = PQconnectStart(dsn.GetUnderlying().c_str());
  if (!conn_) {
    // The only reason the pointer cannot be null is that libpq failed
    // to allocate memory for the structure
    PGCW_LOG_LIMITED_ERROR() << "libpq failed to allocate a PGconn structure"
                             << logging::LogExtra::Stacktrace();
    throw ConnectionFailed{dsn, "Failed to allocate PGconn structure"};
  }

  const auto status = PQstatus(conn_);
  if (CONNECTION_BAD == status) {
    const std::string msg = MsgForStatus(status);
    PGCW_LOG_WARNING() << msg;
    CloseWithError(ConnectionFailed{dsn, msg});
  } else {
    PGCW_LOG_TRACE() << MsgForStatus(status);
  }

  RefreshSocket(dsn);

  // set this as early as possible to avoid dumping notices to stderr
  PQsetNoticeReceiver(conn_, &NoticeReceiver, this);

  if (kVerboseErrors) {
    PQsetErrorVerbosity(conn_, PQERRORS_VERBOSE);
  }
}

void PGConnectionWrapper::WaitConnectionFinish(Deadline deadline,
                                               const Dsn& dsn) {
  auto poll_res = PGRES_POLLING_WRITING;
  auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline.TimeLeft());
  while (poll_res != PGRES_POLLING_OK) {
    switch (poll_res) {
      case PGRES_POLLING_READING:
        if (!WaitSocketReadable(deadline)) {
          if (engine::current_task::ShouldCancel()) {
            throw ConnectionInterrupted(
                "Task cancelled while polling connection for reading");
          }
          PGCW_LOG_LIMITED_WARNING()
              << "Timeout while polling PostgreSQL connection "
                 "socket for reading, timeout was "
              << timeout.count() << "ms";
          throw ConnectionTimeoutError(
              "Timed out while polling connection for reading");
        }
        break;
      case PGRES_POLLING_WRITING:
        if (!WaitSocketWriteable(deadline)) {
          if (engine::current_task::ShouldCancel()) {
            throw ConnectionInterrupted(
                "Task cancelled while polling connection for writing");
          }
          PGCW_LOG_LIMITED_WARNING()
              << "Timeout while polling PostgreSQL connection "
                 "socket for writing, timeout was "
              << timeout.count() << "ms";
          throw ConnectionTimeoutError(
              "Timed out while polling connection for writing");
        }
        break;
      case PGRES_POLLING_ACTIVE:
        // This is an obsolete state, just ignore it
        break;
      case PGRES_POLLING_FAILED:
        PGCW_LOG_LIMITED_WARNING() << " libpq polling failed";
        CheckError<ConnectionError>("PQconnectPoll", 0);
        break;
      default:
        UASSERT(!"Unexpected enumeration value");
        break;
    }
    poll_res = PQconnectPoll(conn_);
    PGCW_LOG_TRACE() << MsgForStatus(PQstatus(conn_));

    // Libpq may reopen sockets during PQconnectPoll while trying different
    // security/encryption schemes (SSL, GSS etc.). We must keep track of the
    // current socket to avoid polling the wrong one in the future.
    RefreshSocket(dsn);
  }

  // fe-exec.c: Needs to be called only on a connected database connection.
  if (PQsetnonblocking(conn_, 1)) {
    PGCW_LOG_LIMITED_ERROR()
        << "libpq failed to set non-blocking connection mode";
    throw ConnectionFailed{dsn, "Failed to set non-blocking connection mode"};
  }
}

void PGConnectionWrapper::RefreshSocket(const Dsn& dsn) {
  const auto fd = PQsocket(conn_);
  if (fd < 0) {
    PGCW_LOG_LIMITED_ERROR() << "Invalid PostgreSQL socket " << fd;
    throw ConnectionFailed{dsn, "Invalid socket handle"};
  }
  if (fd == socket_.Fd()) return;

  if (socket_.IsValid()) {
    auto old_fd = std::move(socket_).Release();
    PGCW_LOG_DEBUG() << "Released abandoned PostgreSQL socket " << old_fd;
  }
  socket_ = engine::io::Socket(fd);
}

bool PGConnectionWrapper::WaitSocketReadable(Deadline deadline) {
  return socket_.WaitReadable(deadline);
}

bool PGConnectionWrapper::WaitSocketWriteable(Deadline deadline) {
  return socket_.WaitWriteable(deadline);
}

void PGConnectionWrapper::Flush(Deadline deadline) {
  while (const int flush_res = PQflush(conn_)) {
    if (flush_res < 0) {
      throw CommandError(PQerrorMessage(conn_));
    }
    if (!WaitSocketWriteable(deadline)) {
      if (engine::current_task::ShouldCancel()) {
        throw ConnectionInterrupted("Task cancelled while flushing connection");
      }
      PGCW_LOG_LIMITED_WARNING()
          << "Timeout while flushing PostgreSQL connection socket";
      throw ConnectionTimeoutError("Timed out while flushing connection");
    }
    UpdateLastUse();
  }
}

bool PGConnectionWrapper::TryConsumeInput(Deadline deadline) {
  while (PQXisBusy(conn_)) {
    if (!WaitSocketReadable(deadline)) {
      return false;
    }
    CheckError<CommandError>("PQconsumeInput", PQconsumeInput(conn_));
    UpdateLastUse();
  }
  return true;
}

void PGConnectionWrapper::ConsumeInput(Deadline deadline) {
  if (!TryConsumeInput(deadline)) {
    if (engine::current_task::ShouldCancel()) {
      throw ConnectionInterrupted("Task cancelled while consuming input");
    }
    PGCW_LOG_LIMITED_WARNING()
        << "Timeout while consuming input from PostgreSQL connection socket";
    throw ConnectionTimeoutError("Timed out while consuming input");
  }
}

ResultSet PGConnectionWrapper::WaitResult(Deadline deadline, ScopeTime& scope) {
  scope.Reset(scopes::kLibpqWaitResult);
  Flush(deadline);
  auto handle = MakeResultHandle(nullptr);
  ConsumeInput(deadline);
  while (auto pg_res = PQXgetResult(conn_)) {
    if (handle) {
      // TODO Decide about the severity of this situation
      PGCW_LOG_DEBUG()
          << "Query returned several result sets, a result set is discarded";
    }
    handle = MakeResultHandle(pg_res);
    ConsumeInput(deadline);
  }
  return MakeResult(std::move(handle));
}

void PGConnectionWrapper::DiscardInput(Deadline deadline) {
  Flush(deadline);
  auto handle = MakeResultHandle(nullptr);
  ConsumeInput(deadline);
  while (auto pg_res = PQXgetResult(conn_)) {
    handle = MakeResultHandle(pg_res);
    ConsumeInput(deadline);
  }
}

void PGConnectionWrapper::FillSpanTags(tracing::Span& span) const {
  span.AddTags(log_extra_, ::utils::InternalTag{});
}

ResultSet PGConnectionWrapper::MakeResult(ResultHandle&& handle) {
  if (!handle) {
    LOG_DEBUG() << "Empty result";
    return ResultSet{nullptr};
  }

  auto wrapper = std::make_shared<detail::ResultWrapper>(std::move(handle));
  auto status = wrapper->GetStatus();
  switch (status) {
    case PGRES_EMPTY_QUERY:
      throw LogicError{"Empty query"};
    case PGRES_COMMAND_OK:
      PGCW_LOG_TRACE()
          << "Successful completion of a command returning no data";
      break;
    case PGRES_TUPLES_OK:
      PGCW_LOG_TRACE() << "Successful completion of a command returning data";
      break;
    case PGRES_SINGLE_TUPLE:
      PGCW_LOG_LIMITED_ERROR()
          << "libpq was switched to SINGLE_ROW mode, this is not supported.";
      CloseWithError(NotImplemented{"Single row mode is not supported"});
    case PGRES_COPY_IN:
    case PGRES_COPY_OUT:
    case PGRES_COPY_BOTH:
      PGCW_LOG_LIMITED_ERROR()
          << "PostgreSQL COPY command invoked which is not implemented"
          << logging::LogExtra::Stacktrace();
      CloseWithError(NotImplemented{"Copy is not implemented"});
    case PGRES_BAD_RESPONSE:
      CloseWithError(ConnectionError{"Failed to parse server response"});
    case PGRES_NONFATAL_ERROR: {
      Message msg{wrapper};
      switch (msg.GetSeverity()) {
        case Message::Severity::kDebug:
          PGCW_LOG_DEBUG() << "Postgres " << msg.GetSeverityString()
                           << " message: " << msg.GetMessage()
                           << msg.GetLogExtra();
          break;
        case Message::Severity::kLog:
        case Message::Severity::kInfo:
        case Message::Severity::kNotice:
          PGCW_LOG_INFO() << "Postgres " << msg.GetSeverityString()
                          << " message: " << msg.GetMessage()
                          << msg.GetLogExtra();
          break;
        case Message::Severity::kWarning:
          PGCW_LOG_LIMITED_WARNING()
              << "Postgres " << msg.GetSeverityString()
              << " message: " << msg.GetMessage() << msg.GetLogExtra();
          break;
        case Message::Severity::kError:
        case Message::Severity::kFatal:
        case Message::Severity::kPanic:
          PGCW_LOG_LIMITED_WARNING()
              << "Postgres " << msg.GetSeverityString()
              << " message (marked as non-fatal): " << msg.GetMessage()
              << msg.GetLogExtra();
          break;
      }
      break;
    }
    case PGRES_FATAL_ERROR: {
      Message msg{wrapper};
      if (!IsWhitelistedState(msg.GetSqlState())) {
        PGCW_LOG_LIMITED_ERROR()
            << "Fatal error occured: " << msg.GetMessage() << msg.GetLogExtra();
      } else {
        PGCW_LOG_LIMITED_WARNING()
            << "Fatal error occured: " << msg.GetMessage() << msg.GetLogExtra();
      }
      LOG_DEBUG() << "Ready to throw";
      msg.ThrowException();
      break;
    }
  }
  LOG_DEBUG() << "Result checked";
  return ResultSet{wrapper};
}

void PGConnectionWrapper::SendQuery(const std::string& statement,
                                    ScopeTime& scope) {
  scope.Reset(scopes::kLibpqSendQueryParams);
  CheckError<CommandError>(
      "PQsendQueryParams `" + statement + "`",
      PQsendQueryParams(conn_, statement.c_str(), 0, nullptr, nullptr, nullptr,
                        nullptr, io::kPgBinaryDataFormat));
  UpdateLastUse();
}

void PGConnectionWrapper::SendQuery(const std::string& statement,
                                    const QueryParameters& params,
                                    ScopeTime& scope) {
  if (params.Empty()) {
    SendQuery(statement, scope);
    return;
  }
  scope.Reset(scopes::kLibpqSendQueryParams);
  CheckError<CommandError>(
      "PQsendQueryParams",
      PQsendQueryParams(conn_, statement.c_str(), params.Size(),
                        params.ParamTypesBuffer(), params.ParamBuffers(),
                        params.ParamLengthsBuffer(),
                        params.ParamFormatsBuffer(), io::kPgBinaryDataFormat));
  UpdateLastUse();
}

void PGConnectionWrapper::SendPrepare(const std::string& name,
                                      const std::string& statement,
                                      const QueryParameters& params,
                                      ScopeTime& scope) {
  scope.Reset(scopes::kLibpqSendPrepare);
  if (params.Empty()) {
    CheckError<CommandError>(
        "PQsendPrepare",
        PQsendPrepare(conn_, name.c_str(), statement.c_str(), 0, nullptr));
  } else {
    CheckError<CommandError>(
        "PQsendPrepare",
        PQsendPrepare(conn_, name.c_str(), statement.c_str(), params.Size(),
                      params.ParamTypesBuffer()));
  }
  UpdateLastUse();
}

void PGConnectionWrapper::SendDescribePrepared(const std::string& name,
                                               ScopeTime& scope) {
  scope.Reset(scopes::kLibpqSendDescribePrepared);
  CheckError<CommandError>("PQsendDescribePrepared",
                           PQsendDescribePrepared(conn_, name.c_str()));
  UpdateLastUse();
}

void PGConnectionWrapper::SendPreparedQuery(const std::string& name,
                                            const QueryParameters& params,
                                            ScopeTime& scope) {
  scope.Reset(scopes::kLibpqSendQueryPrepared);
  if (params.Empty()) {
    CheckError<CommandError>(
        "PQsendQueryPrepared",
        PQsendQueryPrepared(conn_, name.c_str(), 0, nullptr, nullptr, nullptr,
                            io::kPgBinaryDataFormat));
  } else {
    CheckError<CommandError>(
        "PQsendQueryPrepared",
        PQsendQueryPrepared(conn_, name.c_str(), params.Size(),
                            params.ParamBuffers(), params.ParamLengthsBuffer(),
                            params.ParamFormatsBuffer(),
                            io::kPgBinaryDataFormat));
  }
  UpdateLastUse();
}

void PGConnectionWrapper::SendPortalBind(const std::string& statement_name,
                                         const std::string& portal_name,
                                         const QueryParameters& params,
                                         ScopeTime& scope) {
  scope.Reset(scopes::kPqSendPortalBind);
  if (params.Empty()) {
    CheckError<CommandError>(
        "PQXSendPortalBind",
        PQXSendPortalBind(conn_, statement_name.c_str(), portal_name.c_str(), 0,
                          nullptr, nullptr, nullptr, io::kPgBinaryDataFormat));
  } else {
    CheckError<CommandError>(
        "PQXSendPortalBind",
        PQXSendPortalBind(
            conn_, statement_name.c_str(), portal_name.c_str(), params.Size(),
            params.ParamBuffers(), params.ParamLengthsBuffer(),
            params.ParamFormatsBuffer(), io::kPgBinaryDataFormat));
  }
  UpdateLastUse();
}

void PGConnectionWrapper::SendPortalExecute(const std::string& portal_name,
                                            std::uint32_t n_rows,
                                            ScopeTime& scope) {
  scope.Reset(scopes::kPqSendPortalExecute);
  CheckError<CommandError>(
      "PQXSendPortalExecute",
      PQXSendPortalExecute(conn_, portal_name.c_str(), n_rows));
  UpdateLastUse();
}

void PGConnectionWrapper::LogNotice(const PGresult* pg_res) const {
  const auto severity =
      Message::SeverityFromString(GetMachineReadableSeverity(pg_res));

  auto msg = PQresultErrorMessage(pg_res);
  if (msg) {
    ::logging::Level lvl = ::logging::Level::kInfo;
    if (severity >= Message::Severity::kError) {
      lvl = ::logging::Level::kError;
    } else if (severity == Message::Severity::kWarning) {
      lvl = ::logging::Level::kWarning;
    } else if (severity < Message::Severity::kInfo) {
      lvl = ::logging::Level::kDebug;
    }
    PGCW_LOG(lvl) << msg;
  }
}

void PGConnectionWrapper::UpdateLastUse() {
  last_use_ = std::chrono::steady_clock::now();
}

TimeoutDuration PGConnectionWrapper::GetIdleDuration() const {
  return std::chrono::duration_cast<TimeoutDuration>(
      std::chrono::steady_clock::now() - last_use_);
}

void PGConnectionWrapper::MarkAsBroken() { is_broken_ = true; }

}  // namespace storages::postgres::detail
