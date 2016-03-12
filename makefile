CC = gcc
FLAGS = -pthread

all: server_udp client_udp clean

server_udp: server_udp.o
	$(CC) -o $@ $^ $(FLAGS)

client_udp: client_udp.o
	$(CC) -o $@ $^

clean:
	rm *.o
