#ifndef TASK2_H
#define TASK2_H

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#define MAX_PKT 4 /* determines packet size in bytes */
#define MAX_EVENTS 64

typedef enum {false, true} boolean; /* boolean type */
typedef unsigned int seq_nr; /* sequence of ack numbers */

typedef struct {unsigned char data[MAX_PKT];} packet; /* packet definition */
typedef enum {data, ack, nak} frame_kind; /* frame_kind definition */

typedef struct __attribute__((packed)) /* frames that are transported in this layer */
{
    uint8_t client_id; 
    frame_kind kind; /* what kind of frame is it? */
    seq_nr seq; /* sequence number */
    seq_nr ack; /* acknowledgement number */
    packet info; /* the network layer packet */
} frame;

typedef enum {frame_arrival, cksum_err, timeout} event_type;

typedef struct
{
    int socket_fd;
    int epoll_fd; 
    struct sockaddr_in server_addr;
    socklen_t addr_len;
    int client_id;
} context_t;

// Protocol 3 functions
void sender3(context_t *ctx, long long *tx_cnt, long long *rx_cnt);
void receiver3(context_t *ctx);

/* Pass the frame to the physical layer for transmission. */
void to_physical_layer(context_t *ctx, frame *s);

/* Go get an inbound frame from the physical layer and copy it to r. */
void from_physical_layer(context_t *ctx, frame *r);

void wait_for_event(context_t *ctx, event_type *event);

/* Start the clock running and enable the timeout event. */
void start_timer(seq_nr k);

/* Stop the clock and disable the timeout event. */
void stop_timer(seq_nr k);

/* Macro inc is expanded in-line: Increment k circularly. */
#define inc(k) if(k < MAX_SEQ) k = k + 1; else k = 0

#endif TASK2_H
