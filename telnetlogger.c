/******************************************************************************
 * 
 * TELNETLOGGER
 * 
 * Implementation of a telnet honeypot. This version is based on the
 * implementation of Robert D. Graham and is just slightly modified.
 * 		https://github.com/robertdavidgraham/telnetlogger
 * 
 * Modifications:
 *
 *		* Removed Windows-components to reduce complexity
 *		* Reduced to CSV-like output to STDOUT
 *		* No timestamp in output
 * 
******************************************************************************/

#define _CRT_SECURE_NO_WARNINGS 1
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#define WSAGetLastError() (errno)
#define closesocket(fd) close(fd)
#define WSA(err) (err)
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32")
#endif

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/******************************************************************************
 * A mutex so multiple threads printing output don't conflict with
 * each other
 ******************************************************************************/
pthread_mutex_t output;


/******************************************************************************
 * Arguments pass to each thread. Creating a thread only allows passing in
 * a single pointer, so we have to put everything we want passed to the
 * thread in a structure like this.
 ******************************************************************************/
struct ThreadArgs {
	pthread_t handle;
	int fd;
	struct sockaddr_in6 peer;
	socklen_t peerlen;
	char peername[256];
};


/******************************************************************************
 * Translate sockets error codes to helpful text for printing
 ******************************************************************************/
static const char* error_msg(unsigned err) {
	static char buf[256];
	switch (err) {
	case WSA(ECONNRESET): return "TCP connection reset";
	case WSA(ECONNREFUSED): return "Connection refused";
	case WSA(ETIMEDOUT): return "Timed out";
	case WSA(ECONNABORTED): return "Connection aborted";
	case WSA(EACCES): return "Access denied";
	case WSA(EADDRINUSE): return "Port already in use";
	case 11: return "Timed out";
	case 0: return "TCP connection closed";
	default:   
		snprintf(buf, sizeof(buf), "err#%u", err);
		return buf;
	}
}

/******************************************************************************
 * Print to stderr. Right now, it's just a wrapper aroun fprintf(stderr), but
 * I do it this way so I can later add different DEBUG levels.
 ******************************************************************************/
int ERROR_MSG(const char *fmt, ...) {
	va_list marker;
	va_start(marker, fmt);
	vfprintf(stderr, fmt, marker);
	va_end(marker);
	return -1;
}

/******************************************************************************
 * On modern systems (Win7+, macOS, Linux, etc.), an "anyipv6" socket always
 * is backwards compatible with IPv4. So we create an IPv6 socket to handle
 * both versions simultaneously. This will inevitably fail on some system,
 * so eventually I'll have to write an IPv4 version of this function.
 ******************************************************************************/
int create_ipv6_socket(int port) {
	int fd;
	int err;
	struct sockaddr_in6 localaddr;

	/* Create a generic socket. IPv6 includes IPv4 */
	fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (fd <= 0) {
		ERROR_MSG("socket(AF_INET6): could not create socket: %s\n",
			error_msg(WSAGetLastError()));
		return -1;
	}

	/* Make it a dual stack IPv4/IPv6 socket. This step is unnecessary on
	 * some operating systems/versions, but necessary on some others */
	{
		int no = 0;
		err = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no));
		if (err != 0) {
			ERROR_MSG("setsockopt(!IPV6_V6ONLY): %s\n",
				error_msg(WSAGetLastError()));
			closesocket(fd);
			return -1;
		}
	}

	/* Reuse address */
	{
		int yes = 1;
		err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
		if (err != 0) {
			ERROR_MSG("setsockopt(SO_REUSEADDR): %s\n",
				error_msg(WSAGetLastError()));
			closesocket(fd);
			return -1;
		}
	}

	/* Bind to local port. Again note that while I"m binding for IPv6, it's
	 * also setting up a service for IPv4. */
	memset(&localaddr, 0, sizeof(localaddr));
	localaddr.sin6_family = AF_INET6;
	localaddr.sin6_port = htons(port);
	localaddr.sin6_addr = in6addr_any;
	err = bind(fd, (struct sockaddr*)&localaddr, sizeof(localaddr));
	if (err < 0) {
		ERROR_MSG("bind(%u): %s\n", port,
			error_msg(WSAGetLastError()));
		closesocket(fd);
		return -1;
	}

	/* Now the final initializaiton step */
	err = listen(fd, 10);
	if (err < 0) {
		ERROR_MSG("listen(%u): %s\n", port,
			error_msg(WSAGetLastError()));
		closesocket(fd);
		return -1;
	}

	return fd;
}


/******************************************************************************
 * Blacklist some bad characters to avoid the most obvious attempts of 
 * entering bad passwords designed to hack the system (shell injection,
 * HTML injection, SQL injection).
 ******************************************************************************/
void print_string(FILE *fp, const char *str, int len) {
	int i;
	for (i = 0; i < len; i++) {
		char c = str[i];
		if (!isprint(c & 0xFF) || c == '\\' || c == '<' || c == '\'' || c == ' ' || c == '\"' || c == ',')
			fprintf(fp, "\\x%02x", c & 0xFF);
		else
			fprintf(fp, "%c", c);
	}
}

/******************************************************************************
 * Compares two strings, one nul-terminated, the other length encoded
 ******************************************************************************/
int
matches(const char *rhs, const char *lhs, int len)
{
	if (strlen(rhs) == len && memcmp(rhs, lhs, len) == 0)
		return 1;
	else
		return 0;
}

/******************************************************************************
* Print the results.
******************************************************************************/
void
print_passwords(FILE *fp, const char *login, int login_len, const char *password, int password_len)
{
	if (fp == NULL)
		return;

	if (matches("shell", login, login_len) && matches("sh", password, password_len))
		return;
	if (matches("enable", login, login_len) && matches("system", password, password_len))
		return;

	/* pretty print the two fields */
	pthread_mutex_lock(&output);
	print_string(fp, login, login_len);
	fprintf(fp, " ");
	print_string(fp, password, password_len);
	fprintf(fp, "\n");
	fflush(fp);
	pthread_mutex_unlock(&output);
}

/******************************************************************************
 * Print which machines are connecting
 ******************************************************************************/
void
print_ip(FILE *fp, const char *hostname)
{
	if (fp == NULL)
		return;

	pthread_mutex_lock(&output);
	fprintf(fp, "%s\n", hostname);
	fflush(fp);
	pthread_mutex_unlock(&output);
}


/******************************************************************************
 * Create a CSV formatted line with all the information on one line.
 ******************************************************************************/
void print_csv(FILE *fp, const char *hostname, const char *login, int login_len,
	const char *password, int password_len) {
	if (fp == NULL)
		return;

	pthread_mutex_lock(&output);

	/* time-integer, time-formatted, username, password*/
	fprintf(fp, "%s,", hostname);
	print_string(fp, login, login_len);
	fprintf(fp, ",");
	print_string(fp, password, password_len);
	fprintf(fp, "\n");

	fflush(fp);
	pthread_mutex_unlock(&output);
}


/******************************************************************************
 * Receive a line of NVT text, until the <return> character. While doing this
 * we may also have to participate in some NVT option negotiation.
 * This is the function that reads a username or password.
 ******************************************************************************/
int recv_nvt_line(int fd, char *buf, int sizeof_buf, int flags, int *in_state) {
	int offset = 0;
	int state = *in_state;
	int done = 0;

	while (!done) {
		unsigned char c;
		int len;

		/* Receivine ONE byte at a time and process it. This is rather 
		 * slow, but we don't care about speed */
		len = recv(fd, (char*)&c, 1, flags);
		if (len < 0) {
			return -1;
		}
		if (len <= 0) {
			if (offset == 0) {
				*in_state = state;
				return len;
			} else
				break;
		}

		/* Handle the NVT state-machine */
		switch (state) {
		case 0:
		case 1:
			if (c == 0xFF) {
				state = 2;
			} else if (c == 0) {
			} else if (c == '\n') {
			} else if (c == '\r') {
				done = 1;
				break;
			} else if (c == '\x7f') {
				if (offset) {
					offset--;
					if (state == 0)
						send(fd, "\b \b", 3, flags);
				}
				break;
			} else {
				/************************************************************
				 * This is where we append the byte onto our username/password
				 ************************************************************/
				if (offset + 1 < sizeof_buf)
					buf[offset++] = c;
				else
					done = 1;
				if (state == 0) {
					if (c <= 26) {
						char zz[16];

						zz[0] = '^';
						zz[1] = c + 'A' - 1;
						send(fd, zz, 2, flags);
					} else
						send(fd, &c, 1, flags);
				}
				if (c == 3 /*ctrl-c*/ || c == 4 /*ctrl-d*/)
					done = 1;
			}
			break;
		case 2: /* IAC escape */
			switch (c) {
			case 250: /* subneg */
				state = 20;
				break;
			case 251: /* will */
				state = 3;
				break;
			case 252: /* won't */
				state = 4;
				break;
			case 253: /* do */
				state = 5;
				break;
			case 254: /* don't */
				state = 6;
				break;
			case 255:
				if (offset + 1 < sizeof_buf)
					buf[offset++] = 0xFF;
				state = 0;
				break;
			default:
				state = 0;
				break;
			}
			break;
		case 20: /*sub neg start */
			switch (c) {
			case 0xff: /*iac*/
				state = 21;
				break;
			default:
				/* do nothing */
				break;
			}
			break;
		case 21:
			switch (c) {
			case 240:
				state = 0;
				break;
			default:
				state = 20; /* go back to subnegotiation */
				break;
			}
			break;
		case 3: /* will */
		case 4: /* won't */
		case 5: /* do */
		case 6: /* don't */
			state = 0;
			break;
		default:
			fprintf(stderr, "err,internal error,unknown state\n");
			state = 0;
			break;
		}
	}

	/* save the state across multiple calls */
	*in_state = state;
	return offset;
}

/******************************************************************************
 * This is a thread created whenever a connection is accepted, which is then
 * responsible for handling the connection with blocking calls, and eventually
 * cleanup when the connection ends. We set a recv timeout so that the 
 * connection won't stay alive indefinitely.
 ******************************************************************************/
void *handle_connection(void *v_args) {
	struct ThreadArgs *args = (struct ThreadArgs *)v_args;
	int fd = args->fd;
	char login[256];
	int login_length;
	char password[256];
	int password_length;
	int state = 0;
	char *hello;
	int tries = 0;
	int flags = 0;

#ifdef MSG_NOSIGNAL
	flags |= MSG_NOSIGNAL;
#endif


	/* Set receive timeout of 1 minute. */
	{
		struct timeval tv;
		int err;

		tv.tv_sec = 60;
		tv.tv_usec = 0;

		err = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
		if (err) {
			ERROR_MSG("setsockopt(SO_RECVTIMEO): %s\n",
				error_msg(WSAGetLastError()));
		}
	}


	/* The initial hello, which also includes some basic negotiation.
	 * Apparently, the Mirai won't continue unless the right negotiation
	 * happens. I haven't figured out exactly what that is, but this seems
	 * adequate to make the bot continue */
	hello = "\xff\xfb\x03" /* Will Suppress Go Ahead */
		"\xff\xfb\x01" /* Will Echo */
		"\xff\xfd\x1f" /* Do Negotiate Window Size */
		"\xff\xfd\x18" /* Do Negotiate Terminal Type */
		"\r\nlogin: ";

again:

	/* LOGIN: send the "login: " string, then wait for response */
	send(fd, hello, strlen(hello), flags);
	login_length = recv_nvt_line(fd, login, sizeof(login), flags, &state);
	if (login_length <= 0)
		goto error;

	/* PASSWORD: send the "password: " string, then wait for response */
	send(fd, "\r\nPassword: ", 12, flags);
	if (state == 0)
		state = 1;
	password_length = recv_nvt_line(fd, password, sizeof(password), flags, &state);
	if (password_length <= 0)
		goto error;

	/* Print the peering & login information */
	print_csv(stdout, args->peername, login, login_length, password, password_length);

	/* Print error and loop around to do it again */
	if (state == 1)
		state = 0;
	send(fd, "\r\nLogin incorrect\r\n", 19, flags);
	if (tries++ < 5) {
		sleep(2);
		hello = "\r\nlogin: ";
		goto again;
	}


end:
	closesocket(fd);
	ERROR_MSG("close,%s\n", args->peername);
	free(args);
	return NULL;
error:
	ERROR_MSG("recv,%s,%s\n", args->peername, error_msg(WSAGetLastError()));
	goto end;
}


/******************************************************************************
 ******************************************************************************/
void daemon_thread(int port) {
	int fd;
	
	fd = create_ipv6_socket(port);
	if (fd <= 0)
		return;

	for (;;) {
		int newfd;
		struct ThreadArgs *args;

		/* accept a new connection */
		newfd = accept(fd, 0, 0);
		if (newfd <= 0) {
			ERROR_MSG("accept(%u): %s\n", port,
				error_msg(WSAGetLastError()));
			break;
		}

		/* Create new structure to hold per-thread-data */
		args = malloc(sizeof(*args));
		memset(args, 0, sizeof(*args));
		args->fd = newfd;
		args->peerlen = sizeof(args->peer);
		getpeername(args->fd, (struct sockaddr*)&args->peer, &args->peerlen);
		getnameinfo((struct sockaddr*)&args->peer, args->peerlen, args->peername, sizeof(args->peername), NULL, 0, NI_NUMERICHOST| NI_NUMERICSERV);
		if (memcmp(args->peername, "::ffff:", 7) == 0)
			memmove(args->peername, args->peername + 7, strlen(args->peername + 7) + 1);
		fprintf(stderr, "connect,%s\n", args->peername);

		pthread_create(&args->handle, 0, handle_connection, args);

		/* clean up the thread handle, otherwise we have a small memory
		 * leak of handles. Thanks to Stefan Laudemann for pointing
		 * this out. I suspect it's more than just 8 bytes for the handle,
		 * but that there are kernel resources that we'll run out of
		 * too. */
		pthread_detach(args->handle);
	}

	closesocket(fd);
}

/******************************************************************************
 ******************************************************************************/
int main(int argc, char *argv[]) {
	int i;
	int port = 23;

	pthread_mutex_init(&output, 0);

	/* Read configuration parameters */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			fprintf(stderr, "startup,unknown parameter: %s\n", argv[i]);
			exit(1);
		}
		switch (argv[i][1]) {
		case 'l':
		{
			char *arg;
			if (isdigit(argv[i][2])) {
				arg = &argv[i][2];
			} else {
				if (++i >= argc) {
					fprintf(stderr, "startup,expected parameter after -%c\n", 'i');
					exit(1);
				}
				arg = argv[i];
			}
			if (strtoul(arg, 0, 0) < 1 || strtoul(arg, 0, 0) > 65535) {
				fprintf(stderr, "startup,expected port number between 1..65535\n");
				exit(1);
			}
			port = strtoul(arg, 0, 0);
		}
			break;
		case 'h':
		case '?':
		case 'H':
			fprintf(stderr, "usage:\n telnetlogger [-l port]\n");
			exit(1);
			break;
		}
	}

	daemon_thread(port);

	return 0;
}

