#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/stat.h> 
#include <arpa/inet.h>
#include<fcntl.h>
#include <sys/sendfile.h>
#include<bits/stdc++.h>
#include <openssl/md5.h>

using namespace std;

#define SA struct sockaddr
#define SENDER_QUEUE_SIZE	102400
#define RECEIVER_QUEUE_SIZE  102400
#define RECEIVER_BUFFER_SIZE 102400
#define MSS		1024
#define TIME_RECV_WAIT 1

typedef pair<string,string>	pssi;

void error(char*c){
	printf("[ERROR] :: %s\n",c);
	return;
}

void debug(char*c){
	printf("%s\n",c );
	return;
}

typedef struct packet_{				//	packet can be a data or ack 

	bool isack;						//	whether ack or not
	char data[MSS];
	int seq_no;						//	It is the initial byte sequence number
	int window;						//	for the window advertisment
	int length;						//	data length	

}packet;

typedef struct connnection_info_{
	bool isrecv;					//	Have we created thread for receiver
	bool issend;					//	Have we created congestion thread
	pthread_t congestion_t;			//	thread storing the congestion thread it of the connection
	pthread_t recvbuffer_handle_t;	//	receiving thread
	mutex mQ;						//	mutex for all the same accessible things
	mutex mRQ;
	mutex mack;
	mutex mdata;
	queue<char>*Q;					//	sender queue pointer
	queue<char>*R_Q;				//	Receiver queue
	int curr_s;						//	current seq no we gonna send
	int ack_r;						//	seq no whose ack we are expecting
	int data_r;						//	seq no of the data we are accpecting
	int cnwd;
	int ssthresh;
	int rnwd;						//	receiver window size
	int actual_send;				//	actual bytes we can send
	queue<packet>*ack;				//	a queue holding the ack arrived but not checked
	queue<packet>*data;				//	queue holding a bit data just for temporary purpose
	struct sockaddr_in serv_addr;	//	server address
	socklen_t serv_len ;			

	int ini(char* ip,char* port){
		serv_len = sizeof(serv_addr);
		bzero(&serv_addr,serv_len);
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_port = htons(atoi(port));
		if(inet_pton(AF_INET,ip,&serv_addr.sin_addr) != 1)
		{
			error("inet_pton()");
			return -1;
		}
		return 0;
	}

}conn_info;

typedef struct my_addr_
{
	int sockfd;
	struct sockaddr_in my_addr;
	socklen_t my_len;

	int ini(string port){
		socklen_t my_len = sizeof(my_addr);
	
		if( (sockfd = socket(AF_INET,SOCK_DGRAM,0)) < 0){
			error("socket()");
			return -1;
		}

		struct timeval timeout;
		timeout.tv_sec = 5;
		timeout.tv_usec = 0;
		
		if(setsockopt(sockfd,SOL_SOCKET,SO_RCVTIMEO,(void*)&timeout,sizeof(timeout)) < 0)error("setsockopt(recvtimeout) not established");
		
		bzero(&my_addr,my_len);
		my_addr.sin_family = AF_INET;
		my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		my_addr.sin_port = htons(stoi(port));
		
		if(bind(sockfd,(SA*)&my_addr,my_len)<0){
			error("bind()");
			return -1;
		}

		return 0;
	}

}my_addr;


map<pssi,conn_info*>connections;	//	contain the info related to a particular connection
my_addr *MY_ADDR;

void  signal_handler(int signo){
	signal(signo,signal_handler);
}

void create_packet(char* C1,int length,string ip,string port){
	conn_info *C=connections[{ip,port}];		//	extracitng connection info regarding this thread
	int temp=C->curr_s-length;
	while(length>0){
		packet* P=(packet*)malloc(sizeof(packet));
		P->isack=false;
		P->window=-1;
		P->length=min(MSS,length);
		P->seq_no=temp;							//	sequence no in the packet is the initail byte sequence number
		for(int i=0;i<P->length;i++){
			P->data[i]=C1[0];
			C1++;
			length--;
			temp++;
		}

		printf("Sending data packet to ip : %s  port: %s sequence no: %d length: %d \n",ip.c_str(),port.c_str(),P->seq_no,P->length);
		if(sendto(MY_ADDR->sockfd,P,sizeof(packet),0,(SA*)&(C->serv_addr),C->serv_len)< sizeof(packet))
			error("sendto()");
		free(P);
	}
	return;
}

void update_Window(string ip,string port,int case_){
	conn_info *C=connections[{ip,port}];		//	extracitng connection info regarding this thread
	if(case_==1){								//	if 3 duplicate 
		C->ssthresh=max(C->ssthresh/2,1);
		C->cnwd=C->ssthresh;
	}
	else if(case_==2){							//	if timeout
		C->ssthresh=max(C->ssthresh/2,1);
		C->cnwd=1;
	}			
	else{										//	correct ack
		if(C->cnwd<C->ssthresh)
			C->cnwd+=MSS;
		else
			C->cnwd+=MSS/C->cnwd;
	}
	// printf("Window updated for connection ip : %s  port: %s cnwd: %d \n",ip.c_str(),port.c_str(),C->cnwd);
	C->actual_send=min(C->cnwd,C->rnwd);
	return;
}


void *congestion(void* argument){
	signal(SIGUSR1,signal_handler);
	pssi *p=(pssi*)argument;		
	string ip=p->first;
	string port=p->second;

	while(connections.find({ip,port})==connections.end());	//	waiting till the connection is initialized

	conn_info *C=connections[{ip,port}];		//	extracitng connection info regarding this thread
	queue<char>*Q=C->Q;							//	sender queue
	deque<char>Window;							//	Window for which we are waiting for ACK
	deque<char>tobesend;						//	These are also to be send 

	int check=1;
	int ini=1;
	int duplicate=0;

	while(true){
		if(ini==1){
			int t=min((int)Q->size(),MSS);
			char *C1=(char*)malloc(sizeof(char)*t);

			C->mQ.lock();

			for(int i=0;i<t;i++){
				C1[i]=Q->front();
				Q->pop();
				C->curr_s++;							//	updating current seq number
				Window.push_back(C1[i]);				//	packet pushed in window
			}

			C->mQ.unlock();

			create_packet(C1,t,ip,port);
			free(C1);
			ini=0;
			check=sleep(TIME_RECV_WAIT);
			continue;
		}
		else if(check!=0){								//	ack received  or ack packets in queue
			C->mack.lock();

			packet P=C->ack->front();
			packet *p=&P;
			C->ack->pop();

			C->mack.unlock();

			if(p->seq_no<C->ack_r){						//	if duplicate packet
				duplicate++;
				if(duplicate==3){
					duplicate=0;
					update_Window(ip,port,1);
				}
				else{
					check=sleep(TIME_RECV_WAIT);
					continue;
				}
			}
			else{										//	if correct packet
				duplicate=0;
				C->rnwd=p->window;
				update_Window(ip,port,3);
				int t7=p->seq_no-C->ack_r+1;
				int t8=Window.size();
				int t9=min(t7,t8);
				for(int i=0;i<t9;i++){		//	poping for received amount
					Window.pop_front();	
					C->ack_r++;
					t7--;							
				}
				for(int i=0;i<t7;i++){
					tobesend.pop_front();
					C->ack_r++;
					t7--;
					C->curr_s++;
				}

				C->mQ.lock();

				int t=tobesend.size()+Q->size();

				C->mQ.unlock();

				t=min(C->actual_send,t);
				int t1=t;
				char *C1=(char*)malloc(sizeof(char)*t);
				int y=0;
				int t5=min(t1,(int)tobesend.size());
				for(int i=0;i<t5;i++){
					C1[y++]=tobesend.front();
					Window.push_back(C1[y-1]);
					tobesend.pop_front();
					C->curr_s++;					//	Updating current sequence number
					t1--;
				}

				C->mQ.lock();

				for(int i=0;i<t1;i++){
					C1[y++]=Q->front();
					Q->pop();
					Window.push_back(C1[y-1]);
					C->curr_s++;					//	Updating current sequence number
				}

				C->mQ.unlock();

				create_packet(C1,t,ip,port);
				free(C1);
				check=sleep(TIME_RECV_WAIT);
				continue;
			}
		}
		else{
			duplicate=0;
			update_Window(ip,port,2);
		}
		
		//	send accordingly

		int win_siz=Window.size();

		if(C->actual_send>win_siz){

			C->mQ.lock();

			int t=tobesend.size()+Q->size()+win_siz;

			C->mQ.unlock();

			t=min(C->actual_send,t);
			int t1=t;
			char *C1=(char*)malloc(sizeof(char)*t);
			int y=0;
			for(int i=0;i<win_siz;i++){
				C1[y++]=Window.front();
				Window.pop_front();
				Window.push_back(C1[y-1]);
				t1--;
			}
			int t5=min(t1,(int)tobesend.size());
			for(int i=0;i<t5;i++){
				C1[y++]=tobesend.front();
				Window.push_back(C1[y-1]);
				tobesend.pop_front();
				C->curr_s++;					//	Updating current sequence number
				t1--;
			}

			C->mQ.lock();

			for(int i=0;i<t1;i++){
				C1[y++]=Q->front();
				Q->pop();
				Window.push_back(C1[y-1]);
				C->curr_s++;					//	Updating current sequence number
			}

			C->mQ.unlock();

			create_packet(C1,t,ip,port);
			free(C1);
		}
		else{
			int t=win_siz-C->actual_send;
			for(int i=0;i<t;i++){
				char c=Window.back();
				tobesend.push_front(c);
				Window.pop_back();
				C->curr_s--;					//	Updating current sequence number
			}
			char *C1=(char*)malloc(sizeof(char)*C->actual_send);
			for(int i=0;i<C->actual_send;i++){
				C1[i]=Window.front();
				Window.pop_front();
				Window.push_back(C1[i]);
			}
			create_packet(C1,C->actual_send,ip,port);
			free(C1);
		}
		check=sleep(TIME_RECV_WAIT);
	}
	pthread_exit(0);
}

int sendbuffer_handle(char*data,int length,string ip,string port){		//	It is working on the principle of busy waiting till the whole data is not transfered on the queue
	int total_bytes=0;
	queue<char>*Q=connections[{ip,port}]->Q;
	while(total_bytes!=length){

		connections[{ip,port}]->mQ.lock();

		if(Q->size()<SENDER_QUEUE_SIZE){
			int t=SENDER_QUEUE_SIZE-Q->size();
			for(int i=0;i<t && total_bytes!=length;i++){
				Q->push(data[0]);
				data++;
				total_bytes++;
			}
		}

		connections[{ip,port}]->mQ.unlock();
	}
	return 0;
}

int appSend(char*data,int length,char* DIP,char* Dport){
	//	-------------------------------------------------------------------------------------------------------Check if ip and port is correct
	string ip=string(DIP);
	string port=string(Dport);
	if(connections.find({ip,port})==connections.end()){				//	If it a new port then you have to create thread
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setscope(&attr,PTHREAD_SCOPE_SYSTEM);
		pthread_t i;
		pssi *p=new pssi;
		p->first=ip;
		p->second=port;
		queue<char> *temp=new queue<char>;							//	sender queue
		pthread_create(&i,&attr,congestion,p);						//	congestion thread
		conn_info* cc=(conn_info*)malloc(sizeof(conn_info));

		int t1;
		if(cc->ini(DIP,Dport)==-1){
			return -1;
		}
		queue<packet>*temp1=new queue<packet>;
		cc->congestion_t=i;	
		cc->Q=temp;
		cc->ack=temp1;
		cc->issend=true;
		cc->isrecv=false;
		cc->curr_s=0;
		cc->ack_r=0;
		cc->cnwd=MSS;
		cc->rnwd=MSS;
		cc->ssthresh=MSS;
		cc->actual_send=MSS;

		connections[*p]=cc;
	}
	else if(connections.find({ip,port})!=connections.end() && connections[{ip,port}]->issend==false){
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setscope(&attr,PTHREAD_SCOPE_SYSTEM);
		pthread_t i;
		queue<char> *temp=new queue<char>;							//	sender queue
		pssi *p=new pssi;
		p->first=ip;
		p->second=port;
		pthread_create(&i,&attr,congestion,p);						//	congestion thread
		conn_info* cc=connections[{ip,port}];
		queue<packet>*temp1=new queue<packet>;
		cc->ack=temp1;
		cc->congestion_t=i;	
		cc->Q=temp;
		cc->issend=true;
		cc->curr_s=0;
		cc->ack_r=0;
		cc->cnwd=MSS;
		cc->rnwd=MSS;
		cc->ssthresh=MSS;
		cc->actual_send=MSS;
	}
	return sendbuffer_handle(data,length,ip,port);
}

void parse_packets(packet*P,struct sockaddr_in* client_addr,socklen_t client_len){
	char* temp1=inet_ntoa(client_addr->sin_addr);
	int temp=ntohs(client_addr->sin_port);
	string ip=string(temp1);
	string port=to_string(temp);
	if(connections.find({ip,port})!=connections.end() && P->isack==true){				//	If it is a ack packet

		connections[{ip,port}]->mack.lock();

		connections[{ip,port}]->ack->push(*P);

		connections[{ip,port}]->mack.unlock();

		pthread_kill(connections[{ip,port}]->congestion_t,SIGUSR1);
		printf("Received ack packet from ip : %s  port: %s sequence no: %d\n",ip.c_str(),port.c_str(),P->seq_no);
	}
	else if(connections.find({ip,port})!=connections.end() && P->isack!=true){			//	If it is a data packet 

		connections[{ip,port}]->mdata.lock();

		connections[{ip,port}]->data->push(*P);

		connections[{ip,port}]->mdata.unlock();

		pthread_kill(connections[{ip,port}]->recvbuffer_handle_t,SIGUSR1);
		printf("Received data packet from ip : %s  port: %s sequence no: %d length %d\n",ip.c_str(),port.c_str(),P->seq_no,P->length);
	}
	return;
}

void* upd_receive(void *argument){			//	single thread receiving from the kernel
	struct sockaddr_in *client_addr=(struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
	socklen_t client_len=sizeof(struct sockaddr_in);
	packet* P=(packet*)malloc(sizeof(packet));
	bzero(P,sizeof(packet));
	while(true){
		if(recvfrom(MY_ADDR->sockfd,(void*)P,sizeof(packet),0,(SA*)client_addr,&client_len)==sizeof(packet)){
			parse_packets(P,client_addr,client_len);
		}
	}
	free(P);
	pthread_exit(0);
}

void send_ack(int window_size,int ack_no,string ip,string port){
	conn_info *C=connections[{ip,port}];		//	extracitng connection info regarding this thread
	packet* P=(packet*)malloc(sizeof(packet));
	P->isack=true;
	P->window=window_size;
	P->seq_no=ack_no;							//	sequence no in the packet is the initail byte sequence number
	if(sendto(MY_ADDR->sockfd,P,sizeof(packet),0,(SA*)&(C->serv_addr),C->serv_len) < sizeof(packet))
			error("sendto()");
	free(P);
	return;
}

void* recvbuffer_handle(void *argument){
	signal(SIGUSR1,signal_handler);
	pssi *p=(pssi*)argument;		
	string ip=p->first;
	string port=p->second;

	while(connections.find({ip,port})==connections.end());	//	waiting till the connection is initialized

	conn_info *C=connections[{ip,port}];							//	extracitng connection info regarding this thread
	vector<char>recv_oob(RECEIVER_BUFFER_SIZE);						//	Receiver out of order buffer
	vector<bool>recv_oob_b(RECEIVER_BUFFER_SIZE,false);					//	Tell which index of buffer contiain element
	while(true){

		C->mdata.lock();

		int t=C->data->size();

		C->mdata.unlock();

		if(t!=0){

			C->mdata.lock();

			packet P=C->data->front();
			C->data->pop();

			C->mdata.unlock();

			if(P.seq_no<=C->data_r){
				if(P.seq_no+P.length>C->data_r){					//	Duplicate packet ignore and non duplicate are onlu condidered
					int t1=C->data_r-P.seq_no;
					int t2=P.length-t1;
					for(int i=0;i<t2 && i<RECEIVER_BUFFER_SIZE;i++){
						recv_oob[i]=P.data[i+t1];
						recv_oob_b[i]=true;
					}
				}
			}
			else{													//	Aceepting out of order packets
				int t=P.seq_no-C->data_r;
				for(int i=t;i<t+P.length && i<RECEIVER_BUFFER_SIZE;i++){
					recv_oob[i]=P.data[i-t];
					recv_oob_b[i]=true;
				}
			}

			int t3=0;
			for(int i=0;i<RECEIVER_BUFFER_SIZE;i++){
				if(recv_oob_b[i]==false){
					break;
				}
				t3++;
			}
			int t4=RECEIVER_BUFFER_SIZE-C->R_Q->size();
			int t5=min(t3,t4);

			C->mRQ.lock();

			for(int i=0;i<t5;i++){
				C->R_Q->push(recv_oob[i]);
				C->data_r++;
			}

			C->mRQ.unlock();

			for(int i=t5;i<RECEIVER_BUFFER_SIZE;i++){
				recv_oob[i-t5]=recv_oob[i];
				recv_oob_b[i-t5]=recv_oob_b[i];
			}
			for(int i=0;i<t5;i++){
				recv_oob_b[RECEIVER_BUFFER_SIZE-i-1]=false;
			}

			if(t3<=t4 && t3!=0)
				send_ack(RECEIVER_BUFFER_SIZE,C->data_r-1,ip,port);
			else if(t4!=0)
				send_ack(RECEIVER_BUFFER_SIZE-(t3-t4),C->data_r-1,ip,port);

			if(C->data->size()!=0)
				continue;
			sleep(1);
		}
	}
}

int appRecv(char*data,int length,char* DIP,char* Dport){			//	length is the max lenght of the buffer, it will return the length read
	//	-------------------------------------------------------------------------------------------------------Check if ip and port is correct
	string ip=string(DIP);
	string port=string(Dport);
	if(connections.find({ip,port})==connections.end()){				//	If it a new port then you have to create thread
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setscope(&attr,PTHREAD_SCOPE_SYSTEM);
		pthread_t i;
		pssi *p=new pssi;
		p->first=ip;
		p->second=port;
		queue<char> *temp=new queue<char>;							//	Receiver queue
		pthread_create(&i,&attr,recvbuffer_handle,p);				//	Receive buffer handle thread
		conn_info* cc=(conn_info*)malloc(sizeof(conn_info));		//	Make the new connection

		if(cc->ini(DIP,Dport)==-1){
			return -1;
		}

		cc->recvbuffer_handle_t=i;	
		cc->R_Q=temp;
		cc->issend=false;
		cc->isrecv=true;
		cc->data_r=0;
		queue<packet> *temp1=new queue<packet>;
		cc->data=temp1;

		connections[*p]=cc;
	}
	else if(connections.find({ip,port})!=connections.end() && connections[{ip,port}]->isrecv==false){	//	if it is already created but receiver buffer is not made
		conn_info* cc=connections[{ip,port}];
		queue<char> *temp=new queue<char>;							//	Receiver queue
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setscope(&attr,PTHREAD_SCOPE_SYSTEM);
		pthread_t i;
		pssi *p=new pssi;
		p->first=ip;
		p->second=port;
		pthread_create(&i,&attr,recvbuffer_handle,p);				//	Receive buffer handle thread



		cc->recvbuffer_handle_t=i;	
		cc->R_Q=temp;
		cc->issend=false;
		cc->isrecv=true;
		cc->data_r=0;
		queue<packet> *temp1=new queue<packet>;
		cc->data=temp1;
	}

	conn_info* C=connections[{ip,port}];

	int total=0;
	while(total!=length){

		C->mRQ.lock();

		int t=C->R_Q->size();
		if(t!=0){
			for(int i=0;i<t && total!=length;i++){
				data[total++]=C->R_Q->front();
				C->R_Q->pop();
			}
		}

		C->mRQ.unlock();
	}

	return 0;
}



int init_transport_layer(string port){		//	Returns 1 if successful or else return -1 
	printf("Host running on port: %s\n", port.c_str());
	if(MY_ADDR!=NULL)
		return -1;
	MY_ADDR=(my_addr*)malloc(sizeof(my_addr));
	int t=MY_ADDR->ini(port);
	if(t==-1)
		return -1;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setscope(&attr,PTHREAD_SCOPE_SYSTEM);
	pthread_t i;
	pthread_create(&i,&attr,upd_receive,NULL);	
	return 0;
}