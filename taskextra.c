#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/time.h>

#include "PA2-task2.h"

#define DEFAULT_CLIENT_THREADS 4
#define NUM_REQUESTS 1000
#define TIMEOUT_MS 100

const char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = NUM_REQUESTS;

/* Protocol 3 (par) allows unidirectional data flow over an unreliable channel. */

void to_physical_layer(context_t *ctx, frame *s)
{
    sendto(ctx->socket_fd, s, sizeof(frame), 0, (struct sockaddr *)&ctx->server_addr, ctx->addr_len);
}

void from_physical_layer(context_t *ctx, frame *r)
{
    recvfrom(ctx->socket_fd, r, sizeof(frame), 0, (struct sockaddr *)&ctx->server_addr, &ctx->addr_len);
}

void wait_for_event(context_t *ctx, event_type *event, int timeout_ms) {
    struct epoll_event events[MAX_EVENTS];
    int ret = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, timeout_ms);
    if (ret == 0) {
        *event = timeout;
    } else if (ret > 0) {
        *event = frame_arrival;
    } else {
        perror("epoll_wait");
        exit(EXIT_FAILURE);
    }
}

void sender3(context_t *ctx, long long *tx_cnt, long long *rx_cnt, long long *total_rtt) {
    seq_nr next_frame_to_send = 0;
    frame s, r;
    packet buffer;
    event_type event;

    memcpy(buffer.data, "DATA", MAX_PKT);

    for (int i = 0; i < num_requests; i++) {
        // Build frame
        s.client_id = ctx->client_id;
        s.kind = data;
        s.seq = next_frame_to_send;
        s.ack = 0;
        s.info = buffer;

        to_physical_layer(ctx, &s);
        (*tx_cnt)++;
        
        struct timeval send_time;
        gettimeofday(&send_time, NULL);

        while (1) {
            wait_for_event(ctx, &event, TIMEOUT_MS);

            if (event == frame_arrival) {
                from_physical_layer(ctx, &r);
                if (r.ack == next_frame_to_send && r.client_id == ctx->client_id) {
                    struct timeval recv_time;
                    gettimeofday(&recv_time, NULL);

                    long long rtt = (recv_time.tv_sec - send_time.tv_sec) * 1000000LL +
                                    (recv_time.tv_usec - send_time.tv_usec);
                    *total_rtt += rtt;

                    (*rx_cnt)++;
                    inc(next_frame_to_send);
                    break; // Next packet
                }
            } else if (event == timeout) {
                printf("[Client %d] Timeout, retransmitting frame %d\n", ctx->client_id, next_frame_to_send);
                to_physical_layer(ctx, &s); // retransmit, don't increment tx count
                gettimeofday(&send_time, NULL); // reset timer
            }
        }
    }
}

void receiver3(context_t *ctx) {
    seq_nr frame_expected = 0;
    frame r, s;
    event_type event;

    while (1) {
        wait_for_event(ctx, &event, -1);

        if (event == frame_arrival) {
            from_physical_layer(ctx, &r);

            if (r.seq == frame_expected) {
                printf("[Server] Received frame from client %d, seq %d\n", r.client_id, r.seq);
                inc(frame_expected);
            }

            // Send ACK
            s.client_id = r.client_id;
            s.kind = ack;
            s.seq = 0;
            s.ack = r.seq;
            to_physical_layer(ctx, &s);
        }
    }
}

typedef struct
{
    int client_id;
    int socket_fd;
    int epoll_fd;
    struct sockaddr_in server_addr;
    socklen_t addr_len;
} client_thread_data_t;

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;

    context_t ctx = {
        .socket_fd = data->socket_fd,
        .epoll_fd = data->epoll_fd,
        .server_addr = data->server_addr,
        .addr_len = sizeof(data->server_addr),
        .client_id = data->client_id
    };

    long long tx_cnt = 0, rx_cnt = 0, total_rtt = 0;

    sender3(&ctx, &tx_cnt, &rx_cnt, &total_rtt);

    printf("[Client %d] Finished. tx_cnt = %lld, rx_cnt = %lld, Avg RTT = %lld us\n", data->client_id, tx_cnt, rx_cnt, total_rtt / rx_cnt);

    close(data->socket_fd);
    close(data->epoll_fd);
    return NULL;
}

// === Run Client ===

void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];

    for (int i = 0; i < num_client_threads; i++) {
        thread_data[i].client_id = i;
        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        thread_data[i].epoll_fd = epoll_create1(0);

        thread_data[i].server_addr.sin_family = AF_INET;
        thread_data[i].server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip, &thread_data[i].server_addr.sin_addr);

        struct epoll_event event = { .events = EPOLLIN, .data.fd = thread_data[i].socket_fd };
        epoll_ctl(thread_data[i].epoll_fd, EPOLL_CTL_ADD, thread_data[i].socket_fd, &event);

        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("[Client] All threads finished.\n");
}

// === Run Server ===

void run_server() {
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int epoll_fd = epoll_create1(0);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    struct epoll_event event = { .events = EPOLLIN, .data.fd = server_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    context_t ctx = {
        .socket_fd = server_fd,
        .epoll_fd = epoll_fd,
        .server_addr = client_addr,
        .addr_len = client_len,
        .client_id = -1 // Not used in server
    };

    printf("[Server] Listening on port %d...\n", server_port);
    receiver3(&ctx);

    close(server_fd);
    close(epoll_fd);
}

// === Main ===

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_port = atoi(argv[2]);
        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);
        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}

