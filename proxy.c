#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* Headers */
static const char *user_agent = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection = "Proxy-Connection: close\r\n";

/* Cache Block Structure */
typedef struct cache_node {
    char url[MAXLINE];
    char content[MAX_OBJECT_SIZE];
    int content_size;
    int access_time;
    struct cache_node *next_ptr;
} cache_node_t;

/* Web Cache Structure */
typedef struct {
    cache_node_t *first;
    int current_size;
    int readers;
    sem_t read_lock;
    sem_t write_lock;
} cache_manager;

cache_manager global_cache;
int access_counter = 0;

/* Function Declarations */
void handle_sigpipe(int sig);
void process_request(int client_fd);
void send_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void extract_uri(char *uri, char *host, char *path, char *port, char *request_header);
void *handle_client(void *arg);
void initialize_cache(cache_manager *cache);
cache_node_t *check_cache(cache_manager *cache, char *uri);
void add_to_cache(cache_manager *cache, char *uri, char *buffer, int size);
int update_counter() { return ++access_counter; }
void remove_oldest(cache_manager *cache);
void process_headers(rio_t *client_rio, int server_fd);

/* Main Function */
int main(int argc, char **argv) {
    int listen_fd, *client_fd;
    char host[MAXLINE], port[MAXLINE];
    socklen_t client_len;
    struct sockaddr_storage client_addr;
    pthread_t thread_id;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, handle_sigpipe);
    initialize_cache(&global_cache);
    listen_fd = Open_listenfd(argv[1]);

    while (1) {
        client_len = sizeof(client_addr);
        client_fd = Malloc(sizeof(int));
        *client_fd = Accept(listen_fd, (SA *)&client_addr, &client_len);

        Getnameinfo((SA *) &client_addr, client_len, host, MAXLINE, port, MAXLINE, 0);
        printf("Connection from %s:%s\n", host, port);

        Pthread_create(&thread_id, NULL, handle_client, client_fd);
    }
}

/* Cache Initialization */
void initialize_cache(cache_manager *cache) {
    cache->first = NULL;
    cache->current_size = 0;
    cache->readers = 0;
    Sem_init(&cache->read_lock, 0, 1);
    Sem_init(&cache->write_lock, 0, 1);
}

/* Cache Lookup */
cache_node_t *check_cache(cache_manager *cache, char *uri) {
    cache_node_t *current = NULL;

    P(&cache->read_lock);
    cache->readers++;
    if (cache->readers == 1)
        P(&cache->write_lock);
    V(&cache->read_lock);

    for (current = cache->first; current; current = current->next_ptr) {
        if ((strcmp(current->url, uri) == 0) || 
            (uri[strlen(uri)-1] == '/' && strncmp(uri, current->url, strlen(uri)-1) == 0)) {
            current->access_time = update_counter();
            break;
        }
    }

    P(&cache->read_lock);
    cache->readers--;
    if (cache->readers == 0)
        V(&cache->write_lock);
    V(&cache->read_lock);

    return current;
}

/* Add to Cache */
void add_to_cache(cache_manager *cache, char *uri, char *buffer, int size) {
    if (size > MAX_OBJECT_SIZE)
        return;

    P(&cache->write_lock);

    while (cache->current_size + size > MAX_CACHE_SIZE)
        remove_oldest(cache);

    cache_node_t *new_node = malloc(sizeof(cache_node_t));
    strcpy(new_node->url, uri);
    memcpy(new_node->content, buffer, size);
    new_node->content_size = size;
    new_node->access_time = update_counter();

    new_node->next_ptr = cache->first;
    cache->first = new_node;
    cache->current_size += size;

    V(&cache->write_lock);
}

/* Remove Oldest Cache Entry */
void remove_oldest(cache_manager *cache) {
    cache_node_t *prev = NULL, *current = cache->first;
    cache_node_t *to_remove_prev = NULL, *to_remove = NULL;

    if (!current) return;

    int oldest = current->access_time;
    to_remove = current;

    while (current) {
        if (current->access_time < oldest) {
            oldest = current->access_time;
            to_remove_prev = prev;
            to_remove = current;
        }
        prev = current;
        current = current->next_ptr;
    }

    if (to_remove_prev)
        to_remove_prev->next_ptr = to_remove->next_ptr;
    else
        cache->first = to_remove->next_ptr;

    cache->current_size -= to_remove->content_size;
    free(to_remove);
}

/* Client Handler Thread */
void *handle_client(void *arg) {
    int client_fd = *((int *)arg);
    Pthread_detach(pthread_self());
    Free(arg);

    process_request(client_fd);
    Close(client_fd);
    return NULL;
}

/* SIGPIPE Handler */
void handle_sigpipe(int sig) {
    printf("Received SIGPIPE signal\n");
    return;
}

/* Process Client Request */
void process_request(int client_fd) {
    char buffer[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], path[MAXLINE], port[MAXLINE], request_header[MAXLINE];
    rio_t client_rio, server_rio;

    Rio_readinitb(&client_rio, client_fd);
    if (!Rio_readlineb(&client_rio, buffer, MAXLINE)) return;
    sscanf(buffer, "%s %s %s", method, uri, version);
    
    if (strcasecmp(method, "GET")) {
        send_error(client_fd, method, "501", "Not Implemented", 
                  "This proxy only supports GET requests");
        return;
    }

    cache_node_t *cached = check_cache(&global_cache, uri);
    if (cached) {
        Rio_writen(client_fd, cached->content, cached->content_size);
        return;
    }

    extract_uri(uri, host, path, port, request_header);
    int server_fd = Open_clientfd(host, port);
    Rio_writen(server_fd, request_header, strlen(request_header));
    process_headers(&client_rio, server_fd);

    Rio_readinitb(&server_rio, server_fd);
    char object_buffer[MAX_OBJECT_SIZE] = {0};
    int total_size = 0;
    size_t n;

    while ((n = Rio_readlineb(&server_rio, buffer, MAXLINE)) > 0) {
        if (total_size + n < MAX_OBJECT_SIZE) {
            memcpy(object_buffer + total_size, buffer, n);
        }
        total_size += n;
        Rio_writen(client_fd, buffer, n);
    }

    Close(server_fd);

    if (total_size <= MAX_OBJECT_SIZE) {
        add_to_cache(&global_cache, uri, object_buffer, total_size);
    }
}

/* Process HTTP Headers */
void process_headers(rio_t *client_rio, int server_fd) {
    char buf[MAXLINE];

    sprintf(buf, "%s", user_agent);
    Rio_writen(server_fd, buf, strlen(buf));
    sprintf(buf, "%s", connection_hdr);
    Rio_writen(server_fd, buf, strlen(buf));
    sprintf(buf, "%s", proxy_connection);
    Rio_writen(server_fd, buf, strlen(buf));

    while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) break;

        if (strncmp(buf, "Host:", 5) == 0 || 
            strncmp(buf, "User-Agent:", 11) == 0 ||
            strncmp(buf, "Connection:", 11) == 0 ||
            strncmp(buf, "Proxy-Connection:", 17) == 0)
            continue;

        Rio_writen(server_fd, buf, strlen(buf));
    }

    Rio_writen(server_fd, "\r\n", 2);
}

/* Send Error Response */
void send_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE];
    
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<html><title>Proxy Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=\"ffffff\">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>Web Proxy Server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}

/* URI Parser */
void extract_uri(char *uri, char *host, char *path, char *port, char *request_header) {
    strcpy(port, "80");

    char *host_start = strstr(uri, "//");
    host_start = host_start ? host_start + 2 : uri;

    char *host_end = host_start;
    while (*host_end != '/' && *host_end != ':' && *host_end != '\0') host_end++;

    strncpy(host, host_start, host_end - host_start);
    host[host_end - host_start] = '\0';

    if (*host_end == ':') {
        char *port_start = host_end + 1;
        char *port_end = strchr(port_start, '/');
        if (port_end) {
            strncpy(port, port_start, port_end - port_start);
            port[port_end - port_start] = '\0';
            strcpy(path, port_end);
        } else {
            strcpy(port, port_start);
            strcpy(path, "/");
        }
    } else {
        strcpy(path, host_end);
    }

    sprintf(request_header, "GET %s HTTP/1.0\r\nHost: %s\r\n", path, host);
}
