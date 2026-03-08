#ifndef NETCAT_H
#define NETCAT_H

#include <sys/socket.h>  // struct sockaddr, socklen_t

typedef struct Options {
  int listen_mode;
  const char* ip;
  const char* port;
} Options;

/* Public API only */
int netcat_run(int argc, char** argv);

#endif /* NETCAT_H */
