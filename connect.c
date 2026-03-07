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

#include "netcat.h"

#include <arpa/inet.h>   // inet_ntop
#include <errno.h>       // errno
#include <netdb.h>       // getaddrinfo, freeaddrinfo, gai_strerror
#include <netinet/in.h>  // sockaddr_in, sockaddr_in6
#include <signal.h>      // signal, SIGPIPE
#include <stdio.h>       // fprintf, printf, perror, fgets, fwrite, fflush
#include <stdlib.h>      // EXIT_SUCCESS, EXIT_FAILURE, strtol
#include <string.h>      // memset, strcmp, strerror
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
 * Core event loop:
 *   - Wait with select() for readability on:
 *       a) socket fd
 *       b) stdin fd (while stdin still open)
 *
 *   Socket readable:
 *     recv() > 0  -> write bytes to stdout
 *     recv() == 0 -> peer performed orderly shutdown; return success
 *     recv() < 0  -> error; return failure
 *
 *   stdin readable:
 *     read() > 0  -> send all bytes to peer (loop until fully sent)
 *     read() == 0 -> local EOF:
 *                    shutdown(SHUT_WR), disable stdin monitoring,
 *                    continue receiving from peer
 *     read() < 0  -> error; return failure
 *
 * Why select():
 *   Avoids threads while still allowing simultaneous inbound/outbound traffic.
 */
static int run_duplex_loop(int sockfd) {
  /* Tracks whether local stdin is still active.
   * 1 = monitor stdin and forward input to socket.
   * 0 = stdin reached EOF (or was intentionally closed), so stop monitoring it.
   */
  int stdin_open = 1;

  /* Buffer for bytes read from local stdin before sending to peer. */
  char inbuf[IO_BUFFER_SIZE];

  /* Buffer for bytes received from socket before printing to stdout. */
  char sockbuf[IO_BUFFER_SIZE];

  /* Main event loop:
   * Use select() to wait for whichever input source becomes readable first.
   */
  for (;;) {
    fd_set readfds; /* Read-interest set for select(). */
    int maxfd;      /* Highest fd in readfds; select() needs maxfd + 1. */
    int sel_rc;     /* Return code from select(). */

    /* Start with an empty descriptor set each iteration. */
    FD_ZERO(&readfds);

    /* Always monitor the connected socket for incoming network data. */
    FD_SET(sockfd, &readfds);
    maxfd = sockfd;

    /* Monitor stdin only while still open.
     * After local EOF, stdin is removed and only socket receive continues.
     */
    if (stdin_open) {
      FD_SET(STDIN_FILENO, &readfds);
      if (STDIN_FILENO > maxfd) {
        maxfd = STDIN_FILENO;
      }
    }

    /* Block until at least one monitored fd is readable.
     * Timeout is NULL -> wait indefinitely.
     */
    sel_rc = select(maxfd + 1, &readfds, NULL, NULL, NULL);

    /* Handle select() failure.
     * EINTR means interrupted by signal; safe to retry loop.
     */
    if (sel_rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("select");
      return -1;
    }

    /* If socket is readable, receive bytes from peer. */
    if (FD_ISSET(sockfd, &readfds)) {
      ssize_t n = recv(sockfd, sockbuf, sizeof(sockbuf), 0);

      if (n < 0) {
        /* recv() failed. */
        perror("recv");
        return -1;
      } else if (n == 0) {
        /* Peer performed orderly shutdown (connection closed). */
        fprintf(stderr, "Peer closed connection.\n");
        return 0;
      } else {
        /* recv() returned n bytes.
         * Write exactly n bytes to stdout (handle partial fwrite).
         */
        size_t off = 0;
        while (off < (size_t)n) {
          size_t remaining = (size_t)n - off;
          size_t written = fwrite(sockbuf + off, 1, remaining, stdout);

          /* fwrite returning 0 indicates output failure. */
          if (written == 0) {
            fprintf(stderr, "Error writing to stdout.\n");
            return -1;
          }

          off += written;
        }

        /* Force immediate display of received data. */
        fflush(stdout);
      }
    }

    /* If stdin is still active and readable, forward local input to socket. */
    if (stdin_open && FD_ISSET(STDIN_FILENO, &readfds)) {
      ssize_t n = read(STDIN_FILENO, inbuf, sizeof(inbuf));

      if (n < 0) {
        /* Local input read failure. */
        perror("read(stdin)");
        return -1;
      } else if (n == 0) {
        /* Local EOF (Ctrl-D or end of piped input).
         * Half-close socket write side: no more outgoing data.
         * Keep reading from peer until it closes.
         */
        if (shutdown(sockfd, SHUT_WR) < 0) {
          perror("shutdown(SHUT_WR)");
          return -1;
        }

        stdin_open = 0;
        fprintf(stderr, "Local input closed; waiting for peer data...\n");
      } else {
        /* Send all n bytes to peer.
         * send() may write fewer bytes than requested, so loop until complete.
         */
        size_t sent_total = 0;
        while (sent_total < (size_t)n) {
          ssize_t s =
              send(sockfd, inbuf + sent_total, (size_t)n - sent_total, 0);

          if (s < 0) {
            perror("send");
            return -1;
          }

          sent_total += (size_t)s;
        }
      }
    }
  }
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
  int client_fd = -1;
  int result = EXIT_FAILURE;

  listen_fd = create_listen_socket(opt->ip, opt->port);
  if (listen_fd < 0) {
    close_if_valid(&client_fd);
    close_if_valid(&listen_fd);
    return EXIT_FAILURE;
  }

  client_fd = accept_one_client(listen_fd);
  if (client_fd < 0) {
    close_if_valid(&client_fd);
    close_if_valid(&listen_fd);
    return EXIT_FAILURE;
  }

  if (run_duplex_loop(client_fd) == 0) {
    result = EXIT_SUCCESS;
  }

  close_if_valid(&client_fd);
  close_if_valid(&listen_fd);
  return result;
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
