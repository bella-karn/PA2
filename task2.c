/*
 * # Copyright 2025 University of Kentucky
 * #
 * # Licensed under the Apache License, Version 2.0 (the "License");
 * # you may not use this file except in compliance with the License.
 * # You may obtain a copy of the License at
 * #
 * #      http://www.apache.org/licenses/LICENSE-2.0
 * #
 * # Unless required by applicable law or agreed to in writing, software
 * # distributed under the License is distributed on an "AS IS" BASIS,
 * # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * # See the License for the specific language governing permissions and
 * # limitations under the License.
 * #
 * # SPDX-License-Identifier: Apache-2.0
 * */

/*
 * Please specify the group members here
 *
 *
# Student #1: Gavin Catron
# Student #2: Bella Karn
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define DEFAULT_CLIENT_THREADS 4
#define MAX_PACKET 4 //Max packet size
#define MAX_SEQUENCE 1 //Max sequence number

#define incre(k) ((k) = ((k) + 1) % (MAX_SEQUENCE+1)) //Increment sequence so the number is only 0 or 1
//number = (number + 1) % 2; since MAX_SEQ is already 1 it flips between 0 and 1
char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

typedef struct {
	    unsigned int client_id;
	    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
	    int socket_fd;       /* File descriptor for the client socket connected to the server. */
	    long long send_cnt;
	    long long recv_cnt;
	    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
	    struct sockaddr_in server_addr;
	    long total_messages; /* Total number of messages sent and received. */
	    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
} client_thread_data_t;

typedef unsigned int seq_number_type;
typedef enum {p, ack} frame_kind_type;
typedef struct {
	frame_kind_type frame_kind;
	unsigned int client_id;
	seq_number_type sequence;
	seq_number_type ack;
	char info[MAX_PACKET];
} frame_type; // Frame definition


void *client_thread_func(void *arg) {
	    client_thread_data_t *data = (client_thread_data_t *)arg;
	    struct epoll_event event, events[MAX_EVENTS];
	    char send_buf[sizeof(frame_type)];
	    char recv_buf[sizeof(frame_type)];
	    frame_type *send = (frame_type *)send_buf;
	    frame_type *recv = (frame_type *)recv_buf;
	    struct timeval start, end;
	    seq_number_type next_frame_to_send = 0;
	    // Register the client_thread's socket in its epoll instance
	    event.events = EPOLLOUT; 
	    event.data.fd = data->socket_fd;

	    // Add the socket to the epoll instance
	    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
		    perror("epoll_ctl");
		    pthread_exit(NULL);
	    }

	    // Set initial values to 0
	    data->total_rtt = 0;
	    data->total_messages = 0;
	    data->send_cnt = 0;
	    data->recv_cnt = 0;
	    data->request_rate = 0.0;

	    int j = 0;
	    while (j < num_requests) {
		    // Send message to the serve
		    snprintf(send->info,MAX_PACKET, "ABC"); // 4 bytes instead of 16
		    send->frame_kind = p;
		    send->sequence = next_frame_to_send;
		    send->client_id = data->client_id;
		    gettimeofday(&start, NULL);
		    	if (sendto(data->socket_fd, send_buf, sizeof(frame_type), 0,
						(struct sockaddr *)&data->server_addr, sizeof(data->server_addr)) == -1) {
				perror("error with sendto");
				pthread_exit(NULL);
			}

			// Wait for the socket to be ready for receiving
			event.events = EPOLLIN; // Change to monitor for readiness to receive
			if (epoll_ctl(data->epoll_fd, EPOLL_CTL_MOD, data->socket_fd, &event) == -1) {
				perror("epoll_ctl (EPOLL_CTL_MOD)");
				pthread_exit(NULL);
			}

			// Wait for response from server
			int wait_return = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 100); // 100ms timeout
			if (wait_return == 0) {
				// Timeout occurred
				fprintf(stderr, "Timeout\n");
				continue;
			} else if (wait_return == -1) {
				perror("error waiting for response");
				pthread_exit(NULL);
			}
			//increment send count afterwards to make sure it securely went through
			data->send_cnt++;

			// Receive response from the server
			socklen_t server_len = sizeof(data->server_addr);
			if (recvfrom(data->socket_fd, recv_buf, sizeof(frame_type), 0,
						(struct sockaddr *)&data->server_addr, &server_len) == -1) {
				perror("error with recvfrom");
				pthread_exit(NULL);
			}

			if (recv->ack != next_frame_to_send)
			{
				//damaged frame
				data->send_cnt--;
				printf("damaged frame");
				continue;

			}
			//increment recieve count
			data->recv_cnt++;

			// Get end time and update totals
			gettimeofday(&end, NULL);
			data->total_messages++;
			data->total_rtt += (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
			incre(next_frame_to_send);
			j++;
	    }

	    // Calculate request rate
	    data->request_rate = (float)data->total_messages / (data->total_rtt / 1000000.0);

	    // Close the socket and epoll instance
	    close(data->socket_fd);
	    close(data->epoll_fd);
	    
	    return NULL;
}

void run_client() {
	// establish thread information
	pthread_t threads[num_client_threads];
	client_thread_data_t thread_data[num_client_threads];

	for (int i = 0; i < num_client_threads; i++) {
		thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // create socket using SOCK_DGRAM
		if (thread_data[i].socket_fd == -1) { // check if the socket was created correctly
			perror("error creating socket");
			exit(EXIT_FAILURE);
		}

		thread_data[i].epoll_fd = epoll_create1(0); // create epoll instance
		if (thread_data[i].epoll_fd == -1) { // check if the epoll instance was created correctly
			perror("error creating epoll instance in run_client");
			exit(EXIT_FAILURE);
		}

		thread_data[i].server_addr.sin_family = AF_INET;
		thread_data[i].server_addr.sin_port = htons(server_port);
		// convert IPv4 address from text to binary form
		if (inet_pton(AF_INET, server_ip, &thread_data[i].server_addr.sin_addr) <= 0) {
			perror("error converting IPv4 address");
			exit(EXIT_FAILURE);
		}
	}

	for (int i = 0; i < num_client_threads; i++) {
		        // For each thread, launch a new one and pass the thread datai
			thread_data[i].client_id = i; //client id for each thread
			pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
	}

	long long total_rtt = 0;
	long total_messages = 0;
	float total_request_rate = 0.0;
	long long lost_cnt = 0;
	long long total_send_cnt = 0;
	long long total_recv_cnt = 0;
	for (int i = 0; i < num_client_threads; i++) {
		// Wait for the thread to complete
		pthread_join(threads[i], NULL); 

		total_rtt += thread_data[i].total_rtt;
		total_messages += thread_data[i].total_messages;
		total_request_rate += thread_data[i].request_rate;
		lost_cnt += thread_data[i].send_cnt - thread_data[i].recv_cnt;
		total_send_cnt += thread_data[i].send_cnt;
		total_recv_cnt += thread_data[i].recv_cnt;


		// Close the epoll file descriptor
		close(thread_data[i].epoll_fd);
	}

	printf("Average RTT: %lld us\n", total_rtt / total_messages);
	printf("Total Request Rate: %f messages/s\n", total_request_rate);
	printf("Total Packets Sent: %lld messages\n", total_send_cnt);
	printf("Total Packets Recieved: %lld messages\n", total_recv_cnt);
	printf("Total Packets Lost: %lld messages\n", lost_cnt);
}

void run_server() {
	// initializing event information
	struct epoll_event event;
	struct epoll_event events[MAX_EVENTS];
	int server_fd;
	int epoll_fd;
	char recv_buf[sizeof(frame_type)];
	char send_buf[sizeof(frame_type)];
	frame_type *send = (frame_type *)send_buf;
	frame_type *recv = (frame_type *)recv_buf;

	seq_number_type frame_expected[num_client_threads];
	for (int i = 0; i < num_client_threads; i++)
	{
		frame_expected[i] = 0;
	}

	// Create a UDP socket
	server_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (server_fd == -1) {
		perror("error with creating a socket");
		exit(EXIT_FAILURE);
	}

	// establishing server information
	struct sockaddr_in server_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(server_port);

	// bind the server to the ip and port
	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
		perror("error binding the server to ip port");
		close(server_fd);
		exit(EXIT_FAILURE);
	}

	// create the epoll instance
	epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		perror("error creating epoll instance in run_server()");
		close(server_fd);
		exit(EXIT_FAILURE);
	}

	// add the server socket to the epoll instance
	event.events = EPOLLIN;
	event.data.fd = server_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
		perror("error adding server socket to epoll instance in run_server()");
		close(server_fd);
		close(epoll_fd);
		exit(EXIT_FAILURE);
	}

	while (1) {
		// wait for a client to connect
		int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if (num_events == -1) {
			perror("error with epoll_wait");
			close(server_fd);
			close(epoll_fd);
			exit(EXIT_FAILURE);
		}
		

		 // for each event received, handle it
		 for (int i = 0; i < num_events; i++) {
			 if (events[i].data.fd == server_fd) {
				 // Receive data from a client
				 int clientdata = recvfrom(server_fd, recv_buf, sizeof(frame_type), 0, (struct sockaddr *)&client_addr, &client_len);
				 if (clientdata <= 0) {
					 perror("error with recvfrom");
				 } else {
					 //print data recieved
					 printf("Recieved, client number %u, sequence number %u \n", recv->client_id, recv->sequence);

					 //check if frame is still not damaged
					 if (recv->frame_kind == p && recv->sequence == frame_expected[recv->client_id])
					 {
						 send->frame_kind = ack;
						 send->client_id = recv->client_id;
						 send->ack = recv->sequence;
						 //echo the data back
						 if (sendto(server_fd, send_buf, sizeof(frame_type), 0, (struct sockaddr *)&client_addr, client_len) == -1) {
							 perror("error with sendto");
						 }
						 inc(frame_expected[recv->client_id]); //increment the sequence number
					 }
					 //allow all packets
					 else if (recv->frame_kind == p && recv->sequence != frame_expected[recv->client_id]) 
					 {
						 send->frame_kind = ack;
						 send->client_id = recv->client_id;
						 send->ack = recv->sequence;
						 //echo the data back
						 if (sendto(server_fd, send_buf, sizeof(frame_type), 0, (struct sockaddr *)&client_addr, client_len) == -1) 
						 {
							 perror("error with sendto");
						 }
					 }
				 }
			 }
		 }
	}
	// close the file descriptors
	close(server_fd);
	close(epoll_fd);
}

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "server") == 0) {
		if (argc > 2) server_ip = argv[2];
		if (argc > 3) server_port = atoi(argv[3]);

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

