/* Generic */
#include <errno.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Network */
#include <netdb.h>
#include <sys/socket.h>

/* This flag controls termination of the main loop. */
volatile sig_atomic_t keep_going = 1;

// Get host information (used to establishConnection)
struct addrinfo *getHostInfo(char* host, char* port) {
  int r;
  struct addrinfo hints, *getaddrinfo_res;
  // Setup hints
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if ((r = getaddrinfo(host, port, &hints, &getaddrinfo_res))) {
    fprintf(stderr, "[getHostInfo:21:getaddrinfo] %s\n", gai_strerror(r));
    return NULL;
  }

  return getaddrinfo_res;
}

// Establish connection with host
int establishConnection(struct addrinfo *info) {
  if (info == NULL) return -1;

  int clientfd;
  for (;info != NULL; info = info->ai_next) {
    if ((clientfd = socket(info->ai_family,
                           info->ai_socktype,
                           info->ai_protocol)) < 0) {
      perror("[establishConnection:35:socket]");
      continue;
    }

    if (connect(clientfd, info->ai_addr, info->ai_addrlen) < 0) {
      close(clientfd);
      perror("[establishConnection:42:connect]");
      continue;
    }

    freeaddrinfo(info);
    return clientfd;
  }

  freeaddrinfo(info);
  return -1;
}

// Send GET request
void GET(int clientfd, char* host, char* port, char *path) {
  char req[1000] = {0};
  sprintf(req, "GET %s HTTP/1.1\r\nHost: %s:%s\r\nAccept: */*\r\n\r\n", path, host, port);
  //fprintf(stderr, "%s\n", req);
  send(clientfd, req, strlen(req), MSG_NOSIGNAL);
}

/* The signal handler just clears the flag and re-enables itself. */
void catch_alarm (int sig)
{
  keep_going = 0;
}

ssize_t try_recv(int sockfd, void *buf, size_t len, int flags)
{
  ssize_t count;
  while (1) {
     errno = 0;
     count = recv(sockfd, buf, len, flags);
     if (!count || !(keep_going && count < 0 && errno == EINTR))
       break;
  }
  return count;
}

#define STRING_SIZE 100

int main(int argc, char **argv) {
  int clientfd;
  int items;
  //char buf[BUF_SIZE];
  char* buf = NULL;
  char proto[STRING_SIZE];
  char host[STRING_SIZE];
  char port[STRING_SIZE];
  char request_method[] = "GET";
  char request_path[STRING_SIZE];
  char line[STRING_SIZE];
  char status_msg[STRING_SIZE];
  int buf_offset;
  size_t buf_size;
  int length;
  int status = -1;
  int ret = 0;
  int count = -1;
  int prev_count = 0;
  int line_breaks = 0;
  int in_body = 0;
  int body_remaining = 0;
  int duration = 0;
  long total_received = 0;
  int requests = 0;
  int recv_len;
  int connections = 1;
  int successful_requests = 0;
  char* recv_loc;
  struct timespec time_begin, time_end;
  size_t maxGroups = 5;

  regex_t re;
  regmatch_t groupArray[maxGroups];
  unsigned int m;
  char * cursor;

  if (argc != 6) {
    fprintf(stderr, "USAGE: ./httpclient <hostname> <port> <request path> <buf size> <duration>\n");
    return 1;
  }

  buf_size = atoi(argv[4]);
  errno = 0;
  buf = malloc(buf_size);
  if (errno != 0) {
    perror("malloc failed");
    return 1;
  }

  duration = atoi(argv[5]);
  char* pattern = "^([^:]+)://([^:/]+):([0-9]+)(/.+)";
  if (regcomp(&re, pattern, REG_EXTENDED)) {
    perror("regcomp failed");
    return 1;
  }
  cursor = argv[1];
  if (regexec(&re, cursor, maxGroups, groupArray, 0) == 0) {
    if (groupArray[1].rm_so != (size_t)-1) {
      cursor = argv[1] + groupArray[1].rm_so;
      strncpy(proto, cursor, groupArray[1].rm_eo - groupArray[1].rm_so);
      proto[groupArray[1].rm_eo - groupArray[1].rm_so] = 0;
    }
    if (groupArray[2].rm_so != (size_t)-1) {
      cursor = argv[1] + groupArray[2].rm_so;
      strncpy(host, cursor, groupArray[2].rm_eo - groupArray[2].rm_so);
      host[groupArray[2].rm_eo - groupArray[2].rm_so] = 0;
    }
    if (groupArray[3].rm_so != (size_t)-1) {
      cursor = argv[1] + groupArray[3].rm_so;
      strncpy(port, cursor, groupArray[3].rm_eo - groupArray[3].rm_so);
      port[groupArray[3].rm_eo - groupArray[3].rm_so] = 0;
    }
    if (groupArray[4].rm_so != (size_t)-1) {
      cursor = argv[1] + groupArray[4].rm_so;
      strncpy(request_path, cursor, groupArray[4].rm_eo - groupArray[4].rm_so);
      request_path[groupArray[4].rm_eo - groupArray[4].rm_so] = 0;
    }
  } else {
    strncpy(host, argv[1], STRING_SIZE);
    strncpy(port, argv[2], STRING_SIZE);
    strncpy(request_path, argv[3], STRING_SIZE);
  }
  regfree(&re);

  // Establish connection with <hostname>:<port>
  clientfd = establishConnection(getHostInfo(host, port));
  if (clientfd == -1) {
    fprintf(stderr,
            "[main:73] Failed to connect to: %s:%s%s \n",
            host, port, argv[3]);
    return 3;
  }

  keep_going = 1;

  struct sigaction sa, oldsa;
  sigset_t mask;
  if (clock_gettime(CLOCK_MONOTONIC, &time_begin) < 0) {
    perror("clock_gettime");
    return 1;
  }

  signal(SIGALRM, catch_alarm);
  alarm(duration);
  buf_offset = 0;
  count = 0;
  int buf_offset_prev = buf_offset;

  while (keep_going) {
    requests += 1;
    GET(clientfd, host, port, request_path);

    length = -1;
    in_body = 0;
    while (1) {
      if (buf_offset == count) {
        recv_len = buf_size;
        recv_loc = buf;
        count = 0;
        prev_count = 0;
        buf_offset = 0;
        memset(buf, 0, buf_size);
      } else if (buf_offset == buf_offset_prev) {
        recv_len = buf_size - count;
        recv_loc = buf + count;
        prev_count = count;
      } else {
        recv_len = 0;
        recv_loc = NULL;
        prev_count = 0;
      }
      if (recv_len) {
        count = try_recv(clientfd, recv_loc, recv_len, 0);
        if (count < 0) {
          perror("recv failed");
          ret = 1;
          goto out;
        }
        if (!count)
          break;
        total_received += count;
        count += prev_count;
      }
      buf_offset_prev = buf_offset;
      if (!count)
        break;

      while (!in_body && 1 == (items = sscanf(buf + buf_offset, "%[^\r\n]", line))) {
        if (status < 0) {
          items = sscanf(buf + buf_offset, "HTTP/1.1 %d %[^\r\n]", &status, status_msg);
          if (items != 2) {
            fprintf(stderr, "bad status line: \"%s\"\n", line);
            ret = 1;
            goto out;
          }
        } else if (length < 0) {
          items = sscanf(buf + buf_offset, "Content-Length: %d", &length);
        }
        buf_offset += strlen(line);
        line_breaks = 0;
        while (buf_offset < count && 1 == (items = sscanf(buf + buf_offset, "%[\r\n]", line))) {
          line_breaks = strlen(line);
          buf_offset += line_breaks;
          if (line_breaks == 4) {
            in_body = 1;
            break;
          }
        }
      }
      if (!in_body) {
        continue;
      }

      if (length < 0) {
          fprintf(stderr, "missing Content-Length\n");
          ret = 1;
          goto out;
      }

      if (status < 0) {
          fprintf(stderr, "missing HTTP status line\n");
          ret = 1;
          goto out;
      }

      if (!(status >= 200 && status < 300)) {
        fprintf(stderr, "HTTP status: %d\n", status);
        close(clientfd);
        clientfd = establishConnection(getHostInfo(host, port));
        if (clientfd == -1) {
          fprintf(stderr,
                  "[main:73] Failed to connect to: %s:%s%s \n",
                  host, port, request_path);
          ret = 3;
          goto out;
        }
        connections += 1;
        break;
      }

      body_remaining = length;
      body_remaining -= count - buf_offset;
      while (body_remaining > 0 && (count = try_recv(clientfd, buf, (buf_size > body_remaining) ? body_remaining : buf_size, 0)) > 0) {
        if (count > body_remaining) {
          fprintf(stderr, "received unexpected data after body of length %d\n", length);
          ret = 1;
          goto out;
        } else {
          total_received += count;
          body_remaining -= count;
        }
      }
      successful_requests += 1;
      buf_offset = 0;
      count = 0;
      break;
    }
  }

  if (clock_gettime(CLOCK_MONOTONIC, &time_end) < 0) {
    perror("clock_gettime");
    return 1;
  }

  out:
/*
    fprintf(stderr, "keep_going %d\n", keep_going);
    fprintf(stderr, "connections %d\n", connections);
    fprintf(stderr, "requests %d\n", requests);
    fprintf(stderr, "successful requests %d\n", successful_requests);
    fprintf(stderr, "total_received %ld\n", total_received);
    fprintf(stderr, "start time %ld\n", time_begin.tv_sec);
    fprintf(stderr, "end time %ld\n", time_end.tv_sec);
    fprintf(stderr, "time %ld\n", time_end.tv_sec - time_begin.tv_sec);
*/
    fprintf(stdout, "%f\n", total_received / 1000000000.0f * 8 / (time_end.tv_sec - time_begin.tv_sec));

    free(buf);
    close(clientfd);
  return ret;
}
