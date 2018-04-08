# TCP-Tahoe

Implementing a Network Protocol using Sockets: A Modular Approach
Implemented using UDP send and receive.
Provide reliability over UDP.
Provide Congestion control and Flow control

# Usage

1. Include socket.h  file in the program file
2. Call init_tranport_layer(string port) fucntion providing it the port you want o receive data on
3. Use appSend(char*data,int length,char* DIP,char* Dport) to send data to address(ip,port) == (DIP,Dport) , length id the length of data it will send
4. Use appRecv(char*data,int length,char* DIP,char* Dport) to receive data from address(ip,port) == (DIP,Dport). This is a blocking call until it receive data of size = length

Only fucntions accessible to the application is 
init_tranport_layer(string port)
appSend(char*data,int length,char* DIP,char* Dport)
appRecv(char*data,int length,char* DIP,char* Dport)

# Note: Plesae refer design_of_transport_wrapper.pptx for design details 
