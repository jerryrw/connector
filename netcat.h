#ifndef NETCAT_H
#define NETCAT_H

#include <sys/socket.h>  // struct sockaddr, socklen_t

typedef struct Options {
  int listen_mode;
  const char* ip;
  const char* port;
} Options;

/* Prototypes from netcat.c (excluding main) */
static void print_usage(const char* prog);
static int parse_args(int argc, char** argv, Options* opt);
static void print_sockaddr(const char* label, const struct sockaddr* sa,
                           socklen_t salen);
static int connect_client(const char* ip, const char* port);
static int create_listen_socket(const char* ip, const char* port);
static int run_duplex_loop(int sockfd);
static int accept_one_client(int listen_fd);
static void close_if_valid(int* fd);
static int run_client_mode(const Options* opt);
static int run_server_mode(const Options* opt);
int netcat_run(int argc, char** argv);

#endif /* NETCAT_H */
