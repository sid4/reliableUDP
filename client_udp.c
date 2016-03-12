#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
//new additions
#include <time.h>
#include <sys/time.h>
//buffer size for reading from socket
#define BUFFER_SIZE 1007
#define MIN_ARGS 6
//argument for host name
#define ARG_HOSTNAME argv[1]
//argument for port
#define ARG_PORT argv[2]
//argument for input file
#define ARG_FILE argv[3]
//argument for dropping packet
#define ARG_PACKET_DROP argv[4]
//argument for delay
#define ARG_PACKET_DELAY argv[5]
//Max no. of files to be read from a file
#define MAX_FILES 100
//Max characters allowed for a file name
#define MAX_FILE_NAME_SIZE 100
//Max time to wait (in micro seconds) for chunked response from server
//TODO change it to 120000000
#define MAX_WAIT_FOR_RESPONSE_CHUNK 120000000
//Max size of ack no arr
#define SIZE_ACK_ARR 6
//Max acknowledge no supported
#define MAX_ACK_NO 99998
#define DEBUG_MODE 0

int dropPacket(int seq_no, long double *packets_dropped, int divisor) {
	int drop = 0;
	if (divisor <= 0) {
		return drop;
	}
	struct timeval prob_drop;
	gettimeofday(&prob_drop, NULL);
	unsigned long rand = prob_drop.tv_sec * 1000000 + prob_drop.tv_usec;
	drop = rand % divisor == 0 ? 1 : 0;
	if (drop) {
		*packets_dropped = *packets_dropped + 1;
		if (DEBUG_MODE) {
			printf("\ndropping packet with seq no:%d", seq_no);
		}
	}
	return drop;
}

/*
 * Prints the error to the standard error stream and exits the program
 */
void signal_error(char *err_msg) {
	fprintf(stderr, err_msg);
	fprintf(stderr, "\nshutting down");
	exit(1);
}
int main(int argc, char *argv[]) {
	if (argc < MIN_ARGS) {
		//if min. arguments are not present
		if (argc == (MIN_ARGS - 1)) {
			signal_error(
					"Insufficient arguments.\n Please enter the delay required (in ms).  \nExample of proper invocation with delay,but no packet loss: ./client localhost 50413 file.txt 0 12\nExample of proper invocation with delay and packet loss: ./client localhost 50413 file.txt 2 12\n Please note packet loss=0 means no packet loss\ndelay=0 no delay\npacket loss=1, drop all packets\npacket loss>1 probabilistic dropping");
		} else if (argc == (MIN_ARGS - 2)) {
			signal_error(
					"Insufficient arguments.\n Please enter the packet loss (>1) and delay required (in ms). \n Example of proper invocation with delay,but no packet loss: ./client localhost 50413 file.txt 0 12\nExample of proper invocation with delay and packet loss: ./client localhost 50413 file.txt 2 12\n Please note packet loss=0 means no packet loss\ndelay=0 no delay\npacket loss=1, drop all packets\npacket loss>1 probabilistic dropping");

		} else if (argc == (MIN_ARGS - 3)) {
			signal_error(
					"Insufficient arguments. \nPlease enter a file name,delay and drop components. \nExample of proper invocation with delay,but no packet loss: ./client localhost 50413 file.txt 0 12\nExample of proper invocation with delay and packet loss: ./client localhost 50413 file.txt 2 12\n Please note packet loss=0 means no packet loss\ndelay=0 no delay\npacket loss=1, drop all packets\npacket loss>1 probabilistic dropping");
		} else if (argc == (MIN_ARGS - 4)) {
			signal_error(
					"Insufficient arguments. \nPlease enter a port no of the server to connect to, file name, delay and drop components. \n Example of proper invocation with delay,but no packet loss: ./client localhost 50413 file.txt 0 12\nExample of proper invocation with delay and packet loss: ./client localhost 50413 file.txt 2 12\n Please note packet loss=0 means no packet loss\ndelay=0 no delay\npacket loss=1, drop all packets\npacket loss>1 probabilistic dropping");
		}

		else {
			signal_error(
					"Insufficient arguments. \nPlease enter host name, port no of the server to connect to,file name. \n Example of proper invocation: Example of proper invocation with delay,but no packet loss: ./client localhost 50413 file.txt 0 12\nExample of proper invocation with delay and packet loss: ./client localhost 50413 file.txt 2 12\n Please note packet loss=0 means no packet loss\ndelay=0 no delay\npacket loss=1, drop all packets\npacket loss>1 probabilistic dropping");
		}

	} else {
		int socket_file_descr, port;
		long double packets_dropped = 0;
		unsigned int sockaddr_in_length;
		struct hostent *server_entry;
		struct sockaddr_in server, f;
		port = atoi(ARG_PORT);
		sockaddr_in_length = sizeof(struct sockaddr_in);
		//AF_INET is the domain, SOCK_DGRAM is the communication style
		socket_file_descr = socket(AF_INET, SOCK_DGRAM, 0);
		if (socket_file_descr <= -1) {
			signal_error("Failed creating a socket for the client");
		}
		//fetching host information
		server_entry = gethostbyname(ARG_HOSTNAME);
		if (server_entry == NULL) {
			signal_error("Host not found");
		}
		//clearing the struct
		memset(&server, 0, sizeof(server));
		server.sin_family = AF_INET;
		memmove((char *) &server.sin_addr.s_addr, (char *) server_entry->h_addr,
		server_entry->h_length);
		//assigning the network byte equivalent port no
		server.sin_port = htons(port);
		//sending request
		if (sendto(socket_file_descr, ARG_FILE,
		strlen(ARG_FILE)+1,0,(const struct sockaddr *)&server,sockaddr_in_length)<0) {
			signal_error("Error in sending request to server");
		}
		//total packet received
		long double packets_received = 0;
		//no of bytes received from socket
		int n = 1;
		//buffer used to read the response from server
		unsigned char response[BUFFER_SIZE];
		//start and end time of client
		struct timeval start, end;
		unsigned long time_taken;
		//start measuring time
		gettimeofday(&start, NULL);
		//setting up the max waiting time for chunk of a response to arrive
		fd_set set;
		struct timeval max_wait;
		FD_ZERO(&set);
		FD_SET(socket_file_descr, &set);
		max_wait.tv_sec = 0;
		max_wait.tv_usec = MAX_WAIT_FOR_RESPONSE_CHUNK;
		bzero(response, sizeof(response));
		int expectedSequenceNo = 0;
		int ack = 1;

		while (select(socket_file_descr + 1, &set, NULL, NULL, &max_wait) == 1) {
			n = recvfrom(socket_file_descr, response, sizeof(response), 0,
					(struct sockaddr *) &f, &sockaddr_in_length);
			if (n == -1) {
				signal_error("error in receiving data");
			}
			if (DEBUG_MODE) {
				printf("\nexpected paket:%d", expectedSequenceNo);
				printf("\nreceived paket:%d", atoi(response));
			}
			//update the packet received count
			packets_received += 1;

			if (atoi(response) == expectedSequenceNo
					&& !dropPacket(atoi(response), &packets_dropped,
							atoi(ARG_PACKET_DROP))) {
				//if its the expected response and packet is not to be dropped
				expectedSequenceNo = (expectedSequenceNo + 1) % MAX_ACK_NO;
				unsigned char ackNo[SIZE_ACK_ARR];
				//sending ack for packet received
				snprintf(ackNo, sizeof(ackNo), "%05d", ack);
				if(ARG_PACKET_DELAY) {
					//making it high latency communication

					struct timespec sleepTime, t2;
					sleepTime.tv_sec = 0;
					//1000000ns= 1ms
					sleepTime.tv_nsec = 1000000*atoi(ARG_PACKET_DELAY);
					nanosleep(&sleepTime, &t2);
				}
				int bytesSent = sendto(socket_file_descr, ackNo, sizeof(ackNo),
				0, (const struct sockaddr *) &server,
				sockaddr_in_length);

				if (bytesSent == -1) {
					signal_error("error in sending ack");
				}
				if(DEBUG_MODE) {
					printf("\nsending ack:%d", ack);
				}
				ack = (expectedSequenceNo + 1) % MAX_ACK_NO;

				if(!DEBUG_MODE) {
					printf("%s",response+SIZE_ACK_ARR+2);
				}

			}

			else {
				//if its the not the expected response or packet has been dropped
				unsigned char ackNo[SIZE_ACK_ARR];
				//sending ack for packet received
				snprintf(ackNo, sizeof(ackNo), "%05d", ack-1);
				if(ARG_PACKET_DELAY) {
					//making it high latency communication
					struct timespec sleepTime, t2;
					sleepTime.tv_sec = 0;
					//1000000ns= 1ms
					sleepTime.tv_nsec = 1000000*atoi(ARG_PACKET_DELAY);
					nanosleep(&sleepTime, &t2);
				}

				int bytesSent = sendto(socket_file_descr, ackNo, sizeof(ackNo),
				0, (const struct sockaddr *) &server,
				sockaddr_in_length);

				if (bytesSent == -1) {
					signal_error("error in sending ack");
				}
				if(DEBUG_MODE) {
					printf("\nsending ack:%d", ack-1);
				}
			}

			bzero(response, sizeof(response));
			//setting up the max waiting time for chunk of a response to arrive
			FD_ZERO(&set);
			FD_SET(socket_file_descr, &set);
			max_wait.tv_sec = 0;
			max_wait.tv_usec = MAX_WAIT_FOR_RESPONSE_CHUNK;
		}
		//end measuring time
		gettimeofday(&end, NULL);
		time_taken = (end.tv_sec - start.tv_sec) * 1000000.0;
		time_taken += (end.tv_usec - start.tv_usec);
		printf("\n**Time taken for receiving the response(micro seconds):%lu",
				time_taken);
		printf("\n**Total packets received:%Lf\n", packets_received);
		printf("\n**Simulated packet drops:%Lf\n", packets_dropped);
		if (packets_dropped == 0) {
			printf("\n**Simulated packet loss: 0%%\n");
		} else {
			printf("\n**Simulated packet loss: %Lf%%\n",
					(packets_dropped / packets_received) * 100);
		}
	}
	return 0;
}

