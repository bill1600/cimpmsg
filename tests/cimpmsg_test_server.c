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

#define UNUSED(x) (void )(x)

/*------------------------------------------------------------------
*  server receive should be blocking
*  server send should be non-bocking so we can do send all
---------------------------------------------------------------------*/

#define SOCK_SEND_TIMEOUT_MSEC 2000
#define IP_ADDR "127.0.0.1"


typedef struct connection {
  int sock;
  bool stopped;
  unsigned int rcv_count;
  unsigned int max_rcv_count;
  struct connection * next;
} connection_t;

static struct server_stuff {
  server_opts_t opts;
  unsigned int port;
  unsigned int max_rcv_count;  // only applies to the first client
  unsigned int idle_notify_count;
  unsigned int max_idle_count;
  const char *waiting_msg;
  bool close_inactive;
  bool rcv_process_terminated;
  bool send_process_terminated;
  pthread_mutex_t list_mutex;
  struct connection * connection_list;
} SRV
 = {
     .opts = {.terminate_on_keypress = true, 
       .all_idle_notify_secs = 2,
       .inactive_conn_notify_secs = 30},
     .port = 0,
     .max_rcv_count = 0,
     .idle_notify_count = 0,
     .max_idle_count = 0,
     .waiting_msg = "Waiting for receive. Press <Enter> to terminate.\n",
     .close_inactive = false,
     .rcv_process_terminated = false,
     .send_process_terminated = false,
     .list_mutex = PTHREAD_MUTEX_INITIALIZER,
     .connection_list = NULL
   };


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


pthread_t server_send_thread_id;
bool server_received_something = false;

typedef struct {
  int allocated, used;
  int *sockets;
} socket_list_t;

void init_socket_list (socket_list_t *slist)
{
  slist->allocated = 0;
  slist->used = 0;
  slist->sockets = NULL;
}

void free_socket_list (socket_list_t *slist)
{
  if (NULL != slist->sockets)
    free (slist->sockets);
  init_socket_list (slist);
}

int append_to_socket_list (socket_list_t *slist, int socket)
{
  if (slist->allocated == 0) {
    slist->sockets = (int *) malloc (20 * sizeof (int));
    if (NULL == slist->sockets) {
      printf ("Unable to allocate memory for sockets list\n");
      return -1;
    }
    slist->allocated = 20;
  }
  if (slist->used >= slist->allocated) {
    int new_alloc = slist->allocated + 20;
    int *new_sockets = (int *) realloc (slist->sockets, new_alloc * sizeof(int));
    if (NULL == new_sockets) {
      printf ("Unable to allocate memory to expand sockets list\n");
      return -1;
    }
    slist->sockets = new_sockets;
    slist->allocated = new_alloc;
  }
  slist->sockets[slist->used++] = socket;
  return 0;
}

int find_socket_in_list (socket_list_t *slist, int socket)
{
  int i;
  for (i=0; i<slist->used; i++)
    if (socket == slist->sockets[i])
      return i;
  return -1;
}

static int create_thread (pthread_t *tid, void *(*thread_func) (void*), void *arg)
{
	int rtn = pthread_create (tid, NULL, thread_func, arg);
	if (rtn != 0) {
	  printf ("Error creating thread\n");
	}
	return rtn; 
}


void server_send_to_next_client (socket_list_t *done_list, socket_list_t *retry_list,
  const char *msg)
{
  int rtn;
  size_t sz_msg = strlen(msg) + 1;
  bool found = false;
  struct connection *conn;

  while (true) {
    found = false;
    pthread_mutex_lock (&SRV.list_mutex);
    LL_FOREACH (SRV.connection_list, conn) {
      if (conn->stopped)
        continue;
      if (find_socket_in_list (done_list, conn->sock) >= 0)
        continue;
      if (find_socket_in_list (retry_list, conn->sock) >= 0)
        continue;
      if ((conn->max_rcv_count > 0) && (conn->rcv_count >= conn->max_rcv_count))
        continue;
      found = true;
      if (conn->rcv_count == 0) {
        append_to_socket_list (retry_list, conn->sock);
        break;
      }
      rtn = cmsg_server_send (conn->sock, msg, sz_msg, true);
      if ((rtn == 0) || (rtn == EBADF))
        append_to_socket_list (done_list, conn->sock);
      else
        append_to_socket_list (retry_list, conn->sock);
      break;
    }
    pthread_mutex_unlock (&SRV.list_mutex);
    if (!found)
      break;
  }
}

int server_send_to_all_clients (const char *msg, unsigned timeout_ms,
  bool *terminated)
{
  int rtn;
  socket_list_t done_list;
  socket_list_t retry_list;
  unsigned delay = 0, total_delay = 0;

  init_socket_list (&done_list);
  init_socket_list (&retry_list);
  
  while (!server_received_something && !*terminated)
    wait_msecs (250);

  server_send_to_next_client (&done_list, &retry_list, msg);

  while (!*terminated) {
    if (delay == 0)
      delay = 10;
    else if (delay == 10)
      delay = 20;
    else if (delay == 20)
      delay = 50;
    else if (delay == 50)
      delay = 100;
    else if (delay == 100)
      delay = 200;
    else if (delay == 200)
      delay = 500;
    else if (delay == 500)
      delay = 1000;
    if (timeout_ms != 0)
      if ((total_delay+delay) > timeout_ms)
        delay = timeout_ms - total_delay;
    wait_msecs (delay);
    init_socket_list (&retry_list);
    server_send_to_next_client (&done_list, &retry_list, msg);
    if (timeout_ms != 0) {
      total_delay += delay;
      if (total_delay >= timeout_ms) {
        init_socket_list (&done_list);
        delay = 0;
        total_delay = 0;
      }
    }
  } // end while
  rtn = retry_list.used;
  free_socket_list (&done_list);
  free_socket_list (&retry_list);
  return rtn;
}

static void *server_send_thread (void *arg)
{
  UNUSED (arg);
  int rtn;

  printf ("Starting server send thread\n");
  rtn = server_send_to_all_clients ("Hello from the server!", 1000,
    &SRV.send_process_terminated);
  if (0 != rtn)
    printf ("Messages not sent to %d clients\n", rtn);
  printf ("Ending server send thread\n");
  return NULL;
}


void show_msg (server_rcv_msg_data_t *rcv_msg_data, connection_t *conn)
{
  unsigned i;
  unsigned count;
  char *buf = rcv_msg_data->rcv_msg;
  size_t bytes = rcv_msg_data->rcv_msg_size;

  if (NULL == conn) {
    printf ("Message socket %d not found in list\n", rcv_msg_data->sock);
    count = 0;
  } else {
    count = conn->rcv_count;
  }
  if ((count & 0xFF) == 0) {
    printf ("RECEIVED \"%s\"\n", buf);
    return;
  }

  for (i=0; i<bytes; i++) {
    if (buf[i] == '.')
      continue;
    printf ("RECEIVED \"%s\"\n", buf+i);
    return;
  }

}

bool check_max_received (connection_t *conn)
{
  conn->rcv_count += 1;
  if (conn->max_rcv_count == 0)
    return false;
  if (conn->rcv_count >= conn->max_rcv_count) {
     printf ("Max receive (%d) reached on socket %d, closing.\n",
            conn->max_rcv_count, conn->sock);
     cmsg_server_close_sock (conn->sock);
     return true;
  }
  return false;
}

void check_for_stop_msg (connection_t *conn, server_rcv_msg_data_t *rcv_msg_data)
{
  if (rcv_msg_data->rcv_msg_size < 4)
    return;
  if (strncmp (rcv_msg_data->rcv_msg, "STOP", 4) == 0) {
    printf ("Received STOP message for socket %d\n", rcv_msg_data->sock);
    printf ("Sending no more on socket %d\n", rcv_msg_data->sock);
    conn->stopped = true;
  }
}

void process_rcv_msg (int action_code, server_rcv_msg_data_t *rcv_msg_data)
{
  connection_t *conn;
  connection_t *tmp;
  int inactive_sock = -1;

  switch (action_code) {
    case CMSG_ACTION_CONN_ADDED:
      pthread_mutex_lock (&SRV.list_mutex);
      conn = (connection_t *) malloc (sizeof (connection_t));
      if (NULL != conn) {
        conn->sock = rcv_msg_data->sock;
        conn->stopped = false;
        conn->rcv_count = 0;
        if (NULL == SRV.connection_list)
          conn->max_rcv_count = SRV.max_rcv_count;
        else
          conn->max_rcv_count = 0;
        LL_APPEND (SRV.connection_list, conn);
      } else {
        printf ("Unable to alloc memory for new connection\n");
      }
      pthread_mutex_unlock (&SRV.list_mutex);
      break;
    case CMSG_ACTION_CONN_DROPPED:
      pthread_mutex_lock (&SRV.list_mutex);
      LL_FOREACH_SAFE (SRV.connection_list, conn, tmp)
        if (conn->sock == rcv_msg_data->sock) {
          printf ("Socket %d dropped\n", conn->sock);
          LL_DELETE (SRV.connection_list, conn);
          free (conn);
          break;
        }
      pthread_mutex_unlock (&SRV.list_mutex);
      break;
    case CMSG_ACTION_CONN_INACTIVE:
      pthread_mutex_lock (&SRV.list_mutex);
      LL_FOREACH_SAFE (SRV.connection_list, conn, tmp)
        if (conn->sock == rcv_msg_data->sock) {
          if (SRV.close_inactive) {
            printf ("Closing inactive socket %d\n", conn->sock);
            inactive_sock = conn->sock;
            LL_DELETE (SRV.connection_list, conn);
            free (conn);
          } else {
            printf ("Socket %d is inactive\n", conn->sock);
          }
          break;
        }
      pthread_mutex_unlock (&SRV.list_mutex);
      if (inactive_sock != -1)
        cmsg_server_close_sock (inactive_sock);
      break;
    case CMSG_ACTION_MSG_RECEIVED:
      conn = NULL;
      pthread_mutex_lock (&SRV.list_mutex);
      LL_FOREACH_SAFE (SRV.connection_list, conn, tmp)
        if (conn->sock == rcv_msg_data->sock) {
          if (check_max_received (conn)) {
            LL_DELETE (SRV.connection_list, conn);
            free (conn);
            conn = NULL;
          } else {
            check_for_stop_msg (conn, rcv_msg_data);
          }
          break;
        }
      pthread_mutex_unlock (&SRV.list_mutex);
      if (NULL != conn)
        show_msg (rcv_msg_data, conn);
      free (rcv_msg_data->rcv_msg);
      rcv_msg_data->rcv_msg = NULL;
      server_received_something = true;
      SRV.idle_notify_count = 0;
      break;
    case CMSG_ACTION_ALL_IDLE_NOTIFY:
      if (!server_received_something) {
        printf (SRV.waiting_msg);
        break;
      }
      ++SRV.idle_notify_count;
      if ((SRV.max_idle_count > 0) && (SRV.idle_notify_count >= SRV.max_idle_count)) {
        printf ("Terminating...\n");
        SRV.rcv_process_terminated = true;
      } else {
        printf (SRV.waiting_msg);
      }
      break;
    default:
      printf ("Invalid action code %d\n", action_code);
  }
}

int get_args (const int argc, const char **argv)
{
  int i;
  int mode = 0;

  for (i=1; i<argc; i++)
  {
    const char *arg = argv[i];
    if ((strlen(arg) == 1) && (arg[0] == 'm')) {
	mode = 'm';
	continue;
    }
    if ((strlen(arg) == 1) && (arg[0] == 'i')) {
	mode = 'i';
	continue;
    }
    if ((strlen(arg) == 1) && (arg[0] == 'p')) {
	mode = 'p';
	continue;
    }
   if ((mode == 0) && (strcmp(arg, "ci") == 0)) {
     SRV.close_inactive = true;
     continue;
   }

    if (mode == 'p') {
      SRV.port = parse_num_arg (arg, "port");
      if (SRV.port == (unsigned) -1)
        return -1;
      mode = 0;
      continue;
    }
    if (mode == 'm') {
      SRV.max_rcv_count = parse_num_arg (arg, "max_rcv_count");
      if (SRV.max_rcv_count == (unsigned) -1)
        return -1;
      mode = 0;
      continue;
    }
    if (mode == 'i') {
      SRV.max_idle_count = parse_num_arg (arg, "max_idle_count");
      if (SRV.max_idle_count == (unsigned) -1)
        return -1;
      mode = 0;
      continue;
    }
    printf ("arg not preceded by p/m specifier\n");
    return -1;
  } 
  if (SRV.port == 0) {
    printf ("Expecting a port number argument\n");
    return -1;
  }
  return 0;
}

int main (const int argc, const char **argv)
{
	if (get_args(argc, argv) != 0)
		exit (4);

	if (cmsg_connect_server (IP_ADDR, SRV.port, &SRV.opts) != 0)
		exit(4);
	if (create_thread (&server_send_thread_id, server_send_thread, NULL) == 0)
	{
	    cmsg_server_listen_for_msgs
                (process_rcv_msg, &SRV.rcv_process_terminated);
	    SRV.send_process_terminated = true;
	    pthread_join (server_send_thread_id, NULL);
	}
	pthread_mutex_destroy (&SRV.list_mutex);

  printf ("%d Done!\n", getpid());
}
