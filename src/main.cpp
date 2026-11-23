// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

/**
 * @file main.cpp
 * @brief Application entry point — configuration, signal handling, worker
 *        pool, and the main event loop.
 *
 * This is the equivalent of running eibread-cgi and eibwrite-cgi as a
 * single unified binary, with these key differences:
 *   - Fork-based worker pool instead of running one process per request
 *   - Persistent bus connections instead of per-request connect/disconnect
 *   - A required listen socket (FCGI_SOCKET) that the workers share
 *   - Load shedding semaphores to prevent long-poll head-of-line blocking
 *   - Signal handling for graceful shutdown (SIGTERM)
 *   - Resource limits (RLIMIT_NOFILE, RLIMIT_AS) for embedded safety
 *
 * Startup order matters: the FCGI listen socket is opened BEFORE the bus
 * connection, so the web server can connect even while the bus is missing.
 *
 * Every piece of shared state (group cache, TX ring, session store) is
 * created here, in the parent, **before** the first fork() — that is what
 * makes it visible to every worker.
 *
 * @see AGENTS.md for the full design rationale.
 */

#include <semaphore.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "bus/bus_config.h"
#include "bus/bus_connector.h"
#include "bus/bus_factory.h"
#include "bus/bus_link.h"
#include "bus/cache_reader.h"
#include "bus/knx_codec.h"
#include "bus/shared_tx_bus_link.h"
#include "fcgi/fcgi_server.h"
#include "handlers/read_handler.h"
#include "knxnetip/discovery.h"
#include "knxnetip/knxnetip_frame.h"
#include "router/router.h"
#include "state/session_store.h"
#include "state/shared_group_cache.h"
#include "state/shared_tx_queue.h"
#include "util/coverage.h"
#include "util/debug_log.h"
#include "util/fork_safety.h"
#include "version.h"

namespace {

/// Maximum long-poll timeout in seconds (see kMaxClientWaitSec: no request may
/// wait longer, whatever the environment says).
inline constexpr int kMaxLongpollTimeoutSec = cvknx::kMaxClientWaitSec;

/// Maximum file descriptors per process. Prevents fd exhaustion on
/// long-running embedded systems where slow leaks accumulate over years.
inline constexpr rlim_t kMaxFileDescriptors = 256;

/// Maximum virtual memory per worker process (64 MB). Prevents a single
/// worker from exhausting system memory via a leak or unbounded growth.
inline constexpr rlim_t kMaxWorkerMemoryBytes = 64ULL * 1024 * 1024;

/// Accepted range for LONGPOLL_TIMEOUT_SEC / SESSION_TTL_SEC.
inline constexpr int kMaxSessionTtlSec = 7 * 24 * 60 * 60;

/// Workers that stay free for the short requests (`/w`, `/l`).
///
/// A long-poll may block a worker for up to kMaxClientWaitSec seconds, so the
/// long-poll semaphores are deliberately sized *below* the pool size: without
/// the reserve a handful of `/r` clients would occupy every worker and a
/// switch press would wait in the listen backlog until a poll returned.
inline constexpr int kReservedWorkers = 2;

// ---- Signal handling ----
// Signal handlers run in a restricted context (only async-signal-safe
// functions are permitted).  They set volatile flags which the main loop
// checks at well-defined points.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t g_shutdown_requested = 0;

extern "C" void handle_sigterm(int /*signum*/) {
  g_shutdown_requested = 1;
}

#if defined(KNX_COVERAGE)
/// Coverage builds only: flush the child's counters when the supervisor stops
/// it.  A child that dies from the default SIGTERM action would otherwise be
/// invisible to the coverage gate — everything it executed would look dead.
extern "C" void handle_coverage_sigterm(int /*signum*/) {
  KNX_COVERAGE_DUMP();
  std::_Exit(0);  // GCOV_EXCL_LINE - gcov records no counter after a noreturn call
}
#endif

/// Set resource limits for long-running embedded operation.
/// Called once at startup (before fork) so all children inherit the limits.
void set_resource_limits() {
  struct rlimit rl {};

  // Limit file descriptors to prevent exhaustion from slow leaks.
  rl.rlim_cur = kMaxFileDescriptors;
  rl.rlim_max = kMaxFileDescriptors;
  // GCOV_EXCL_START - setrlimit() only fails for an unsupported resource or a
  // value above the hard limit; neither can be injected from a test.
  if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) {
    std::cerr << "[WARN] Failed to set RLIMIT_NOFILE: " << std::strerror(errno) << "\n";
  }
  // GCOV_EXCL_STOP

  // Limit virtual memory per worker. Only the soft limit is set so
  // the process can still be profiled/debugged with a higher hard limit.
  rl.rlim_cur = kMaxWorkerMemoryBytes;
  rl.rlim_max = RLIM_INFINITY;
  // GCOV_EXCL_START - see above
  if (::setrlimit(RLIMIT_AS, &rl) != 0) {
    std::cerr << "[WARN] Failed to set RLIMIT_AS: " << std::strerror(errno) << "\n";
  }
  // GCOV_EXCL_STOP
}

/// Read an environment variable with a default value.
const char* get_env_default(const char* name, const char* default_value) {
  const char* val = getenv(name);
  return (val != nullptr && *val != '\0') ? val : default_value;
}

/// Parse an integer from an environment variable.
///
/// A value that is not a number or is below @p min_val is treated as a typo and
/// replaced by @p default_val (never by a silently different number — the same
/// rule BusConfig applies).  A value above @p max_val is capped at the maximum,
/// because "more than allowed" has a natural answer.  The comparison happens in
/// 64-bit arithmetic, so an absurd value cannot be truncated into a plausible
/// one before the range check.
int parse_env_int(const char* name, int default_val, int min_val, int max_val) {
  const char* val = getenv(name);
  if (val == nullptr || *val == '\0') {
    return default_val;
  }
  char* end = nullptr;
  errno = 0;
  const int64_t raw = std::strtol(val, &end, 10);
  if (end == val || *end != '\0' || errno == ERANGE || raw < min_val) {
    return default_val;
  }
  if (raw > max_val) {
    return max_val;
  }
  return static_cast<int>(raw);
}

/// Install the handlers that keep the daemon alive and stoppable.
void install_signal_handlers() {
  // Ignore SIGPIPE so that write() on a closed socket returns EPIPE instead
  // of killing the process. This is essential in the fork-based worker pool
  // where the bus daemon may close connections asynchronously.
  (void)::signal(SIGPIPE, SIG_IGN);

  // Handle SIGTERM for graceful shutdown. The parent forwards the signal to
  // all children, waits for them to exit, then exits itself.
  //
  // sigaction() with sa_flags = 0 (i.e. WITHOUT SA_RESTART) is required: with
  // the default BSD semantics of signal() the supervisor's wait() is restarted
  // transparently after the handler returns, so the shutdown flag is never
  // seen and the parent keeps waiting forever.
  struct sigaction shutdown_action {};
  shutdown_action.sa_handler = handle_sigterm;
  ::sigemptyset(&shutdown_action.sa_mask);
  shutdown_action.sa_flags = 0;
  // GCOV_EXCL_START - sigaction() with a valid handler cannot fail in practice
  if (::sigaction(SIGTERM, &shutdown_action, nullptr) != 0) {
    std::cerr << "[WARN] Failed to install SIGTERM handler: " << std::strerror(errno) << "\n";
  }
  // GCOV_EXCL_STOP

  // NOTE: no SIGCHLD handler.  The parent's supervisor loop calls wait() and
  // must see every exit status — a handler that calls waitpid() would reap the
  // children first, so dead workers would never be restarted.
}

/// Print the command line help.
void print_help() {
  std::cout << cvknx::application_name() << " " << cvknx::version() << "\n"
            << "FastCGI backend bridging CometVisu Protocol to the KNX bus\n"
            << "(transports: knxd, ip-tunnel, ip-routing)\n"
            << "\n"
            << "Usage: cometvisu-knx-fcgi [OPTION]\n"
            << "\n"
            << "Options:\n"
            << "  --version, -v   Print version information and exit\n"
            << "  --discover      Send a KNXnet/IP SEARCH_REQUEST and print the answers\n"
            << "  --help, -h      Print this help message and exit\n"
            << "\n"
            << "Environment variables:\n"
            << "  KNX_TRANSPORT         REQUIRED. knxd | ip-tunnel | ip-routing\n"
            << "  KNXD_SOCKET           knxd: path to the knxd Unix socket (default: /run/knx)\n"
            << "  KNX_IP_HOST           ip-tunnel: KNX interface/router address (required)\n"
            << "  KNX_IP_PORT           KNXnet/IP UDP port (default: 3671)\n"
            << "  KNX_IP_LOCAL_ADDR     ip-tunnel: local address advertised in the HPAI\n"
            << "                        (unset = let the OS choose, NAT friendly)\n"
            << "  KNX_IP_MULTICAST      ip-routing: multicast group (default: 224.0.23.12)\n"
            << "  KNX_IP_INDIVIDUAL_ADDR ip-routing: cEMI source address (default: 15.15.250)\n"
            << "  KNX_IP_IFACE          ip-routing: interface for join/send (default: any)\n"
            << "  KNX_IP_NAT            ip-tunnel: 1 = NAT mode (default: 0)\n"
            << "  KNX_IP_HEARTBEAT_SEC  ip-tunnel: heartbeat interval (default: 60)\n"
            << "  KNX_IP_ACK_TIMEOUT_MS ip-tunnel: ACK timeout in ms (default: 1000)\n"
            << "  KNX_IP_RETRIES        ip-tunnel: retransmissions (default: 3)\n"
            << "  FCGI_SOCKET           REQUIRED: FCGI listen socket (\":port\" for TCP or a\n"
               "                        Unix socket path)\n"
            << "  FCGI_WORKERS          Worker processes (default: 20, max: 256)\n"
            << "  LONGPOLL_TIMEOUT_SEC  Max seconds to wait in long-poll /r (default: 300, max: "
            << kMaxLongpollTimeoutSec << ")\n"
            << "  KNX_TX_MAX_PER_SEC    Max outgoing telegrams per second over ALL clients\n"
            << "                        (default: 10, max: 1000)\n"
            << "  SESSION_TTL_SEC       Lifetime of a session in seconds (default: 1800)\n"
            << "  SESSION_MAX           Max concurrent sessions, oldest evicted (default: 10000)\n"
            << "  KNX_CACHE_CAPACITY    Distinct group addresses kept in the cache, least\n"
            << "                        recently used evicted (default: 16384)\n"
            << "  ADDRESS_PREFIX        Namespace prefix for addresses without explicit prefix\n"
            << "                        (default: empty = no prefix, e.g. set to \"KNX\")\n"
            << "  BASE_URL              URL prefix advertised in login response (unset = omitted)\n"
            << "                        (default: not set, e.g. /proxy/visu)\n"
            << "  LOGIN_EXTRA_CONFIG    Raw JSON key-value pairs injected into login response\n"
            << "                        \"c\" block (empty = none)\n"
            << "  DEBUG_BACKEND         Set to 1 to enable debug logging to stderr\n"
            << "\n"
            << "When run without options, starts the FastCGI server loop.\n";
}

/// `--discover`: send a SEARCH_REQUEST and print every answer.
/// @return process exit code.
int run_discovery() {
  cvknx::DiscoveryOptions discovery;
  if (const char* group = std::getenv("KNX_IP_MULTICAST"); group != nullptr && *group != '\0') {
    discovery.address = group;
  }
  if (const char* iface = std::getenv("KNX_IP_IFACE"); iface != nullptr && *iface != '\0') {
    discovery.iface = iface;
  }
  const auto devices = cvknx::discover_devices(discovery);
  if (devices.empty()) {
    std::cout << "No KNXnet/IP device answered on " << discovery.address << ":" << discovery.port
              << "\n";
    return 1;
  }
  for (const auto& device : devices) {
    std::string name = device.name;
    if (name.empty()) {
      name = "(unnamed)";
    }
    const cvknx::knxnetip::Hpai endpoint{device.address, device.port};
    const uint16_t individual = device.individual_address;
    std::cout << "KNXnet/IP device: " << name << "  individual=" << ((individual >> 12) & 0x0F)
              << "." << ((individual >> 8) & 0x0F) << "." << (individual & 0xFF)
              << "  endpoint=" << cvknx::knxnetip::ipv4_to_string(endpoint.address) << ":"
              << endpoint.port
              << "  multicast=" << cvknx::knxnetip::ipv4_to_string(device.multicast_address)
              << "\n";
  }
  return 0;
}

/// One aligned `[INFO]   NAME  value` line of the startup banner.
///
/// The banner is the first thing an operator reads in a container log, so the
/// values must line up no matter how long the variable name is.
void print_setting(std::string_view name, std::string_view value) {
  constexpr int kNameColumn = 22;
  std::cout << "[INFO]   " << std::left << std::setw(kNameColumn) << name << std::right << value
            << "\n";
}

/// Everything the banner shows about the environment.
struct StartupSettings {
  int longpoll_timeout_sec;
  int num_workers;
  int max_tx_per_sec;
  std::string_view fcgi_socket;
  std::string_view address_prefix;
  std::string_view base_url;
};

/// Print the effective configuration (one line per setting).
void print_startup_info(const cvknx::BusConfig& bus_config, const StartupSettings& settings) {
  std::cout << "[INFO] " << cvknx::application_name() << " " << cvknx::version() << " starting\n";
  std::cout << "[INFO] Configuration:\n";
  using cvknx::bus_transport_to_string;
  print_setting("KNX_TRANSPORT", bus_transport_to_string(bus_config.transport));
  print_setting("BUS", bus_config.describe());
  print_setting("FCGI_SOCKET", settings.fcgi_socket);
  print_setting("FCGI_WORKERS", std::to_string(settings.num_workers));
  print_setting("LONGPOLL_TIMEOUT_SEC", std::to_string(settings.longpoll_timeout_sec));
  print_setting("KNX_TX_MAX_PER_SEC", std::to_string(settings.max_tx_per_sec));
  print_setting("ADDRESS_PREFIX",
                settings.address_prefix.empty() ? "(not set)" : settings.address_prefix);
  print_setting("BASE_URL", settings.base_url.empty() ? "(not set)" : settings.base_url);
  print_setting("LOGIN_EXTRA_CONFIG", get_env_default("LOGIN_EXTRA_CONFIG", "(not set)"));
  print_setting("DEBUG_BACKEND", get_env_default("DEBUG_BACKEND", "(not set)"));
}

/// Create a process-shared semaphore in shared memory (or nullptr).
/// @param initial Initial value (the pool capacity).
/// @param name Human readable name for the warning.
sem_t* create_shared_semaphore(int initial, const char* name) {
  auto* sem = static_cast<sem_t*>(
      ::mmap(nullptr, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  // GCOV_EXCL_START - mmap()/sem_init() failures need resource exhaustion
  if (sem == MAP_FAILED) {
    std::cerr << "[WARN] Failed to create " << name << " semaphore: " << std::strerror(errno)
              << " (continuing without it)\n";
    return nullptr;
  }
  if (::sem_init(sem, 1, static_cast<unsigned>(initial)) != 0) {
    std::cerr << "[WARN] Failed to init " << name << " semaphore: " << std::strerror(errno)
              << " (continuing without it)\n";
    ::munmap(sem, sizeof(sem_t));
    return nullptr;
  }
  // GCOV_EXCL_STOP
  return sem;
}

/// How many requests may block in a long-poll at the same time.
///
/// Slightly below the pool size so the short requests always find a free
/// worker (see kReservedWorkers).  At least one long-poll must stay possible,
/// otherwise a single-worker pool could never serve `/r`.
int longpoll_capacity(int num_workers) {
  return std::max(1, num_workers - kReservedWorkers);
}

/// Everything a forked child needs.  All of it lives in the parent and is
/// inherited; nothing here may be a process-private copy of shared state.
struct PoolResources {
  cvknx::FcgiServer& server;
  const cvknx::BusConfig& bus_config;
  cvknx::SharedGroupCache& cache;
  cvknx::SharedTxQueue& tx_queue;
  cvknx::SharedTxBusLink& worker_link;
  int num_workers;
  int max_tx_per_sec;
};

/// Fork the cache reader child.
/// @return the child pid, or -1 when fork() failed.
pid_t spawn_cache_reader(PoolResources& res) {
  cvknx::flush_before_fork();
  const pid_t pid = ::fork();
  if (pid < 0) {  // GCOV_EXCL_BR_LINE - fork() only fails on resource exhaustion
    return -1;    // GCOV_EXCL_LINE
  }
  if (pid == 0) {
    // ================================================
    // Cache reader child process
    // ================================================
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#if defined(KNX_COVERAGE)
    // Coverage builds: the reader stops through its stop predicate so the
    // graceful shutdown path is exercised and the counters are flushed.
    (void)::signal(SIGTERM, handle_sigterm);
#else
    (void)::signal(SIGTERM, SIG_DFL);
#endif

    // Close inherited FCGI resources — the cache reader does not handle HTTP.
    // It only exists in direct-socket mode, so the socket is always open here.
    if (res.server.is_listening()) {  // GCOV_EXCL_BR_LINE
      res.server.close_listen_socket();
    }

    // The reader owns the only bus connection and reconnects forever: the bus
    // may be missing at startup or restart at any time, and the loop must wait
    // for it without burning CPU.
    auto reader_link = cvknx::create_bus_link(res.bus_config);
    // GCOV_EXCL_START - see the startup probe: the transport is validated
    if (!reader_link) {
      KNX_COVERAGE_DUMP();
      std::_Exit(1);
    }
    // GCOV_EXCL_STOP
    std::cout << "[INFO] Cache reader started, pid=" << ::getpid() << " (" << reader_link->label()
              << ")\n";
    std::cout << std::flush;

    cvknx::CacheReaderOptions reader_options;
    // Global send rate limit: the cache reader is the only process that hands
    // telegrams to the bus, so this bounds the traffic of *every* client and
    // every worker at once.  A client that writes thousands of addresses, or
    // one that retries in a loop, can therefore never flood the bus.
    reader_options.max_tx_per_sec = res.max_tx_per_sec;
    cvknx::run_cache_reader(*reader_link, res.cache, &res.tx_queue, reader_options,
                            [] { return g_shutdown_requested != 0; });

    std::cout << "[INFO] Cache reader shutting down\n";
    std::cout << std::flush;
    reader_link->disconnect();
    // GCOV_EXCL_START - gcov records no counter after the noreturn _Exit()
    KNX_COVERAGE_DUMP();
    std::_Exit(0);
    // GCOV_EXCL_STOP
  }
  return pid;
}

/// Fork one FCGI worker child.
/// @return the child pid, or -1 when fork() failed.
pid_t spawn_worker(PoolResources& res, int index) {
  cvknx::flush_before_fork();
  const pid_t pid = ::fork();
  if (pid < 0) {  // GCOV_EXCL_BR_LINE - fork() only fails on resource exhaustion
    return -1;    // GCOV_EXCL_LINE
  }
  if (pid == 0) {
    // ================================================
    // Worker child process
    // ================================================
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#if defined(KNX_COVERAGE)
    (void)::signal(SIGTERM, handle_coverage_sigterm);
#else
    (void)::signal(SIGTERM, SIG_DFL);
#endif

    // A missing bus must not keep the worker from serving HTTP and must not
    // delay its readiness: the shared TX ring queues and rejects writes on its
    // own, so a worker never needs a bus connection.
    //
    // SharedTxBusLink::connect() only reports whether the shared queue exists,
    // and that was created before the fork — so this warning is unreachable by
    // construction.
    // GCOV_EXCL_START
    if (!res.worker_link.connect()) {
      std::cerr << "[WARN] Worker pid=" + std::to_string(::getpid()) + " (worker " +
                       std::to_string(index) + "): cannot use " + res.worker_link.label() +
                       " — continuing, will retry on demand\n";
    }
    // GCOV_EXCL_STOP

    std::cout << "[INFO] Worker " << index << " ready, pid=" << ::getpid() << "\n";
    std::cout << std::flush;

    const int child_result = res.server.run();
    // The supervisor stops workers with a signal; run() therefore never
    // returns in this role, so the cleanup below is unreachable.
    // GCOV_EXCL_START
    res.worker_link.disconnect();
    KNX_COVERAGE_DUMP();
    std::_Exit(child_result);
    // GCOV_EXCL_STOP
  }
  return pid;
}

/// Restart backoff for a supervised child: a child that dies right after
/// starting is restarted with a growing delay (100 ms … 30 s).
struct RestartBackoff {
  int delay_ms = 100;
  std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();

  void on_spawn() { started_at = std::chrono::steady_clock::now(); }

  [[nodiscard]] int on_death() {
    const auto lived = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_at);
    if (lived.count() > 60) {  // GCOV_EXCL_BR_LINE - needs a child that ran for a minute
      delay_ms = 100;          // GCOV_EXCL_LINE - needs a child that ran for a minute
    }
    const int wait_ms = delay_ms;
    delay_ms = std::min(delay_ms * 2, 30000);
    return wait_ms;
  }
};

/// Give one semaphore token back after a worker died holding it.
///
/// A crash must not shrink the pool permanently; the value never exceeds the
/// initial capacity, so a child that died without holding a token changes
/// nothing.
void restore_semaphore(sem_t* sem, int capacity) {
  // GCOV_EXCL_START - the semaphores only stay null if their creation failed
  if (sem == nullptr) {
    return;
  }
  // GCOV_EXCL_STOP
  int value = 0;
  if (::sem_getvalue(sem, &value) == 0 && value < capacity) {  // GCOV_EXCL_BR_LINE
    ::sem_post(sem);
  }
}

/// Run the fork-based worker pool and supervise it until shutdown.
///
/// The parent is the ONLY process that reaps children.  A SIGCHLD handler must
/// never call waitpid(): doing so steals the exit statuses from this loop, so
/// dead children would never be noticed and the pool would silently shrink to
/// nothing over weeks.
///
/// Every dead child is logged and restarted.  A dead cache reader means no
/// telegram ever reaches the shared cache again, so it is restarted too.
///
/// @return process exit code.
int run_worker_pool(PoolResources& res, sem_t* load_shed_sem, sem_t* concurrency_sem) {
  std::cout << "[INFO] Starting " << res.num_workers << " worker processes\n";
  std::cout << std::flush;  // Flush before fork to avoid buffer duplication in children

  const int num_workers = res.num_workers;

  pid_t cache_reader_pid = spawn_cache_reader(res);
  // GCOV_EXCL_START - fork() failures need resource exhaustion
  if (cache_reader_pid < 0) {
    std::cerr << "[ERROR] fork() for cache reader failed: " << std::strerror(errno) << "\n";
    return 1;
  }
  // GCOV_EXCL_STOP
  std::cout << "[INFO] Started cache reader pid=" << cache_reader_pid << "\n";

  std::vector<pid_t> worker_pids(static_cast<size_t>(num_workers), -1);
  bool fork_failed = false;
  for (int i = 0; i < num_workers; ++i) {
    const pid_t pid = spawn_worker(res, i);
    // GCOV_EXCL_START - fork() failure branch, see above
    if (pid < 0) {
      std::cerr << "[ERROR] fork() for worker " << (i + 1) << "/" << num_workers
                << " failed: " << std::strerror(errno) << " (errno=" << errno << ")\n";
      fork_failed = true;
      break;
    }
    // GCOV_EXCL_STOP
    worker_pids[static_cast<size_t>(i)] = pid;
    std::cout << "[INFO] Started worker " << i << "/" << num_workers << " pid=" << pid << "\n";
  }
  std::cout << std::flush;

  // ---- Parent: inherited resources ----
  // The parent does not handle FCGI requests — it only supervises the
  // children.  It KEEPS the listen socket open: every worker the supervisor
  // forks later inherits it.  Closing it here (as an earlier version did to
  // save one fd) leaves replacement workers without an accept socket, so they
  // exit immediately and the pool can never recover from a crash.  It also
  // keeps the socket alive for the web server while no worker is accepting, so
  // connections queue in the backlog instead of failing.

  // GCOV_EXCL_START - only reachable when a fork() above failed
  if (fork_failed) {
    std::cerr << "[ERROR] fork() failed, terminating all started children\n";
    if (cache_reader_pid > 0) {
      ::kill(cache_reader_pid, SIGTERM);
    }
    for (const pid_t pid : worker_pids) {
      if (pid > 0) {
        ::kill(pid, SIGTERM);
      }
    }
    int status = 0;
    while (::wait(&status) > 0) {
    }
    return 1;
  }
  // GCOV_EXCL_STOP

  std::cout << "[INFO] All " << num_workers << " workers started, supervising children...\n";
  std::cout << std::flush;

  RestartBackoff reader_backoff;
  std::vector<RestartBackoff> worker_backoff(static_cast<size_t>(num_workers));

  while (g_shutdown_requested == 0) {
    int status = 0;
    const pid_t waited = ::wait(&status);
    if (waited < 0) {
      if (errno == EINTR) {  // GCOV_EXCL_BR_LINE - wait() only fails with EINTR here
        continue;            // interrupted by a signal — check the shutdown flag
      }
      break;  // GCOV_EXCL_LINE - ECHILD needs a child reaped elsewhere
    }

    if (WIFEXITED(status)) {
      const int exit_code = WEXITSTATUS(status);
      // GCOV_EXCL_START - a child only exits non-zero when its own startup fails
      if (exit_code != 0) {
        std::cerr << "[WARN] Child pid=" << waited << " exited with code " << exit_code << "\n";
      }
      // GCOV_EXCL_STOP
    } else if (WIFSIGNALED(status)) {  // GCOV_EXCL_BR_LINE - a stopped child never appears
      std::cerr << "[WARN] Child pid=" << waited << " killed by signal " << WTERMSIG(status);
      // GCOV_EXCL_START - depends on the kernel's core-dump configuration
      if (WCOREDUMP(status)) {
        std::cerr << " (core dumped)";
      }
      // GCOV_EXCL_STOP
      std::cerr << "\n";
    }
    std::cerr << std::flush;

    // Needs a child death racing the SIGTERM — excluded as a race.
    if (g_shutdown_requested != 0) {  // GCOV_EXCL_BR_LINE
      break;                          // GCOV_EXCL_LINE
    }

    if (waited == cache_reader_pid) {
      std::cerr << "[ERROR] Cache reader pid=" << waited << " died — restarting it\n";
      ::usleep(static_cast<useconds_t>(reader_backoff.on_death()) * 1000);
      cache_reader_pid = spawn_cache_reader(res);
      // GCOV_EXCL_START - fork() failure branch
      if (cache_reader_pid < 0) {
        std::cerr << "[ERROR] fork() for cache reader failed: " << std::strerror(errno) << "\n";
      } else {
        // GCOV_EXCL_STOP
        reader_backoff.on_spawn();
        std::cout << "[INFO] Restarted cache reader pid=" << cache_reader_pid << "\n";
        std::cout << std::flush;
      }
      continue;
    }

    for (size_t i = 0; i < worker_pids.size(); ++i) {  // GCOV_EXCL_BR_LINE - never empty
      if (worker_pids[i] != waited) {
        continue;
      }
      restore_semaphore(concurrency_sem, longpoll_capacity(num_workers));
      restore_semaphore(load_shed_sem, longpoll_capacity(num_workers));

      std::cerr << "[WARN] Worker " << i << " (pid=" << waited << ") died — restarting it\n";
      ::usleep(static_cast<useconds_t>(worker_backoff[i].on_death()) * 1000);
      worker_pids[i] = spawn_worker(res, static_cast<int>(i));
      // GCOV_EXCL_START - fork() failure branch
      if (worker_pids[i] < 0) {
        std::cerr << "[ERROR] fork() for worker " << i << " failed: " << std::strerror(errno)
                  << "\n";
      } else {
        // GCOV_EXCL_STOP
        worker_backoff[i].on_spawn();
        std::cout << "[INFO] Restarted worker " << i << " pid=" << worker_pids[i] << "\n";
        std::cout << std::flush;
      }
      break;
    }
  }

  // Shutdown requested: kill remaining children and wait for them.
  if (g_shutdown_requested != 0) {  // GCOV_EXCL_BR_LINE
    std::cout << "[INFO] Shutdown requested, stopping all children...\n";
    std::cout << std::flush;
    if (cache_reader_pid > 0) {  // GCOV_EXCL_BR_LINE - a failed restart leaves -1
      ::kill(cache_reader_pid, SIGTERM);
    }
    for (const pid_t pid : worker_pids) {
      if (pid > 0) {  // GCOV_EXCL_BR_LINE - a failed fork leaves -1
        ::kill(pid, SIGTERM);
      }
    }
    int status = 0;
    while (::wait(&status) > 0) {
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace cvknx;

  // Handle command-line flags when invoked directly (e.g. for version queries).
  // FastCGI binaries can be run from a terminal for introspection without
  // needing a running web server.
  if (argc > 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::string_view arg = argv[1];
    if (arg == "--version" || arg == "-v") {
      std::cout << application_name() << " " << version() << "\n";
      return 0;
    }
    if (arg == "--discover") {
      return run_discovery();
    }
    if (arg == "--help" || arg == "-h") {
      print_help();
      return 0;
    }
  }

  // ---- Debug mode ----
  // Enable via environment variable DEBUG_BACKEND=1 (or DEBUG_BACKEND=true/yes/on).
  DebugLog::init_from_env();

  // ---- Signals and resource limits ----
  install_signal_handlers();
  set_resource_limits();

  // ---- Configuration ----
  // Environment variables are inherited from the FCGI-spawning web server.
  // This is safe because:
  //   - The web server (e.g. Apache, nginx, lighttpd) is the trusted parent process.
  //   - The knxd socket is a local Unix socket — redirection only affects the local
  //     machine.
  //   - An attacker who can manipulate the web server's environment already has
  //     sufficient access to compromise the system directly.
  //
  // KNX_TRANSPORT is mandatory: the three transports are equally supported and
  // there is no silent default (a wrong `ip-*` setup must not look like a knxd
  // outage).  Every other value is validated, never replaced by a default.
  std::string bus_config_error;
  const auto bus_config_opt = BusConfig::from_env(bus_config_error);
  if (!bus_config_opt) {
    std::cerr << "[ERROR] " << bus_config_error << "\n";
    return 1;
  }
  const BusConfig bus_config = *bus_config_opt;

  const int longpoll_timeout =
      parse_env_int("LONGPOLL_TIMEOUT_SEC", 300, 1, kMaxLongpollTimeoutSec);
  const int num_workers = parse_env_int("FCGI_WORKERS", 20, 1, 256);
  // Parsed once and handed to the cache reader: the banner and the enforced
  // limit must never disagree.
  const int max_tx_per_sec = parse_env_int("KNX_TX_MAX_PER_SEC", 10, 1, 1000);
  const int session_ttl_sec =
      parse_env_int("SESSION_TTL_SEC", SessionStore::kDefaultSessionTtlSec, 1, kMaxSessionTtlSec);
  const int session_max =
      parse_env_int("SESSION_MAX", static_cast<int>(kDefaultMaxSessions),
                    static_cast<int>(kMinMaxSessions), static_cast<int>(kMaxMaxSessions));
  const int cache_capacity =
      parse_env_int("KNX_CACHE_CAPACITY", static_cast<int>(kDefaultCacheCapacity),
                    static_cast<int>(kMinCacheCapacity), static_cast<int>(kMaxCacheCapacity));

  // ---- Address prefix (namespace) ----
  // When the client sends addresses without a namespace prefix (no colon),
  // this value is used as the default namespace. Default: empty (no prefix).
  // Set ADDRESS_PREFIX=KNX to restore the old "KNX:1/2/3" format.
  const char* address_prefix = get_env_default("ADDRESS_PREFIX", "");

  // ---- Base URL for login response ----
  // When set, the /l endpoint includes a "c" object with "baseURL".
  const char* base_url = std::getenv("BASE_URL");

  // ---- FCGI listen socket (required) ----
  // The backend always owns its listen socket: ":port" for TCP or a
  // filesystem path for a Unix socket.  Serving FastCGI from stdin/stdout is
  // not supported — the worker pool and the cache reader depend on sharing an
  // inherited listen fd.
  const char* fcgi_socket = get_env_default("FCGI_SOCKET", "");
  if (*fcgi_socket == '\0') {
    std::cerr << "[ERROR] FCGI_SOCKET is required (\":port\" for TCP or a Unix socket path)\n";
    return 1;
  }

  KnxAddress::set_default_namespace(address_prefix);
  print_startup_info(bus_config,
                     StartupSettings{longpoll_timeout, num_workers, max_tx_per_sec, fcgi_socket,
                                     address_prefix, base_url != nullptr ? base_url : ""});

  // ---- Initialize FCGI server (before the bus) ----
  // Set up the FastCGI listen socket first so it's ready for connections before
  // attempting the potentially slow bus connection.
  FcgiServer server;
  if (server.listen(fcgi_socket)) {  // GCOV_EXCL_BR_LINE - see the else comment
    std::cout << "[INFO] Direct FCGI socket: " << fcgi_socket << "\n";
    // GCOV_EXCL_START - FCGX_OpenSocket() terminates the process on a bind
    // failure, so listen() never reports one here.
  } else {
    std::cerr << "[ERROR] Failed to open FCGI socket: " << fcgi_socket << "\n";
    return 1;
  }
  // GCOV_EXCL_STOP

  // FastCGI has no transport authentication and the handlers trust the web
  // server to authenticate, so the endpoint itself is the whole access
  // control.  A filesystem socket inherits the umask (typically 0755, i.e.
  // connectable by every local user); reduce it to owner/group rw.  A TCP
  // socket is reachable from the network and must never be exposed.
  if (fcgi_socket[0] != ':') {
    // GCOV_EXCL_START - we just created the socket, so chmod cannot fail
    // unless the directory was replaced between bind and chmod.
    if (::chmod(fcgi_socket, 0660) != 0) {
      std::cerr << "[WARN] Cannot restrict the FCGI socket to 0660: " << std::strerror(errno)
                << "\n";
    }
    // GCOV_EXCL_STOP
  } else {
    std::cerr << "[WARN] FCGI_SOCKET " << fcgi_socket
              << " listens on a TCP port with no authentication or peer check; keep it on a "
                 "loopback/reverse-proxy interface or use a Unix socket path\n";
  }

  // ---- Shared state (created before ANY fork) ----
  SharedGroupCache shared_cache;
  // GCOV_EXCL_START - only fails when mmap() is out of address space
  if (!shared_cache.create(static_cast<std::size_t>(cache_capacity))) {
    std::cerr << "[ERROR] Failed to create shared group cache\n";
    return 1;
  }
  // GCOV_EXCL_STOP
  std::cout << "[INFO] Shared group cache created ("
            << shared_cache.region_size(static_cast<std::size_t>(cache_capacity))
            << " bytes, capacity=" << shared_cache.capacity() << ")\n";

  SharedTxQueue tx_queue;
  // GCOV_EXCL_START - only fails when mmap()/eventfd() are exhausted
  if (!tx_queue.create()) {
    std::cerr << "[ERROR] Failed to create shared TX queue: " << std::strerror(errno) << "\n";
    return 1;
  }
  // GCOV_EXCL_STOP
  std::cout << "[INFO] Shared TX queue created (" << SharedTxQueue::region_size()
            << " bytes, capacity=" << kTxQueueCapacity << ")\n";

  // Sessions must be visible to every worker: a client logs in once and then
  // sends `s=<id>` with requests that any worker may serve.
  SessionStore sessions(session_ttl_sec, static_cast<std::size_t>(session_max));
  // GCOV_EXCL_START - only fails when mmap() is out of address space
  if (!sessions.is_initialized()) {
    std::cerr << "[ERROR] Failed to create the shared session store\n";
    return 1;
  }
  // GCOV_EXCL_STOP
  std::cout << "[INFO] Shared session store created ("
            << sessions.region_size(static_cast<std::size_t>(session_max))
            << " bytes, capacity=" << sessions.capacity() << ", ttl=" << session_ttl_sec << "s)\n";

  // ---- Verify the bus connection (best effort) ----
  // The daemon MUST start even when the bus is unavailable: knxd can die at any
  // time ("Link down, terminating") and a KNX interface may be rebooting.  The
  // cache reader reconnects on its own, so a failed probe is only a warning —
  // never a reason to refuse to serve HTTP.
  {
    auto probe = create_bus_link(bus_config);
    // GCOV_EXCL_START - create_bus_link() only returns null when the knxd
    // transport was compiled out but selected, which from_env() rejects first.
    if (!probe) {
      return 1;
    }
    // GCOV_EXCL_STOP
    // Short, bounded retries: startup must not wait for a long backoff, the
    // cache reader retries forever in the background anyway.
    constexpr std::array<int, 3> kProbeDelaysMs = {100, 200, 400};
    if (connect_bus_with_retry(*probe, kProbeDelaysMs)) {
      std::cout << "[INFO] Bus link usable: " << probe->label()
                << (probe->is_stateless() ? " (stateless — nothing to verify)" : "") << "\n";
      probe->disconnect();
    } else {
      std::cerr << "[WARN] " << probe->label()
                << " not reachable — starting anyway and retrying in the background\n";
      probe->disconnect();
    }
  }

  // ---- Create the router (used by every worker) ----
  // Workers only enqueue: the cache reader owns the single bus connection for
  // RX and TX, so `FCGI_WORKERS` never multiplies knxd client slots or
  // KNX/IP tunnels.
  SharedTxBusLink worker_link(tx_queue, shared_cache);
  Router router(worker_link, shared_cache, sessions, longpoll_timeout,
                base_url != nullptr ? base_url : "", std::string(bus_config.outage_key()));

  server.set_handler(
      [&router](const FcgiRequest& req) -> FcgiResponse { return router.route(req); });

  // ---- Fork-based worker pool ----
  // Each child process runs an independent accept loop on the shared listen
  // socket; the OS serializes accept() across processes.  This avoids Docker
  // < 20.10.10 seccomp issues where clone3/clone with process-sharing flags
  // are blocked, preventing std::thread creation.
  //
  // Two semaphores bound the number of concurrent long-polls across workers:
  // when they are exhausted a new /r answers 503 immediately instead of piling
  // up in the listen backlog (which the reverse proxy reports as 502).
  // Writes and logins never take a token — a spurious 503 would surface in
  // CometVisu as a failed switch operation.
  sem_t* load_shed_sem = create_shared_semaphore(longpoll_capacity(num_workers), "load-shed");
  server.set_load_shed_semaphore(load_shed_sem);
  sem_t* concurrency_sem = create_shared_semaphore(longpoll_capacity(num_workers), "concurrency");
  server.set_concurrency_semaphore(concurrency_sem);

  PoolResources resources{server,      bus_config,  shared_cache,  tx_queue,
                          worker_link, num_workers, max_tx_per_sec};
  const int result = run_worker_pool(resources, load_shed_sem, concurrency_sem);

  // ---- Clean up shared semaphores ----
  if (load_shed_sem != nullptr) {  // GCOV_EXCL_BR_LINE - null only if creation failed
    ::sem_destroy(load_shed_sem);
    ::munmap(load_shed_sem, sizeof(sem_t));
  }
  if (concurrency_sem != nullptr) {  // GCOV_EXCL_BR_LINE - null only if creation failed
    ::sem_destroy(concurrency_sem);
    ::munmap(concurrency_sem, sizeof(sem_t));
  }
  return result;
}
