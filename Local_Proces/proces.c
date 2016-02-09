#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#include "MOPS.h"


int main(void)
{
    int s;
    uint8_t topic[][1]={"Topic"};
    uint8_t Qos[][1]={1};

	s = connectMOPS();
	subscribeMOPS(topic, Qos);

	for(;;){
	    sleep(2);
		publishMOPS(s, "Cos5", "Message");
	}
    close(s);
    return 0;
}


/*
rv = select(s+1, &read_fd, NULL, NULL, &tv);
if(rv > 0){
	len = readMOPS(s, buffer, 100);

	printf("%s \n", buffer);

}
if(rv < 0 ){
	printf("Tutaj \n");

	perror("selecet");
}
if( rv == 0 ){
	//perror("selecet");

}
*/
