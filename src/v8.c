/* Copyright (C) 2013, Renato Fernandes de Queiroz <renatofq@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 3 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <v8/v8.h>
#include <v8/log.h>
#include <v8/dispatcher.h>
#include <v8/scgi.h>
#include <v8/list.h>
#include <v8/job.h>

#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/wait.h>

struct v8_t
{
	int sockfd;
	int sigfd;
	int base_size;
	V8Config * config;
	V8Dispatcher * dispatcher;
	const V8Action * actions;
	void * app_data;
	V8AppInitializer * app_init;
};


static int v8_init_socket(V8 * v8);
static int v8_init_signals(void);
static int v8_set_nonblocking(int fd);

static int v8_fork(V8 * v8, int sock);
static void v8_child_exec(V8 * v8, int sock);

static int v8_request_handler(V8 * v8, int sock);

static void v8_connection_listener_init(V8 * v8, V8Listener * listener);
static void v8_handle_connection(int fd, void * data);
static void v8_handle_socket_error(int fd, void * data);

static void v8_signal_listener_init(V8 * v8, V8Listener * listener);
static void v8_handle_signal(int fd, void * data);
static void v8_handle_signal_error(int fd, void * data);

static const V8 * g_v8 = NULL;


V8 * v8_init(V8Config * config, const V8Action * actions,
             V8AppInitializer * appinit)
{
	V8 * v8 = malloc(sizeof(V8));

	if (v8 != NULL)
	{
		/* FIXME: Handler errors */
		v8->actions = actions;
		v8->config = config;
		v8->sockfd = v8_init_socket(v8);
		v8->sigfd = v8_init_signals();

		v8_log_level_str_set(v8_config_str(v8->config, "v8.log.level",
		                                   "warning"));
		v8->base_size = strlen(v8_config_str(v8->config, "v8.baseurl", ""));
		v8->dispatcher = v8_dispatcher_create();
		v8->app_data = NULL;
		v8->app_init = appinit;

		g_v8 = v8;
	}

	return v8;
}


int v8_start(V8 * v8)
{
	pid_t pid;
	int status;
	V8Listener conn_listener;
	V8Listener sig_listener;

	v8_connection_listener_init(v8, &conn_listener);
	v8_dispatcher_add_listener(v8->dispatcher, v8->sockfd, &conn_listener);

	v8_signal_listener_init(v8, &sig_listener);
	v8_dispatcher_add_listener(v8->dispatcher, v8->sigfd, &sig_listener);

	v8_log_debug("Waiting for connections");

	v8_dispatcher_start(v8->dispatcher);

	v8_log_info("Wainting childs to terminate");
	while ((pid = wait(&status) > 0));

	if (pid < 0 && errno != ECHILD)
	{
		v8_log_error("Failed when waiting for childs: %d", errno);
	}

	v8_log_info("V8 shutting down");

	return 0;
}


void * v8_malloc(size_t size)
{
	return malloc(size);
}

void * v8_app_data(void)
{
	return g_v8->app_data;
}

const char * v8_global_config_str(const char * name, const char * def)
{
	return v8_config_str(g_v8->config, name, def);
}

int v8_global_config_int(const char * name, int def)
{
	return v8_config_int(g_v8->config, name, def);
}


static int v8_init_socket(V8 * v8)
{
	int sock = -1;
	const char * node = NULL;
	const char * port = NULL;
	struct addrinfo hints;
	struct addrinfo * res = NULL;
	int reuseaddr = 1;
	int ret = 0;

	node = v8_config_str(v8->config, "v8.listen", "127.0.0.1");
	port = v8_config_str(v8->config, "v8.port", "4900");

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	ret = getaddrinfo(node, port, &hints, &res);
	if (ret != 0)
	{
		goto cleanup;
	}

	/* Create the socket */
	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock == -1)
	{
		goto cleanup;
	}

	/* Enable the socket to reuse the address */
	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
	                 sizeof(int));
	if (ret == -1)
	{
		goto cleanup;
	}

	ret = v8_set_nonblocking(sock);
	if (ret == -1)
	{
		goto cleanup;
	}

	/* Bind to the address */
	ret = bind(sock, res->ai_addr, res->ai_addrlen);
	if (ret == -1)
	{
		goto cleanup;
	}

	freeaddrinfo(res);

	ret = listen(sock, SOMAXCONN);
	if (ret == -1)
	{
		v8_log_error("Failed to listen socket");
		goto cleanup;
	}

	return sock;

 cleanup:
	if (res != NULL)
	{
		freeaddrinfo(res);
		res = NULL;
	}

	close(sock);

	return -1;
}

static int v8_init_signals(void)
{
	sigset_t mask;
	int sigfd;
	int ret = 0;

	sigemptyset(&mask);

	/* Termination signals */
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGTERM);

	/* Child signalization */
	sigaddset(&mask, SIGCHLD);

	/* Reload configuration */
	sigaddset(&mask, SIGHUP);

	ret = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (ret == -1)
	{
		v8_log_error("Unable to block signals: %d", errno);
		return -1;
	}

	sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
	if (sigfd == -1)
	{
		v8_log_error("Unable to create signalfd: %d", errno);
		return -1;
	}

	return sigfd;
}

static int v8_set_nonblocking(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);

	if(flags == -1)
	{
		v8_log_error("Failed to get fd flags");
		return -1;
	}

	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if(flags == -1)
	{
		v8_log_error("Failed to set nonblock on fd");
		return -1;
	}

	return 0;
}

static int v8_fork(V8 * v8, int sock)
{
	pid_t pid;
	int ret = 0;

	pid = fork();
	if (pid < 0)
	{
		v8_log_error("Failed to create worker process %d", errno);
		ret = -1;
	}
	else if (pid == 0)
	{
		/* Never returns */
		v8_child_exec(v8, sock);
	}

	/* This socket will be handled by child */
	close(sock);

	return ret;

}

static void v8_child_exec(V8 * v8, int sock)
{
	sigset_t mask;
	int ret = 0;

	/* Closing parent's fds */
	close(v8->sigfd);
	v8->sigfd = -1;
	close(v8->sockfd);
	v8->sockfd = -1;

	v8_dispatcher_destroy(v8->dispatcher);
	v8->dispatcher = NULL;

	sigemptyset(&mask);

	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGHUP);

	ret = sigprocmask(SIG_UNBLOCK, &mask, NULL);
	if (ret == -1)
	{
		v8_log_error("Unable to unblock signals: %d", errno);
		exit(-1);
	}


	exit(v8_request_handler(v8, sock));
}

static int v8_request_handler(V8 * v8, int sock)
{
	const V8Action * actions = v8->actions;
	V8Request * request = v8_request_create();
	V8Response * response = v8_response_create(request, sock);
	V8RequestMethod method = V8_METHOD_UNKNOWN;
	V8Handler * filter_handler = NULL;
	const char * route;
	int i = 0;
	int ret = 0;

	ret = v8_scgi_request_read(sock, request);
	if (ret < 0)
	{
		v8_log_warn("Could not read request");
		goto error_cleanup;
	}
	method = v8_request_method(request);
	route = v8_request_route(request);

	if (v8->app_init != NULL)
	{
		v8->app_data = v8->app_init();
	}

	v8_log_debug("Request receiveid -> Method: %d Path: %s", method, route + v8->base_size);

	for (i = 0; actions[i].method != V8_METHOD_UNKNOWN; ++i)
	{
		if (method == actions[i].method
		    && actions[i].route != NULL
		    && strcmp(route + v8->base_size, actions[i].route) == 0)
		{
			if (actions[i].filter != NULL)
			{
				filter_handler = actions[i].filter(request);
				if (filter_handler != NULL)
				{
					filter_handler(request, response);
					break;
				}
			}

			actions[i].handler(request, response);
				break;
		}
	}

	if (actions[i].method == V8_METHOD_UNKNOWN)
	{
		v8_log_warn("Action not found %s", route);
		v8_response_set_status(response, V8_STATUS_NOT_FOUND);
	}

	v8_response_send(response);
	v8_response_destroy(response);
	v8_request_destroy(request);

	close(sock);

	return 0;

 error_cleanup:
	close(sock);

	return -1;
}

static void v8_connection_listener_init(V8 * v8, V8Listener * listener)
{
	listener->data = v8;

	listener->input_handler = v8_handle_connection;
	listener->output_handler = NULL;
	listener->closed_handler = NULL;
	listener->error_handler = v8_handle_socket_error;
	listener->hangup_handler = NULL;

	listener->destructor = NULL;
}

static void v8_signal_listener_init(V8 * v8, V8Listener * listener)
{
	listener->data = v8;

	listener->input_handler = v8_handle_signal;
	listener->output_handler = NULL;
	listener->closed_handler = NULL;
	listener->error_handler = v8_handle_signal_error;
	listener->hangup_handler = NULL;

	listener->destructor = NULL;
}

static void v8_handle_connection(int fd, void * data)
{
	int newsock = 0;
	V8 * v8 = data;

	while (newsock != -1)
	{
		newsock = accept(v8->sockfd, NULL, NULL);

		if (newsock != -1)
		{
			v8_log_debug("Connection accepted");

			if (v8_fork(v8, newsock) != 0)
			{
				v8_log_error("Fail to fork process");
			}
		}
		else if (errno != EAGAIN)
		{
			v8_log_error("Fail to accept connection");
		}
	}

}

static void v8_handle_socket_error(int fd, void * data)
{
	/* TODO: handle error */
	v8_log_error("Error on connection listener %d", errno);
}

static void v8_handle_signal(int fd, void * data)
{
	V8 * v8 = data;
	ssize_t sz;
	int status;
	pid_t pid;
	struct signalfd_siginfo siginfo;

	while (1)
	{
		sz = read(v8->sigfd, &siginfo, sizeof(struct signalfd_siginfo));
		if (sz != sizeof(struct signalfd_siginfo))
		{
			if (errno != EAGAIN)
			{
				v8_log_error("Unable to get signal info: %d", errno);
			}

			break;
		}

		switch (siginfo.ssi_signo)
		{
		case SIGINT:
		case SIGQUIT:
		case SIGTERM:
			v8_log_info("Termination signal received");
			v8_dispatcher_stop(v8->dispatcher);
			break;
		case SIGCHLD:
			v8_log_debug("Child signaled");
			while ((pid = waitpid(-1, &status, WNOHANG) > 0));

			if (pid < 0)
			{
				v8_log_error("Failed when waiting for childs: %d", errno);
			}
			else if (pid == 0)
			{
				v8_log_debug("Child has not changed state");
			}
			else
			{
				if (WIFEXITED(status))
				{
					v8_log_debug("Child %i exited with code: %i", pid, WEXITSTATUS(status));
				}
				else if (WIFSIGNALED(status))
				{
					v8_log_error("Child %i was terminated by signal: %i", pid, WTERMSIG(status));
				}
				else if (WIFSTOPPED(status))
				{
					v8_log_error("Child %i was stopped by signal: %i", pid, WSTOPSIG(status));
				}
				else if (WIFCONTINUED(status))
				{
					v8_log_error("Child %i continued it's execution", pid);
				}
			}

			break;
		case SIGHUP:
			/* TODO: Implement config file reloading */
			v8_log_info("Config file reload not inplemented");
			break;
		default:
			v8_log_error("Unexpected signal arrived: %d", siginfo.ssi_signo);
			break;
		}
	}
}

static void v8_handle_signal_error(int fd, void * data)
{
	v8_log_error("Error on signal handler %d", errno);
	/* TODO: handle error */
}
