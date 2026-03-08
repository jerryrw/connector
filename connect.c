/*
 * netcat.c
 *
 * -----------------------------------------------------------------------------
 * OVERVIEW
 * -----------------------------------------------------------------------------
 * This file implements a small "netcat-style" TCP tool in C99 using POSIX
 * sockets. It supports:
 *
 *   1) Client mode:
 *      Connect to a remote IP/port and exchange bytes interactively.
 *
 *   2) Server mode:
 *      Bind/listen on a local IP/port, accept one client, then exchange bytes.
 *
 * The runtime behavior is intentionally simple:
 *   - stdin  -> socket (anything typed/piped locally is sent to peer)
 *   - socket -> stdout (anything received from peer is printed locally)
 *
 * -----------------------------------------------------------------------------
 * DESIGN NOTES FOR NEW DEVELOPERS
 * -----------------------------------------------------------------------------
 * - This is a single-process, single-threaded program.
 * - Full-duplex I/O is handled with select(), not threads.
 * - Address resolution uses getaddrinfo() so IPv4 and IPv6 are both possible.
 * - In server mode, only ONE client is accepted, then handled until disconnect.
 * - On local stdin EOF, we perform a half-close (shutdown(SHUT_WR)):
 *     * we stop sending
 *     * we continue receiving until peer closes
 * - SIGPIPE is ignored so send() failures are reported as return values/errors
 *   instead of terminating the process.
 *
 * -----------------------------------------------------------------------------
 * BUILD
 * -----------------------------------------------------------------------------
 *   cc -std=c99 -Wall -Wextra -pedantic -O2 -o netcat netcat.c
 *
 * -----------------------------------------------------------------------------
 * RUN
 * -----------------------------------------------------------------------------
 *   Client: ./netcat -i 127.0.0.1 -p 9000
 *   Server: ./netcat -l -i 0.0.0.0 -p 9000
 */

#include "connect.h"

#include <arpa/inet.h>   // inet_ntop
#include <errno.h>       // errno
#include <netdb.h>       // getaddrinfo, freeaddrinfo, gai_strerror
#include <netinet/in.h>  // sockaddr_in, sockaddr_in6
#include <pthread.h>     // pthread_create, pthread_join, pthread_cancel, mutex
#include <signal.h>      // signal, SIGPIPE
#include <stdio.h>       // fprintf, printf, perror, fgets, fwrite, fflush
#include <stdlib.h>      // EXIT_SUCCESS, EXIT_FAILURE, strtol
#include <string.h>      // memset, strcmp, strerror
#include <sys/select.h>  // select, fd_set, timeval
#include <sys/socket.h>  // socket, bind, listen, accept, connect, recv, send, setsockopt
#include <sys/types.h>  // socket-related type definitions
#include <unistd.h>     // close, STDIN_FILENO, read

/* ---------- Configuration constants ----------
 *
 * BACKLOG:
 *   Maximum number of pending TCP connections queued by kernel while waiting
 *   for accept(). Since we only accept one client and then stay in duplex loop,
 *   this mostly provides tolerance for short bursts before first accept.
 *
 * IO_BUFFER_SIZE:
 *   Chunk size used for both reading stdin and receiving socket data.
 *   This is not a message boundary; TCP is stream-oriented.
 */
#define BACKLOG 16
#define IO_BUFFER_SIZE 4096

typedef struct DuplexState {
  int sockfd;
  pthread_mutex_t lock;
  int stop_requested;
  int had_error;
} DuplexState;

typedef struct ClientNode {
  int fd;
  struct ClientNode* next;
} ClientNode;

typedef struct ServerState {
  pthread_mutex_t clients_lock;
  pthread_mutex_t stdout_lock;
  ClientNode* clients;
} ServerState;

typedef struct ClientWorkerArgs {
  ServerState* state;
  int client_fd;
} ClientWorkerArgs;

/* ---------- Shared state for duplex worker threads (client mode) ----------
 *
 * DuplexState:
 *   Represents the state of a single client connection.
 *   - sockfd: socket file descriptor
 *   - lock: mutex to protect shared state
 *   - stop_requested: flag to stop the duplex loop
 *   - had_error: flag to indicate if an error occurred
 *
 * ClientNode:
 *   Represents a client connection in the server's active client list.
 *   - fd: file descriptor
 *   - next: pointer to the next client in the list
 *
 * ServerState:
 *   Represents the state of the server.
 *   - clients_lock: mutex to protect the active client list
 *   - stdout_lock: mutex to protect stdout
 *   - clients: pointer to the head of the active client list
 *
 * ClientWorkerArgs:
 *   Represents the arguments for a client worker thread.
 *   - state: pointer to the server state
 *   - client_fd: file descriptor of the client
 *
 * Thread/helper prototypes used by earlier functions:
 *   - duplex_request_stop: stops the duplex loop
 *   - duplex_should_stop: checks if the duplex loop should stop
 *   - sender_thread_main: main function of the sender thread
 *   - receiver_thread_main: main function of the receiver thread
 *
 * Static functions:
 *   - send_all_fd: sends all bytes from a file descriptor
 *   - server_add_client: adds a client to the server's active client list
 *   - server_remove_client: removes a client from the server's active client
 * list
 *   - server_broadcast: broadcasts a message to all clients
 *   - server_stdin_broadcast_main: main function of the server stdin broadcast
 * thread
 *   - server_client_worker_main: main function of the server client worker
 * thread
 *   - server_close_all_clients: closes all tracked clients
 *
 * Program options structure:
 *   - listen_mode:
 *     0 = act as client (connect)
 *     1 = act as server (bind/listen/accept)
 *
 *   - ip:
 *     Client mode: remote target IP.
 *     Server mode: local bind IP. Defaults to 127.0.0.1 when omitted.
 *
 *   - port:
 *     TCP service/port string, required in both modes.
 *
 * Utility functions:
 *   - print_usage: prints user-facing CLI help
 *   - parse_args: parses command-line arguments
 *   - print_sockaddr: converts a generic sockaddr into numeric host:port text
 *
 * Important:
 *   - Always freeaddrinfo() before returning.
 *   - Each failed candidate fd is closed immediately.
 */
static void duplex_request_stop(DuplexState* st, int mark_error) {
  if (st == NULL) return;
  pthread_mutex_lock(&st->lock);
  st->stop_requested = 1;
  if (mark_error) st->had_error = 1;
  pthread_mutex_unlock(&st->lock);
}

static int duplex_should_stop(DuplexState* st) {
  int stop = 1;
  if (st == NULL) return 1;
  pthread_mutex_lock(&st->lock);
  stop = st->stop_requested;
  pthread_mutex_unlock(&st->lock);
  return stop;
}

static int send_all_fd(int fd, const char* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, buf + off, len - off, 0);
    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
  return 0;
}

static void* sender_thread_main(void* arg) {
  DuplexState* st = (DuplexState*)arg;
  char buf[IO_BUFFER_SIZE];

  for (;;) {
    if (duplex_should_stop(st)) break;

    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n == 0) {
      shutdown(st->sockfd, SHUT_WR);
      duplex_request_stop(st, 0);
      break;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      duplex_request_stop(st, 1);
      break;
    }
    if (send_all_fd(st->sockfd, buf, (size_t)n) != 0) {
      duplex_request_stop(st, 1);
      break;
    }
  }
  return NULL;
}

static void* receiver_thread_main(void* arg) {
  DuplexState* st = (DuplexState*)arg;
  char buf[IO_BUFFER_SIZE];

  for (;;) {
    ssize_t n = recv(st->sockfd, buf, sizeof(buf), 0);
    if (n == 0) {
      duplex_request_stop(st, 0);
      break;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      duplex_request_stop(st, 1);
      break;
    }
    if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) {
      duplex_request_stop(st, 1);
      break;
    }
    fflush(stdout);
  }
  return NULL;
}

static void server_add_client(ServerState* st, int fd) {
  ClientNode* n = (ClientNode*)malloc(sizeof(ClientNode));
  if (n == NULL) return;
  n->fd = fd;
  pthread_mutex_lock(&st->clients_lock);
  n->next = st->clients;
  st->clients = n;
  pthread_mutex_unlock(&st->clients_lock);
}

static void server_remove_client(ServerState* st, int fd) {
  pthread_mutex_lock(&st->clients_lock);
  ClientNode** cur = &st->clients;
  while (*cur != NULL) {
    if ((*cur)->fd == fd) {
      ClientNode* dead = *cur;
      *cur = dead->next;
      free(dead);
      break;
    }
    cur = &((*cur)->next);
  }
  pthread_mutex_unlock(&st->clients_lock);
}

static void server_broadcast(ServerState* st, const char* buf, size_t len) {
  pthread_mutex_lock(&st->clients_lock);
  for (ClientNode* n = st->clients; n != NULL; n = n->next) {
    (void)send_all_fd(n->fd, buf, len);
  }
  pthread_mutex_unlock(&st->clients_lock);
}

static void* server_stdin_broadcast_main(void* arg) {
  ServerState* st = (ServerState*)arg;
  char buf[IO_BUFFER_SIZE];

  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    server_broadcast(st, buf, (size_t)n);
  }
  return NULL;
}

static void* server_client_worker_main(void* arg) {
  ClientWorkerArgs* wa = (ClientWorkerArgs*)arg;
  ServerState* st = wa->state;
  int fd = wa->client_fd;
  char buf[IO_BUFFER_SIZE];
  free(wa);

  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    pthread_mutex_lock(&st->stdout_lock);
    (void)fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&st->stdout_lock);

    server_broadcast(st, buf, (size_t)n);
  }

  server_remove_client(st, fd);
  close(fd);
  return NULL;
}

static void server_close_all_clients(ServerState* st) {
  pthread_mutex_lock(&st->clients_lock);
  ClientNode* n = st->clients;
  st->clients = NULL;
  pthread_mutex_unlock(&st->clients_lock);

  while (n != NULL) {
    ClientNode* next = n->next;
    close(n->fd);
    free(n);
    n = next;
  }
}

/* ---------- Program options structure ----------
 *
 * listen_mode:
 *   0 = act as client (connect)
 *   1 = act as server (bind/listen/accept)
 *
 * ip:
 *   Client mode: remote target IP.
 *   Server mode: local bind IP. Defaults to 127.0.0.1 when omitted.
 *
 * port:
 *   TCP service/port string, required in both modes.
 */

/* ---------- Utility: print usage ----------
 *
 * Prints user-facing CLI help. This function does no validation itself.
 * Validation and error reporting are handled in parse_args().
 */
static void print_usage(const char* prog) {
  fprintf(stderr,
          "Usage:\n"
          "  Client mode: %s -i <ip> -p <port>\n"
          "  Server mode: %s -l -p <port>\n\n"
          "Options:\n"
          "  -l            Listen mode (server, binds to localhost)\n"
          "  -i <ip>       Remote IP address (client mode only)\n"
          "  -p <port>     TCP port number\n",
          prog, prog);
}

/* ---------- Utility: strict argument parser ----------
 *
 * Parsing strategy:
 *   - iterate argv sequentially
 *   - consume value after -i and -p
 *   - reject unknown flags immediately
 *
 * Validation rules:
 *   Server mode (-l):
 *     - requires -p
 *     - optional -i, default = 127.0.0.1
 *
 *   Client mode (default):
 *     - requires both -i and -p
 *
 * Returns:
 *   0  on success
 *  -1  on invalid/insufficient arguments
 */
static int parse_args(int argc, char** argv, Options* opt) {
  int i;

  /* Defensive check: caller must provide a valid Options pointer. */
  if (opt == NULL) {
    fprintf(stderr, "Error: internal parse_args null options pointer.\n");
    return -1;
  }

  /* Initialize all fields to known defaults before parsing.
   * This guarantees deterministic behavior even if parsing fails midway.
   */
  opt->listen_mode = 0; /* Default mode is client unless -l is present. */
  opt->ip =
      NULL; /* Not set until -i is parsed (or defaulted in server mode). */
  opt->port = NULL; /* Must be provided via -p in both modes. */

  /* Walk argv from index 1 (skip program name at argv[0]). */
  for (i = 1; i < argc; i++) {
    /* -l: enable server/listen mode. */
    if (strcmp(argv[i], "-l") == 0) {
      opt->listen_mode = 1;
      continue;
    }

    /* -i <ip>: consume next argument as IP string. */
    if (strcmp(argv[i], "-i") == 0) {
      if (i + 1 >= argc) {
        /* Missing required value after -i. */
        fprintf(stderr, "Error: -i requires an argument.\n");
        return -1;
      }
      opt->ip = argv[++i]; /* Advance to value and store. */
      continue;
    }

    /* -p <port>: consume next argument as port string. */
    if (strcmp(argv[i], "-p") == 0) {
      if (i + 1 >= argc) {
        /* Missing required value after -p. */
        fprintf(stderr, "Error: -p requires an argument.\n");
        return -1;
      }
      opt->port = argv[++i]; /* Advance to value and store. */
      continue;
    }

    /* Any other token is considered unsupported in strict mode. */
    fprintf(stderr, "Error: unknown argument: %s\n", argv[i]);
    return -1;
  }

  /* ------------------ Post-parse validation rules ------------------ */

  if (opt->listen_mode) {
    /* Server mode:
     * - requires port
     * - IP optional; default to localhost if omitted
     */
    if (opt->port == NULL) {
      fprintf(stderr, "Error: -p is required in server mode.\n");
      return -1;
    }

    if (opt->ip == NULL) {
      /* Per requested behavior, server defaults to localhost only. */
      opt->ip = "127.0.0.1";
    }

    return 0;
  }

  /* Client mode:
   * - requires both IP and port
   */
  if (opt->ip == NULL || opt->port == NULL) {
    fprintf(stderr, "Error: client mode requires both -i and -p.\n");
    return -1;
  }

  return 0;
}

/* ---------- Utility: print peer/local address ----------
 *
 * Converts a generic sockaddr into numeric host:port text with getnameinfo().
 * This is diagnostic-only; failures are non-fatal and reported to stderr.
 */
static void print_sockaddr(const char* label, const struct sockaddr* sa,
                           socklen_t salen) {
  /* Buffers for numeric host and service (port) strings.
   * NI_MAXHOST / NI_MAXSERV are system-defined safe sizes.
   */
  char host[NI_MAXHOST];
  char serv[NI_MAXSERV];

  /* Return code from getnameinfo(). */
  int rc;

  /* Convert a generic sockaddr to human-readable numeric "host:port".
   * Flags:
   *   NI_NUMERICHOST -> force numeric IP output (no reverse DNS lookup)
   *   NI_NUMERICSERV -> force numeric port output
   *
   * Using numeric-only output avoids DNS delays and keeps diagnostics stable.
   */
  rc = getnameinfo(sa, salen, host, sizeof(host), serv, sizeof(serv),
                   NI_NUMERICHOST | NI_NUMERICSERV);

  if (rc == 0) {
    /* Successful conversion; print "<label> <ip>:<port>". */
    fprintf(stderr, "%s %s:%s\n", label, host, serv);
  } else {
    /* Non-fatal diagnostic failure (invalid sockaddr, resolver issue, etc.). */
    fprintf(stderr, "%s (address unavailable: %s)\n", label, gai_strerror(rc));
  }
}

/* ---------- Networking: create and connect as client ----------
 *
 * Steps:
 *   1) Resolve target ip/port with getaddrinfo()
 *   2) Iterate each candidate address:
 *       - socket()
 *       - connect()
 *      Stop at first success.
 *   3) On success, return connected fd.
 *   4) On failure, return -1.
 *
 * Important:
 *   - Always freeaddrinfo() before returning.
 *   - Each failed candidate fd is closed immediately.
 */
static int connect_client(const char* ip, const char* port) {
  /* Hints controls what kind of addresses getaddrinfo() returns. */
  struct addrinfo hints;

  /* Head of linked list returned by getaddrinfo(). */
  struct addrinfo* res = NULL;

  /* Iterator pointer over each candidate address in res list. */
  struct addrinfo* rp;

  /* Socket fd for the successfully connected socket, or -1 if none worked. */
  int fd = -1;

  /* Return code holder for getaddrinfo(). */
  int rc;

  /* Zero initialize hints so unspecified fields are deterministic. */
  memset(&hints, 0, sizeof(hints));

  /* AF_UNSPEC means accept IPv4 or IPv6 results. */
  hints.ai_family = AF_UNSPEC;

  /* SOCK_STREAM + IPPROTO_TCP means TCP stream socket candidates only. */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  /* Resolve ip + port into one or more concrete socket addresses.
   * Examples:
   *   "127.0.0.1","9000" -> IPv4 sockaddr list
   *   "::1","9000"       -> IPv6 sockaddr list
   */
  rc = getaddrinfo(ip, port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(client): %s\n", gai_strerror(rc));
    return -1;
  }

  /* Try each candidate until one connect() succeeds. */
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    /* Create socket for this candidate family/type/protocol. */
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      /* Socket creation failed for this candidate; try next. */
      continue;
    }

    /* Attempt TCP connect to this candidate endpoint. */
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      /* Success: keep this fd and stop iterating. */
      break;
    }

    /* Connect failed: close this fd before trying next candidate. */
    close(fd);
    fd = -1;
  }

  /* If fd is still -1, every candidate failed. */
  if (fd < 0) {
    fprintf(stderr, "Error: failed to connect to %s:%s\n", ip, port);
    freeaddrinfo(res);
    return -1;
  }

  /* Optional diagnostic: print the remote endpoint we connected to. */
  {
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);

    if (getpeername(fd, (struct sockaddr*)&ss, &slen) == 0) {
      print_sockaddr("Connected to", (struct sockaddr*)&ss, slen);
    }
  }

  /* Release address list memory from getaddrinfo(). */
  freeaddrinfo(res);

  /* Return connected socket. Caller owns and must close it. */
  return fd;
}

/* ---------- Networking: create listen socket in server mode ----------
 *
 * Steps:
 *   1) Resolve local bind ip/port via getaddrinfo() with AI_PASSIVE.
 *   2) Iterate candidates until bind()+listen() succeeds.
 *   3) Set SO_REUSEADDR (best effort) before bind().
 *   4) Return listening fd, or -1 on total failure.
 *
 * Note:
 *   This function only creates the listening socket.
 *   Accepting a client is done separately by accept_one_client().
 */
static int create_listen_socket(const char* ip, const char* port) {
  /* Address resolution controls. */
  struct addrinfo hints;

  /* Linked list of bind candidates from getaddrinfo(). */
  struct addrinfo* res = NULL;

  /* Iterator over each candidate address. */
  struct addrinfo* rp;

  /* Listening socket fd if successful, else -1. */
  int fd = -1;

  /* Return code for getaddrinfo(). */
  int rc;

  /* Socket option value for SO_REUSEADDR. */
  int yes = 1;

  memset(&hints, 0, sizeof(hints));

  /* Accept IPv4 or IPv6 bind addresses. */
  hints.ai_family = AF_UNSPEC;

  /* TCP stream sockets only. */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  /* AI_PASSIVE tells resolver this is for bind/listen usage.
   * Since we pass an explicit ip, that specific local address is used.
   */
  hints.ai_flags = AI_PASSIVE;

  /* Resolve local bind ip + port into concrete sockaddr candidates. */
  rc = getaddrinfo(ip, port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(server): %s\n", gai_strerror(rc));
    return -1;
  }

  /* Try each candidate until socket + bind + listen all succeed. */
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    /* Create socket for this address candidate. */
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) {
      continue;
    }

    /* Best-effort reuse of local address after restart.
     * Prevents frequent "Address already in use" during development.
     * Failure here is non-fatal, so cast result to void.
     */
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes,
                     (socklen_t)sizeof(yes));

    /* Try to bind this socket to candidate local address. */
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      /* Bind worked; now mark it as listening. */
      if (listen(fd, BACKLOG) == 0) {
        /* Fully successful listen socket found. */
        break;
      }
    }

    /* Either bind or listen failed; close and try next candidate. */
    close(fd);
    fd = -1;
  }

  /* No candidate worked. */
  if (fd < 0) {
    fprintf(stderr, "Error: failed to bind/listen on %s:%s\n", ip, port);
    freeaddrinfo(res);
    return -1;
  }

  /* Optional diagnostic: show final local endpoint actually bound. */
  {
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);

    if (getsockname(fd, (struct sockaddr*)&ss, &slen) == 0) {
      print_sockaddr("Listening on", (struct sockaddr*)&ss, slen);
    }
  }

  /* Free resolver output now that socket is ready. */
  freeaddrinfo(res);

  /* Return listening fd. Caller owns and must close it. */
  return fd;
}

/* ---------- IO loop shared by client/server once connected ----------
 *
 * Threaded design:
 *   - sender thread:   stdin  -> socket
 *   - receiver thread: socket -> stdout
 */
static int run_duplex_loop(int sockfd) {
  DuplexState st;
  pthread_t sender_thr;
  pthread_t receiver_thr;
  int sender_created = 0;
  int receiver_created = 0;
  int rc = -1;

  st.sockfd = sockfd;
  st.stop_requested = 0;
  st.had_error = 0;

  if (pthread_mutex_init(&st.lock, NULL) != 0) {
    fprintf(stderr, "pthread_mutex_init failed.\n");
    return -1;
  }

  if (pthread_create(&sender_thr, NULL, sender_thread_main, &st) != 0) {
    fprintf(stderr, "pthread_create(sender) failed.\n");
    pthread_mutex_destroy(&st.lock);
    return -1;
  }
  sender_created = 1;

  if (pthread_create(&receiver_thr, NULL, receiver_thread_main, &st) != 0) {
    fprintf(stderr, "pthread_create(receiver) failed.\n");
    duplex_request_stop(&st, 1);
    if (sender_created) {
      pthread_join(sender_thr, NULL);
    }
    pthread_mutex_destroy(&st.lock);
    return -1;
  }
  receiver_created = 1;

  if (receiver_created) {
    pthread_join(receiver_thr, NULL);
  }

  /* Cooperative stop: sender checks stop flag every select() timeout. */
  duplex_request_stop(&st, 0);

  if (sender_created) {
    pthread_join(sender_thr, NULL);
  }

  rc = (st.had_error == 0) ? 0 : -1;

  pthread_mutex_destroy(&st.lock);
  return rc;
}

/* ---------- Server accept helper ----------
 *
 * Blocks until one incoming client connects (or accept fails).
 * Prints peer endpoint for visibility.
 */
static int accept_one_client(int listen_fd) {
  /* Generic storage large enough for either IPv4 or IPv6 peer address. */
  struct sockaddr_storage peer;

  /* Length must be initialized to buffer size before accept(). */
  socklen_t peer_len = (socklen_t)sizeof(peer);

  /* accept() blocks until:
   *   - an incoming connection arrives, or
   *   - an error occurs (e.g., interrupted syscall, invalid fd).
   *
   * On success:
   *   returns new connected client socket fd.
   * On failure:
   *   returns -1 and sets errno.
   */
  int client_fd = accept(listen_fd, (struct sockaddr*)&peer, &peer_len);

  if (client_fd < 0) {
    perror("accept");
    return -1;
  }

  /* Log the remote endpoint for operator visibility. */
  print_sockaddr("Accepted connection from", (struct sockaddr*)&peer, peer_len);

  /* Caller owns this connected socket and must close it later. */
  return client_fd;
}

/* ---------- fd lifecycle helper ----------
 *
 * Safely close fd only when valid (>= 0), then poison to -1.
 * This reduces risk of double-close and accidental descriptor reuse.
 */
static void close_if_valid(int* fd) {
  /* Defensive: ensure caller passed a valid pointer. */
  if (fd == NULL) {
    return;
  }

  /* Only close non-negative descriptors.
   * Convention in this file:
   *   -1 = not open / already closed
   *   >=0 = potentially open
   */
  if (*fd >= 0) {
    /* Best-effort close.
     * We intentionally do not fail hard on close() errors here because:
     *   1) cleanup paths should remain simple and robust
     *   2) the descriptor is no longer used after this point
     */
    (void)close(*fd);

    /* Invalidate descriptor to prevent accidental reuse/double-close. */
    *fd = -1;
  }
}

/* ---------- client mode ----------
 *
 * Connect -> duplex loop -> close -> return status.
 * Cleanup is unconditional.
 */
static int run_client_mode(const Options* opt) {
  int sockfd = -1;
  int result = EXIT_FAILURE;

  sockfd = connect_client(opt->ip, opt->port);
  if (sockfd < 0) {
    close_if_valid(&sockfd);
    return EXIT_FAILURE;
  }

  if (run_duplex_loop(sockfd) == 0) {
    result = EXIT_SUCCESS;
  }

  close_if_valid(&sockfd);
  return result;
}

/* ---------- server mode ----------
 *
 * Listen -> accept one client -> duplex loop -> close both -> return status.
 * Cleanup is unconditional on every path.
 */
static int run_server_mode(const Options* opt) {
  int listen_fd = -1;
  ServerState st;
  pthread_t stdin_thr;

  listen_fd = create_listen_socket(opt->ip, opt->port);
  if (listen_fd < 0) {
    return EXIT_FAILURE;
  }

  st.clients = NULL; /* Start with empty active-client list. */
  if (pthread_mutex_init(&st.clients_lock, NULL) != 0) {
    close_if_valid(&listen_fd);
    return EXIT_FAILURE;
  }

  if (pthread_mutex_init(&st.stdout_lock, NULL) != 0) {
    pthread_mutex_destroy(&st.clients_lock);
    close_if_valid(&listen_fd);
    return EXIT_FAILURE;
  }

  /* Optional convenience thread: server stdin is broadcast to all clients. */
  if (pthread_create(&stdin_thr, NULL, server_stdin_broadcast_main, &st) == 0) {
    (void)pthread_detach(stdin_thr); /* Detached: no join needed later. */
  } else {
    fprintf(stderr, "Warning: failed to start server stdin thread.\n");
  }

  for (;;) {
    int client_fd = accept_one_client(listen_fd);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue; /* Retry accept if interrupted by signal. */
      }
      break; /* Hard accept failure: exit loop and clean up. */
    }

    server_add_client(&st, client_fd);

    /* Per-client worker gets its own heap args to avoid stack lifetime issues.
     */
    ClientWorkerArgs* wa = (ClientWorkerArgs*)malloc(sizeof(ClientWorkerArgs));
    if (wa == NULL) {
      fprintf(stderr, "Error: out of memory for client worker args.\n");
      server_remove_client(&st, client_fd);
      close_if_valid(&client_fd);
      continue;
    }

    wa->state = &st;
    wa->client_fd = client_fd;

    pthread_t worker;
    if (pthread_create(&worker, NULL, server_client_worker_main, wa) != 0) {
      fprintf(stderr, "Error: pthread_create(client worker) failed.\n");
      free(wa);
      server_remove_client(&st, client_fd);
      close_if_valid(&client_fd);
      continue;
    }

    (void)pthread_detach(worker); /* Detached worker self-cleans on return. */
  }

  /* Close all tracked clients before destroying locks. */
  server_close_all_clients(&st);
  pthread_mutex_destroy(&st.stdout_lock);
  pthread_mutex_destroy(&st.clients_lock);
  close_if_valid(&listen_fd);

  return EXIT_FAILURE;
}

/* Public entrypoint used by main.c */
int netcat_run(int argc, char** argv) {
  Options opt;

  signal(SIGPIPE, SIG_IGN);

  if (parse_args(argc, argv, &opt) != 0) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (opt.listen_mode) {
    return run_server_mode(&opt);
  } else {
    return run_client_mode(&opt);
  }
}
