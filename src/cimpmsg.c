#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "utlist.h"
#include "cimpmsg.h"
#include "cimpmsg_log.h"

/*------------------------------------------------------------------
 * client receive should be blocking, but have a timeout so we can
*  detect terminate flag 
*  client send should be blocking
* 
*  server receive should be blocking
*  server send should be non-bocking so we can do send all
---------------------------------------------------------------------*/

#define MSG_HEADER_MARK 0xEE


typedef struct conn_user_data {
  bool close_request;
} conn_user_data_t;

typedef struct connection {
  int oserr;
  int rcv_state;
  bool rcv_selected;
  struct conn_user_data *user_data;
  size_t rcv_end_pos;
  server_rcv_msg_data_t rcv_data;
  struct connection * next;
} connection_t;


static struct server_stuff {
  unsigned int port;
  struct sockaddr_in addr;
  int listen_sock;
  bool terminate_on_keypress;
  int listen_state;  // 0=idle, 1=listening, 2=shutting-down
  unsigned idle_notify_secs;
  pthread_mutex_t connect_mutex;
  pthread_mutex_t list_mutex;
  struct connection * connection_list;
} SRV
 = { .port = (unsigned int) -1, .listen_sock = -1,
     .terminate_on_keypress = true,
     .listen_state = 0,
     .idle_notify_secs = 2,
     .connect_mutex = PTHREAD_MUTEX_INITIALIZER,
     .list_mutex = PTHREAD_MUTEX_INITIALIZER,
     .connection_list = NULL
   };



void init_connection (struct connection *conn)
{
  conn->rcv_data.sock = -1;
  conn->oserr = 0;
  conn->rcv_state = -1;
  conn->rcv_selected = false;
  conn->rcv_data.rcv_msg_size = 0;
  conn->user_data = NULL;
  conn->rcv_end_pos = 0;
  conn->rcv_data.rcv_msg = NULL;
  conn->next = NULL;
}

void init_client_conn (struct client_conn *conn)
{
  conn->sock = -1;
  conn->oserr = 0;
  conn->rcv_msg = NULL;
  conn->rcv_msg_size = 0;
  conn->rcv_count = 0;
  conn->terminated = false;
  pthread_mutex_init (&conn->send_mutex, NULL);
  pthread_mutex_init (&conn->rcv_mutex, NULL);
}

int wait_server_ready (process_message_t handle_msg, bool *terminated, bool *any_closing)
{
  struct timeval timeout;
  struct connection *conn;
  int rtn, sock, highest_sock;
  unsigned timeout_count = 0;
  unsigned max_count;
  fd_set fds;
  server_rcv_msg_data_t notify_data = {
    .sock = -1, .rcv_msg = NULL, .rcv_msg_size = 0
  };

  max_count = SRV.idle_notify_secs * 2; 
  highest_sock = -1;

  while (1)
  {
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    FD_ZERO (&fds);
    if (SRV.listen_sock != -1) {
      FD_SET (SRV.listen_sock, &fds);
      highest_sock = SRV.listen_sock;
      // printf ("Waiting on listener %d\n", listen_sock);
    }
    LL_FOREACH (SRV.connection_list, conn) {
      conn->rcv_selected = false;
      if (conn->rcv_state >= 0) {
        sock = conn->rcv_data.sock;
        if (conn->user_data->close_request) {
          conn->rcv_state = -2;
          *any_closing = true;
          cmsg_log (LEVEL_DEBUG, ("CIMPMSG: Got close request for socket %d\n", sock));
          continue;
        } 
        // printf ("Waiting on %d\n", sock);
        if (sock > highest_sock)
          highest_sock = sock;
        FD_SET (sock, &fds);
      }
    }
    if (SRV.terminate_on_keypress) {
      FD_SET (STDIN_FILENO, &fds);
    }
    rtn = select (highest_sock+1, &fds, NULL, NULL, &timeout);
    if (rtn < 0) {
      cmsg_log (LEVEL_ERROR, ("CIMPMSG: Error on select for receive\n"));
      return -1;
    }
    if (rtn != 0)
      break;
    if (*any_closing)
      break;
    if (max_count != 0) {
      ++timeout_count;
      if (timeout_count >= max_count) {
        handle_msg (CMSG_ACTION_IDLE_NOTIFY, &notify_data);
        timeout_count = 0;
      }
    }
    if (NULL != terminated)
      if (*terminated)
        break;
  }
  rtn = 0;
  if (SRV.listen_sock != -1)
    if (FD_ISSET (SRV.listen_sock, &fds))
      rtn = 1;
  LL_FOREACH (SRV.connection_list, conn) {
    if (conn->rcv_state >= 0)
      if (FD_ISSET (conn->rcv_data.sock, &fds)) {
        conn->rcv_selected = true;
        rtn |= 2;
      }
  }
  if (SRV.terminate_on_keypress) {
    if (FD_ISSET (STDIN_FILENO, &fds))
      rtn |= 4;
  }
  return rtn;
}

int make_sockaddr (struct sockaddr_in *addr, 
  const char *ip_addr, unsigned int port, bool rcv_any)
{
  int rtn;
  if (port == (unsigned) -1)
    return -1;
  
  addr->sin_family = AF_INET;
  addr->sin_port = htons (port);
  if (rcv_any)
    addr->sin_addr.s_addr = INADDR_ANY;
  else {
    rtn = inet_pton (AF_INET, ip_addr, &addr->sin_addr);
    if (rtn != 1) {
      cmsg_log (LEVEL_ERROR, ("CIMPMSG: inet_pton error\n"));
      return -1;
    }
  }
  return 0;
}

int cmsg_connect_server (const char *ip_addr, unsigned int port,
  server_opts_t *options)
{
	int sock, rtn;

	pthread_mutex_lock (&SRV.connect_mutex);
	if (SRV.listen_sock != -1) {
	  cmsg_log (LEVEL_ERROR, ("CIMPMSG: server already connected\n"));
	  pthread_mutex_unlock (&SRV.connect_mutex);
	  return EALREADY;
	}
	if (NULL != options) {
		SRV.terminate_on_keypress = options->terminate_on_keypress;
		SRV.idle_notify_secs = options->idle_notify_interval_secs;
	}

	if ((NULL == ip_addr) || ((unsigned int) -1 == port)) {
		SRV.listen_sock = -1;
		cmsg_log (LEVEL_ERROR, 
		  ("CIMPMSG: Invalid ip addr or port for cmsg_server_connect\n"));
		pthread_mutex_unlock (&SRV.connect_mutex);
		return EINVAL;
	}

	if (make_sockaddr (&SRV.addr, ip_addr, port, false) != 0) {
	  pthread_mutex_unlock (&SRV.connect_mutex);
          return EINVAL;
	}
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
	  cmsg_log_err (LEVEL_ERROR, errno, 
		("CIMPMSG: Unable to create rcv socket"));
	  pthread_mutex_unlock (&SRV.connect_mutex);
	  return errno;
	}
#if 0
	int flags = fcntl (sock, F_GETFL);
	if (flags == -1) {
		dbg_err (errno, "Unable to get socket flags: \n");
		close (sock);
 		return -1;
	}
	flags |= O_NONBLOCK;
	if (fcntl (sock, F_SETFL, flags) == -1) {
		dbg_err (errno, "Unable to set socket flags: \n");
		close (sock);
 		return -1;
	}
#endif
	if (bind (sock, (struct sockaddr *) &SRV.addr, 
          sizeof (struct sockaddr_in)) < 0) {
		cmsg_log_err (LEVEL_ERROR, errno, 
		  ("CIMPMSG: Unable to bind to receive socket"));
		rtn = errno;
		close (sock);
		pthread_mutex_unlock (&SRV.connect_mutex);
		return rtn;
	}
	if (listen (sock, 50) == -1) {
	  cmsg_log_err (LEVEL_ERROR, errno, 
		("CIMPMSG: Listen error on receive socket:"));
	  rtn = errno;
	  close (sock);
	  pthread_mutex_unlock (&SRV.connect_mutex);
	  return rtn;
	}
	SRV.listen_sock = sock;
	pthread_mutex_unlock (&SRV.connect_mutex);
	return 0;
}

struct connection *init_server_connection (int sock)
{
  struct connection *conn;

  conn = (struct connection *) malloc (sizeof (struct connection));
  if (NULL == conn) {
    cmsg_log (LEVEL_ERROR, 
	("CIMPMSG: Unable to malloc connection structure in receiver accept\n"));
    return NULL;
  }
  init_connection (conn);
  conn->rcv_state = 0;
  conn->rcv_data.sock = sock;
  conn->user_data = (struct conn_user_data *) malloc (sizeof (struct conn_user_data));
  if (NULL == conn) {
    cmsg_log (LEVEL_ERROR, 
	("CIMPMSG: Unable to malloc connection user data in receiver accept\n"));
    return NULL;
  }
  conn->user_data->close_request = false;
  return conn;
}

int server_accept (process_message_t handle_msg)
{
  int sock;
  struct connection *conn;

  sock = accept (SRV.listen_sock, NULL, NULL);
  if (sock < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return 1;
    cmsg_log_err (LEVEL_ERROR, errno, ("CIMPMSG: Accept error on receive socket:"));
    close (SRV.listen_sock);
    return 2;
  }
  cmsg_log (LEVEL_INFO, ("Accepted %d\n", sock));
#if 0
  int flags = fcntl (sock, F_GETFL);
  if (flags == -1) {
	dbg_err (errno, "Unable to get socket flags: \n");
	close (sock);
	return -1;
  }
  flags |= O_NONBLOCK;
  if (fcntl (sock, F_SETFL, flags) == -1) {
	dbg_err (errno, "Unable to set socket flags: \n");
	close (sock);
	return -1;
  }
#endif
  conn = init_server_connection (sock);
  if (NULL == conn) {
    return -1;
  }
//CONN_INACTIVE  
  pthread_mutex_lock (&SRV.list_mutex);
  LL_APPEND (SRV.connection_list, conn);
  handle_msg (CMSG_ACTION_CONN_ADDED, &conn->rcv_data);
  pthread_mutex_unlock (&SRV.list_mutex);
  return 0;

}

void shutdown_sock (int sock)
{
    struct linger linger_opt = {1, 0};

    if (shutdown (sock, SHUT_RDWR) != 0)
      cmsg_log_err (LEVEL_ERROR, errno, 
       ("CIMPMSG: Error shutting down socket %d", sock));
    if (close (sock) == 0)
      return;
    cmsg_log_err (LEVEL_ERROR, errno, ("CIMPMSG: Error closing socket %d", sock));
    if (setsockopt (sock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof linger_opt) < 0) {
      cmsg_log_err (LEVEL_ERROR, errno, 
        ("CIMPMSG: Error setting linger sockopt for socket %d", sock));
      return;
    }      
    if (close (sock) != 0)
      cmsg_log_err (LEVEL_ERROR, errno, ("CIMPMSG: Error closing socket %d", sock));
}

void shutdown_connection (struct connection *conn)
{
  if (conn->rcv_state != -1) {
    shutdown_sock (conn->rcv_data.sock);
    conn->rcv_data.sock = -1;
    conn->rcv_state = -1;
//CONN_INACTIVE  
    if (NULL != conn->user_data)
      free (conn->user_data);
  }
}
 
void shutdown_server (void)
{
  struct connection *conn;
  struct connection *tmp;

  if (SRV.listen_sock != -1) {
    LL_FOREACH_SAFE (SRV.connection_list, conn, tmp) {
      LL_DELETE (SRV.connection_list, conn);
      shutdown_connection (conn);
      free (conn);
    }
    shutdown_sock (SRV.listen_sock);
  }
}

int cmsg_connect_client (struct client_conn *conn, 
  const char *ip_addr, unsigned int port, unsigned int send_timeout_msecs)
{
	int sock;
	struct timeval send_timeout;
	struct timeval rcv_timeout;

	init_client_conn (conn);

	if ((unsigned int) -1 == port) {
		conn->sock = -1;
		return EINVAL;
	}
	if (make_sockaddr (&conn->addr, ip_addr, port, false) != 0)
          return EINVAL;
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
	  conn->oserr = errno;
	  cmsg_log_err (LEVEL_ERROR, errno, ("CIMPMSG: Unable to create send socket"));
 	  return conn->oserr;
	}
	if (send_timeout_msecs != (unsigned int) -1) {
		send_timeout.tv_sec = send_timeout_msecs / 1000;
		send_timeout.tv_usec = (send_timeout_msecs % 1000) * 1000;
		if (setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, 
		  &send_timeout, sizeof (send_timeout)) < 0) {
			conn->oserr = errno;
			cmsg_log_err (LEVEL_ERROR, errno, 
			  ("CIMPMSG: Unable to set socket send timeout:"));
			close (sock);
	 		return conn->oserr;
		}
	}
	rcv_timeout.tv_sec = 0;
	rcv_timeout.tv_usec = 500000;
	if (setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, 
		  &rcv_timeout, sizeof (rcv_timeout)) < 0) {
			conn->oserr = errno;
			cmsg_log_err (LEVEL_ERROR, errno, 
			  ("CIMPMSG: Unable to set socket rcv timeout:"));
			close (sock);
	 		return conn->oserr;
		}
	if (connect (sock, (struct sockaddr *) &conn->addr, sizeof (conn->addr)) < 0) {
		conn->oserr = errno;
		cmsg_log_err (LEVEL_ERROR, errno, 
		  ("CIMPMSG: Unable to connect to client socket:"));
		shutdown_sock (sock);
		return conn->oserr;
	}
	conn->sock = sock;
	return 0;
}

void cmsg_shutdown_client (struct client_conn *conn)
{
  if (conn->sock != -1) {
	shutdown_sock (conn->sock);
	pthread_mutex_destroy (&conn->send_mutex);
	pthread_mutex_destroy (&conn->rcv_mutex);
	conn->sock = -1;
  }
}


ssize_t socket_receive (struct connection *conn, void *buf, size_t len, bool *terminated)
{
  ssize_t bytes;

  while (true) {
    bytes = recv (conn->rcv_data.sock, buf, len, 0);
    if (bytes >= 0)
      return bytes;
    if (NULL != terminated) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
        if (*terminated)
          return -2;
        continue; 
      }
    }
    conn->oserr = errno;
    return -1;
  }
}

int receive_msg_header (struct connection *conn, bool *terminated)
{
  int sock = conn->rcv_data.sock;
  ssize_t bytes;
  size_t msg_size;
  unsigned char header[4];

  bytes = socket_receive (conn, header, 4, terminated);
  if (bytes < 0) { 
    if (bytes == -2)
      return CMSG_ERR_RCV_TERMINATED;
    cmsg_log_err (LEVEL_ERROR, conn->oserr, 
	("CIMPMSG: Error receiving msg header for socket %d", sock));
    return CMSG_ERR_RCV_OS_ERROR;
  }
  if (bytes == 0) {
    cmsg_log (LEVEL_DEBUG, ("CIMPMSG: Receive message. Socket %d closed by sender\n", sock));
    return CMSG_ERR_RCV_SOCKET_CLOSED;
  }
  if (bytes != 4) {
    cmsg_log (LEVEL_ERROR, 
	  ("CIMPMSG: Expecting 4 byte msg header. Got %ld bytes\n", bytes));
    return CMSG_ERR_RCV_BAD_HDR_BYTE_CT;
  }
  if ((header[0] != MSG_HEADER_MARK) || (header[1] != MSG_HEADER_MARK)) {
	cmsg_log (LEVEL_ERROR, ("CIMPMSG: Invalid msg header mark\n"));
	return CMSG_ERR_RCV_BAD_HDR_MARK;
  }
  msg_size = ((size_t) header[2] << 8) + (size_t) header[3]; 
  conn->rcv_data.rcv_msg = malloc (msg_size);
  if (NULL == conn->rcv_data.rcv_msg) {
    cmsg_log (LEVEL_ERROR, 
      ("CIMPMSG: Unable to malloc msg buffer for socket %d\n", sock));
    return CMSG_ERR_RCV_MSG_MALLOC_FAIL;
  }
  conn->rcv_data.rcv_msg_size = msg_size;
  conn->rcv_end_pos = 0;
  conn->rcv_state = 1;
  return 0;
}


// returned msg must be freed
int receive_msg_data (struct connection *conn, process_message_t handle_msg,
  bool *terminated)
{
  ssize_t bytes;
  size_t read_len = conn->rcv_data.rcv_msg_size - conn->rcv_end_pos;
  int sock = conn->rcv_data.sock;
  char *buf = conn->rcv_data.rcv_msg;

  bytes = socket_receive (conn, buf+conn->rcv_end_pos, read_len, terminated);

  if (bytes < 0) { 
    if (bytes == -2)
      return CMSG_ERR_RCV_TERMINATED;
    cmsg_log_err (LEVEL_ERROR, conn->oserr, 
	("CIMPMSG: Error receiving msg data for socket %d", sock));
    return CMSG_ERR_RCV_OS_ERROR;
  }

  if ((size_t) bytes > read_len) {
    cmsg_log (LEVEL_ERROR, ("CIMPMSG: bytes received %ld not eq read_len in header %lu\n",
	bytes, read_len));
    return CMSG_ERR_RCV_BAD_DATA_BYTE_CT;
  }
  conn->rcv_end_pos += bytes;
  if ((size_t) bytes < read_len) {
    cmsg_log (LEVEL_DEBUG, 
      ("CIMPMSG: Not all bytes received, only %ld of %lu. Waiting for remainder\n",
        bytes, read_len));
    return 0;
  }
  if (NULL != handle_msg)
    handle_msg (CMSG_ACTION_MSG_RECEIVED, &conn->rcv_data);
  conn->rcv_state = 0;
  return 1;
}

int cmsg_client_receive (struct client_conn *cconn)
{
  int rtn;
  struct connection rconn;

  pthread_mutex_lock (&cconn->rcv_mutex);
  init_connection (&rconn);
  rconn.rcv_data.sock = cconn->sock;
  rconn.rcv_state = 0;

  rtn = receive_msg_header (&rconn, &cconn->terminated);
  if (rtn < 0) {
    pthread_mutex_unlock (&cconn->rcv_mutex);
    return rtn;
  }
  while (true) {
    rtn = receive_msg_data (&rconn, NULL, &cconn->terminated);
    if (rtn == 1) {
      cconn->rcv_msg = rconn.rcv_data.rcv_msg;
      cconn->rcv_msg_size = rconn.rcv_data.rcv_msg_size; 
      cconn->rcv_count++;
      rtn = (ssize_t) rconn.rcv_data.rcv_msg_size;
      pthread_mutex_unlock (&cconn->rcv_mutex);
      return rtn;
    }
    if (rtn < 0)
      break;
  }
  pthread_mutex_unlock (&cconn->rcv_mutex);
  return rtn;
}

void server_receive_msgs (process_message_t handle_msg, bool *any_closing)
{
  int rtn;
  struct connection *conn;
  
  LL_FOREACH (SRV.connection_list, conn)
    if (conn->rcv_selected) {
      if (conn->rcv_state == 0)
        rtn = receive_msg_header (conn, NULL);
      else if (conn->rcv_state == 1)
        rtn = receive_msg_data (conn, handle_msg, NULL);
      else
        continue;
      if (rtn < 0) {
        conn->rcv_state = -2;
        *any_closing = true;
        handle_msg (CMSG_ACTION_CONN_DROPPED, &conn->rcv_data);
      }
    }
}

void server_close_connections (void)
{
  struct connection *conn;
  struct connection *tmp;

  cmsg_log (LEVEL_DEBUG, ("CIMPMSG: server_close_connections\n"));
  pthread_mutex_lock (&SRV.list_mutex);
  LL_FOREACH_SAFE (SRV.connection_list, conn, tmp)
    if (conn->rcv_state == -2) {
        LL_DELETE (SRV.connection_list, conn);
        cmsg_log (LEVEL_INFO, 
	  ("CIMPMSG: Closing connection for socket %d\n", conn->rcv_data.sock));
        shutdown_connection (conn);
        free (conn);
    }
  pthread_mutex_unlock (&SRV.list_mutex);
}


int cmsg_server_listen_for_msgs (process_message_t handle_msg, bool *terminated)
{
  int rtn;
  bool any_closing;
  char inbuf[10];

  pthread_mutex_lock (&SRV.connect_mutex);
  if (SRV.listen_sock == -1) {
    cmsg_log (LEVEL_ERROR, ("CIMPMSG: cmsg_server_listen_for_msgs: not connected\n"));
    pthread_mutex_unlock (&SRV.connect_mutex);
    return ENOTCONN;
  }
  if (SRV.listen_state != 0) {
    if (SRV.listen_state == 1)
      cmsg_log (LEVEL_ERROR, ("CIMPMSG: server already listening for messages\n"));
    else
      cmsg_log (LEVEL_ERROR, ("CIMPMSG: server shutting down\n"));
    pthread_mutex_unlock (&SRV.connect_mutex);
    return EALREADY;
  }
  SRV.listen_state = 1;
  pthread_mutex_unlock (&SRV.connect_mutex);

  while (1)
  {
    any_closing = false;
    rtn = wait_server_ready (handle_msg, terminated, &any_closing);
    if (rtn < 0)
      break;
    if (rtn & 1)
      server_accept (handle_msg);
    if (rtn & 2)
      server_receive_msgs (handle_msg, &any_closing);
    if (any_closing)
      server_close_connections ();
    if (SRV.terminate_on_keypress) {
      if (rtn & 4) { // key pressed
	fgets (inbuf, 10, stdin);
	break;
      }
    }
    if (NULL != terminated)
      if (*terminated)
	break;
  }
  cmsg_log (LEVEL_INFO, ("CIMPMSG: Exiting cmsg_server_listen_for_msgs\n"));

  pthread_mutex_lock (&SRV.connect_mutex);
  SRV.listen_state = 2;
  pthread_mutex_unlock (&SRV.connect_mutex);

  shutdown_server ();
  return 0;
}

int __send_msg (int sock, const char *msg, size_t sz_msg, bool non_block)
{
  int flags = 0;
  ssize_t bytes;
  char *msg_buf;

  msg_buf = malloc (sz_msg+4);
  if (NULL == msg_buf) {
    cmsg_log (LEVEL_ERROR, 
	("CIMPMSG: Unable to malloc msg buffer for socket %d\n", sock));
    return ENOMEM;
  }
  msg_buf[0] = MSG_HEADER_MARK;
  msg_buf[1] = MSG_HEADER_MARK;
  msg_buf[2] = sz_msg / 256;
  msg_buf[3] = sz_msg % 256;
  memcpy (msg_buf+4, msg, sz_msg);

#if 0
  if (wait_send_ready () < 0)
     return -1;
#endif
  sz_msg += 4;
  if (non_block)
    flags = MSG_DONTWAIT;
  bytes = send (sock, msg_buf, sz_msg, flags);
  free (msg_buf);
  if (bytes < 0) { 
	cmsg_log_err (LEVEL_ERROR, errno, ("CIMPMSG: Error sending msg:"));
	return errno;
  }
  if ((size_t) bytes != sz_msg) {
    cmsg_log (LEVEL_ERROR, ("CIMPMSG: Not all bytes sent, just %ld\n", bytes));
    return EIO;
  }
  return 0;
}

int cmsg_client_send (struct client_conn *conn, const char *msg, size_t sz_msg, bool non_block)
{
  int rtn;

  if (-1 == conn->sock) {
    cmsg_log (LEVEL_ERROR, ("CIMPMSG: Invalid socket for cmsg_client_send\n"));
    return EBADF;
  }
  pthread_mutex_lock (&conn->send_mutex);
  rtn = __send_msg (conn->sock, msg, sz_msg, non_block);
  pthread_mutex_unlock (&conn->send_mutex);
  return rtn;
}

int cmsg_server_send (int sock, const char *msg, size_t sz_msg, bool non_block)
{
  int rtn = EBADF;
  struct connection *conn;

  pthread_mutex_lock (&SRV.list_mutex);
  if (SRV.listen_state != 1) {
    if (SRV.listen_state == 0)
      cmsg_log (LEVEL_DEBUG, ("CIMPMSG: cannot send, server not started\n"));
    else
      cmsg_log (LEVEL_DEBUG, ("CIMPMSG: cannot send, server shutting down\n"));
    pthread_mutex_unlock (&SRV.list_mutex);
    return rtn;
  }
  LL_FOREACH (SRV.connection_list, conn)
  {
    if (conn->rcv_state >= 0) {
      if (conn->rcv_data.sock == sock) {
        rtn = __send_msg (sock, msg, sz_msg, non_block);
        break;
      }
    }
  }
  pthread_mutex_unlock (&SRV.list_mutex);
  return rtn;
}

int cmsg_server_close_sock (int sock)
{
  int rtn = EBADF;
  struct connection *conn;

  pthread_mutex_lock (&SRV.list_mutex);
  if (SRV.listen_state != 1) {
    if (SRV.listen_state == 0)
      cmsg_log (LEVEL_DEBUG, ("CIMPMSG: cannot close socket, server not started\n"));
    else
      cmsg_log (LEVEL_DEBUG, ("CIMPMSG: cannot close socket, server shutting down\n"));
    pthread_mutex_unlock (&SRV.list_mutex);
    return rtn;
  }
  LL_FOREACH (SRV.connection_list, conn)
  {
    if (conn->rcv_state >= 0) {
      if (conn->rcv_data.sock == sock) {
        conn->user_data->close_request = true;
	rtn = 0;
        break;
      }
    }
  }
  pthread_mutex_unlock (&SRV.list_mutex);
  if (rtn != 0)
     cmsg_log (LEVEL_DEBUG, ("CIMPMSG: Requested close socket (%d) not found\n", sock));
  return rtn;
}

