/*
 * Copyright (c) 2013-2018 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>

#include <sched.h>

#include "kore.h"

#if defined(KORE_USE_PGSQL)
#include "pgsql.h"
#endif

#if defined(KORE_USE_TASKS)
#include "tasks.h"
#endif

static int			efd = -1;
static u_int32_t		event_count = 0;
static struct epoll_event	*events = NULL;

void
kore_platform_init(void)
{
	long		n;

	if ((n = sysconf(_SC_NPROCESSORS_ONLN)) == -1) {
		kore_debug("could not get number of cpu's falling back to 1");
		cpu_count = 1;
	} else {
		cpu_count = (u_int16_t)n;
	}
}

void
kore_platform_worker_setcpu(struct kore_worker *kw)
{
	cpu_set_t	cpuset;

	CPU_ZERO(&cpuset);
	CPU_SET(kw->cpu, &cpuset);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == -1) {
		kore_debug("kore_worker_setcpu(): %s", errno_s);
	} else {
		kore_debug("kore_worker_setcpu(): worker %d on cpu %d",
		    kw->id, kw->cpu);
	}
}

void
kore_platform_event_init(void)
{
	if (efd != -1)
		close(efd);
	if (events != NULL)
		kore_free(events);

	if ((efd = epoll_create(10000)) == -1)
		fatal("epoll_create(): %s", errno_s);

	event_count = worker_max_connections + nlisteners;
	events = kore_calloc(event_count, sizeof(struct epoll_event));
}

void
kore_platform_event_cleanup(void)
{
	if (efd != -1) {
		close(efd);
		efd = -1;
	}

	if (events != NULL) {
		kore_free(events);
		events = NULL;
	}
}

void
kore_platform_event_wait(u_int64_t timer)
{
	u_int32_t		r;
	struct kore_event	*evt;
	int			n, i;

	n = epoll_wait(efd, events, event_count, timer);
	if (n == -1) {
		if (errno == EINTR)
			return;
		fatal("epoll_wait(): %s", errno_s);
	}

	if (n > 0) {
		kore_debug("main(): %d sockets available", n);
	}

	r = 0;
	for (i = 0; i < n; i++) {
		if (events[i].data.ptr == NULL)
			fatal("events[%d].data.ptr == NULL", i);

		r = 0;
		evt = (struct kore_event *)events[i].data.ptr;

		if (events[i].events & EPOLLIN)
			evt->flags |= KORE_EVENT_READ;

		if (events[i].events & EPOLLOUT)
			evt->flags |= KORE_EVENT_WRITE;

		if (events[i].events & EPOLLERR ||
		    events[i].events & EPOLLHUP ||
		    events[i].events & EPOLLRDHUP)
			r = 1;

		evt->handle(events[i].data.ptr, r);
	}
}

void
kore_platform_event_all(int fd, void *c)
{
	kore_platform_event_schedule(fd,
	    EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, 0, c);
}

void
kore_platform_event_schedule(int fd, int type, int flags, void *udata)
{
	struct epoll_event	evt;

	kore_debug("kore_platform_event_schedule(%d, %d, %d, %p)",
	    fd, type, flags, udata);

	evt.events = type;
	evt.data.ptr = udata;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &evt) == -1) {
		if (errno == EEXIST) {
			if (epoll_ctl(efd, EPOLL_CTL_MOD, fd, &evt) == -1)
				fatal("epoll_ctl() MOD: %s", errno_s);
		} else {
			fatal("epoll_ctl() ADD: %s", errno_s);
		}
	}
}

void
kore_platform_schedule_read(int fd, void *data)
{
	kore_platform_event_schedule(fd, EPOLLIN | EPOLLET, 0, data);
}

void
kore_platform_schedule_write(int fd, void *data)
{
	kore_platform_event_schedule(fd, EPOLLOUT | EPOLLET, 0, data);
}

void
kore_platform_disable_read(int fd)
{
	if (epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL) == -1)
		fatal("kore_platform_disable_read: %s", errno_s);
}

void
kore_platform_enable_accept(void)
{
	struct listener		*l;

	kore_debug("kore_platform_enable_accept()");

	LIST_FOREACH(l, &listeners, list)
		kore_platform_event_schedule(l->fd, EPOLLIN, 0, l);
}

void
kore_platform_disable_accept(void)
{
	struct listener		*l;

	kore_debug("kore_platform_disable_accept()");

	LIST_FOREACH(l, &listeners, list) {
		if (epoll_ctl(efd, EPOLL_CTL_DEL, l->fd, NULL) == -1)
			fatal("kore_platform_disable_accept: %s", errno_s);
	}
}

void
kore_platform_proctitle(char *title)
{
	if (prctl(PR_SET_NAME, title) == -1) {
		kore_debug("prctl(): %s", errno_s);
	}
}

#if defined(KORE_USE_PLATFORM_SENDFILE)
int
kore_platform_sendfile(struct connection *c, struct netbuf *nb)
{
	off_t		smin;
	ssize_t		sent;
	size_t		len, prevoff;

	prevoff = nb->fd_off;
	smin = nb->fd_len - nb->fd_off;
	len = MIN(SENDFILE_PAYLOAD_MAX, smin);

resend:
	sent = sendfile(c->fd, nb->file_ref->fd, &nb->fd_off, len);
	if (sent == -1) {
		if (errno == EAGAIN) {
			c->evt.flags &= ~KORE_EVENT_WRITE;
			return (KORE_RESULT_OK);
		}

		return (KORE_RESULT_ERROR);
	}

	if (nb->fd_off - prevoff != (size_t)len)
		goto resend;

	if (sent == 0 || nb->fd_off == nb->fd_len) {
		net_remove_netbuf(c, nb);
		c->snb = NULL;
	}

	return (KORE_RESULT_OK);
}
#endif
