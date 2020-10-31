#define _GNU_SOURCE

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

#define LIBHTTP_REQUEST_MAX_SIZE 8192
pthread_t *thread_pool = NULL;

struct proxy_session_info {
  char *server_hostname;
  int server_port;
  int client_fd, server_fd;
  pthread_cond_t cond;
  pthread_mutex_t client_mut, server_mut;  
};

/* HELPER FUNCTIONS */
void http_create_dirlist(int n,
			 struct dirent **fnames,
			 char *buffer) {
  printf("Creating directory list...\n");
  char *li = NULL;
  int len;
  strcpy(buffer, "<h1>Files</h1>"
  	         "<ul>"
	         "<li><a href='../'>"
		 "Parent directory"
		 "</a></li>");
  for (int i = 0; i < n; i++) {
    if (strcmp(fnames[i]->d_name, ".") == 0
    	|| strcmp(fnames[i]->d_name, "..") == 0
	|| (fnames[i]->d_type != DT_REG && fnames[i]->d_type != DT_DIR)) {
      continue;
    }
    /* <li><a href="fnames[i]">fnames[i]</a></li> */
    len = 24 + 2*strlen(fnames[i]->d_name);
    li = malloc(sizeof(char) * len + 1);
    strcpy(li, "<li><a href='");
    strcat(li, fnames[i]->d_name);
    strcat(li, "'>");
    strcat(li, fnames[i]->d_name);
    strcat(li, "</a></li>");
    
    strcat(buffer, li);
    free(li);
  }
  strcat(buffer, "</ul>");
}

void http_send_file(int fd, char* path) {
  printf("Sending file %s to socket %d...\n", path, fd);
  char *buffer;
  FILE* file;
  long length;

  file = fopen(path, "rb");
  if (file == NULL) {
    printf("Failed to open file %s\n", path);
  } else {
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer = malloc(length + 1);
    if (buffer) {
      fread(buffer, 1, length, file);
      buffer[length] = '\0';

      http_start_response(fd, 200);
      http_send_header(fd, "Content-Type", http_get_mime_type(path));
      http_end_headers(fd);
      http_send_string(fd, buffer);
    }
    free(buffer);
  }
  fclose(file);
}

void http_send_directory(int fd, char* path) {
  printf("Sending directory %s to socket %d...\n", path, fd);
  char buffer[1000];
  char *index_path;
  int len = strlen(path) + 12, n;
  struct dirent **fname_list;
  struct stat info;

  index_path = malloc(len + 1);
  strcpy(index_path, path);
  strcat(index_path, "/index.html");
  index_path[len] = '\0';

  /* Directory contains an index.html file? */
  if (stat(index_path, &info) == 0) {
    http_send_file(fd, index_path);
  } else {
    /* Create page with links to all files in the directory */
    n = scandir(path, &fname_list, NULL, alphasort);
    if (n < 0) {
      printf("Error occurred while reading directory %s\n", path);
    } else {
      http_create_dirlist(n, fname_list, buffer);
      http_start_response(fd, 200);
      http_send_header(fd, "Content-Type", "text/html");
      http_end_headers(fd);
      http_send_string(fd, buffer);
    }
  }
  free(index_path);
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
  /*
   * TODO: Your solution for Task 1 goes here! Feel free to delete/modify *
   * any existing code.
   */
  printf("Handling files request from socket %d...\n", fd);
  struct http_request *request = http_request_parse(fd);
  struct stat info;

  /* Get the absolute path to the requested file or directory*/
  char *abs_path;

  /* Something here may cause occasional SEGFAULT */
  int len = strlen(server_files_directory)
  	    + strlen(request->path);

  abs_path = malloc(len + 1);
  strcpy(abs_path, server_files_directory);
  strcat(abs_path, request->path);
  abs_path[len] = '\0';
   
  /* Does the file/directory exist? */
  if (stat(abs_path, &info) == 0) {
    /* Is it a file or a directory? */
    if (S_ISREG(info.st_mode)) {
      /* Handle regular file */
      http_send_file(fd, abs_path);
    } else if (S_ISDIR(info.st_mode)) {
      /* Handle directory */
      http_send_directory(fd, abs_path);
    }
  } else {
    /* Send a 404 response */
    http_start_response(fd, 404);
  }
  free(abs_path);
  free(request);
}

/* PROXY CLIENT THREAD FUNCTION */
void *proxy_client_thread_func(void *arg) {
  printf("Entering proxy client thread function...\n");
  char buffer[LIBHTTP_REQUEST_MAX_SIZE];
  char first_line[LIBHTTP_REQUEST_MAX_SIZE];
  char second_line[LIBHTTP_REQUEST_MAX_SIZE];
  char port_num[8];
  int n_bytes;
  struct proxy_session_info *info = arg;

  while (1) {
    // clear the buffer
    memset(&buffer, '\0', sizeof(buffer));

    // read request from client
    pthread_mutex_lock(&info->client_mut);
    n_bytes = read(info->client_fd, buffer, sizeof(buffer));
    pthread_mutex_unlock(&info->client_mut);
    
    if (n_bytes < 0) {
      // client_fd must be closed, quit
      pthread_mutex_lock(&info->server_mut);
      close(info->server_fd);
      pthread_mutex_unlock(&info->server_mut);
      return NULL;
    } else if (n_bytes == 0) {
      // keep checking for data
      sleep(1);
    } else {
      // write the request to the server
      // TODO: re-structure the request so it has the right hostname header
      printf("request:\n%s\n", buffer);

      memset(&first_line, '\0', sizeof(first_line));
      memset(&second_line, '\0', sizeof(second_line));
      int i, j;
      
      for (i = 0; i < n_bytes && buffer[i] != '\n'; i++) {
        
      }
      i++;
      memcpy(&first_line, buffer, i); // copy first line into first_line
      for (j = i; j < n_bytes && buffer[j] != '\n'; j++) {

      }
      j++;
      memcpy(&second_line, buffer + i, j - i);

      printf("First line:\n%s", first_line);
      printf("Second line:\n%s", second_line);

      memset(&second_line, '\0', sizeof(second_line));
      strcpy(second_line, "Host: ");
      strcat(second_line, "www.");
      strcat(second_line, info->server_hostname);
      strcat(second_line, ":");
      
      sprintf(port_num, "%d", info->server_port);

      strcat(second_line, port_num);
      strcat(second_line, "\r\n");

      printf("New second line:\n%s", second_line);

      pthread_mutex_lock(&info->server_mut);
      write(info->server_fd, first_line, strlen(first_line));
      write(info->server_fd, second_line, strlen(second_line));
      printf("New rest of request:\n%s", buffer + j);
      n_bytes = write(info->server_fd, buffer + j, n_bytes);
      pthread_cond_signal(&info->cond);
      pthread_cond_wait(&info->cond, &info->server_mut);
      pthread_mutex_unlock(&info->server_mut);
    }
  }
}

/* PROXY SERVER THREAD FUNCTION */
void *proxy_server_thread_func(void *arg) {
  printf("Entering proxy server thread function...\n");
  int chunk_size = 200;
  int full_size = chunk_size;
  char static_buf[chunk_size];
  char *dyn_buf = malloc(chunk_size);
  int n_bytes;
  struct proxy_session_info *info = arg;

  while (1) {
    // read from the server
    pthread_mutex_lock(&info->server_mut);
    printf("reading response...\n");
    n_bytes = read(info->server_fd, static_buf, chunk_size);
    pthread_mutex_unlock(&info->server_mut);

    if (n_bytes < 0) {
      // server socket must be closed, so quit
      pthread_mutex_lock(&info->client_mut);
      close(info->client_fd);
      pthread_mutex_unlock(&info->client_mut);
      return NULL;
    } else if (n_bytes == 0) {
      // keep checking for data
      sleep(1);
    } else {
      memcpy(dyn_buf, static_buf, chunk_size);
      pthread_mutex_lock(&info->server_mut);
      memset(static_buf, '\0', chunk_size);
      while ( (n_bytes = read(info->server_fd, static_buf, chunk_size)) > 0) {
        pthread_mutex_unlock(&info->server_mut);
        printf("full size: %d, n_bytes: %d\n", full_size, n_bytes);
        full_size += n_bytes;
        dyn_buf = realloc(dyn_buf, full_size);
        strcat(dyn_buf, static_buf);
        memset(static_buf, '\0', chunk_size);
        pthread_mutex_lock(&info->server_mut);
      }
      pthread_mutex_unlock(&info->server_mut);
      dyn_buf[full_size] = '\0';

      // write response to the client
      printf("response:\n%s\n", dyn_buf);
      pthread_mutex_lock(&info->client_mut);
      /*if (n_bytes < 0) {
        perror("client socket closed\n");
        pthread_mutex_lock(&info->server_mut);
        close(info->server_fd);
        pthread_mutex_unlock(&info->server_mut);
        return NULL;
      }*/

      http_send_string(info->client_fd, dyn_buf);
      free(dyn_buf);
      //printf("wrote %d bytes to client socket\n", n_bytes);
      pthread_cond_signal(&info->cond);
      pthread_cond_wait(&info->cond, &info->client_mut);
      pthread_mutex_unlock(&info->client_mut);
    }
  }
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

  /* 
  * TODO: Your solution for task 3 belongs here! 
  */
  struct proxy_session_info *info = malloc(sizeof(struct proxy_session_info));
  pthread_t client_thread, server_thread;

  info->server_hostname = malloc(sizeof(server_proxy_hostname));
  memcpy(info->server_hostname, server_proxy_hostname, strlen(server_proxy_hostname));
  info->server_hostname[strlen(server_proxy_hostname)] = '\0';
  info->server_port = server_proxy_port;
  info->client_fd = fd;
  info->server_fd = client_socket_fd;
  pthread_cond_init(&info->cond, NULL);
  pthread_mutex_init(&info->client_mut, NULL);
  pthread_mutex_init(&info->server_mut, NULL);
  
  pthread_create(&client_thread, NULL, &proxy_client_thread_func, (void*) info);
  pthread_create(&server_thread, NULL, &proxy_server_thread_func, (void*) info);
  pthread_join(client_thread, NULL);
  pthread_join(server_thread, NULL);
}

/* THREAD FUNCTION */
void *thread_function(void *arg) {
  printf("Entering the thread function...\n");
  int connection_socket;
  void (*request_handler)(int) = arg;
  while (1) {
    connection_socket = wq_pop(&work_queue);
    request_handler(connection_socket);
    printf("In thread function, closing socket %d\n", connection_socket);
    close(connection_socket);
  }
}


void init_thread_pool(int num_threads, void (*request_handler)(int)) {
  /*
   * TODO: Part of your solution for Task 2 goes here!
   */
  printf("Initializing thread pool with %d threads...\n", num_threads);
  wq_init(&work_queue);
  thread_pool = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
  for (int i = 0; i < num_threads; i++) {
    pthread_create(&thread_pool[i], NULL, &thread_function, request_handler);
  }
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

  init_thread_pool(num_threads, request_handler);

  while (1) {
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

    // TODO: Change me?
    wq_push(&work_queue, client_socket_number); // add each new connection to the queue

    /* request_handler(client_socket_number);
    close(client_socket_number); */

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);
  }

  /* Deallocate thread pool */
  free(thread_pool);

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
