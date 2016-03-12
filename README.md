Please refer to the wiki section for introduction, features and implementation details.

# Insrtuctions for running multithreaded reliable UDP client/server program:

1. In the current folder, run make in terminal to create executable files server_udp client_udp

2. Any files which is to be used for testing, should be copied in this folder. As the server can only find files in the current folder

3. run server with following command:

  ./server_udp _port-no_ _advertised-window_

  Example: ./server_udp 50410 15

  Also please note, here advertised windows units is MSS, as in when 10 is entered as advertised window, it means advertised windows is of 10 MSS. Wheren in 1 MSS in 1007 bytes

4. run the client with following command:
  ./client_udp _hostname_ _port-no_ _filename_ _packet-drop_ _packet-delay_
  Example: ./client_udp localhost 50410 sample 0 0

  Usage of packet-drop and packet-delay params:

  * packet-drop

    * If no packet drop is required, pass 0,
   
    * If all the incoming packets need to be dropped, pass 1.
   
    * The formula used for packet drop is, if <current time>mod <user input for drop> ==0, drop the packet.
   
    * To drop high no. of packtes, pass some value like 2 or 3 or 4. You can use the above formula to get an idea.
   
    * To drop low no. of packets, pass some value like 123, 133, 179.  You can use the above formula to get an idea.

  * packet_delay
    - If no packet delay/high latency is to exhibited, pass 0.
    
    - The units inputted in this param is assumed to be ms.
    
    - For delaying every acknowledgment by 1 ms, pass 1.
    
    - For delaying every acknowledgment by 2 ms, pass 2.
    
    - For delaying every acknowledgment by 3 ms, pass 3.
     .

     .

     .

     so on

5. The server need not be restarted for every new request, but just in case it starts behaving weird, restart it
