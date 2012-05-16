#include <xseg/xseg.h>
#include <xseg/domain.h>
#include <sys/util.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define XSEG_NR_TYPES 16
#define XSEG_NR_PEER_TYPES 64
#define XSEG_MIN_PAGE_SIZE 4096

static struct xseg_type *__types[XSEG_NR_TYPES];
static unsigned int __nr_types;
static struct xseg_peer *__peer_types[XSEG_NR_PEER_TYPES];
static unsigned int __nr_peer_types;

static void __lock_segment(struct xseg *xseg)
{
	volatile uint64_t *flags;
	flags = &xseg->shared->flags;
	while (__sync_fetch_and_or(flags, XSEG_F_LOCK));
}

static void __unlock_segment(struct xseg *xseg)
{
	volatile uint64_t *flags;
	flags = &xseg->shared->flags;
	__sync_fetch_and_and(flags, ~XSEG_F_LOCK);
}

static struct xseg_type *__find_type(const char *name, long *index)
{
	long i;
	for (i = 0; (*index = i) < __nr_types; i++)
		if (!strncmp(__types[i]->name, name, XSEG_TNAMESIZE))
			return __types[i];
	return NULL;
}

static struct xseg_peer *__find_peer_type(const char *name, int64_t *index)
{
	int64_t i;
	for (i = 0; (*index = i) < __nr_peer_types; i++) {
		if (!strncmp(__peer_types[i]->name, name, XSEG_TNAMESIZE))
			return __peer_types[i];
	}
	return NULL;
}

void xseg_report_peer_types(void)
{
	long i;
	XSEGLOG("total %u peer types:\n", __nr_peer_types);
	for (i = 0; i < __nr_peer_types; i++)
		XSEGLOG("%ld: '%s'\n", i, __peer_types[i]->name);
}

static struct xseg_type *__find_or_load_type(const char *name)
{
	long i;
	struct xseg_type *type = __find_type(name, &i);
	if (type)
		return type;

	__load_plugin(name);
	return __find_type(name, &i);
}

static struct xseg_peer *__find_or_load_peer_type(const char *name)
{
	int64_t i;
	struct xseg_peer *peer_type = __find_peer_type(name, &i);
	if (peer_type)
		return peer_type;

	__load_plugin(name);
	return __find_peer_type(name, &i);
}

static struct xseg_peer *__get_peer_type(struct xseg *xseg, uint32_t serial)
{
	char *name;
	struct xseg_peer *type;
	struct xseg_private *priv = xseg->priv;
	char (*shared_peer_types)[XSEG_TNAMESIZE];

	if (serial >= xseg->max_peer_types) 
		return NULL;

	type = priv->peer_types[serial];
	if (type)
		return type;

	if (serial >= (1 << xseg->config.page_shift) / XSEG_TNAMESIZE)
		return NULL;

	/* xseg->shared->peer_types is an append-only array,
	 * therefore this should be safe
	 * without either locking or string copying. */
	shared_peer_types = XSEG_TAKE_PTR(xseg->shared->peer_types, xseg->segment);
	name = shared_peer_types[serial];
	if (!*name)
		return NULL;

	type = __find_or_load_peer_type(name);
	priv->peer_types[serial] = type;
	return type;
}

static inline int __validate_port(struct xseg *xseg, uint32_t portno)
{
	return portno < xseg->config.nr_ports;
}

/* type:name:nr_ports:nr_requests:request_size:extra_size:page_shift */

#define TOK(s, sp, def) \
	(s) = (sp); \
	for (;;) { \
		switch (*(sp)) { \
		case 0: \
			s = (def); \
			break; \
		case ':': \
			*(sp)++ = 0; \
			break; \
		default: \
			(sp) ++; \
			continue; \
		} \
		break; \
	} \

static unsigned long strul(char *s)
{
	unsigned long n = 0;
	for (;;) {
		unsigned char c = *s - '0';
		if (c >= 10)
			break;
		n = n * 10 + c;
		s ++;
	}
	return n;
}

/*
static char *strncopy(char *dest, const char *src, uint32_t n)
{
	uint32_t i;
	char c;
	for (i = 0; i < n; i++) {
		c = src[i];
		dest[i] = c;
		if (!c)
			break;
	}
	dest[n-1] = 0;
	return dest;
}
*/

int xseg_parse_spec(char *segspec, struct xseg_config *config)
{
	/* default: "posix:globalxseg:4:512:64:1024:12" */
	char *s = segspec, *sp = segspec;

	/* type */
	TOK(s, sp, "posix");
	strncpy(config->type, s, XSEG_TNAMESIZE);
	config->type[XSEG_TNAMESIZE-1] = 0;

	/* name */
	TOK(s, sp, "globalxseg");
	strncpy(config->name, s, XSEG_NAMESIZE);
	config->name[XSEG_NAMESIZE-1] = 0;

	/* nr_ports */
	TOK(s, sp, "4");
	config->nr_ports = strul(s);

	/* nr_requests */
	TOK(s, sp, "512");
	config->nr_requests = strul(s);

	/* request_size */
	TOK(s, sp, "64");
	config->request_size = strul(s);

	/* extra_size */
	TOK(s, sp, "128");
	config->extra_size = strul(s);

	/* page_shift */
	TOK(s, sp, "12");
	config->page_shift = strul(s);
	return 0;
}

int xseg_register_type(struct xseg_type *type)
{
	long i;
	int r = -1;
	struct xseg_type *__type;
	__lock_domain();
	__type = __find_type(type->name, &i);
	if (__type) {
		XSEGLOG("type %s already exists\n", type->name);
		goto out;
	}

	if (__nr_types >= XSEG_NR_TYPES) {
		XSEGLOG("maximum type registrations reached: %u\n", __nr_types);
		r -= 1;
		goto out;
	}

	type->name[XSEG_TNAMESIZE-1] = 0;
	__types[__nr_types] = type;
	__nr_types += 1;
	r = 0;
out:
	__unlock_domain();
	return r;
}

int xseg_unregister_type(const char *name)
{
	long i;
	int r = -1;
	struct xseg_type *__type;
	__lock_domain();
	__type = __find_type(name, &i);
	if (!__type) {
		XSEGLOG("segment type '%s' does not exist\n", name);
		goto out;
	}

	__nr_types -= 1;
	__types[i] = __types[__nr_types];
	__types[__nr_types] = NULL;
	r = 0;
out:
	__unlock_domain();
	return r;
}

int xseg_register_peer(struct xseg_peer *peer_type)
{
	int64_t i;
	int r = -1;
	struct xseg_peer *type;
	__lock_domain();
	type = __find_peer_type(peer_type->name, &i);
	if (type) {
		XSEGLOG("peer type '%s' already exists\n", type->name);
		goto out;
	}

	if (__nr_peer_types >= XSEG_NR_PEER_TYPES) {
		XSEGLOG("maximum peer type registrations reached: %u",
			__nr_peer_types);
		r -= 1;
		goto out;
	}

	if (peer_type->peer_ops.signal_init()) {
		XSEGLOG("peer type '%s': signal initialization failed\n",
			peer_type->name);
		r -= 1;
		goto out;
	}

	peer_type->name[XSEG_TNAMESIZE-1] = 0;
	__peer_types[__nr_peer_types] = peer_type;
	__nr_peer_types += 1;
	r = 0;

out:
	__unlock_domain();
	return r;
}

int xseg_unregister_peer(const char *name)
{
	int64_t i;
	struct xseg_peer *driver;
	int r = -1;
	__lock_domain();
	driver = __find_peer_type(name, &i);
	if (!driver) {
		XSEGLOG("peer type '%s' does not exist\n", name);
		goto out;
	}

	__nr_peer_types -= 1;
	__peer_types[i] = __peer_types[__nr_peer_types];
	__peer_types[__nr_peer_types] = NULL;
	driver->peer_ops.signal_quit();
	r = 0;
out:
	__unlock_domain();
	return r;
}

int64_t __enable_driver(struct xseg *xseg, struct xseg_peer *driver)
{
	int64_t r;
	char (*drivers)[XSEG_TNAMESIZE];
	uint32_t max_drivers = xseg->max_peer_types;

	if (xseg->shared->nr_peer_types >= max_drivers) {
		XSEGLOG("cannot register '%s': driver namespace full\n",
			driver->name);
		return -1;
	}

	drivers = XSEG_TAKE_PTR(xseg->shared->peer_types, xseg->segment);
	for (r = 0; r < max_drivers; r++) {
		if (!*drivers[r])
			goto bind;
		if (!strncmp(drivers[r], driver->name, XSEG_TNAMESIZE))
			goto success;
	}

	/* Unreachable */
	return -666;

bind:
	/* assert(xseg->shared->nr_peer_types == r); */
	xseg->shared->nr_peer_types = r + 1;
	strncpy(drivers[r], driver->name, XSEG_TNAMESIZE);
	drivers[r][XSEG_TNAMESIZE-1] = 0;

success:
	xseg->priv->peer_types[r] = driver;
	return r;
}

int64_t xseg_enable_driver(struct xseg *xseg, const char *name)
{
	int64_t r = -1;
	struct xseg_peer *driver;

	__lock_domain();
	driver = __find_peer_type(name, &r);
	if (!driver) {
		XSEGLOG("driver '%s' not found\n", name);
		goto out;
	}

	__lock_segment(xseg);
	r = __enable_driver(xseg, driver);
	__unlock_segment(xseg);
out:
	__unlock_domain();
	return r;
}

int xseg_disable_driver(struct xseg *xseg, const char *name)
{
	int64_t i;
	int r = -1;
	struct xseg_private *priv = xseg->priv;
	struct xseg_peer *driver;
	__lock_domain();
	driver =  __find_peer_type(name, &i);
	if (!driver) {
		XSEGLOG("driver '%s' not found\n", name);
		goto out;
	}

	for (i = 0; i < xseg->max_peer_types; i++)
		if (priv->peer_types[i] == driver)
			priv->peer_types[i] = NULL;
	r = 0;
out:
	__unlock_domain();
	return r;
}

/* NOTE: calculate_segment_size() and initialize_segment()
 * must always be exactly in sync!
*/

static uint64_t calculate_segment_size(struct xseg_config *config)
{
	uint64_t size = 0;
	uint32_t page_size, page_shift = config->page_shift;

	/* assert(sizeof(struct xseg) <= (1 << 9)); */

	if (page_shift < 9) {
		XSEGLOG("page_shift must be >= %d\n", 9);
		return 0;
	}

	page_size = 1 << page_shift;

	/* struct xseg itself */
	size += page_size;
	/* free requests queue struct */
	size += page_size;

	size += config->nr_requests * sizeof(struct xseg_request);
	size = __align(size, page_shift);

	size += config->nr_requests * config->request_size * page_size;

	size += config->nr_ports * sizeof(struct xseg_port);
	size = __align(size, page_shift);

	/* queue entries for 3 xqueues per port... */
	size += config->nr_ports * 3 * config->nr_requests * sizeof(xqindex);
	size = __align(size, page_shift);

	/* ...and one global free queue */
	size += config->nr_requests * sizeof(xqindex);
	size = __align(size, page_shift);

	size += config->extra_size;
	size = __align(size, page_shift);

	size += sizeof(struct xseg_shared);
	size = __align(size, page_shift);

	/* page for type names */
	size += page_size;

	return size;
}

static long initialize_segment(struct xseg *xseg, struct xseg_config *cfg)
{
	uint32_t page_shift = cfg->page_shift, page_size = 1 << page_shift;
	struct xseg_shared *shared;
	char *segment = (char *)xseg;
	struct xq *q;
	void *qmem;
	uint64_t bodysize, size = page_size, i, nr_requests = cfg->nr_requests;

	if (page_size < XSEG_MIN_PAGE_SIZE)
		return -1;

	xseg->free_requests = XSEG_MAKE_PTR(segment + size, segment);
	size += page_size;

	xseg->requests = XSEG_MAKE_PTR(segment + size, segment);
	size += nr_requests * sizeof(struct xseg_request);
	size = __align(size, page_shift);

	xseg->buffers = XSEG_MAKE_PTR(segment + size, segment);
	size += nr_requests * cfg->request_size * page_size;

	for (i = 0; i < nr_requests; i++) {
		struct xseg_request *req = XSEG_TAKE_PTR(&xseg->requests[i], segment);
		/* xseg_allocate() zeroes the segment out */
		req->buffer = xseg->buffers + i * cfg->request_size * page_size;
		req->bufferlen = cfg->request_size * page_size;
		req->data = req->buffer;
		req->datalen = req->bufferlen;
	}

	xseg->ports = XSEG_MAKE_PTR(segment + size, segment);
	size += cfg->nr_ports * sizeof(struct xseg_port);
	bodysize = nr_requests * sizeof(xqindex);
	for (i = 0; i < cfg->nr_ports; i++) {
		struct xseg_port *port = XSEG_TAKE_PTR(&xseg->ports[i], segment);

		q = &port->free_queue;
		qmem = segment + size;
		xq_init_empty(q, nr_requests, qmem);
		size += bodysize;

		q = &port->request_queue;
		qmem = segment + size;
		xq_init_empty(q, nr_requests, qmem);
		size += bodysize;

		q = &port->reply_queue;
		qmem = segment + size;
		xq_init_empty(q, nr_requests, qmem);
		size += bodysize;
	}
	size = __align(size, page_shift);

	q = XSEG_TAKE_PTR(xseg->free_requests, segment);
	qmem = segment + size;
	xq_init_seq(q, nr_requests, nr_requests, qmem);
	size += bodysize;
	size = __align(size, page_shift);

	xseg->extra = XSEG_MAKE_PTR(segment + size, segment);
	size += cfg->extra_size;
	size = __align(size, page_shift);

	shared = (struct xseg_shared *)(segment + size);
	xseg->shared = XSEG_MAKE_PTR(shared, segment);
	shared->flags = 0;
	shared->nr_peer_types = 0;
	size += sizeof(struct xseg_shared);
	size = __align(size, page_shift);

	shared->peer_types = XSEG_MAKE_PTR(segment + size, segment);
	size += page_size;
	xseg->max_peer_types = page_size / XSEG_TNAMESIZE;

	xseg->segment_size = size;
	memcpy(&xseg->config, cfg, sizeof(struct xseg_config));

	xseg->counters.req_cnt = 0;
	xseg->counters.avg_req_lat = 0;

	return 0;
}

int xseg_create(struct xseg_config *cfg)
{
	struct xseg *xseg = NULL;
	struct xseg_type *type;
	struct xseg_operations *xops;
	uint64_t size;
	long r;

	type = __find_or_load_type(cfg->type);
	if (!type) {
		cfg->type[XSEG_TNAMESIZE-1] = 0;
		XSEGLOG("type '%s' does not exist\n", cfg->type);
		goto out_err;
	}

	size = calculate_segment_size(cfg);
	if (!size) {
		XSEGLOG("invalid config!\n");
		goto out_err;
	}

	xops = &type->ops;
	cfg->name[XSEG_NAMESIZE-1] = 0;
	r = xops->allocate(cfg->name, size);
	if (r) {
		XSEGLOG("cannot allocate segment!\n");
		goto out_err;
	}

	xseg = xops->map(cfg->name, size, NULL);
	if (!xseg) {
		XSEGLOG("cannot map segment!\n");
		goto out_deallocate;
	}

	r = initialize_segment(xseg, cfg);
	xops->unmap(xseg, size);
	if (r) {
		XSEGLOG("cannot initilize segment!\n");
		goto out_deallocate;
	}


	return 0;

out_deallocate:
	xops->deallocate(cfg->name);
out_err:
	return -1;
}

void xseg_destroy(struct xseg *xseg)
{
	struct xseg_type *type;

	__lock_domain();
	type = __find_or_load_type(xseg->config.type);
	if (!type) {
		XSEGLOG("no segment type '%s'\n", xseg->config.type);
		goto out;
	}

	/* should destroy() leave() first? */
	type->ops.deallocate(xseg->config.name);
out:
	__unlock_domain();
}

static int pointer_ok(	unsigned long ptr,
			unsigned long base,
			uint64_t size,
			char *name)
{
	int ret = !(ptr >= base && ptr < base + size);
	if (ret)
		XSEGLOG("invalid pointer '->%s' [%llx on %llx]!\n",
			(unsigned long long)ptr,
			(unsigned long long)base,
			name);
	return ret;
}

#define POINTER_OK(xseg, field, base) \
	 pointer_ok(	(unsigned long)((xseg)->field), \
			(unsigned long)(base), \
			(xseg)->segment_size, \
			#field)

static int xseg_validate_pointers(struct xseg *xseg)
{
	int r = 0;
	r += POINTER_OK(xseg, requests, xseg->segment);
	r += POINTER_OK(xseg, free_requests, xseg->segment);
	r += POINTER_OK(xseg, ports, xseg->segment);
	r += POINTER_OK(xseg, buffers, xseg->segment);
	r += POINTER_OK(xseg, extra, xseg->segment);
	r += POINTER_OK(xseg, shared, xseg->segment);
	return r;
}

struct xseg *xseg_join(	char *segtypename,
			char *segname,
			char *peertypename,
			void (*wakeup)
			(	struct xseg *xseg,
				uint32_t portno		))
{
	struct xseg *xseg, *__xseg;
	uint64_t size;
	struct xseg_peer *peertype;
	struct xseg_type *segtype;
	struct xseg_private *priv;
	struct xseg_operations *xops;
	struct xseg_peer_operations *pops;
	int r;

	__lock_domain();

	peertype = __find_or_load_peer_type(peertypename);
	if (!peertype) {
		XSEGLOG("Peer type '%s' not found\n", peertypename);
		__unlock_domain();
		goto err;
	}

	segtype = __find_or_load_type(segtypename);
	if (!segtype) {
		XSEGLOG("Segment type '%s' not found\n", segtypename);
		__unlock_domain();
		goto err;
	}

	__unlock_domain();

	xops = &segtype->ops;
	pops = &peertype->peer_ops;

	xseg = pops->malloc(sizeof(struct xseg));
	if (!xseg) {
		XSEGLOG("Cannot allocate memory");
		goto err;
	}

	priv = pops->malloc(sizeof(struct xseg_private));
	if (!priv) {
		XSEGLOG("Cannot allocate memory");
		goto err_seg;
	}

	__xseg = xops->map(segname, XSEG_MIN_PAGE_SIZE, NULL);
	if (!__xseg) {
		XSEGLOG("Cannot map segment");
		goto err_priv;
	}

	size = __xseg->segment_size;
	/* XSEGLOG("joined segment of size: %lu\n", (unsigned long)size); */
	xops->unmap(__xseg, XSEG_MIN_PAGE_SIZE);

	__xseg = xops->map(segname, size, xseg);
	if (!__xseg) {
		XSEGLOG("Cannot map segment");
		goto err_priv;
	}

	priv->segment_type = *segtype;
	priv->peer_type = *peertype;
	priv->wakeup = wakeup;
	xseg->max_peer_types = __xseg->max_peer_types;

	priv->peer_types = pops->malloc(sizeof(void *) * xseg->max_peer_types);
	if (!priv->peer_types) {
		XSEGLOG("Cannot allocate memory");
		goto err_unmap;
	}
	memset(priv->peer_types, 0, sizeof(void *) * xseg->max_peer_types);

	xseg->priv = priv;
	xseg->config = __xseg->config;
	xseg->version = __xseg->version;
	xseg->requests = XSEG_TAKE_PTR(__xseg->requests, __xseg);
	xseg->free_requests = XSEG_TAKE_PTR(__xseg->free_requests, __xseg);
	xseg->ports = XSEG_TAKE_PTR(__xseg->ports, __xseg);
	xseg->buffers = XSEG_TAKE_PTR(__xseg->buffers, __xseg);
	xseg->extra = XSEG_TAKE_PTR(__xseg->extra, __xseg);
	xseg->shared = XSEG_TAKE_PTR(__xseg->shared, __xseg);
	xseg->segment_size = size;
	xseg->segment = __xseg;

	r = xseg_validate_pointers(xseg);
	if (r) {
		XSEGLOG("found %d invalid xseg pointers!\n", r);
		goto err_free_types;
	}

	/* Do we need this?
	r = xops->signal_join(xseg);
	if (r) {
		XSEGLOG("Cannot attach signaling to segment! (error: %d)\n", r);
		goto err_free_types;
	}
	*/

	return xseg;

err_free_types:
	pops->mfree(priv->peer_types);
err_unmap:
	xops->unmap(__xseg, size);
err_priv:
	pops->mfree(priv);
err_seg:
	pops->mfree(xseg);
err:
	return NULL;
}

void xseg_leave(struct xseg *xseg)
{
	struct xseg_type *type;

	__lock_domain();
	type = __find_or_load_type(xseg->config.type);
	if (!type) {
		XSEGLOG("no segment type '%s'\n", xseg->config.type);
		__unlock_domain();
		return;
	}
	__unlock_domain();

	type->ops.unmap(xseg->segment, xseg->segment_size);
}

int xseg_prepare_wait(struct xseg *xseg, uint32_t portno)
{
	if (!__validate_port(xseg, portno))
		return -1;

	return xseg->priv->peer_type.peer_ops.prepare_wait(xseg, portno);
}

int xseg_cancel_wait(struct xseg *xseg, uint32_t portno)
{
	if (!__validate_port(xseg, portno))
		return -1;
	return xseg->priv->peer_type.peer_ops.cancel_wait(xseg, portno);
}

int xseg_wait_signal(struct xseg *xseg, uint32_t usec_timeout)
{
	return xseg->priv->peer_type.peer_ops.wait_signal(xseg, usec_timeout);
}

int xseg_signal(struct xseg *xseg, uint32_t portno)
{
	struct xseg_peer *type;
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		return -1;

	port = &xseg->ports[portno];
	type = __get_peer_type(xseg, port->peer_type);
	if (!type)
		return -1;

	return type->peer_ops.signal(xseg, portno);
}

int xseg_alloc_requests(struct xseg *xseg, uint32_t portno, uint32_t nr)
{
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		return -1;

	port = &xseg->ports[portno];
	return xq_head_to_tail(xseg->free_requests, &port->free_queue, nr);
}

int xseg_free_requests(struct xseg *xseg, uint32_t portno, int nr)
{
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		return -1;

	port = &xseg->ports[portno];
	return xq_head_to_tail(&port->free_queue, xseg->free_requests, nr);
}

struct xseg_request *xseg_get_request(struct xseg *xseg, uint32_t portno)
{
	struct xseg_request *req;
	struct xseg_port *port;
	xqindex xqi;
	if (!__validate_port(xseg, portno))
		return NULL;

	port = &xseg->ports[portno];
	xqi = xq_pop_head(&port->free_queue);
	if (xqi == None)
		return NULL;

	req = &xseg->requests[xqi];
	req->portno = portno;

	req->elapsed = 0;
	req->timestamp.tv_sec = 0;

	return req;
}

int xseg_put_request (  struct xseg *xseg,
			uint32_t portno,
			struct xseg_request *xreq )
{
	xqindex xqi = xreq - xseg->requests;
	xreq->data = xreq->buffer;
	xreq->datalen = xreq->bufferlen;
	xreq->target = NULL;
	xreq->targetlen = 0;

#ifdef DEBUG_PERF
	XSEGLOG("request's @%p rtt is: %llu usecs\n", xreq, xreq->elapsed);
#endif
	if (xreq->elapsed != 0) {
		__lock_segment(xseg);
		++(xseg->counters.req_cnt);
		xseg->counters.avg_req_lat += xreq->elapsed;
		__unlock_segment(xseg);
	}

	return xq_append_head(&xseg->ports[portno].free_queue, xqi) == None;
}

int xseg_prep_request ( struct xseg_request *req,
			uint32_t targetlen, uint64_t datalen )
{
	if (targetlen + datalen > req->bufferlen)
		return -1;

	req->data = req->buffer;
	req->target = req->buffer + req->bufferlen - targetlen;
	req->datalen = datalen;
	req->targetlen = targetlen;
	return 0;
}

static void __update_timestamp(struct xseg_request *xreq)
{
	struct timeval tv;

	__get_current_time(&tv);
	if (xreq->timestamp.tv_sec != 0)
		/*
		 * FIXME: Make xreq->elapsed timeval/timespec again to avoid the
		 * 		  multiplication?
		 */
		xreq->elapsed += (tv.tv_sec - xreq->timestamp.tv_sec) * 1000000 
						+ (tv.tv_usec - xreq->timestamp.tv_usec);

	xreq->timestamp.tv_sec = tv.tv_sec;
	xreq->timestamp.tv_usec = tv.tv_usec;
}

xserial xseg_submit (	struct xseg *xseg, uint32_t portno,
			struct xseg_request *xreq	)
{
	xserial serial = NoSerial;
	xqindex xqi;
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		goto out;

	__update_timestamp(xreq);

	port = &xseg->ports[portno];
	xqi = xreq - xseg->requests;
	serial = xq_append_tail(&port->request_queue, xqi);
out:
	return serial;
}

struct xseg_request *xseg_receive(struct xseg *xseg, uint32_t portno)
{
	xqindex xqi;
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		return NULL;

	port = &xseg->ports[portno];
	xqi = xq_pop_head(&port->reply_queue);
	if (xqi == None)
		return NULL;

	__update_timestamp(&xseg->requests[xqi]);

	return xseg->requests + xqi;
}

struct xseg_request *xseg_accept(struct xseg *xseg, uint32_t portno)
{
	xqindex xqi;
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		return NULL;

	port = &xseg->ports[portno];
	xqi = xq_pop_head(&port->request_queue);
	if (xqi == None)
		return NULL;

	__update_timestamp(&xseg->requests[xqi]);

	return xseg->requests + xqi;
}

xserial xseg_respond (  struct xseg *xseg, uint32_t portno,
			struct xseg_request *xreq  )
{
	xserial serial = NoSerial;
	xqindex xqi;
	struct xseg_port *port;
	if (!__validate_port(xseg, portno))
		goto out;

	__update_timestamp(xreq);

	port = &xseg->ports[portno];
	xqi = xreq - xseg->requests;
	serial = xq_append_tail(&port->reply_queue, xqi);
out:
	return serial;
}


struct xseg_port *xseg_bind_port(struct xseg *xseg, uint32_t req)
{
	uint32_t portno, maxno, id = __get_id(), force;
	struct xseg_port *port;

	if (req > xseg->config.nr_ports) {
		portno = 0;
		maxno = xseg->config.nr_ports;
		force = 0;
	} else {
		portno = req;
		maxno = req;
		force = 1;
	}

	__lock_segment(xseg);
	for (; portno <= maxno; portno++) {
		int64_t driver;
		port = &xseg->ports[portno];
		if (port->owner && !force)
			continue;
		driver = __enable_driver(xseg, &xseg->priv->peer_type);
		if (driver < 0)
			break;
		port->peer_type = (uint64_t)driver;
		port->owner = id;
		goto out;
	}
	port = NULL;
out:
	__unlock_segment(xseg);
	return port;
}


int xseg_initialize(void)
{
	return __xseg_preinit();	/* with or without lock ? */
}

int xseg_finalize(void)
{
	/* finalize not supported yet */
	return -1;
}

#ifdef __KERNEL__
#include <linux/module.h>
#include <xseg/xseg_exports.h>
#endif

