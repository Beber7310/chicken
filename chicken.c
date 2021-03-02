#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

void * http_loop(void * arg);

int http_hold=0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

int main(void)
{ 

	pthread_t th_http;
	
	if (pthread_create(&th_http, NULL, http_loop, 0) < 0)
	{
		printf("pthread_create error for thread http_loop");
		exit(1);
	}
	
	
    while(1)
    {
	sleep(1);
	time_t now = time(NULL);
	struct tm *tm_struct = localtime(&now);

	int mon  	= tm_struct->tm_mon;
	int hour 	= tm_struct->tm_hour;
	int minute 	= tm_struct->tm_min;
	
	int openHour    = 8;
	int openMinute  = 0;

	int closeHour   = 18;
	int closeMinute =  0;
		
	
	printf("Current time: %i %i.%0i\n",mon, hour, minute);
	
	system("hdate -z1 -s -l45 -L6");
	
	switch (mon)
	{
		case 0: //Janvier
			openHour    = 8;
			openMinute  = 0;
			closeHour   = 17;
			closeMinute = 45;
	      	break;

		case 1:
			openHour    = 8;
			openMinute  = 0;
			closeHour   = 18;
			closeMinute = 30;
		break;
		
		case 2:
			openHour    = 8;
			openMinute  = 0;
			closeHour   = 18;
			closeMinute = 30;
		break;
	
		default:
		// default statements
		break;
		
	}
	
	pthread_mutex_lock(&mutex);
	if(http_hold<1)
	{
		if((hour==openHour)&&(minute==openMinute))
		{
			system("./openChicken");
		}
		else if((hour==closeHour)&&(minute==closeMinute))
		{
			system("./closeChicken");
		}
		else
		{
			system("./stopChicken");
		}
	}else{		
		http_hold--;		
	}
	pthread_mutex_unlock(&mutex);
	
	
    }
}
