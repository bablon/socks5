#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#define NR_EVENTS	1024

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

static inline void list_init(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void list_insert(struct list_head *list, struct list_head *new)
{
	new->next = list->next;
	new->prev = list;

	list->next->prev = new;
	list->next = new;
}

static inline void list_insert_tail(struct list_head *list, struct list_head *new)
{
	new->next = list;
	new->prev = list->prev;

	list->prev->next = new;
	list->prev = new;
}

static inline int list_empty(struct list_head *list)
{
	return list->next == list->prev;
}

static inline void list_del(struct list_head *node)
{
	if (!list_empty(node)) {
		struct list_head *prev = node->prev;
		struct list_head *next = node->next;

		prev->next = next;
		next->prev = prev;
	}
}

#define list_for_each_safe(ptr, next, head)	\
	for (ptr = (head)->next, next = ptr->next; ptr != (head); ptr = next, next = ptr->next)

#define list_for_each(ptr, head)	\
	for (ptr = (head)->next; ptr != (head); ptr = ptr->next)

struct timer {
	struct list_head list;
	unsigned long key;
	void (*handler)(void *data);
};

struct source {
	int fd;
	int events;
	void *data;
	void (*func)(struct source *s, int events);
	struct loop *loop;

	unsigned active:1;

	struct timer timer;
};

struct loop {
	int ep;

	struct epoll_event event_list[NR_EVENTS];
	struct source source_list[NR_EVENTS];
	void *free_source;

	struct list_head timer_list;
};

struct stream_info {
	unsigned long status;

	struct source *source;

	struct sockaddr_in local;
	struct sockaddr_in remote;

	char local_name[24];
	char remote_name[24];

	char *host;
};

struct stream_chain {
	struct stream_info arr[2];
};

void source_add_timer(struct source *s, unsigned long usec, void (*func)(struct source *s))
{
	struct timespec ts;
	unsigned long key = 0;
	struct loop *loop = s->loop;
	struct list_head *ptr;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
		perror("clock_gettime");

	key = ts.tv_sec * 1000000;
	key += ts.tv_nsec / 1000;

	if (!list_empty(&s->timer.list))
		list_del(&s->timer.list);

	list_for_each(ptr, &loop->timer_list) {
		struct timer *timer;

		timer = (struct timer *)((char *)ptr - offsetof(typeof(*timer), list));
		if (key < timer->key)
			break;
	}

	list_insert(ptr->prev, &s->timer.list);
}

struct loop *loop_create(void)
{
	struct loop *loop;
	int i;
	void **prev;

	loop = calloc(1, sizeof(*loop));
	if (!loop)
		return NULL;

	list_init(&loop->timer_list);

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

#define list_entry(ptr, type, member)	\
	(type *)((char *)(ptr) - offsetof(type, member))

#define list_first_entry(ptr, type, member)	\
	list_entry((ptr)->next, type, member)

unsigned long get_current_usec(void)
{
	struct timespec ts = { 0, 0 };

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int loop_process_events_timers(struct loop *loop, int usec)
{
	int i, ret;
	int timeout = -1;
	struct list_head *ptr, *next;

	if (!list_empty(&loop->timer_list)) {
		struct timer *timer;

		timer = list_first_entry(&loop->timer_list, struct timer, list);
		timeout = timer->key;
	}

	if (usec > 0) {
		if (timeout < 0)
			timeout = usec;
		else if (timeout > usec)
			timeout = usec;
	}

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

		if (!s->active)
			continue;

		if (events & (EPOLLERR| EPOLLHUP)) {
			if (!(events & (EPOLLIN|EPOLLOUT)))
				events |= (EPOLLIN|EPOLLOUT);
		}

		s->func(s, events);
	}

	timeout = get_current_usec();

	list_for_each_safe(ptr, next, &loop->timer_list) {
		struct timer *timer;
		struct source *s;

		timer = list_entry(ptr, struct timer, list);
		if (timer->key > timeout)
			break;

		list_del(ptr);

		s = (struct source *)((char *)timer - offsetof(struct source, timer));

		timer->handler(s);
	}

	return 0;
}

void loop_del_source(struct source *s, int ctldel)
{
	struct loop *loop = s->loop;
	struct epoll_event ee;

	s->data = loop->free_source;
	loop->free_source = s;
	s->active = 0;

	if (ctldel) {
		ee.events = 0;
		ee.data.ptr = NULL;

		if (epoll_ctl(loop->ep, EPOLL_CTL_DEL, s->fd, &ee) == -1)
			perror("epoll_ctl del");
	}
}

struct source *loop_get_source(struct loop *loop, int fd, int events,
		void (*func)(struct source *, int), void *data)
{
	struct source *s;
	struct epoll_event ee;

	if (!loop->free_source)
		return NULL;
	s = loop->free_source;
	loop->free_source = s->data;

	memset(s, 0, sizeof(*s));

	ee.events = events;
	ee.data.ptr = s;
	if (epoll_ctl(loop->ep, EPOLL_CTL_ADD, fd, &ee) == -1) {
		perror("epoll_ctl add");
		loop_del_source(s, 0);
		return NULL;
	}

	s->fd = fd;
	s->active = 1;
	s->events = events;
	s->loop = loop;
	s->func = func;
	s->data = data;

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
	if (ret != 0) {
		fprintf(stderr, "getaddrinfo: %s:%s %s\n", host, service, gai_strerror(ret));
		return -1;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		struct timeval tv = { 0, 200000 };
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

void source_close(struct source *s, void (*destroy)(void *data))
{
	if (!s)
		return;

	if (s->data && destroy) {
		destroy(s->data);
		s->data = NULL;
	}
	if (s->fd > 0) {
		loop_del_source(s, 1);
		close(s->fd);
	}
	s->fd = -1;
}

void close_stream_chain(struct stream_chain *chain)
{
	int i;
	struct stream_info *si = &chain->arr[0];
	struct stream_info *di = &chain->arr[1];

	printf("%s %d sources (%p, %p)\n", __FUNCTION__, __LINE__, si->source, di->source);

	for (i = 0; i < 2; i++) {
		if (chain->arr[i].host)
			free(chain->arr[i].host);
		source_close(chain->arr[i].source, NULL);
	}

	free(chain);
}

void get_sockname_info(struct stream_info *data, int fd)
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
	struct stream_chain *chain = s->data;

	if (buf[0] != 5) {
		close_stream_chain(chain);
	} else if (ret != 2 + buf[1]) {
		close_stream_chain(chain);
	} else {
		int i;
		struct stream_info *si = &chain->arr[0];
		uint8_t res[2] = { 0x05, 0x00 };

		for (i = 0; i < buf[1]; i++) {
			if (buf[2+i] == 0)
				break;
		}

		if (i == buf[1]) {
			close_stream_chain(chain);
			return;
		}

		si->status = SOCKS5_CONNECT;
		i = write(s->fd, res, sizeof(res));
		if (i != sizeof(res)) {
			if (i < 0) {
				perror("write");
			}
			close_stream_chain(chain);
		}
	}
}

void handle_remote(struct source *s, int events)
{
	char buf[16384];
	struct stream_chain *chain = s->data;
	struct stream_info *si = &chain->arr[1];
	struct stream_info *di = &chain->arr[0];
	struct source *peer = chain->arr[0].source;
	ssize_t ret, r;

	r = read(s->fd, buf, sizeof(buf));
	if (r <= 0) {
		if (r == -1)
			perror("read");

		close_stream_chain(chain);
		return;
	}

	ret = write(peer->fd, buf, r);
	printf("%s -> %s %zd:%zd bytes\n", si->host, di->remote_name, r, ret);
	if (ret <= 0) {
		if (ret < 0)
			perror("write");

		close_stream_chain(chain);
	}
}

void handle_socks5_connect(struct source *s, uint8_t *buf, size_t ret)
{
	struct stream_chain *chain = s->data;

	if (buf[0] != 5 || buf[1] != 0x1) {
		close_stream_chain(chain);
	} else if (buf[3] != 3) {
		close_stream_chain(chain);
	} else {
		int len = buf[4];
		int total = 4 + 1 + len + 2;
		struct stream_info *si = &chain->arr[0];
		struct stream_info *di = &chain->arr[1];
		struct source *c;
		int fd;
		uint16_t port;
		char service[16];
		char *host = (char *)&buf[5];
		uint8_t res[] = { 0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

		if (total != ret) {
			fprintf(stderr, "received an unexpected packet\n");
			close_stream_chain(chain);
			return;
		}

 		port = ntohs(*(uint16_t *)&buf[5+len]);
		buf[5+len] = 0;
		sprintf(service, "%u", port);

		fd = connect_host(host, service);
		if (fd == -1) {
			fprintf(stderr, "failed to connect %s:%s\n", host, service);
			close_stream_chain(chain);
			return;
		}

		c = loop_get_source(s->loop, fd, EPOLLIN, handle_remote, s->data);
		if (!c) {
			fprintf(stderr, "failed to get source for %s:%s\n", host, service);
			close(fd);
			close_stream_chain(chain);
			return;
		}

		di->source = c;
		get_sockname_info(di, fd);

		di->host = calloc(1, buf[4] + strlen(service) + 2);
		if (di->host) {
			sprintf(di->host, "%s:%s", host, service);
		}

		printf("%s <- %s <- %s <- %s\n", di->host, di->local_name, si->local_name, si->remote_name);

		if (write(s->fd, res, sizeof(res)) < 0) {
			perror("error");
			close_stream_chain(chain);
		}

		si->status = SOCKS5_CONNECT + 1;
	}
}

void handle_socks5_data(struct source *s, uint8_t *buf, size_t ret)
{
	struct stream_chain *chain = s->data;
	struct source *peer = chain->arr[1].source;
	struct stream_info *si = &chain->arr[0];
	struct stream_info *di = &chain->arr[1];
	ssize_t r = -1;

	r = write(peer->fd, buf, ret);
	printf("%s <- %s %zd bytes\n", di->host, si->remote_name, r);
	if (r <= 0) {
		if (r < 0)
			perror("write");
		close_stream_chain(chain);
	}
}

void handle_request(struct source *s, int events)
{
	uint8_t buf[512];
	ssize_t ret;
	struct stream_chain *chain = s->data;
	struct stream_info *si = &chain->arr[0];

	ret = read(s->fd, buf, sizeof(buf));
	if (ret <= 0) {
		if (ret == -1) {
			perror("read");
		}

		close_stream_chain(chain);
		return;
 	}

	if (si->status == SOCKS5_REQ) {
		handle_socks5_req(s, buf, ret);
	} else if (si->status == SOCKS5_CONNECT) {
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
	struct stream_info *si;
	struct stream_chain *chain;

	fd = accept(s->fd, (struct sockaddr *)&sin, &socklen);
	if (fd == -1) {
		perror("accept");
		source_close(s, NULL);
		return;
	}

	chain = calloc(1, sizeof(*chain));
	if (chain == NULL) {
		close(fd);
		return;
	}

	c = loop_get_source(s->loop, fd, EPOLLIN, handle_request, chain);
	if (!c) {
		printf("Too many connection!\n");
		free(chain);
		close(fd);
		return;
	}

	si = &chain->arr[0];
	si->source = c;
	get_sockname_info(si, fd);

	printf("%s <- %s\n", si->local_name, si->remote_name);
}

int main(int argc, char *argv[])
{
	struct loop *loop;
	int fd;
	struct source *s;
	int ret;

	signal(SIGPIPE, SIG_IGN);

	loop = loop_create();
	if (!loop)
		exit(1);

	fd = server_create(9000);
	if (fd < 0) {
		loop_destroy(loop);
		exit(1);
	}

	s = loop_get_source(loop, fd, EPOLLIN, server_do_accept, NULL);
	if (s == NULL) {
		loop_destroy(loop);
		exit(2);
	}

	for (;;) {
		ret = loop_process_events_timers(loop, -1);
		if (ret < 0) {
			fprintf(stderr, "exiting %d from loop\n", ret);
			break;
		}
	}

	source_close(s, NULL);
	loop_destroy(loop);

	return ret < 0 ? 3 : 0;
}
