#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>

#define DEBUG_MODE 0

//minimum arguments required for running server
#define MIN_ARGS 3
//Buffer size for reading client requests
#define READ_BUFFER_SIZE 200
//buffer size for reading from files and writing to socket
#define CHUNKED_READ_WRITE_BUFFER_SIZE 1000
//Size of array used for holding sequence
#define SIZE_SEQUENCE_ARR 6
//Size of delimiter used in header of packets being sent
#define SIZE_DELIMITER 1
//Maximum sequence no. supported
#define MAX_SEQUENCE_NO 99998
#define SLOW_START 1
#define CONGESTION_AVOIDANCE 2
#define FAST_RECOVERY 3
#define WRITER_RUNNING 1
#define READER_RUNNING 2
#define TIMEOUT_RUNNING 3
//TODO increase the timeout to 60s
//max time (microseconds) to wait before reader thread terminates after termination of main writer thread
#define MAX_WAIT_READER 60000000
//TODO increase the timeout to 60s
//max time (microseconds)to wait before time out thread terminates after termination of reader thread
#define MAX_WAIT_TIME_OUT 60000000

typedef struct {
	//sequence no of the packet sent
	unsigned long sequenceNo;
	//stored in microseconds
	unsigned long time_stamp;
	// no of times the packet has been sent
	short sent;
	//pay load
	unsigned char* packet;
} sent;

typedef struct {
	pthread_mutex_t lock;
	//no of consecutive duplicate ack received for the packet
	int count;
	//sequence no of the packet
	int sequence_no;
} duplicate;

typedef struct {
	pthread_mutex_t lock;
	//represent the effective window
	int size;
	int actual_size;
	//no. of element currently in the logs/window
	int inserts;
	int deletes;
	//sent packet
	sent **sentLog;
} log_store;
//parameters for writer thread
struct paramWriteData {
	//file requested by the receiver
	char fileName[READ_BUFFER_SIZE];
	int socket_fd;
	struct sockaddr_in *client;
	socklen_t *client_addr_size;
};
//parameters for timeout thread
typedef struct {
	int socket_fd;
	struct sockaddr_in *client;
	socklen_t *client_addr_size;
	pthread_t *curr_thread;
} paramTimeout;
//parameters for read thread
typedef struct {
	int socket_fd;
	struct sockaddr_in *client;
	socklen_t *client_addr_size;
	pthread_t *curr_thread;
} paramReadData;
//controls the life cycle of threads
static volatile short controller_flag;
static int advertised_window,exitval_timout,exitval_reader;
//timeout interval
static volatile double timeout_interval;
//mode represents slow start, congestion avoidance or fast recovery
static volatile int mode, ssthresh;
//keeps the count of packets sent in different modes
static volatile long double current_mode_packets, total_packets, slow_packets,
		c_a_packets, f_r_packets;
//lock for writing to socket
static pthread_mutex_t write_lock;
//common data structure or log store, used by all the threads
static log_store *logs;
//structure for checking duplicates
static duplicate *dupl;
void mode_switch(int switchTo);
void signal_error(char *err_msg);
void sendError(int socket_fd, struct sockaddr_in *client,
		socklen_t *client_addr_size);
void getRequest(int socket_fd, char *request, int *readc,
		struct sockaddr_in *client, socklen_t *client_addr_size);
void processRequest(int socket_fd);
void *sendData(void* params);
void *readData(void* params);
void *timout(void* params);
void writeToSocket(int socket_fd, struct sockaddr_in *client,
		socklen_t *client_addr_size, char *fileName);
static int insert_arr(sent *sentp, log_store **log_s) {
	//printf("\n %s", "in insert");
	if (((*log_s)->inserts) >= ((*log_s)->size)) {
		//printf("\n %s", "returning 0");

		return 0;
	} else {
		if (DEBUG_MODE) {
			printf("\ninserting seq:%d", sentp->sequenceNo);
		}
		(*log_s)->inserts = ((*log_s)->inserts) + 1;
		int i = 0;
		for (; i < (*log_s)->size; i++) {
			if (((*log_s)->sentLog[i]) == NULL) {
				break;
			}
		}
		if (i < (*log_s)->size) {
			(*log_s)->sentLog[i] = sentp;
			if (DEBUG_MODE) {
				printf("\ninserting at index:%d", i);
			}
		} else {
			if (DEBUG_MODE) {
				printf(
						"\n***************BUG FOUND****************************");
				signal_error(
						"***************BUG FOUND****************************");
			}
			return 0;

		}
		//printf("\n %s", "returning 1");
		return 1;
	}
}

static sent *getLog(long seq_no, log_store **log_s) {
	int i;
	if (DEBUG_MODE) {
		printf("\nin get log");
	}
	for (i = 0; i < (*log_s)->actual_size; i++) {
		if (DEBUG_MODE) {
			printf("\nlooked at index:%d for seq_no:%d", i, seq_no);
		}
		if (DEBUG_MODE && (*log_s)->sentLog[i] != NULL) {
			printf("actual seqno here:%d", ((*log_s)->sentLog[i])->sequenceNo);
		}
		if ((*log_s)->sentLog[i] != NULL
				&& ((*log_s)->sentLog[i])->sequenceNo == seq_no) {
			return (*log_s)->sentLog[i];
		}
	}
	return NULL;
}

static void arr_delete_by_ack(int ack, log_store **log_s) {
	int seq_no = ack - 1;
	if (DEBUG_MODE) {
		printf("\ndeleting seq:%d", seq_no);
	}
	int i;
	for (i = 0; i < (*log_s)->actual_size; i++) {
		if ((*log_s)->sentLog[i] != NULL
				&& ((*log_s)->sentLog[i])->sequenceNo == seq_no) {
			free((*log_s)->sentLog[i]);
			(*log_s)->sentLog[i] = NULL;
			//printf("deleted i:%d", i);
			(*log_s)->inserts = ((*log_s)->inserts) - 1;
			break;
		}
	}
}

static void resize_log_store(log_store **log_s, int size, int advertised_window) {
	if (size > advertised_window) {
		//LastByteSent – LastByteAcked <= min{cwnd, rwnd}, chapter 3 pg. 270
		size = advertised_window;
	}
	if ((*log_s)->actual_size < size) {
		//printf("\nhere");
		sent** temp = (**log_s).sentLog;
		//*((**log_s).sentLog)= (sent *) calloc(size,sizeof(sent *));
		((**log_s).sentLog) = (sent **) calloc(size, sizeof(sent *));
		//printf("\nhere0");
		/*
		 int i=0;
		 for(;i<size;i++){
		 (*log_s)->sentLog[i]=NULL;
		 }
		 */
		memcpy((**log_s).sentLog, temp, (*log_s)->size * sizeof(sent *));
		free(temp);
		(*log_s)->size = size;
		(*log_s)->actual_size = size;
		//printf("\nhere1");

	} else {
		(*log_s)->size = size;
		//printf("here2");

	}
	//printf("here3");

}

/*
 * Prints the error to the standard error stream and exits the program
 */
void signal_error(char *err_msg) {
	fprintf(stderr, err_msg);
	fprintf(stderr, "shutting down");
	exit(1);
}
int main(int argc, char *argv[]) {
	if (!argc < MIN_ARGS) {
		//setvbuf(stdout, NULL, _IOLBF, 0);
		int bind_status, socket_file_descr, port, serving_request;
		struct sockaddr_in server;
		advertised_window = atoi(argv[2]);
		port = atoi(argv[1]);
		socket_file_descr = socket(AF_INET, SOCK_DGRAM, 0);
		if (socket_file_descr == -1) {
			signal_error("Failed creating a socket for the server");
		}
		memset(&server, 0, sizeof(server));
		//populate the server address details
		server.sin_family = AF_INET;
		//assigning the network byte order equivalent of port no
		server.sin_port = htons(port);
		server.sin_addr.s_addr = INADDR_ANY;
		//bind socket
		bind_status = bind(socket_file_descr, (struct sockaddr *) &server,
				sizeof(server));
		if (bind_status == -1) {
			signal_error("Socket binding failed");
		}
		serving_request=0;
		while (1) {
			serving_request++;
			if(serving_request>1 ){
				//preparing server for new request
				pthread_mutex_destroy(&(*logs).lock);
				pthread_mutex_destroy(&(dupl->lock));
				pthread_mutex_destroy(&(write_lock));
				int i;
				for(i=0;i<logs->actual_size;i++){
					if(logs->sentLog[i]!=NULL){
					free(logs->sentLog[i]->packet);
					free(logs->sentLog[i]);
					}
				}
				free(logs);
				free(dupl);
			}
			printf("\nReady to serve new requests");
			//initialization start
			logs = (log_store *) calloc(1, sizeof(log_store));
			//*((*logs).sentLog)= (sent *) calloc(1,sizeof(sent *));
			logs->sentLog = (sent **) calloc(1, sizeof(sent *));
			logs->sentLog[0] = NULL;
			(*logs).size = 1;
			(*logs).actual_size = 1;
			dupl = (duplicate *) calloc(1, sizeof(duplicate));
			dupl->count = 0;
			dupl->sequence_no = -1;
			mode = SLOW_START;
			timeout_interval = 1000000;
			current_mode_packets = total_packets = slow_packets = c_a_packets =
					f_r_packets = 0;
			//setting it to 1/3 advertised window, though the RFC 5681 asks to set it to advertised window. Not following that, since advertised window is constant and we will never really go beyond advertised window, because send traffic cannot exceed min{cwnd,rwnd}
			ssthresh = advertised_window / 3;
			controller_flag = WRITER_RUNNING;
			//initialization end
			if (pthread_mutex_init(&((*logs).lock), NULL) != 0
					&& pthread_mutex_init(&(dupl->lock), NULL) != 0
					&& pthread_mutex_init(&(write_lock), NULL) != 0) {
				signal_error("Initialization of synchronization locks failed");
			}
			processRequest(socket_file_descr);
		}
	} else {
		signal_error(
				"insufficient arguments. Port # and advertised window is required for server boot up.");
	}
}

/**
 * get request from the socket and evaluate/parse
 */
void getRequest(int socket_fd, char *request, int *readc,
		struct sockaddr_in *client, socklen_t *client_addr_size) {
	*client_addr_size = sizeof client;
	*readc = recvfrom(socket_fd, request, READ_BUFFER_SIZE, 0,
			(struct sockaddr *) client, client_addr_size);
	if(DEBUG_MODE){
		printf("request received for:%s",request);
	}
	if (readc < 0) {
		signal_error("Error in reading client request");
	}
}
/**
 * Processes Requests.
 * Reads the request from the socket.
 * In case it is a valid request, checks for the resource in file system.
 * If the resource is found then it sends it to the client
 * If the resource is not found, then sends a message resource is not found
 */
void processRequest(int socket_fd) {
	struct sockaddr_in client;
	socklen_t client_addr_size;
	char request[READ_BUFFER_SIZE] = "";
	int readc;
	getRequest(socket_fd, request, &readc, &client, &client_addr_size);
	//Get the file descriptor of the requested resource
	int req_file_fd = open(request, O_RDONLY);
	if (req_file_fd == -1) {
		//case:file not found
		//Send file not found error to client
		sendError(socket_fd, &client, &client_addr_size);
	} else {
		//case:file found
		//getting the file size
		struct stat file_stat;
		fstat(req_file_fd, &file_stat);
		unsigned long size = (unsigned long) file_stat.st_size;
		close(req_file_fd);
		//Write file to response
		writeToSocket(socket_fd, &client, &client_addr_size, request);
		//free(resource);
	}
}

void writeToSocket(int socket_fd, struct sockaddr_in *client,
		socklen_t *client_addr_size, char *fileName) {
	pthread_t writer, reader, timeout_checker;
	int writer_thread_id, reader_thread_id, timeout_checker_thread_id;
	struct paramWriteData *params = (struct paramWriteData*) calloc(1,
			sizeof(struct paramWriteData));
	params->client = client;
	params->socket_fd = socket_fd;
	params->client_addr_size = client_addr_size;
	strcpy(params->fileName, fileName);

	paramReadData *params_read_data = (paramReadData*) calloc(1,
			sizeof(paramReadData));
	params_read_data->client = client;
	params_read_data->socket_fd = socket_fd;
	params_read_data->client_addr_size = client_addr_size;
	params_read_data->curr_thread = &reader;

	paramTimeout *params_timeout_data = (paramTimeout*) calloc(1,
			sizeof(paramTimeout));
	params_timeout_data->client = client;
	params_timeout_data->socket_fd = socket_fd;
	params_timeout_data->client_addr_size = client_addr_size;
	params_read_data->curr_thread = &timeout_checker;

	//scheduling config start
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
	//pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	int config_success = pthread_attr_setschedpolicy(&attr, SCHED_RR);
	if (config_success != 0) {
		signal_error("Failed to configure thread scheduler policy");
	}
	//scheduling config end

	//writer_thread_id = pthread_create(&writer, NULL, sendData, (void*) params);
	writer_thread_id = pthread_create(&writer, &attr, sendData, (void*) params);

	//reader_thread_id = pthread_create(&reader, NULL, readData,(void*) params_read_data);
	reader_thread_id = pthread_create(&reader, &attr, readData,
			(void*) params_read_data);

	//timeout_checker_thread_id = pthread_create(&timeout_checker, NULL, timout,(void*) params_timeout_data);
	timeout_checker_thread_id = pthread_create(&timeout_checker, &attr, timout,
			(void*) params_timeout_data);

	if (writer_thread_id < 0 || reader_thread_id < 0
			|| timeout_checker_thread_id < 0) {
		signal_error(
				"Failed to create threads for starting reliable communication");
	}

	(void) pthread_join(writer, NULL);
	(void) pthread_join(reader, NULL);
	(void) pthread_join(timeout_checker, NULL);
	if(DEBUG_MODE){
	printf("\n************************exited all the threads");
	}
	time_t current_time = time(NULL);
	//last phase packets
	total_packets += current_mode_packets;

	if (mode == SLOW_START) {
			slow_packets += current_mode_packets;
		} else if (mode == f_r_packets) {
			f_r_packets += current_mode_packets;
		} else {
			c_a_packets += current_mode_packets;
		}
	printf("\n last byte of response written at:%s", ctime(&current_time));
	printf("\n********Start of Stats**********");
	printf("\nPackets transferred in slow start mode:%Lf or %LF%% of total packets", slow_packets,(slow_packets / total_packets) * 100);
	printf("\nPackets transferred in fast recovery mode:%Lf or %LF%% of total packets", f_r_packets,(f_r_packets / total_packets) * 100);
	printf("\nPackets transferred in congestion avoidance mode:%Lf or %LF%% of total packets", c_a_packets,(c_a_packets / total_packets) * 100);
	printf("\nTotal Packets transferred %LF", total_packets);
	printf("\n********End of Stats*************");

	//last phase packets
}
/**
 * Timeout thread
 */
void *timout(void* params) {
	paramTimeout* actualParams = (paramTimeout*) params;
	int cancellation;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &cancellation);
	unsigned long reader_termination_time = 0;
	while (controller_flag > 0) {
		if (controller_flag == TIMEOUT_RUNNING) {
			if(DEBUG_MODE){
				printf("\nreceiver thread died");
			}
			if (reader_termination_time == 0) {
				struct timeval termination_timestamp;
				gettimeofday(&termination_timestamp, NULL);
				reader_termination_time = termination_timestamp.tv_sec * 1000000
						+ termination_timestamp.tv_usec;
			} else {
				struct timeval curr_timestamp;
				gettimeofday(&curr_timestamp, NULL);
				unsigned long curr_time = curr_timestamp.tv_sec * 1000000
						+ curr_timestamp.tv_usec;
				if ((curr_time - reader_termination_time) > MAX_WAIT_TIME_OUT) {
					//pthread_cancel(*actualParams->curr_thread);
					if(DEBUG_MODE){
						printf("exiting timeout thread");
					}
					pthread_exit(&exitval_timout);
				}
			}

		}
		if (DEBUG_MODE) {
			printf("\nin timeout");
		}
		//printf("\nin timeout chk point 1");
		/*
		 struct timespec sleepTime, t2;
		 sleepTime.tv_sec = 0;
		 //1000000ns= 1ms
		 sleepTime.tv_nsec = 20000000;
		 nanosleep(&sleepTime, &t2);
		 */
		pthread_mutex_lock(&((*logs).lock));
		//printf("\nin timeout chk point 2");

		int timed_out = 0;
		//checking timeout for each packet sent
		int i = 0;
		for (; !timed_out && i < logs->actual_size; i++) {
			struct timeval current_timestamp;
			gettimeofday(&current_timestamp, NULL);
			unsigned long ts_current = current_timestamp.tv_sec * 1000000
					+ current_timestamp.tv_usec;
			if (logs->sentLog[i] != NULL
					&& (ts_current - (logs->sentLog[i]->time_stamp))
							> timeout_interval) {
				if (DEBUG_MODE) {
					printf("\npacket seq:%d timed out",
							logs->sentLog[i]->sequenceNo);
				}
				//when the sent packet has timed out
				pthread_mutex_lock(&(dupl->lock));
				//updating with current time stamp
				logs->sentLog[i]->time_stamp = ts_current;
				pthread_mutex_lock(&(write_lock));
				//increasing the sent counter
				logs->sentLog[i]->sent = (logs->sentLog[i]->sent) + 1;
				if (DEBUG_MODE) {
					printf("\npacket seq:%d resent in time out oprn",
							logs->sentLog[i]->sequenceNo);
				}
				sendto(actualParams->socket_fd, logs->sentLog[i]->packet,
						CHUNKED_READ_WRITE_BUFFER_SIZE + SIZE_SEQUENCE_ARR
								+ SIZE_DELIMITER, 0,
						(struct sockaddr *) actualParams->client,
						*(actualParams->client_addr_size));
				current_mode_packets += 1;
				pthread_mutex_unlock(&(write_lock));
				timed_out = 1;
				if (mode == SLOW_START) {
					ssthresh = logs->size / 2;
					resize_log_store(&logs, 1, advertised_window);
					dupl->count = 0;
					dupl->sequence_no = -1;
				} else {
					//for congestion avoidance & fast recovery
					mode_switch(SLOW_START);
					ssthresh = logs->size / 2;
					resize_log_store(&logs, 1, advertised_window);
					dupl->count = 0;
					dupl->sequence_no = -1;
				}
				pthread_mutex_unlock(&(dupl->lock));
			}
		}
		pthread_mutex_unlock(&((*logs).lock));

		if (!timed_out) {
			//if nothing has timedout wait for atleast 1/2 timeout for checking for rtt
			struct timespec sleepTime, t2;
			sleepTime.tv_sec = 0;
			//1000000ns= 1ms
			sleepTime.tv_nsec = timeout_interval * 1000 / 2;
			nanosleep(&sleepTime, &t2);
		}

	}
	return 0;
}
/**
 * Calculate the estimatedRTT, devRTT and timeout value
 */
void rttCalc(double *estimatedRTT, double *devRTT, unsigned long packet_sent) {
	struct timeval current_timestamp;
	gettimeofday(&current_timestamp, NULL);
	unsigned long ts_current = current_timestamp.tv_sec * 1000000
			+ current_timestamp.tv_usec;
	//calculating estimated rtt
	double dev = (ts_current - packet_sent) - *devRTT;
	if (dev < 0) {
		//taking absolute
		dev = -1 * (dev);
	}
	//using the formula: devRTT=(1-beta)*devRTT+beta.|dev|, where beta=.25
	*devRTT = 0.75 * (*devRTT) + 0.25 * dev;
	//using the formula: estimatedRTT=(1-alpha)*estimatedRTT+alpha.sampleRTT,where alpha=.125
	*estimatedRTT = 0.875 * (*estimatedRTT)
			+ 0.125 * (ts_current - packet_sent);
	timeout_interval = *estimatedRTT + 4 * (*devRTT);
	//beta=.25
}
/**
 * Switches mode from current mode to the desired mode. Prints the status change messages. Updates the packet count for modes andf the cummulative count
 */
void mode_switch(int switchTo) {
	total_packets += current_mode_packets;
	printf("\n**************************");
	printf("\nTransitioning from:%s",
			mode == SLOW_START ? "Slow Start" :
			mode == CONGESTION_AVOIDANCE ?
					"Congestion Avoidance" : "Fast Recovery");
	//printf("\nPackets transferred in this mode:%Lf", current_mode_packets);
	//printf("\nWhich is %Lf %% of total packets transferred till now",	(current_mode_packets / total_packets) * 100);
	if (mode == SLOW_START) {
		slow_packets += current_mode_packets;
	} else if (mode == f_r_packets) {
		f_r_packets += current_mode_packets;
	} else {
		c_a_packets += current_mode_packets;
	}
	current_mode_packets = 0;
	printf("\nEntering: %s",
			switchTo == SLOW_START ? "Slow Start" :
			switchTo == CONGESTION_AVOIDANCE ?
					"Congestion Avoidance" : "Fast Recovery");
	printf("\n**************************");

	mode = switchTo;
}
/**
 * Reader thread
 */
void *readData(void* params) {
	paramReadData* actualParams = (paramReadData*) params;
	int cancellation;
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &cancellation);

	//in microseconds
	double estimatedRTT = 500000;
	double devRTT = estimatedRTT / 2;
	unsigned long writer_termination_time = 0;
	while (controller_flag > 0) {
		//thread termination logic
		if (controller_flag == READER_RUNNING) {
			//if writer thread has terminated

			if (writer_termination_time == 0) {
				//calculate writer thread termination time, if not calculated before
				if(DEBUG_MODE){
									printf("\nwriter thread died");
				}
				struct timeval termination_timestamp;
				gettimeofday(&termination_timestamp, NULL);
				writer_termination_time = termination_timestamp.tv_sec * 1000000
						+ termination_timestamp.tv_usec;
			} else {
				//check if the reader thread has exceeded the maximum time to live for after writer thread has terminated
				struct timeval curr_timestamp;
				gettimeofday(&curr_timestamp, NULL);
				unsigned long curr_time = curr_timestamp.tv_sec * 1000000
						+ curr_timestamp.tv_usec;
				if ((curr_time - writer_termination_time) > MAX_WAIT_READER) {
					if(DEBUG_MODE){
							printf("\nkilling reader thread");
					}
					controller_flag=TIMEOUT_RUNNING;
					//pthread_cancel(*actualParams->curr_thread);
					pthread_exit(&exitval_reader);
				}
			}
		}
		if (DEBUG_MODE) {
			printf("\nin read");
		}
		char ack[READ_BUFFER_SIZE] = "";
		int readc;
		if (controller_flag == READER_RUNNING) {
			//if writer thread has terminated. Do not infinitely wait for an ack, if an ack is not received within maximum time to live after termination of writer thread, terminate.
			fd_set set;
			struct timeval max_wait;
			FD_ZERO(&set);
			FD_SET(actualParams->socket_fd, &set);
			max_wait.tv_sec = 0;
			max_wait.tv_usec = MAX_WAIT_READER;
			int select_status = select(actualParams->socket_fd + 1, &set, NULL,
					NULL, &max_wait);
			if (select_status == 1) {
				//receive ack from receiver
				readc = recvfrom(actualParams->socket_fd, ack, READ_BUFFER_SIZE,
						0, (struct sockaddr *) actualParams->client,
						actualParams->client_addr_size);
				FD_ZERO(&set);
				FD_SET(actualParams->socket_fd, &set);
				max_wait.tv_sec = 0;
				max_wait.tv_usec = MAX_WAIT_READER;
			} else {
				controller_flag=TIMEOUT_RUNNING;
				if(DEBUG_MODE){
					printf("\nkilling reader thread");
				}
				//terminating current thread, as it has exceeded  the max time to live after termination of writer thread
				pthread_exit(&exitval_reader);
			}
		}

		else {
			//receive ack from receiver
			readc = recvfrom(actualParams->socket_fd, ack, READ_BUFFER_SIZE, 0,
					(struct sockaddr *) actualParams->client,
					actualParams->client_addr_size);
		}
		if (readc == -1) {
			if(DEBUG_MODE){
			printf("error in receiving ack from client");
			}
		} else {
			pthread_mutex_lock(&((*logs).lock));
			pthread_mutex_lock(&(dupl->lock));
			sent *log = getLog(atoi(ack) - 1, &logs);
			if (log != NULL) {

				if (log->sent == 1) {
					//if packet is not resent packet or timed out packet. Calculate the estimatedRTT, devRTT and timeout
					if (DEBUG_MODE) {
						printf("************************");
						printf("\nvalues before rtt calc");
						printf("\nestimatedRTT:%f,devRTT:%f,timeout%f",
								estimatedRTT, devRTT, timeout_interval);
					}
					rttCalc(&estimatedRTT, &devRTT, log->time_stamp);
					if (DEBUG_MODE) {
						printf("\nvalues after rtt calc");
						printf("\nestimatedRTT:%f,devRTT:%f,timeout%f",
								estimatedRTT, devRTT, timeout_interval);
						printf("************************");

					}
				}
				arr_delete_by_ack(atoi(ack), &logs);
			}
			if (dupl->sequence_no == atoi(ack)) {
				//if last ack was for the same sequence no.
				dupl->count = (dupl->count) + 1;
			} else {
				//if last ack was not for the same sequence no.
				dupl->count = 0;
				dupl->sequence_no = atoi(ack);
			}
			if (dupl->count == 3) {
				//three duplicate ack received for a same sequence no

				//starting of resending the desired sequence packet
				struct timeval current_timestamp;
				gettimeofday(&current_timestamp, NULL);
				unsigned long ts_current = current_timestamp.tv_sec * 1000000
						+ current_timestamp.tv_usec;
				sent *log = getLog(dupl->sequence_no, &logs);

				if (log == NULL) {
					//handle case when packet has not even been sent by the writer thread
				if(DEBUG_MODE){

					printf(
							"\n********************received null log for sequence no:%d",
							dupl->sequence_no);
					printf(
							"\nlogs size:%d, actual size:%d, inserts:%d, deletes: %d",
							logs->size, logs->actual_size, logs->inserts,
							logs->deletes);
					printf("\n in logs");
					int m;
					for (m = 0; m < (logs)->actual_size; m++) {
						if (logs->sentLog[m] != NULL) {
							printf("\nseq:%d", logs->sentLog[m]->sequenceNo);
							printf(
									"***************************************************");
						}
					}
					}
					pthread_mutex_unlock(&(dupl->lock));
					pthread_mutex_unlock(&((*logs).lock));
					continue;
				}

				log->time_stamp = ts_current;
				log->sent = (log->sent) + 1;
				pthread_mutex_lock(&(write_lock));
				sendto(actualParams->socket_fd, log->packet,
						CHUNKED_READ_WRITE_BUFFER_SIZE + SIZE_SEQUENCE_ARR
								+ SIZE_DELIMITER, 0,
						(struct sockaddr *) actualParams->client,
						*(actualParams->client_addr_size));
				current_mode_packets += 1;
				pthread_mutex_unlock(&(write_lock));
				//ending of resending the desired sequence packet
				mode_switch(FAST_RECOVERY);
				ssthresh = (logs->size) / 2;
				resize_log_store(&logs, ssthresh + 3, advertised_window);
			} else if (dupl->count > 3) {
				//more than three duplicate ack received for a same sequence no
				resize_log_store(&logs, (logs->size) + 1, advertised_window);
			} else {
				//regular case, when duplicate acknowledgment <3
				if (mode == SLOW_START) {
					//increasing the congestion window exponentially
					resize_log_store(&logs, (logs->size) * 2,
							advertised_window);
					if ((logs->size) > ssthresh) {
						mode_switch(CONGESTION_AVOIDANCE);
					}
				} else if (mode == FAST_RECOVERY) {
					mode_switch(CONGESTION_AVOIDANCE);
					resize_log_store(&logs, ssthresh, advertised_window);
					//duplicate count ==0 handled in the code for if last ack was not for the same sequence no.
				} else {
					//if in congestion avoidance, tyr to increase the eff window linearly
					resize_log_store(&logs, (logs->size) + 1,
							advertised_window);
				}
			}
			//printf("\nresize end");
			pthread_mutex_unlock(&(dupl->lock));
			pthread_mutex_unlock(&((*logs).lock));
			//printf("\nexited resize");

		}

	}
	return 0;
}
/**
 * Writer Thread
 */
void *sendData(void* params) {
	printf("\nStarting in slow start");
	struct paramWriteData* actualParams = (struct paramWriteData *) params;
	int req_file_fd = open(actualParams->fileName, O_RDONLY);
	unsigned char file_buffer[CHUNKED_READ_WRITE_BUFFER_SIZE] = "";
	unsigned long long packetCount = 0;
	while (read(req_file_fd, file_buffer, (sizeof file_buffer)) > 0) {
		if (DEBUG_MODE) {
			printf("\nin write");
		}
		unsigned char sequenceNo[SIZE_SEQUENCE_ARR];
		//writing sequence no
		snprintf(sequenceNo, sizeof(sequenceNo), "%05d",
				packetCount % MAX_SEQUENCE_NO);
		unsigned char packet[CHUNKED_READ_WRITE_BUFFER_SIZE + SIZE_SEQUENCE_ARR
				+ SIZE_DELIMITER];
		//creating packet to be sent
		//adding sequence to it
		memcpy(packet, sequenceNo, SIZE_SEQUENCE_ARR);
		//addding delimiter
		packet[SIZE_SEQUENCE_ARR] = ';';
		//adding payload
		memcpy(packet + SIZE_SEQUENCE_ARR + SIZE_DELIMITER, file_buffer,
		CHUNKED_READ_WRITE_BUFFER_SIZE);
		int inserted = 0;
		int retry = 0;
		while (!inserted) {
			if (retry) {
				//if inserting to the log store fails, wait for some time, before retrying
				struct timespec sleepTime, t2;
				sleepTime.tv_sec = 0;
				//2000000ns= 1 ms
				sleepTime.tv_nsec = 1000000;
				nanosleep(&sleepTime, &t2);
			}
			struct timeval timestamp;
			gettimeofday(&timestamp, NULL);
			//creating log for the packet to be sent
			sent *sentp = (sent *) calloc(1, sizeof(sent));
			sentp->sequenceNo = packetCount % MAX_SEQUENCE_NO;
			sentp->time_stamp = timestamp.tv_sec * 1000000 + timestamp.tv_usec;
			sentp->packet = (unsigned char*) calloc(sizeof(packet),
					sizeof(unsigned char));
			sentp->sent = 1;
			memcpy(sentp->packet, packet, sizeof(packet));
			//printf("\n packet being stored:%s",sentp->packet+7);
			//printf("\n packet being written:%s",packet+7);
			pthread_mutex_lock(&((*logs).lock));
			inserted = insert_arr(sentp, &logs);
			pthread_mutex_unlock(&((*logs).lock));
			if (inserted == 0) {
				free(sentp->packet);
				free(sentp);
			}
			retry += 1;
		}
		pthread_mutex_lock(&(write_lock));
		//writing data chunk to the socket
		sendto(actualParams->socket_fd, packet, sizeof(packet), 0,
				(struct sockaddr *) actualParams->client,
				*(actualParams->client_addr_size));
		current_mode_packets += 1;
		pthread_mutex_unlock(&(write_lock));
		packetCount = (packetCount + 1) % MAX_SEQUENCE_NO;
		bzero(file_buffer, sizeof(file_buffer));
	}
	controller_flag = READER_RUNNING;

	close(req_file_fd);
	return 0;
}

void sendError(int socket_fd, struct sockaddr_in *client,
		socklen_t *client_addr_size) {
	char msg[] = "**File Not Found**";
	sendto(socket_fd, msg, sizeof(msg), 0, (struct sockaddr *) client,
			*client_addr_size);
}
