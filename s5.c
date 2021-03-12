#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NR_EVENTS	1024

struct source {
	int fd;
	int events;
	void *data;
	void (*func)(struct source *s, int events);
	struct loop *loop;
};

struct loop {
	int ep;

	struct epoll_event event_list[NR_EVENTS];
	struct source source_list[NR_EVENTS];
	void *free_source;
};

struct source_data {
	union {
		unsigned long status;
		struct source *peer;
	};

	struct sockaddr_in local;
	struct sockaddr_in remote;

	char local_name[24];
	char remote_name[24];

	char *host;
};

struct loop *loop_create(void)
{
	struct loop *loop;
	int i;
	void **prev;

	loop = calloc(1, sizeof(*loop));
	if (!loop)
		return NULL;

	loop->ep = epoll_create1(EPOLL_CLOEXEC);
	if (loop->ep == -1) {
		free(loop);
		return NULL;
	}

	prev = &loop->free_source;
	for (i = 0; i < NR_EVENTS; i++) {
		*prev = &loop->source_list[i];
		prev = &loop->source_list[i].data;
	}
	*prev = NULL;

	return loop;
}

void loop_destroy(struct loop *loop)
{
	close(loop->ep);
	free(loop);
}

int loop_process_events(struct loop *loop, int timeout)
{
	int i, ret;

again:
	ret = epoll_wait(loop->ep, loop->event_list, NR_EVENTS, timeout);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;
		else {
			perror("epoll_wait");
			return -1;
		}
	}

	for (i = 0; i < ret; i++) {
		struct source *s = loop->event_list[i].data.ptr;
		int events = loop->event_list[i].events;

		s->func(s, events);
	}

	return 0;
}

void loop_del_source(struct source *s, int ctldel)
{
	struct loop *loop = s->loop;
	struct epoll_event ee;

	s->data = loop->free_source;
	loop->free_source = s;

	if (ctldel) {
		ee.events = 0;
		ee.data.ptr = NULL;

		if (epoll_ctl(loop->ep, EPOLL_CTL_DEL, s->fd, &ee) == -1)
			perror("epoll_ctl del");
	}
}

struct source *loop_get_source(struct loop *loop, int fd, int events)
{
	struct source *s;
	struct epoll_event ee;

	if (!loop->free_source)
		return NULL;
	s = loop->free_source;
	loop->free_source = s->data;

	memset(s, 0, sizeof(*s));
	s->fd = fd;
	s->events = events;
	s->loop = loop;

	ee.events = events;
	ee.data.ptr = s;
	if (epoll_ctl(loop->ep, EPOLL_CTL_ADD, fd, &ee) == -1) {
		perror("epoll_ctl add");
		loop_del_source(s, 0);
		return NULL;
	}

	return s;
}

int loop_mod_source(struct source *s, int events)
{
	int op;
	struct epoll_event ee;

	op = s->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

	if ((events & (EPOLLIN|EPOLLOUT)) == EPOLLIN) {
		s->events &= ~EPOLLOUT;
	} else if ((events & (EPOLLIN|EPOLLOUT)) == EPOLLOUT) {
		s->events &= ~(EPOLLIN|EPOLLRDHUP);
	}

	s->events |= events;
	ee.events = s->events;
	ee.data.ptr = s;

	if (epoll_ctl(s->loop->ep, op, s->fd, &ee) == -1) {
		perror("epoll_ctl mod");
		return -1;
	}

	return 0;
}

int setreuseaddr(int sock)
{
	int opt = 1;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
		perror("setsockopt so_reuseaddr");
		return -errno;
	}

	return 0;
}

#ifdef SO_REUSEPORT
int setreuseport(int sock)
{
	int opt = 1;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
		perror("setsockopt so_reuseport");
		return -errno;
	}

	return 0;
}
#else
int setreuseport(int sock)
{
	return 0;
}
#endif

int server_create(uint16_t port)
{
	int fd;
	struct sockaddr_in sin;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
		return -errno;

	setreuseaddr(fd);
	setreuseport(fd);

	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		perror("bind");
		close(fd);
		return -errno;
	}

	if (listen(fd, 16) == -1) {
		perror("listen");
		close(fd);
		return -errno;
	}

	return fd;
}

enum {
	SOCKS5_REQ,
	SOCKS5_CONNECT,
};

int connect_host(const char *host, const char *service)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int ret;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	ret = getaddrinfo(host, service, &hints, &result);
	if (ret != 0)
		exit(1);

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		struct timeval tv = { 0, 100000 };
		socklen_t slen = sizeof(tv);

		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1)
			continue;

		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, slen);
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, slen);

		if (!connect(fd, rp->ai_addr, rp->ai_addrlen)) {
			break;
		}

		close(fd);
		fd = -1;
	}

	freeaddrinfo(result);

	return fd;
}

void source_close(struct source *s)
{
	loop_del_source(s, 1);
	close(s->fd);
	s->fd = -1;
}

void get_sockname_info(struct source_data *data, int fd)
{
	socklen_t slen;

	slen = sizeof(struct sockaddr_in);
	if (getsockname(fd, (struct sockaddr *)&data->local, &slen) == -1)
		perror("getsockname");
	else {
		const char *p;
		p = inet_ntop(AF_INET, &data->local.sin_addr, data->local_name, sizeof(data->local_name));
		sprintf(data->local_name + strlen(p), ":%d", ntohs(data->local.sin_port));
	}

	slen = sizeof(struct sockaddr_in);
	if (getpeername(fd, (struct sockaddr *)&data->remote, &slen) == -1)
		perror("getpeername");
	else {
		const char *p;
		p = inet_ntop(AF_INET, &data->remote.sin_addr, data->remote_name, sizeof(data->remote_name));
		sprintf(data->remote_name + strlen(p), ":%d", ntohs(data->remote.sin_port));
	}
}

void handle_socks5_req(struct source *s, uint8_t *buf, size_t ret)
{
	if (buf[0] != 5) {
		source_close(s);
	} else if (ret != 2 + buf[1]) {
		source_close(s);
	} else {
		int i;
		struct source_data *data = s->data;
		uint8_t res[2] = { 0x05, 0x00 };

		for (i = 0; i < buf[1]; i++) {
			if (buf[2+i] == 0)
				break;
		}

		if (i == buf[1]) {
			source_close(s);
		}

		data->status = SOCKS5_CONNECT;
		if (write(s->fd, res, sizeof(res)) != sizeof(res))
			source_close(s);
	}
}

void handle_remote(struct source *s, int events)
{
	char buf[16384];
	struct source_data *data = s->data;
	ssize_t ret;

	ret = read(s->fd, buf, sizeof(buf));
	if (ret <= 0) {
		if (ret == -1)
			perror("read");

		if (data->peer) {
			struct source_data *peer_data = data->peer->data;
			peer_data->peer = NULL;
		}

		free(s->data);
		close(s->fd);
		loop_del_source(s, 0);

		return;
	}

	printf("%s -> %s %zd bytes\n", data->host, data->local_name, ret);
	if (data->peer) {
		struct source_data *peer_data = data->peer->data;
		ret = write(data->peer->fd, buf, ret);
		printf("%s -> %s %zd bytes\n", peer_data->local_name, peer_data->remote_name, ret);
	}
}

void handle_socks5_connect(struct source *s, uint8_t *buf, size_t ret)
{
	if (buf[0] != 5 || buf[1] != 0x1) {
		source_close(s);
	} else if (buf[3] != 3) {
		source_close(s);
	} else {
		int len = buf[4];
		int total = 4 + 1 + len + 2;
		struct source_data *data;
		struct source *c;
		int fd;
		uint16_t port;
		char service[16];
		char *host = (char *)&buf[5];
		struct source_data *sdata = s->data;
		uint8_t res[] = { 0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

		if (total != ret) {
			source_close(s);
			return;
		}

 		port = ntohs(*(uint16_t *)&buf[5+len]);
		buf[5+len] = 0;
		sprintf(service, "%u", port);

		fd = connect_host(host, service);
		if (fd == -1) {
			source_close(s);	
			return;
		}

		c = loop_get_source(s->loop, fd, EPOLLIN);
		if (!c) {
			close(fd);	
			source_close(s);
			return;
		}

		data = malloc(sizeof(struct source_data));
		if (!data) {
			close(fd);
			loop_del_source(c, 0);
			source_close(s);
			return;
		}

		data->peer = s;

		get_sockname_info(data, fd);

		data->host = calloc(1, buf[4] + strlen(service) + 2);
		if (data->host) {
			sprintf(data->host, "%s:%s", host, service);
		}

		c->func = handle_remote;
		c->data = data;
		sdata->peer = c;
		printf("%s <- %s\n", data->host, data->local_name);

		write(s->fd, res, sizeof(res));
	}
}

void handle_socks5_data(struct source *s, uint8_t *buf, size_t ret)
{
	struct source_data *data = s->data;
	struct source *peer = data->peer;

	if (peer) {
		ret = write(peer->fd, buf, ret);
		printf("%s <- %s %zd bytes\n", data->local_name, data->remote_name, ret);
	}
}

void handle_request(struct source *s, int events)
{
	uint8_t buf[512];
	ssize_t ret;
	struct source_data *data = s->data;

	ret = read(s->fd, buf, sizeof(buf));
	if (ret <= 0) {
		if (ret == -1) {
			perror("read");
		}

		if (data->status > SOCKS5_CONNECT) {
			struct source_data *peer_data = data->peer->data;
			peer_data->peer = NULL;
		}

		free(s->data);
		loop_del_source(s, 1);
		close(s->fd);
		return;
 	}

	if (data->status == SOCKS5_REQ) {
		handle_socks5_req(s, buf, ret);
	} else if (data->status == SOCKS5_CONNECT) {
		handle_socks5_connect(s, buf, ret);
	} else {
		handle_socks5_data(s, buf, ret);
	}
}

void server_do_accept(struct source *s, int events)
{
	int fd;
	struct sockaddr_in sin;
	socklen_t socklen = sizeof(sin);
	struct source *c;
	struct source_data *data;

	fd = accept(s->fd, (struct sockaddr *)&sin, &socklen);
	if (fd == -1) {
		loop_del_source(s, 1);
		close(s->fd);
		return;
	}

	c = loop_get_source(s->loop, fd, EPOLLIN);
	if (!c) {
		printf("Too many connection!\n");
		close(fd);
		return;
	}

	c->func = handle_request;
	data = calloc(1, sizeof(struct source_data));
	if (!data) {
		close(fd);
		loop_del_source(s, 0);
		return;
	}
	c->data = data;

	get_sockname_info(data, fd);

	printf("%s <- %s\n", data->local_name, data->remote_name);
}

int main(int argc, char *argv[])
{
	struct loop *loop;
	int fd;
	struct source *s;
	int ret;

	loop = loop_create();
	if (!loop)
		exit(1);

	fd = server_create(9000);
	if (fd < 0) {
		loop_destroy(loop);	
		exit(1);
	}

	s = loop_get_source(loop, fd, EPOLLIN);
	s->func = server_do_accept;
	s->data = NULL;

	for (;;) {
		ret = loop_process_events(loop, -1);
		if (ret < 0)
			break;
	}

	loop_del_source(s, 0);
	close(fd);

	loop_destroy(loop);

	return ret < 0 ? 1 : 0;
}
