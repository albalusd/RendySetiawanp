/*
 * Copyright (c) 2017 Joris Vink <joris@coders.se>
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

#include "kore.h"

#if !defined(KORE_NO_HTTP)
#include "http.h"
#endif

#if defined(KORE_USE_PYTHON)
#include "python_api.h"
#endif

static int	native_runtime_onload(void *, int);
static void	native_runtime_connect(void *, struct connection *);
#if !defined(KORE_NO_HTTP)
static int	native_runtime_http_request(void *, struct http_request *);
static int	native_runtime_validator(void *, struct http_request *, void *);
#endif

struct kore_runtime kore_native_runtime = {
	KORE_RUNTIME_NATIVE,
#if !defined(KORE_NO_HTTP)
	.http_request = native_runtime_http_request,
	.validator = native_runtime_validator,
#endif
	.onload = native_runtime_onload,
	.connect = native_runtime_connect
};

struct kore_runtime_call *
kore_runtime_getcall(const char *symbol)
{
	void				*ptr;
	struct kore_runtime_call	*rcall;
	struct kore_runtime		*runtime;

	ptr = kore_module_getsym(symbol, &runtime);
	if (ptr == NULL)
		return (NULL);

	rcall = kore_malloc(sizeof(*rcall));
	rcall->addr = ptr;
	rcall->runtime = runtime;

	return (rcall);
}

int
kore_runtime_onload(struct kore_runtime_call *rcall, int action)
{
	return (rcall->runtime->onload(rcall->addr, action));
}

void
kore_runtime_connect(struct kore_runtime_call *rcall, struct connection *c)
{
	rcall->runtime->connect(rcall->addr, c);
}

#if !defined(KORE_NO_HTTP)
int
kore_runtime_http_request(struct kore_runtime_call *rcall,
    struct http_request *req)
{
	return (rcall->runtime->http_request(rcall->addr, req));
}

int
kore_runtime_validator(struct kore_runtime_call *rcall,
    struct http_request *req, void *data)
{
	return (rcall->runtime->validator(rcall->addr, req, data));
}
#endif

static void
native_runtime_connect(void *addr, struct connection *c)
{
	void	(*cb)(struct connection *);

	*(void **)&(cb) = addr;
	cb(c);
}

static int
native_runtime_onload(void *addr, int action)
{
	int		(*cb)(int);

	*(void **)&(cb) = addr;
	return (cb(action));
}

#if !defined(KORE_NO_HTTP)
static int
native_runtime_http_request(void *addr, struct http_request *req)
{
	int		(*cb)(struct http_request *);

	*(void **)&(cb) = addr;
	return (cb(req));
}

static int
native_runtime_validator(void *addr, struct http_request *req, void *data)
{
	int		(*cb)(struct http_request *, void *);

	*(void **)&(cb) = addr;
	return (cb(req, data));
}
#endif