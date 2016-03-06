/*
 * MOPS.c
 *
 *  Created on: Jan 20, 2016
 *      Author: rudy
 */

#include <sys/select.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/time.h>
#include <mqueue.h>
#include <rtnet.h>
#include <rtmac.h>
#include <sys/mman.h>
#include <limits.h>

#include "MOPS.h"
#include "MQTT.h"
#include "MOPS_RTnet_Con.h"

// *************** Global variables for local processes *************** //
static MOPS_Queue proc_mops_queue;
// *************** Global variables for local processes *************** //



// *************** Global variables for MOPS broker *************** //
static uint8_t MOPS_State = SEND_REQUEST;
uint8_t input_buffer[UDP_MAX_SIZE];          //Buffer for receiving data from RTnet
uint8_t output_buffer[UDP_MAX_SIZE]; 		 //Buffer for sending data to RTnet
uint8_t waiting_output_buffer[UDP_MAX_SIZE]; //Buffer for incoming data from processes
											 //(waiting for sending them to RTnet)
uint8_t waiting_input_buffer[UDP_MAX_SIZE];  //Buffer for outgoing data to processes
											 //(waiting for sending them to processes)

uint16_t input_index = 0;
uint16_t output_index = 0;
uint16_t waiting_output_index = 0;
uint16_t waiting_input_index = 0;

TopicID list[MAX_NUMBER_OF_TOPIC];						//list of all known topic with theirs ID
SubscriberList sub_list[MAX_NUMBER_OF_SUBSCRIPTIONS];	//list of all subscribers ID and subscribed topics by them
MOPS_Queue mops_queue[MAX_PROCES_CONNECTION];			//list of connected processes to broker

#if TARGET_DEVICE == Linux
pthread_mutex_t output_lock, input_lock, waiting_output_lock, waiting_input_lock;
#endif
#if TARGET_DEVICE == RTnode
SemaphoreHandle_t output_lock, input_lock, waiting_output_lock, waiting_input_lock;
#endif
// *************** Global variables for MOPS broker *************** //



// ***************   Funtions for local processes   ***************//
#if TARGET_DEVICE == Linux
int connectToMOPS(){
	uint8_t temp;
    mqd_t mq;
    struct mq_attr attr;
    char buffer[10] = {'/',0,0,0,0,0,0,0,0,0};
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_QUEUE_MESSAGE;
    attr.mq_msgsize = MAX_QUEUE_SIZE;
    attr.mq_curmsgs = 0;
    sprintf(buffer+1, "%d", getpid());

    mq = mq_open(QUEUE_NAME, O_WRONLY);
    if( !((mqd_t)-1 != mq) )
    	perror("MQueue Open");

	temp = strlen(buffer);
	buffer[temp] = 'a';
	proc_mops_queue.MOPSToProces_fd = mq_open(buffer, O_CREAT | O_RDONLY, 0644, &attr);
    if( !((mqd_t)-1 != proc_mops_queue.MOPSToProces_fd) )
    	perror("MQueue Open MOPSToProces");
	buffer[temp] = 'b';
	proc_mops_queue.ProcesToMOPS_fd = mq_open(buffer, O_CREAT | O_WRONLY, 0644, &attr);
    if( !((mqd_t)-1 != proc_mops_queue.ProcesToMOPS_fd) )
    	perror("MQueue Open ProcesToMOPS");

    buffer[temp] = 0;
    if( !(0 <= mq_send(mq, buffer, 10, 0)) )
    	perror("Send MQueue");
    if( !((mqd_t)-1 != mq_close(mq)) )
    	perror("Close MQueue");
    return 0;
}

int sendToMOPS(char *buffer, uint16_t buffLen){
	return mq_send(proc_mops_queue.ProcesToMOPS_fd, (char*)buffer, buffLen, 0);
}

int recvFromMOPS(char *buffer, uint16_t buffLen){
	return mq_receive(proc_mops_queue.MOPSToProces_fd, buffer, buffLen, NULL);
}
#endif //TARGET_DEVICE == Linux

#if TARGET_DEVICE == RTnode
int connectMOPS(){}
int sendToMOPS(int fd, uint8_t *buffer, uint16_t buffLen){}
int recvFromMOPS(int fd, uint8_t *buffer, uint16_t buffLen){}
#endif //TARGET_DEVICE == RTnode

void publishMOPS(int fd, char *Topic, char *Message){
	char buffer[MAX_QUEUE_SIZE];
	memset(buffer, 0, MAX_QUEUE_SIZE);
	uint16_t packetID, written;
	written = BuildClientPublishMessage((uint8_t*)buffer, sizeof(buffer), (uint8_t*)Topic, (uint8_t*)Message, 0, 0, &packetID);
    if (sendToMOPS(buffer, written) == -1) {
        perror("send");
    }
}

void subscribeMOPS(char **TopicList, uint8_t *QosList, uint8_t NoOfTopics){
	char buffer[MAX_QUEUE_SIZE];
	memset(buffer, 0, MAX_QUEUE_SIZE);
	uint16_t packetID, written;
	written = BuildSubscribeMessage((uint8_t*)buffer, sizeof(buffer), (uint8_t**)TopicList, QosList, NoOfTopics, &packetID);

    if (sendToMOPS(buffer, written) == -1) {
        perror("send");
    }
}

int readMOPS(char *buf, uint8_t length){
	char temp[MAX_QUEUE_SIZE];
    int t;
	memset(temp,0,MAX_QUEUE_SIZE);
	memset(buf,0,length);

	if ((t=recvFromMOPS(temp, MAX_QUEUE_SIZE)) > 0) {
		return InterpretFrame(buf, temp, t);
    } else {
        if (t < 0) perror("recv");
        else printf("Server closed connection\n");
    }
    return t;
}

int InterpretFrame(char *messageBuf, char *frameBuf, uint8_t frameLen){
	FixedHeader FHeader;
	uint8_t Qos, topicLen, messsageLen;
	uint16_t headLen = 0, index = 3;

	headLen = sizeof(FHeader);
	memcpy(&FHeader, frameBuf, headLen);
	Qos = (FHeader.Flags & 6) >> 1;

	topicLen = MSBandLSBTou16(frameBuf[index], frameBuf[index+1]);
	index += (2+topicLen);
	if(Qos > 0)
		index += 2;
	messsageLen = MSBandLSBTou16(frameBuf[index], frameBuf[index+1]);
	index += 2;
	if( (index+messsageLen) == frameLen){
		memcpy(messageBuf, frameBuf+index, messsageLen);
		return messsageLen;
	}
	return 0;
}
// ***************   Funtions for local processes   ***************//



// ***************   Funtions for MOPS broker   ***************//
int StartMOPSBroker(void)
{
	mlockall(MCL_CURRENT | MCL_FUTURE);

	mutex_init(&input_lock);
	mutex_init(&output_lock);
	mutex_init(&waiting_output_lock);
	mutex_init(&waiting_input_lock);

	InitTopicList(list);
	MOPS_QueueInit(mops_queue);
	SubListInit(sub_list);
	connectToRTnet();
	startNewThread((void*)&threadSendToRTnet, NULL);
	startNewThread((void*)&threadRecvFromRTnet, NULL);
	InitProcesConnection();

	return 0;
}

void MOPS_QueueInit(MOPS_Queue *queue){
	int i = 0;
	for(i=0; i<MAX_PROCES_CONNECTION; i++)
	{
		queue[i].MOPSToProces_fd = 0;
		queue[i].ProcesToMOPS_fd = 0;
	}
}

void SubListInit(SubscriberList *sublist){
	int i;
	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++){
		sublist[i].ClientID = -1;
		memset(sublist[i].Topic, 0, MAX_TOPIC_LENGTH+1);
	}
}

void DeleteProcessFromSubList(int ClientID, SubscriberList *sublist){
	int i;
	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++)
		if(i == ClientID){
			sublist[i].ClientID = -1;
			memset(sublist[i].Topic, 0, MAX_TOPIC_LENGTH+1);
		}
}

void threadRecvFromRTnet(){
    for(;;){
    	lock_mutex(&input_lock);
    	input_index = receiveFromRTnet(input_buffer, UDP_MAX_SIZE);
		AnalyzeIncomingUDP(input_buffer, input_index);
		memset(input_buffer, 0, UDP_MAX_SIZE);
		unlock_mutex(&input_lock);
    }
}

void threadSendToRTnet(){
	uint8_t are_local_topics = 0;
	int err = 0, _fd;

	// Open tdma device
	_fd = rt_dev_open("TDMA0", O_RDWR);
	if (_fd < 0)
		return;

	for(;;){
		err = rt_dev_ioctl(_fd, RTMAC_RTIOC_WAITONCYCLE, (void*)TDMA_WAIT_ON_SYNC);
		if(err)
			printf("Failed to issue RTMAC_RTIOC_WAITONCYCLE, err=%d\n", err);
		switch(MOPS_State){
		case SEND_NOTHING:
			//check if there are local topic to announce
			are_local_topics = ApplyIDtoNewTopics();
			MoveWaitingToFinal();
			if(are_local_topics)
				SendLocalTopics(list);
			else
				SendEmptyMessage();
			break;
		case SEND_REQUEST:
			SendTopicRequestMessage();
			break;
		case SEND_TOPIC_LIST:
			ApplyIDtoNewTopics();
			MoveWaitingToFinal();
			SendTopicList(list);
			break;
		}

		lock_mutex(&output_lock);
		if ( (output_index > sizeof(MOPSHeader)) || (output_buffer[0] == TOPIC_REQUEST) ){
			sendToRTnet(output_buffer, output_index);
			MOPS_State = SEND_NOTHING;
		}
		memset(output_buffer, 0, UDP_MAX_SIZE);
		output_index = 0;

		unlock_mutex(&output_lock);
	}
}

uint16_t SendEmptyMessage(){
	uint8_t tempLen = 0;
	uint16_t writtenBytes = 0;
	tempLen += sizeof(MOPSHeader);
	if ( tempLen > (UDP_MAX_SIZE-output_index) )
		printf("Not enough space to send Empty Header\n");

	lock_mutex(&output_lock);
	memmove(output_buffer+tempLen, output_buffer, output_index); //Move all existing data
	writtenBytes = buildEmptyMessage(output_buffer, UDP_MAX_SIZE-output_index);
	output_index += writtenBytes;
	unlock_mutex(&output_lock);
	return writtenBytes;
}

uint16_t SendTopicRequestMessage(){
	uint8_t tempLen = 0;
	uint16_t writtenBytes = 0;
	tempLen += sizeof(MOPSHeader);
	if ( tempLen > (UDP_MAX_SIZE-output_index) )
		printf("Not enough space to send Topic Request\n");

	lock_mutex(&output_lock);
	memmove(output_buffer+tempLen, output_buffer, output_index); //Move all existing data
	writtenBytes = buildTopicRequestMessage(output_buffer, UDP_MAX_SIZE-output_index);
	output_index += writtenBytes;
	unlock_mutex(&output_lock);
	return writtenBytes;
}

/*
 * Sending all available (not candidate) topics to RTnet,
 * after that local topics become global.
 */
uint16_t SendTopicList(TopicID list[]){
	int i = 0, counter = 0, tempLen;
	uint8_t *tempTopicList[MAX_NUMBER_OF_TOPIC];
	uint16_t tempTopicIDs[MAX_NUMBER_OF_TOPIC];
	uint16_t writtenBytes;

	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		if (list[i].ID != 0){
			tempTopicList[counter] = (uint8_t*)(&list[i].Topic);
			tempTopicIDs[counter] = list[i].ID;
			if(list[i].LocalTopic == 1)
				list[i].LocalTopic = 0;
			counter++;
		}
	}
	tempLen = sizeof(MOPSHeader);
	for (i=0; i<counter; i++)
		tempLen += 2 + 2 + strlen((char*)tempTopicList[i]); //2 for ID msb, ID lsb, 2 for length msb, length lsb.
	if ( tempLen > (UDP_MAX_SIZE-output_index) )
		printf("Not enough space to send all Topics from list\n");

	lock_mutex(&output_lock);
	memmove(output_buffer+tempLen, output_buffer, output_index); //Move all existing data
	writtenBytes = buildNewTopicMessage(output_buffer, UDP_MAX_SIZE-output_index, tempTopicList, tempTopicIDs, counter);
	output_index += writtenBytes;
	unlock_mutex(&output_lock);
	return writtenBytes;
}

/*
 * Sending only local topics to RTnet,
 * after that local topics become global.
 */
uint16_t SendLocalTopics(TopicID list[]){
	int i = 0, counter = 0, tempLen;
	uint8_t *(tempTopicList[MAX_NUMBER_OF_TOPIC]);
	uint16_t tempTopicIDs[MAX_NUMBER_OF_TOPIC];
	uint16_t writtenBytes;

	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		if (list[i].ID != 0 && list[i].LocalTopic==1){
			tempTopicList[counter] = (uint8_t*)(&list[i].Topic);
			tempTopicIDs[counter] = list[i].ID;
			list[i].LocalTopic = 0;
			counter++;
		}
	}

	tempLen = sizeof(MOPSHeader);
	for (i=0; i<counter; i++)
		tempLen += 2 + 2 + strlen((char*)tempTopicList[i]); //2 for ID msb, ID lsb, 2 for length msb, length lsb.
	if ( tempLen > (UDP_MAX_SIZE-output_index) )
		printf("Not enough space to send local Topics from list\n");

	lock_mutex(&output_lock);
	memmove(output_buffer+tempLen, output_buffer, output_index); //Move all existing data
	writtenBytes = buildNewTopicMessage(output_buffer, UDP_MAX_SIZE-output_index, tempTopicList, tempTopicIDs, counter);
	output_index += writtenBytes;
	unlock_mutex(&output_lock);
	return writtenBytes;
}


uint8_t AddTopicToList(TopicID list[], uint8_t *topic, uint16_t topicLen, uint16_t id){
	int i = 0;
	uint16_t tempTopicLength;
	tempTopicLength = (topicLen<MAX_TOPIC_LENGTH) ? topicLen : MAX_TOPIC_LENGTH;

	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		//if candidate, apply ID
		if(strncmp((char*)list[i].Topic, (char*)topic, tempTopicLength)==0 && list[i].Topic[0]!=0 && list[i].ID==0 ){
			list[i].ID = id;
			//printf("Dodalem ID kandydatowi: %s \n", list[i].Topic);
			return 0;
		}
		// if exists such topic (or at least ID) available, do not do anything
		if ( (list[i].ID == id) || (strncmp((char*)list[i].Topic, (char*)topic, tempTopicLength)==0 && list[i].Topic[0]!=0) ){
			//printf("Nie dodam bo jest: %s \n", list[i].Topic);
			return 2;
		}
	}

	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		//else add new topic in the first empty place
		if ( list[i].ID==0 && strlen((char*)list[i].Topic)==0 ){
			memcpy(list[i].Topic, topic, tempTopicLength);
			//printf("Dodany: %s \n", list[i].Topic);
			list[i].ID = id;
			return 0;
		}
	}
	//there is no place in TopicList
	return 1;
}


uint8_t ApplyIDtoNewTopics(){
	int i;
	uint8_t localTopicFlag = 0;
	uint16_t max = 0;

	lock_mutex(&output_lock);
	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		if(list[i].ID > max)
			max = list[i].ID;
	}
	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		if ( list[i].ID==0 && strlen((char*)list[i].Topic)!=0 ){
			list[i].ID = max+1;
			list[i].LocalTopic = 1;
			max++;
			localTopicFlag = 1;
		}
	}
	unlock_mutex(&output_lock);
	return localTopicFlag;
}

void AddTopicCandidate(uint8_t *topic, uint16_t topicLen){
	int i;
	uint16_t tempTopicLength;

	tempTopicLength = (topicLen<MAX_TOPIC_LENGTH) ? topicLen : MAX_TOPIC_LENGTH;
	if(GetIDfromTopicName(topic, tempTopicLength) == -1)
		for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
			if ( list[i].ID==0 && strlen((char*)list[i].Topic)==0 ){
				memcpy(list[i].Topic, topic, tempTopicLength);
				return;
			}
		}
}

/*
 * return:
 *  ID (uint16_t value) if topic exist already in TopicList and is available
 *  0					if topic is candidate in TopicList
 *  -1					if topic is not available, and not candidate
 */
int GetIDfromTopicName(uint8_t *topic, uint16_t topicLen){
	int i;
	uint16_t tempTopicLength;

	tempTopicLength = (topicLen<MAX_TOPIC_LENGTH) ? topicLen : MAX_TOPIC_LENGTH;
	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		if (strncmp((char*)list[i].Topic, (char*)topic, tempTopicLength)==0 && list[i].Topic[0]!=0)  //when  are the same
				return list[i].ID;
	}
	return -1;
}

/*
 * POST: variable 'topic' is set as Topic with id 'id',
 * if there is not a topic in TopicList with that id
 * variable 'topic' is set to \0.
 */
uint16_t GetTopicNameFromID(uint16_t id, uint8_t *topic){
	int i;
	uint16_t len = 0;

	memset(topic, 0, MAX_TOPIC_LENGTH+1);
	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		if (list[i].ID == id){  //when  are the same
			len = strlen((char*)list[i].Topic);
			memcpy(topic, &list[i].Topic, len);
			return len;
		}
	}
	return 0;
}

void InitTopicList(TopicID list[]){
	int i = 0;
	for (i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		list[i].ID = 0;
		list[i].LocalTopic = 0;
		memset(&list[i].Topic, 0, MAX_TOPIC_LENGTH+1);
	}
}

void PrintfList(TopicID list[]){
	int i;
	printf("Lista{\n");
	for(i=0; i<MAX_NUMBER_OF_TOPIC; i++){
		printf("    Topic: %s, ID: %d \n", list[i].Topic, list[i].ID);
	}
	printf("};\n");
}

void PrintfSubList(SubscriberList sublist[]){
	int i;
	printf("SubList{\n");
	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++){
		printf("    Topic: %s, SubscriberID: %d \n", sublist[i].Topic, sublist[i].ClientID);
	}
	printf("};\n");
}

void AnalyzeIncomingUDP(uint8_t *Buffer, int written_bytes){
	MOPSHeader MHeader;
	uint16_t MOPSMessageLen;
	uint8_t HeadLen = sizeof(MHeader);

	memcpy(&MHeader, Buffer, HeadLen);
	MOPSMessageLen = MSBandLSBTou16(MHeader.RemainingLengthMSB, MHeader.RemainingLengthLSB) + HeadLen;

	switch(MHeader.MOPSMessageType){
	case TOPIC_REQUEST:
		lock_mutex(&output_lock);
		MOPS_State = SEND_TOPIC_LIST;
		unlock_mutex(&output_lock);
		break;
	case NEW_TOPICS:
		lock_mutex(&output_lock);
		UpdateTopicList(Buffer, written_bytes);
		unlock_mutex(&output_lock);
		break;
	case NOTHING:
		//do not change state
		break;
	}
	//Move remaining data to buffer beginning
	lock_mutex(&waiting_input_lock);
	if( (UDP_MAX_SIZE-waiting_input_index)>=(written_bytes-MOPSMessageLen) ){ //If we have enough space
		memmove(waiting_input_buffer+waiting_input_index, Buffer+MOPSMessageLen, written_bytes-MOPSMessageLen);
		waiting_input_index += (written_bytes-MOPSMessageLen);
	}
	unlock_mutex(&waiting_input_lock);
}

void UpdateTopicList(uint8_t *Buffer, int BufferLen){
	uint16_t index = 0, messageLength = 0;
	uint16_t tempTopicLength = 0, tempTopicID = 0;
	uint8_t err;

	messageLength = MSBandLSBTou16(Buffer[1], Buffer[2]) + 3;
	index += 3;
	for(; index<messageLength; ){
		tempTopicID = MSBandLSBTou16(Buffer[index], Buffer[index+1]);
		tempTopicLength = MSBandLSBTou16(Buffer[index+2], Buffer[index+3]);
		index += 4;

		err = AddTopicToList(list, Buffer+index, tempTopicLength, tempTopicID);
		index += tempTopicLength;
		if(err == 1)
			printf("Brak miejsca na liscie! \n");
		if(err == 0)
			printf("Dodalem, id: %d \n", tempTopicID);
		if(err == 2)
			printf("Topic, id: %d, juz istnieje. \n", tempTopicID);
	}
}

int AddToMOPSQueue(int MOPS_Proces_fd, int Proces_MOPS_fd){
	int i = 0;
	for(i=0; i<MAX_PROCES_CONNECTION; i++)
		if(mops_queue[i].MOPSToProces_fd==0 && mops_queue[i].ProcesToMOPS_fd==0){
			mops_queue[i].MOPSToProces_fd = MOPS_Proces_fd;
			mops_queue[i].ProcesToMOPS_fd = Proces_MOPS_fd;
			return i;
		}
	return -1;
}

#if TARGET_DEVICE == Linux
void InitProcesConnection(){
    mqd_t mq_listener, new_mq_Proces_MOPS;
    struct mq_attr attr;
    struct timeval tv;
    int fdmax, rv, i;
    fd_set master, read_fd;  //master fd list, temp fd list for select()
	FD_ZERO(&master);
	FD_ZERO(&read_fd);

    /* initialize the queue attributes */
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_QUEUE_MESSAGE;
    attr.mq_msgsize = MAX_QUEUE_SIZE;
    attr.mq_curmsgs = 0;

	mq_listener = mq_open(QUEUE_NAME, O_CREAT | O_RDONLY, 0644, &attr);
    if( !((mqd_t)-1 != mq_listener) )
    	perror("MQueue Open listener");

    FD_SET(mq_listener, &master);
    fdmax = mq_listener;
    for (;;){
    	tv.tv_sec = 0;
    	tv.tv_usec = 1;
    	read_fd = master;
    	rv = select(fdmax+1, &read_fd, NULL, NULL, &tv);
    	if(rv > 0){		// there are file descriptors to serve
    		for(i = 0; i <=fdmax; i++){
    			if (FD_ISSET(i, &read_fd)){
					if(i == mq_listener){
						new_mq_Proces_MOPS = ServeNewProcessConnection(&master, mq_listener);
						if(new_mq_Proces_MOPS > fdmax)
							fdmax = new_mq_Proces_MOPS;
					}
					else{
						ReceiveFromProcess(i);
					}
    			}
    		}
    	}
    	if(rv < 0)		// error occurred in select()
    	    perror("select");
    	if(rv == 0)		// timeout, we can do our things
    		ServeSendingToProcesses();
    }
}

int ReceiveFromProcess(int file_de){
	int bytes_read, ClientID;
    uint8_t temp[MAX_QUEUE_SIZE+1];

	bytes_read = mq_receive(file_de, (char*)temp, MAX_QUEUE_SIZE, NULL);
	if(bytes_read == -1){
		CloseProcessConnection(file_de);
	}
	if(bytes_read>=sizeof(FixedHeader)){
		ClientID = FindClientIDbyFileDesc(file_de);
		AnalyzeProcessMessage(temp, bytes_read, ClientID);
	}
	return 0;
}

int SendToProcess(uint8_t *buffer, uint16_t buffLen, int file_de){
	struct mq_attr attr;
	attr.mq_flags = 0;
	attr.mq_maxmsg = MAX_QUEUE_MESSAGE;
	attr.mq_msgsize = MAX_QUEUE_SIZE;
	attr.mq_curmsgs = 0;

	mq_getattr(file_de, &attr);
	if(attr.mq_curmsgs < MAX_QUEUE_MESSAGE)
		return mq_send(file_de, (char*)buffer, buffLen, 0);
	return 0;
}

/*
 * Return:
 * 	file descriptor (int) - when there is place in MOPSQueue array
 * 	-1 					  - if there is not place in MOPSQueue array or no message received from listener_fd
 */
int ServeNewProcessConnection(fd_set *set, int listener_fd){
    uint8_t buffer[MAX_QUEUE_SIZE+1], temp;
    int new_mq_Proces_MOPS, new_mq_MOPS_Proces;

    memset(buffer, 0, MAX_QUEUE_SIZE+1);
    if(mq_receive(listener_fd, (char*)buffer, MAX_QUEUE_SIZE, NULL) > 0){
    	temp = strlen((char*)buffer);
    	buffer[temp] = 'b';
    	new_mq_Proces_MOPS = mq_open((char*)buffer, O_RDONLY);
		if( !((mqd_t)-1 != new_mq_Proces_MOPS) )
			perror("MQueue Open Proces_MOPS");

    	buffer[temp] = 'a';
		new_mq_MOPS_Proces = mq_open((char*)buffer, O_WRONLY);
		if( !((mqd_t)-1 != new_mq_MOPS_Proces) )
			perror("MQueue Open MOPS_Proces");

		if (AddToMOPSQueue(new_mq_MOPS_Proces, new_mq_Proces_MOPS) >= 0){
			FD_SET(new_mq_Proces_MOPS, set);
			printf("Nowy deskryptor: %d, nazwa kolejki: %s \n", new_mq_Proces_MOPS, buffer);
			return new_mq_Proces_MOPS;
		}
    }
    return -1;
}

void DeleteProcessFromQueueList(int ClientID, MOPS_Queue *queue){
	mq_close(queue[ClientID].MOPSToProces_fd);
	mq_close(queue[ClientID].ProcesToMOPS_fd);

	queue[ClientID].MOPSToProces_fd = 0;
	queue[ClientID].ProcesToMOPS_fd = 0;
}

#endif //TARGET_DEVICE == Linux


//TODO
#if TARGET_DEVICE == RTnode
void InitProcesConnection(){

	for(;;){}
}
#endif //TARGET_DEVICE == RTnode

void CloseProcessConnection(int file_de){
	int ClientID;
	printf("Proces ubijam!\n");
	ClientID = FindClientIDbyFileDesc(file_de);
	DeleteProcessFromQueueList(ClientID, mops_queue);
	DeleteProcessFromSubList(ClientID, sub_list);
}

int ServeSendingToProcesses(){
	uint8_t tempBuffer[UDP_MAX_SIZE], HeadLen;
	uint16_t FrameLen = 0, OldFrameLen = 0, written_bytes = 0;
	FixedHeader FHeader;
	memset(tempBuffer, 0, UDP_MAX_SIZE);

	lock_mutex(&waiting_input_lock);
	if(waiting_input_index > 0){
		written_bytes = waiting_input_index;
		memcpy(tempBuffer, waiting_input_buffer, waiting_input_index);
		memset(waiting_input_buffer, 0 , UDP_MAX_SIZE);
		waiting_input_index = 0;
	}
	unlock_mutex(&waiting_input_lock);

	if(written_bytes>0){
		HeadLen = sizeof(FHeader);
		memcpy(&FHeader, tempBuffer + FrameLen, HeadLen);
		FrameLen += MSBandLSBTou16(FHeader.RemainingLengthMSB, FHeader.RemainingLengthLSB) + HeadLen;
		while(FHeader.MessageType!=0 && FrameLen<=written_bytes)
		{
			PrepareFrameToSendToProcess(tempBuffer+OldFrameLen, FrameLen-OldFrameLen);
			memcpy(&FHeader, tempBuffer + FrameLen, HeadLen);
			OldFrameLen = FrameLen;
			FrameLen += MSBandLSBTou16(FHeader.RemainingLengthMSB, FHeader.RemainingLengthLSB) + HeadLen;
		}
	}
	return 0;
}

void PrepareFrameToSendToProcess(uint8_t *Buffer, int written_bytes){
	uint16_t topicID, topicLen, index = 0;
	uint8_t tempBuffer[MAX_QUEUE_SIZE], HeaderLen;
	uint8_t tempTopic[MAX_TOPIC_LENGTH+1], tempMSB = 0, tempLSB = 0;
	FixedHeader FHeader;
	int clientID[MAX_PROCES_CONNECTION], i;

	memset(tempBuffer, 0, MAX_QUEUE_SIZE);
	memcpy(tempBuffer, Buffer, written_bytes);
	HeaderLen = sizeof(FHeader);

	topicID = MSBandLSBTou16(tempBuffer[HeaderLen], tempBuffer[HeaderLen+1]);
	topicLen = GetTopicNameFromID(topicID, tempTopic);
	FindClientsIDbyTopic(clientID, tempTopic, topicLen);
	u16ToMSBandLSB(topicLen, &tempMSB, &tempLSB);

	tempBuffer[ HeaderLen ] = tempMSB;
	tempBuffer[HeaderLen+1] = tempLSB;
	index = HeaderLen+2;
	memmove(tempBuffer+index+topicLen, tempBuffer+index, written_bytes-index);
	memcpy(tempBuffer+index, tempTopic, topicLen);

	for(i=0; i<MAX_PROCES_CONNECTION; i++)
		if (clientID[i] != -1)
			SendToProcess(tempBuffer, written_bytes+topicLen, mops_queue[clientID[i]].MOPSToProces_fd);
}

void FindClientsIDbyTopic(int *clientsID, uint8_t *topic, uint16_t topicLen){
	int i;
	int counter = 0;
	for(i=0; i<MAX_PROCES_CONNECTION; i++)
		clientsID[i] = -1;

	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++){
		if(strncmp((char*)sub_list[i].Topic, (char*)topic, topicLen) == 0){
			clientsID[counter] = sub_list[i].ClientID;
			counter++;
		}
	}
}

int FindClientIDbyFileDesc(int file_de){
	int i = 0;
	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++)
		if( mops_queue[i].MOPSToProces_fd==file_de || mops_queue[i].ProcesToMOPS_fd==file_de)
			return i;
	return -1;
}

void AnalyzeProcessMessage(uint8_t *buffer, int bytes_wrote, int ClientID){
	FixedHeader FHeader;
	uint8_t HeadLen = 0;
	uint16_t FrameLen = 0, OldFrameLen = 0;
	HeadLen = sizeof(FHeader);

	memcpy(&FHeader, buffer + FrameLen, HeadLen);
	FrameLen += MSBandLSBTou16(FHeader.RemainingLengthMSB, FHeader.RemainingLengthLSB) + HeadLen;
	while(FHeader.MessageType!=0 && FrameLen<=bytes_wrote)
	{
		switch(FHeader.MessageType){
		case PUBLISH:
			ServePublishMessage(buffer+OldFrameLen, FrameLen-OldFrameLen);
			break;
		case SUBSCRIBE:
			ServeSubscribeMessage(buffer+OldFrameLen, FrameLen-OldFrameLen, ClientID);
			break;
		}
		memcpy(&FHeader, buffer + FrameLen, HeadLen);
		OldFrameLen = FrameLen;
		FrameLen += MSBandLSBTou16(FHeader.RemainingLengthMSB, FHeader.RemainingLengthLSB) + HeadLen;
	}
}

void ServePublishMessage(uint8_t *buffer, int FrameLen){
	uint8_t topicTemp[MAX_TOPIC_LENGTH+1];
	uint16_t TopicLen, index = 0, tempTopicLength;
	int topicID;
	memset(topicTemp, 0, MAX_TOPIC_LENGTH+1);

	index+=3;
	TopicLen = MSBandLSBTou16(buffer[index], buffer[index+1]);
	index+=2;
	tempTopicLength = (TopicLen<MAX_TOPIC_LENGTH) ? TopicLen : MAX_TOPIC_LENGTH;
	memcpy(topicTemp, buffer+index, tempTopicLength);
	index+=TopicLen;
	topicID = GetIDfromTopicName(topicTemp, TopicLen);
	switch(topicID){
	case -1:
		AddTopicCandidate(topicTemp, TopicLen);
		AddPacketToWaitingTab(buffer, FrameLen);
		break;
	case 0:
		AddPacketToWaitingTab(buffer, FrameLen);
		break;
	default:
		AddPacketToFinalTab(buffer, FrameLen, topicID);
		break;
	}
}

void ServeSubscribeMessage(uint8_t *buffer, int FrameLen, int ClientID){
	uint16_t TopicLen, index = 0;

	index+=5;
	do{
		TopicLen = MSBandLSBTou16(buffer[index], buffer[index+1]);
		index+=2;
		AddToSubscribersList(buffer+index, TopicLen, ClientID);
		index+=(TopicLen+1);
	}while(index<FrameLen);
}

int AddToSubscribersList(uint8_t *topic, uint16_t topicLen, int ClientID){
	int i = 0;
	uint16_t tempTopicLen;

	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++){
		if(sub_list[i].ClientID==ClientID && strncmp((char*)sub_list[i].Topic, (char*)topic, topicLen)==0 && sub_list[i].Topic[0]!=0){
			return -1; //This subscription for that client already exists
		}
	}
	tempTopicLen = (topicLen < MAX_TOPIC_LENGTH) ? topicLen : MAX_TOPIC_LENGTH;
	for(i=0; i<MAX_NUMBER_OF_SUBSCRIPTIONS; i++){
		if(sub_list[i].ClientID == -1){
			memcpy(sub_list[i].Topic, topic, tempTopicLen);
			sub_list[i].ClientID = ClientID;
			return i; //Subscription has been added successfully
		}
	}
	return 0; //There is no place to store subscription!
}

void AddPacketToWaitingTab(uint8_t *buffer, int FrameLen){
	lock_mutex(&waiting_output_lock);
	if(waiting_output_index <= (uint16_t)(UDP_MAX_SIZE*9)/10){
		memcpy(waiting_output_buffer+waiting_output_index, buffer, FrameLen);
		waiting_output_index += FrameLen;
	}
	unlock_mutex(&waiting_output_lock);
}

void AddPacketToFinalTab(uint8_t *buffer, int FrameLen, uint16_t topicID){
	uint8_t tempBuff[MAX_QUEUE_SIZE];
	uint8_t MSBtemp, LSBtemp, headLen, index = 0;
	uint16_t TopicLen, MessageLen;
	memset(tempBuff,0,MAX_QUEUE_SIZE);

	headLen = sizeof(FixedHeader);
	u16ToMSBandLSB(topicID, &MSBtemp, &LSBtemp);
	memcpy(tempBuff, buffer, headLen);
	MessageLen = MSBandLSBTou16(buffer[1], buffer[2]);

	tempBuff[ headLen ] = MSBtemp;
	tempBuff[headLen+1] = LSBtemp;
	index = headLen+2;

	TopicLen = MSBandLSBTou16(buffer[headLen], buffer[headLen+1]);
	MessageLen = MessageLen - TopicLen;
	u16ToMSBandLSB(MessageLen, &MSBtemp, &LSBtemp);
	tempBuff[1] = MSBtemp; //New message len MSB
	tempBuff[2] = LSBtemp; //New message len LSB

	memcpy( tempBuff+index, buffer+index+TopicLen, FrameLen-(index+TopicLen) );

	lock_mutex(&output_lock);
	if(output_index <= (uint16_t)(UDP_MAX_SIZE*9)/10){
		memcpy(output_buffer+output_index, tempBuff, FrameLen-TopicLen);
		output_index += (FrameLen-TopicLen);
	}
	unlock_mutex(&output_lock);
}

void MoveWaitingToFinal(){
	uint8_t tempTab[UDP_MAX_SIZE];
	uint16_t tempIndex = 0;

	lock_mutex(&waiting_output_lock);
	memcpy(tempTab, waiting_output_buffer, waiting_output_index);
	memset(waiting_output_buffer, 0 , UDP_MAX_SIZE);
	tempIndex = waiting_output_index;
	waiting_output_index = 0;
	unlock_mutex(&waiting_output_lock);

	AnalyzeProcessMessage(tempTab, tempIndex, -1);
}
// ***************   Funtions for MOPS broker   ***************//


void u16ToMSBandLSB(uint16_t u16bit, uint8_t *MSB, uint8_t *LSB){
	uint16_t temp;
	*LSB = (uint8_t) u16bit;
	temp = u16bit>>8;
	*MSB = (uint8_t) temp;
}

uint16_t MSBandLSBTou16(uint8_t MSB, uint8_t LSB){
	uint16_t temp;
	temp = MSB;
	temp = temp<<8;
	temp += LSB;
	return temp;
}