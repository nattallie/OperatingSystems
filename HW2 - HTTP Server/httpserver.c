#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "wq.h"

/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue;
int num_threads;
int server_port;
char *server_files_directory;
char *server_proxy_hostname;
int server_proxy_port;
pthread_t* threads;
int num_workers;
pthread_mutex_t lock; 
pthread_cond_t continue_accepting;
pthread_cond_t got_job;
int finish_work;
int num_finished_threads;

typedef struct proxy_helper {
  int read_fd;
  int write_fd;
  pthread_mutex_t* lock;
  pthread_cond_t* cond;
  int* finished;
} proxy_helper_t;

#define BUF_SIZE 1024
#define REQUEST_MAX_SIZE 8192

void send_file_content(int stream_fd, int file_fd, int file_size) {
  int already_done = 0;
  int tmp = 0;
  char* buf = malloc(BUF_SIZE + 1);
  
  while (already_done < file_size) {
    buf[0] = '\n';
    tmp = read(file_fd, buf, BUF_SIZE);
    if (tmp <= 0) return;
    already_done += tmp;
    http_send_data(stream_fd, buf, BUF_SIZE);
  }
}


void write_response_file(char* path, int stream_fd, off_t size) {
  int file_fd = open(path, O_RDONLY);

  if (file_fd == -1) {
    fprintf(stderr, "Cannot open the file: %s\n", path);
    return;
  } 

  http_start_response(stream_fd, 200);
  http_send_header(stream_fd, "Content-Type", http_get_mime_type(path));

  char len[BUF_SIZE];
  sprintf(len, "%lu", size);
  http_send_header(stream_fd, "Content-Length", len);
  http_end_headers(stream_fd);

  send_file_content(stream_fd, file_fd, atoi(len));
  close(file_fd);
}

void write_404_response(int fd) {
  http_start_response(fd, 404);
  http_send_header(fd, "Content-Type", "text/html");
  http_end_headers(fd);
  http_send_string(fd, "404 Not Found\n");
}

int dir_has_index(char* full_path, char* ind_path) {
  strcpy(ind_path, full_path);

  if (ind_path[strlen(ind_path) - 1] != '/') {
    strcat(ind_path, "/");
  }

  strcat(ind_path, "index.html");
  FILE* file = fopen(ind_path, "r");

  if (file == NULL)
    return 0;

  return 1;
}

void write_ref(char* name, int fd) {
  char buf[BUF_SIZE];
  buf[0] = '\n';
  strcpy(buf, "<a href=\"./");
  strcat(buf, name);
  strcat(buf, "\">");
  strcat(buf, name);
  strcat(buf, "</a><br>");
  http_send_string(fd, buf);
}

void write_children_refs(char* path, int fd) {
  DIR* curDir;
  struct dirent* entry;
  curDir = opendir(path);
  if (curDir == NULL) {
    fprintf(stderr, "Cannot open directory: %s\n", path);
    return;
  }

  http_send_string(fd, "<a href=\"../\">Parent directory</a><br>");
  while ((entry = readdir(curDir)) != NULL) {
    if ((strcmp(entry->d_name, "..") != 0)  && (strcmp(entry->d_name, ".") != 0)) {
      write_ref(entry->d_name, fd);
    }
  }
  closedir(curDir);
}

void write_response_dir(char* path, int fd) {
  http_start_response(fd, 200);
  http_send_header(fd, "Content-Type", "text/html");
  http_end_headers(fd);
  write_children_refs(path, fd);
}

/*
 * Reads an HTTP request from stream (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 */
void handle_files_request(int fd) {
  struct http_request *request = http_request_parse(fd);
  if (request == NULL) {
    return;
  }

  if (strcmp(request->method, "GET") == 0) {
    char full_path[BUF_SIZE];
    char ind_path[BUF_SIZE];
    full_path[0] = ind_path[0] = '\n';
    strcpy(full_path, server_files_directory);
    strcat(full_path, request->path + 1);
    struct stat statbuf;

    if (stat(full_path, &statbuf) == 0) {
      if (S_ISREG(statbuf.st_mode)) {
        write_response_file(full_path, fd, statbuf.st_size);
      } else if (S_ISDIR(statbuf.st_mode)) {
        if (dir_has_index(full_path, ind_path) && stat(ind_path, &statbuf) == 0) {
          write_response_file(ind_path, fd, statbuf.st_size);
        } else {
          write_response_dir(full_path, fd);
        }
      } else {
        write_404_response(fd);
      }
    } else {
      write_404_response(fd);
    }
  } else {
    fprintf(stderr, "I only handle GET method, yours is: %s\n", request->method);
  }
}

void* proxy_worker(void* helper) {
  proxy_helper_t* help_struct = helper;

  char* req_string = malloc(REQUEST_MAX_SIZE + 1);
  while (1) {
    req_string[0] = '\n';
    int num_buf = 0;

    while ((num_buf = read(help_struct->read_fd, req_string, REQUEST_MAX_SIZE)) > 0) {
      http_send_data(help_struct->write_fd, req_string, num_buf);
      req_string[0] = '\n';
    }
    
    if (num_buf < 0) {
      close(help_struct->read_fd);
      close(help_struct->write_fd);
      break;
    }
  }

  free(req_string);
  pthread_mutex_lock(help_struct->lock);
  pthread_cond_signal(help_struct->cond);
  pthread_mutex_unlock(help_struct->lock);
  return NULL;
}


void read_write_for_proxy(int for_client, int for_proxy) {
  pthread_t threads[2];
  proxy_helper_t helpers[2];
  pthread_mutex_t proxy_lock;
  pthread_cond_t proxy_cond;
  pthread_mutex_init(&proxy_lock, NULL);
  pthread_cond_init(&proxy_cond, NULL);

  helpers[0].read_fd = for_client;
  helpers[0].write_fd = for_proxy;
  helpers[0].lock = &proxy_lock;
  helpers[0].cond = &proxy_cond;

  helpers[1].write_fd = for_client;
  helpers[1].read_fd = for_proxy;
  helpers[1].lock = &proxy_lock;
  helpers[1].cond = &proxy_cond;

  pthread_create(threads, NULL, proxy_worker, helpers);
  pthread_create(threads + 1, NULL, proxy_worker, helpers + 1);

  pthread_mutex_lock(&proxy_lock);
  pthread_cond_wait(&proxy_cond, &proxy_lock);
  pthread_mutex_unlock(&proxy_lock);

  pthread_mutex_destroy(&proxy_lock);
  pthread_cond_destroy(&proxy_cond);
}


/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target. HTTP requests from the client (fd) should be sent to the
 * proxy target, and HTTP responses from the proxy target should be sent to
 * the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 */
void handle_proxy_request(int fd) {

  /*
  * The code below does a DNS lookup of server_proxy_hostname and 
  * opens a connection to it. Please do not modify.
  */
  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  struct hostent *target_dns_entry = gethostbyname2(server_proxy_hostname, AF_INET);

  int client_socket_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (client_socket_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    exit(ENXIO);
  }

  char *dns_address = target_dns_entry->h_addr_list[0];

  memcpy(&target_address.sin_addr, dns_address, sizeof(target_address.sin_addr));
  int connection_status = connect(client_socket_fd, (struct sockaddr*) &target_address,
      sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    http_request_parse(fd);

    http_start_response(fd, 502);
    http_send_header(fd, "Content-Type", "text/html");
    http_end_headers(fd);
    http_send_string(fd, "<center><h1>502 Bad Gateway</h1><hr></center>");
    return;

  }

  read_write_for_proxy(fd, client_socket_fd);
}

void* work_per_thread() {
  int sock_fd;
  void (*handler)(int) = NULL;

  while (1) {
    pthread_mutex_lock(&lock);
    if (finish_work) {
      break;
    }
    pthread_mutex_unlock(&lock); 

    sock_fd = wq_pop(&work_queue);

    pthread_mutex_lock(&lock);
    num_workers++;
    handler = work_queue.request_handler;
    pthread_mutex_unlock(&lock);

    handler(sock_fd);
    close(sock_fd);

    pthread_mutex_lock(&lock);
    num_workers--;
    if (!finish_work && work_queue.size == 0) {
      pthread_cond_signal(&continue_accepting);
    }
    pthread_mutex_unlock(&lock);
  }

  num_finished_threads++;
  pthread_cond_signal(&continue_accepting);
  pthread_mutex_unlock(&lock);
  return NULL;
}

void init_thread_pool(int num_threads) {
    pthread_mutex_init(&lock, NULL); 
    pthread_cond_init(&continue_accepting, NULL);
    pthread_cond_init(&got_job, NULL);
    
    threads = malloc(sizeof(pthread_t) * num_threads);
    num_workers = 0;
    finish_work = 0;
    num_finished_threads = 0;

    for (int i = 0; i < num_threads; i++) {
      pthread_create(threads + i, NULL, work_per_thread, NULL);
      pthread_detach(*(threads+i));
    }
}

void handle_for_single(int client_socket_number, struct sockaddr_in* client_address, void (*request_handler)(int)) {
  request_handler(client_socket_number);
  close(client_socket_number);

  printf("Accepted connection from %s on port %d\n",
          inet_ntoa(client_address->sin_addr),
          client_address->sin_port);
}

void finish_thread_pool() {
  pthread_mutex_lock(&lock);
  finish_work = 1;
  while(num_finished_threads != num_threads) {
    pthread_cond_wait(&continue_accepting, &lock);
  }
  pthread_mutex_unlock(&lock);

  pthread_mutex_destroy(&lock);
  pthread_cond_destroy(&got_job);
  pthread_cond_destroy(&continue_accepting);
  free(threads);
}

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *socket_number, void (*request_handler)(int)) {

  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  if (bind(*socket_number, (struct sockaddr *) &server_address,
        sizeof(server_address)) == -1) {
    perror("Failed to bind on socket");
    exit(errno);
  }

  if (listen(*socket_number, 1024) == -1) {
    perror("Failed to listen on socket");
    exit(errno);
  }

  printf("Listening on port %d...\n", server_port);

  if (num_threads != -1) {
    init_thread_pool(num_threads);
    wq_init(&work_queue, request_handler);
  }

  while (1) {
    if (num_threads != -1) {
      pthread_mutex_lock(&lock);
      while (num_workers == num_threads) {
        pthread_cond_wait(&continue_accepting, &lock);
      }
      pthread_mutex_unlock(&lock);
    } 

    client_socket_number = accept(*socket_number,
        (struct sockaddr *) &client_address,
        (socklen_t *) &client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

    if (num_threads == -1) {
      handle_for_single(client_socket_number, &client_address, request_handler);
    } else {
      wq_push(&work_queue, client_socket_number);
      pthread_mutex_lock(&lock);
      pthread_cond_broadcast(&got_job);
      pthread_mutex_unlock(&lock);
    }
  }

  if (num_threads != -1) {
    finish_thread_pool();
  }

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char *USAGE =
  "Usage: ./httpserver --files www_directory/ --port 8000 [--num-threads 5]\n"
  "       ./httpserver --proxy inst.eecs.berkeley.edu:80 --port 8000 [--num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  signal(SIGINT, signal_callback_handler);

  /* Default settings */
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  num_threads = -1;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      free(server_files_directory);
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char *num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL && server_proxy_hostname == NULL) {
    fprintf(stderr, "Please specify either \"--files [DIRECTORY]\" or \n"
                    "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

  serve_forever(&server_fd, request_handler);
  return EXIT_SUCCESS;
}
