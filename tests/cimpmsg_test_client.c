#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include "utlist.h"
#include "cimpmsg.h"

/*------------------------------------------------------------------
 * client receive should be blocking, but have a timeout so we can
*  detect terminate flag 
*  client send should be blocking
---------------------------------------------------------------------*/

#define SOCK_SEND_TIMEOUT_MSEC 2000
#define IP_ADDR "127.0.0.1"

struct options {
  bool send_random;
  bool print_send_msgs;
  bool sleep_at_end;
  bool send_stop_msg_at_end;
  unsigned int msg_filler;
} OPT;

size_t msg_buf_size = 128;

struct client_stuff {
  const char *port_str;
  unsigned int send_count;
  const char *send_msg;
  struct client_conn conn;
} CLI = {
  .port_str = NULL, .send_count = 0, .send_msg = NULL,
  .conn = CMSG_CLIENT_CONN_INITIALIZER
};


void init_options (void)
{
  OPT.send_random = false;
  OPT.print_send_msgs = false;
  OPT.sleep_at_end = false;
  OPT.send_stop_msg_at_end = false;
  OPT.msg_filler = 0;
}


// waits from 0 to 0x1FFFF (131071) usecs
void wait_random (void)
{
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = random() >> 15;
  select (0, NULL, NULL, NULL, &timeout);
}

void wait_msecs (unsigned msecs)
{
  struct timeval timeout;
  unsigned msecs_lo = msecs % 1000;
  timeout.tv_sec = msecs / 1000;
  timeout.tv_usec = 1000 * msecs_lo;
  select (0, NULL, NULL, NULL, &timeout);
}

unsigned int parse_num_arg (const char *arg, const char *arg_name)
{
	unsigned int result = 0;
	int i;
	char c;
	
	if (arg[0] == '\0') {
		printf ("Empty %s argument\n", arg_name);
		return (unsigned int) -1;
	}
	for (i=0; '\0' != (c=arg[i]); i++)
	{
		if ((c<'0') || (c>'9')) {
			printf ("Non-numeric %s argument\n", arg_name);
			return (unsigned int) -1;
		}
		result = (result*10) + c - '0';
	}
	return result;
}


pthread_t client_rcv_thread_id;

static int create_thread (pthread_t *tid, void *(*thread_func) (void*), void *arg)
{
	int rtn = pthread_create (tid, NULL, thread_func, arg);
	if (rtn != 0) {
	  printf ("Error creating thread\n");
	}
	return rtn; 
}

static void *client_receiver_thread (void *arg)
{
  client_conn_t *conn = (client_conn_t *) arg;
  ssize_t rtn;

  //printf ("Started client receiver thread for %d\n", getpid());
  while (true) {
    rtn = cmsg_client_receive (conn);
    if (rtn < 0)
      break;
    printf ("Client %d received: %s\n", getpid(), conn->rcv_msg);
    free (conn->rcv_msg);
    conn->rcv_msg = NULL;
  }
  //printf ("Ending client receiver thread for %d\n", getpid());
  return NULL;
}

void make_filled_msg (const char *msg, unsigned msg_num, char *filled_msg)
{
  unsigned int i;
  for (i=0; i<OPT.msg_filler; i++)
     filled_msg[i] = '.';
  sprintf (filled_msg+OPT.msg_filler, "%s %d", msg, msg_num);
}

void wait_with_msg (unsigned wait_secs)
{
  unsigned i;
  for (i=0; i<wait_secs; i+=5) {
    printf ("Sleeping..\n");
    sleep (5);
  }
}

void client_send_multiple (void)
{
  unsigned long i;
  size_t sz_msg;
  char buf[msg_buf_size+OPT.msg_filler];

  if (CLI.send_count == 0)
    CLI.send_count = 1;

  printf ("Sending %u messages from pid %d\n", CLI.send_count, getpid());

  for (i=0; i<CLI.send_count; i++) {
    //if (i==100) // allow other senders to catch up
    //  wait_msecs (2000);
	  if ((i>0) && (CLI.conn.rcv_count == 0))
	    wait_msecs (250);
	  else if (OPT.send_random)
	    wait_random ();
	  make_filled_msg (CLI.send_msg, i, buf);
	  sz_msg = strlen(buf) + 1;
	  if (cmsg_client_send(&CLI.conn, buf, sz_msg, false) != 0)
		break;
	  if (OPT.print_send_msgs)
	    printf ("Sent msg %lu\n", i);
  }
  if (OPT.send_stop_msg_at_end) {
    printf ("Sending STOP message\n");
    strncpy (buf, "STOP\n", 6);
    cmsg_client_send (&CLI.conn, buf, 6, false);
    wait_with_msg (30);
  }
}


int get_args (const int argc, const char **argv)
{
	int i;
	int mode = 0;

	for (i=1; i<argc; i++)
	{
		const char *arg = argv[i];
		if ((strlen(arg) == 1) && 
		    ((arg[0] == 's') || (arg[0] == 'p')) ) 
		{
			mode = 'p';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 'm')) {
			mode = 'm';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 'n')) {
			mode = 'n';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 'f')) {
			mode = 'f';
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "pr") == 0)) {
			OPT.print_send_msgs = true;
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "rnd") == 0)) {
			OPT.send_random = true;
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "sl") == 0)) {
			OPT.sleep_at_end = true;
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "st") == 0)) {
			OPT.send_stop_msg_at_end = true;
			continue;
		}
		if (mode == 'p') {
			CLI.port_str = arg;
			mode = 0;
			continue;
		}
		if (mode == 'm') {
			CLI.send_msg = arg;
			mode = 0;
			continue;
		}
		if (mode == 'n') {
			CLI.send_count = parse_num_arg (arg, "send_count");
			if (CLI.send_count == (unsigned) -1)
			  return -1;
			mode = 0;
			continue;
		}
		if (mode == 'f') {
			OPT.msg_filler = parse_num_arg (arg, "long_msg_filler");
			if (OPT.msg_filler == (unsigned) -1)
			  return -1;
			mode = 0;
			continue;
		}
		printf ("arg not preceded by r/s/m/n/f specifier\n");
		return -1;
	} 
	return 0;
}


int main (const int argc, const char **argv)
{
  unsigned int port;

	srandom (getpid());

	init_options ();

	if (get_args(argc, argv) != 0)
		exit (4);

	if ((NULL != CLI.send_msg) && (NULL == CLI.port_str)) {
		printf ("msg specified witlhout a sender ID\n");
		exit(4);
	}

	if (NULL == CLI.port_str) {
		printf ("port not specified\n");
		exit(0);
	}


	port = parse_num_arg (CLI.port_str, "port");
	if (port == (unsigned int) (-1))
	    exit (4);
	if (NULL == CLI.send_msg) {
		printf ("Message not specified for client\n");
		exit(4);
	}
	if (cmsg_connect_client (&CLI.conn, IP_ADDR, port, 
		SOCK_SEND_TIMEOUT_MSEC) < 0)
	    exit(4);
	if (create_thread (&client_rcv_thread_id, client_receiver_thread, &CLI.conn) == 0)
	{
 	    client_send_multiple ();
	    if (OPT.sleep_at_end) {
	      if (OPT.send_stop_msg_at_end)
		wait_with_msg (15);
              else
		wait_with_msg (45);
	    }
            CLI.conn.terminated = true;
            pthread_join (client_rcv_thread_id, NULL);
	}
        cmsg_shutdown_client (&CLI.conn);


  printf ("%d Done!\n", getpid());
}
