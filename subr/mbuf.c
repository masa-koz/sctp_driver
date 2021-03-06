/*-
 * Copyright (c) 1982, 1986, 1988, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)uipc_mbuf.c	8.2 (Berkeley) 1/4/94
 */

#if 0
__FBSDID("$FreeBSD: /repoman/r/ncvs/src/sys/kern/uipc_mbuf.c,v 1.148.2.6 2006/03/23 23:24:32 sam Exp $");
#endif

#include "globals.h"
#include "mbuf.h"

NPAGED_LOOKASIDE_LIST zone_mbuf;
NPAGED_LOOKASIDE_LIST zone_clust;
NPAGED_LOOKASIDE_LIST zone_jumbop;
NPAGED_LOOKASIDE_LIST zone_jumbo9;
NPAGED_LOOKASIDE_LIST zone_jumbo16;

int max_linkhdr;
int max_protohdr;
int max_hdr;
int max_datalen;
#ifdef MBUF_STRESS_TEST
int m_defragpackets;
int m_defragbytes;
int m_defraguseless;
int m_defragfailure;
int m_defragrandomfailures;
#endif


struct mbstat mbstat;

void
mbuf_init(void)
{
	ExInitializeNPagedLookasideList(&zone_mbuf, NULL, NULL, 0, MSIZE + sizeof(struct m_extbuf), 0x64657246, 0);
	ExInitializeNPagedLookasideList(&zone_clust, NULL, NULL, 0, MCLBYTES + sizeof(struct m_extbuf), 0x64657246, 0);
	ExInitializeNPagedLookasideList(&zone_jumbop, NULL, NULL, 0, MJUMPAGESIZE + sizeof(struct m_extbuf), 0x64657246, 0);
	ExInitializeNPagedLookasideList(&zone_jumbo9, NULL, NULL, 0, MJUM9BYTES + sizeof(struct m_extbuf), 0x64657246, 0);
	ExInitializeNPagedLookasideList(&zone_jumbo16, NULL, NULL, 0, MJUM16BYTES + sizeof(struct m_extbuf), 0x64657246, 0);
}

void
mbuf_destroy(void)
{
	ExDeleteNPagedLookasideList(&zone_mbuf);
	ExDeleteNPagedLookasideList(&zone_clust);
	ExDeleteNPagedLookasideList(&zone_jumbop);
	ExDeleteNPagedLookasideList(&zone_jumbo9);
	ExDeleteNPagedLookasideList(&zone_jumbo16);
}


/*
 * Constructor for Mbuf master zone.
 *
 * The 'arg' pointer points to a mb_args structure which
 * contains call-specific information required to support the
 * mbuf allocation API.  See mbuf.h.
 */
static int
mb_ctor_mbuf(void *mem, int size, void *arg)
{
	struct mbuf *m;
	struct mb_args *args;
	int flags;
	short type;

	m = (struct mbuf *)mem;
	args = (struct mb_args *)arg;
	flags = args->flags;
	type = args->type;

	/*
	 * The mbuf is initialized later.  The caller has the
	 * responsibility to set up any MAC labels too.
	 */
	if (type == MT_NOINIT)
		return (0);

	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_len = 0;
	m->m_flags = flags;
	m->m_type = type;
	if (flags & M_PKTHDR) {
		m->m_data = m->m_pktdat;
		m->m_pkthdr.rcvif = NULL;
		m->m_pkthdr.len = 0;
		m->m_pkthdr.header = NULL;
		m->m_pkthdr.csum_flags = 0;
		m->m_pkthdr.csum_data = 0;
	} else
		m->m_data = m->m_dat;
	mbstat.m_mbufs += 1;	/* XXX */
	return (0);
}

/*
 * The Mbuf master zone destructor.
 */
static void
mb_dtor_mbuf(void *mem, int size, void *arg)
{
	struct mbuf *m;

	mbstat.m_mbufs -= 1;	/* XXX */
}

/*
 * The Cluster and Jumbo[PAGESIZE|9|16] zone constructor.
 *
 * Here the 'arg' pointer points to the Mbuf which we
 * are configuring cluster storage for.  If 'arg' is
 * empty we allocate just the cluster without setting
 * the mbuf to it.  See mbuf.h.
 */
static int
mb_ctor_clust(void *mem, int size, void *arg)
{
	struct mbuf *m;
	int type = 0;

 	m = (struct mbuf *)arg;
	if (m != NULL) {
		switch (size) {
		case MCLBYTES:
			type = EXT_CLUSTER;
			break;
#if MJUMPAGESIZE != MCLBYTES
		case MJUMPAGESIZE:
			type = EXT_JUMBOP;
			break;
#endif
		case MJUM9BYTES:
			type = EXT_JUMBO9;
			break;
		case MJUM16BYTES:
			type = EXT_JUMBO16;
			break;
		default:
			panic("unknown cluster size");
			break;
		}
		m->m_ext.ext_buf = (struct m_extbuf *)mem;
		m->m_ext.ext_buf->ref_cnt = 1;
		m->m_data = m->m_ext.ext_buf->data;
		m->m_flags |= M_EXT;
		m->m_ext.ext_free = NULL;
		m->m_ext.ext_args = NULL;
		m->m_ext.ext_size = size;
		m->m_ext.ext_type = type;
	}
	mbstat.m_mclusts += 1;	/* XXX */
	return (0);
}

/*
 * The Mbuf Cluster zone destructor.
 */
static void
mb_dtor_clust(void *mem, int size, void *arg)
{
	mbstat.m_mclusts -= 1;	/* XXX */
}

/*
 * The "packet" keg constructor.
 */
static int
mb_ctor_pack(void *mem, int size, void *arg)
{
	struct mbuf *m;
	struct mb_args *args;
	int flags;
	short type;

	m = (struct mbuf *)mem;
	args = (struct mb_args *)arg;
	flags = args->flags;
	type = args->type;

	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_data = m->m_ext.ext_buf->data;
	m->m_len = 0;
	m->m_flags = (flags | M_EXT);
	m->m_type = type;

	if (flags & M_PKTHDR) {
		m->m_pkthdr.rcvif = NULL;
		m->m_pkthdr.len = 0;
		m->m_pkthdr.header = NULL;
		m->m_pkthdr.csum_flags = 0;
		m->m_pkthdr.csum_data = 0;
	}
	/* m_ext is already initialized. */

	mbstat.m_mbufs += 1;	/* XXX */
	mbstat.m_mclusts += 1;	/* XXX */
	return (0);
}

/*
 * The Mbuf Packet zone destructor.
 */
static void
mb_dtor_pack(void *mem, int size, void *arg)
{
	struct mbuf *m;

	m = (struct mbuf *)mem;

	/* Make sure we've got a clean cluster back. */
	KASSERT((m->m_flags & M_EXT) == M_EXT, ("%s: M_EXT not set", __func__));
	KASSERT(m->m_ext.ext_buf != NULL, ("%s: ext_buf == NULL", __func__));
	KASSERT(m->m_ext.ext_free == NULL, ("%s: ext_free != NULL", __func__));
	KASSERT(m->m_ext.ext_args == NULL, ("%s: ext_args != NULL", __func__));
	KASSERT(m->m_ext.ext_size == MCLBYTES, ("%s: ext_size != MCLBYTES", __func__));
	KASSERT(m->m_ext.ext_type == EXT_PACKET, ("%s: ext_type != EXT_PACKET", __func__));
	mbstat.m_mbufs -= 1;	/* XXX */
	mbstat.m_mclusts -= 1;	/* XXX */
}


struct mbuf *
m_get(int how, short type)
{
	struct mbuf *m;
	struct mb_args args;

	args.flags = 0;
	args.type = type;

	m = (struct mbuf *)ExAllocateFromNPagedLookasideList(&zone_mbuf);
	mb_ctor_mbuf(m, MSIZE, &args);
	return m;
}

struct mbuf *
m_gethdr(int how, short type)
{
	struct mbuf *m;
	struct mb_args args;

	args.flags = M_PKTHDR;
	args.type = type;

	m = (struct mbuf *)ExAllocateFromNPagedLookasideList(&zone_mbuf);
	if (m == NULL) {
		return NULL;
	}
	mb_ctor_mbuf(m, MSIZE, &args);
	return m;
}

struct mbuf *
m_getcl(int how, short type, int flags)
{
	struct mbuf *m;
	void *ext;
	struct mb_args args;

	args.flags = flags;
	args.type = type;

	m = (struct mbuf *)ExAllocateFromNPagedLookasideList(&zone_mbuf);
	if (m == NULL) {
		return NULL;
	}
	mb_ctor_mbuf(m, MSIZE, &args);

	ext = ExAllocateFromNPagedLookasideList(&zone_clust);
	if (ext == NULL) {
		ExFreeToNPagedLookasideList(&zone_mbuf, m);
		return NULL;
	}
	mb_ctor_clust(ext, MCLBYTES, m);
	m->m_ext.ext_type = EXT_PACKET;

	return m;
}

/*
 * m_getjcl() returns an mbuf with a cluster of the specified size attached.
 * For size it takes MCLBYTES, MJUMPAGESIZE, MJUM9BYTES, MJUM16BYTES.
 */
struct mbuf *
m_getjcl(int how, short type, int flags, int size)
{
	struct mb_args args;
	struct mbuf *m;
	void *ext;
	PNPAGED_LOOKASIDE_LIST zone;

	args.flags = flags;
	args.type = type;

	m = (struct mbuf *)ExAllocateFromNPagedLookasideList(&zone_mbuf);
	if (m == NULL) {
		return NULL;
	}
	mb_ctor_mbuf(m, MSIZE, &args);

	switch (size) {
	case MCLBYTES:
		zone = &zone_clust;
		break;
#if MJUMPAGESIZE != MCLBYTES
	case MJUMPAGESIZE:
		zone = &zone_jumbop;
		break;
#endif
	case MJUM9BYTES:
		zone = &zone_jumbo9;
		break;
	case MJUM16BYTES:
		zone = &zone_jumbo16;
		break;
#if 0
	default:
		panic("%s: m_getjcl: invalid cluster type", __func__);
#endif
	}
	ext = ExAllocateFromNPagedLookasideList(zone);
	if (ext == NULL) {
		ExFreeToNPagedLookasideList(&zone_mbuf, m);
		return NULL;
	}
	mb_ctor_clust(ext, size, m);
	return m;
}

void
m_clget(struct mbuf *m, int how)
{
	void *ext;

	m->m_ext.ext_buf = NULL;
	ext = ExAllocateFromNPagedLookasideList(&zone_clust);
	if (ext == NULL) {
		ExFreeToNPagedLookasideList(&zone_mbuf, m);
		return;
	}
	mb_ctor_clust(ext, MCLBYTES, m);
}

/*
 * m_cljget() is different from m_clget() as it can allocate clusters
 * without attaching them to an mbuf.  In that case the return value
 * is the pointer to the cluster of the requested size.  If an mbuf was
 * specified, it gets the cluster attached to it and the return value
 * can be safely ignored.
 * For size it takes MCLBYTES, MJUMPAGESIZE, MJUM9BYTES, MJUM16BYTES.
 */
void *
m_cljget(struct mbuf *m, int how, int size)
{
	void *ext;
	PNPAGED_LOOKASIDE_LIST zone;

	if (m && m->m_flags & M_EXT)
		printf("%s: %p mbuf already has cluster\n", "m_cljget", m);
	if (m != NULL)
		m->m_ext.ext_buf = NULL;

	switch (size) {
	case MCLBYTES:
		zone = &zone_clust;
		break;
#if MJUMPAGESIZE != MCLBYTES
	case MJUMPAGESIZE:
		zone = &zone_jumbop;
		break;
#endif
	case MJUM9BYTES:
		zone = &zone_jumbo9;
		break;
	case MJUM16BYTES:
		zone = &zone_jumbo16;
		break;
#if 0
	default:
		panic("%s: m_getjcl: invalid cluster type", __func__);
#endif
	}
	
	ext = ExAllocateFromNPagedLookasideList(zone);
	if (ext == NULL) {
		return NULL;
	}
	mb_ctor_clust(ext, size, m);
	return ext;
}

struct mbuf *
m_free(struct mbuf *m)
{
	struct mbuf *n = m->m_next;

	if (m->m_flags & M_EXT)
		mb_free_ext(m);
	else
		ExFreeToNPagedLookasideList(&zone_mbuf, m);
	return n;
}

void
m_chtype(struct mbuf *m, short new_type)
{
	m->m_type = new_type;
}

/*
 * Allocate a given length worth of mbufs and/or clusters (whatever fits
 * best) and return a pointer to the top of the allocated chain.  If an
 * existing mbuf chain is provided, then we will append the new chain
 * to the existing one but still return the top of the newly allocated
 * chain.
 */
struct mbuf *
m_getm(struct mbuf *m, int len, int how, short type)
{
	struct mbuf *mb, *top, *cur, *mtail;
	int num, rem;
	int i;

	KASSERT(len >= 0, ("m_getm(): len is < 0"));

	/* If m != NULL, we will append to the end of that chain. */
	if (m != NULL)
		for (mtail = m; mtail->m_next != NULL; mtail = mtail->m_next);
	else
		mtail = NULL;

	/*
	 * Calculate how many mbufs+clusters ("packets") we need and how much
	 * leftover there is after that and allocate the first mbuf+cluster
	 * if required.
	 */
	num = len / MCLBYTES;
	rem = len % MCLBYTES;
	top = cur = NULL;
	if (num > 0) {
		if ((top = cur = m_getcl(how, type, 0)) == NULL)
			goto failed;
		top->m_len = 0;
	}
	num--;

	for (i = 0; i < num; i++) {
		mb = m_getcl(how, type, 0);
		if (mb == NULL)
			goto failed;
		mb->m_len = 0;
		cur = (cur->m_next = mb);
	}
	if (rem > 0) {
		mb = (rem > MINCLSIZE) ?
		    m_getcl(how, type, 0) : m_get(how, type);
		if (mb == NULL)
			goto failed;
		mb->m_len = 0;
		if (cur == NULL)
			top = mb;
		else
			cur->m_next = mb;
	}

	if (mtail != NULL)
		mtail->m_next = top;
	return top;
failed:
	if (top != NULL)
		m_freem(top);
	return NULL;
}

/*
 * Free an entire chain of mbufs and associated external buffers, if
 * applicable.
 */
void
m_freem(struct mbuf *mb)
{

	while (mb != NULL)
		mb = m_free(mb);
}

#if 0
/*-
 * Configure a provided mbuf to refer to the provided external storage
 * buffer and setup a reference count for said buffer.  If the setting
 * up of the reference count fails, the M_EXT bit will not be set.  If
 * successfull, the M_EXT bit is set in the mbuf's flags.
 *
 * Arguments:
 *    mb     The existing mbuf to which to attach the provided buffer.
 *    buf    The address of the provided external storage buffer.
 *    size   The size of the provided buffer.
 *    freef  A pointer to a routine that is responsible for freeing the
 *           provided external storage buffer.
 *    args   A pointer to an argument structure (of any type) to be passed
 *           to the provided freef routine (may be NULL).
 *    flags  Any other flags to be passed to the provided mbuf.
 *    type   The type that the external storage buffer should be
 *           labeled with.
 *
 * Returns:
 *    Nothing.
 */
void
m_extadd(struct mbuf *mb, caddr_t buf, u_int size,
    void (*freef)(void *, void *), void *args, int flags, int type)
{
	if (mb->m_ext.ref_cnt != NULL) {
		*(mb->m_ext.ref_cnt) = 1;
		mb->m_flags |= (M_EXT | flags);
		mb->m_ext.ext_buf = buf;
		mb->m_data = mb->m_ext.ext_buf;
		mb->m_ext.ext_size = size;
		mb->m_ext.ext_free = freef;
		mb->m_ext.ext_args = args;
		mb->m_ext.ext_type = type;
        }
}
#endif

/*
 * Non-directly-exported function to clean up after mbufs with M_EXT
 * storage attached to them if the reference count hits 0.
 */
void
mb_free_ext(struct mbuf *m)
{
	u_int cnt;
	int dofree = 0;

	/*
	 * This is tricky.  We need to make sure to decrement the
	 * refcount in a safe way but to also clean up if we're the
	 * last reference.  This method seems to do it without race.
	 */
	while (dofree == 0) {
		cnt = m->m_ext.ext_buf->ref_cnt;
		if (atomic_cmpset_int(&m->m_ext.ext_buf->ref_cnt, cnt, cnt - 1)) {
			if (cnt == 1)
				dofree = 1;
			break;
		}
	}

	if (dofree) {
		/*
		 * Do the free, should be safe.
		 */
		switch (m->m_ext.ext_type) {
		case EXT_PACKET:
		case EXT_CLUSTER:
			ExFreeToNPagedLookasideList(&zone_clust, m->m_ext.ext_buf);
			break;
		case EXT_JUMBOP:
			ExFreeToNPagedLookasideList(&zone_jumbop, m->m_ext.ext_buf);
			break;
		case EXT_JUMBO9:
			ExFreeToNPagedLookasideList(&zone_jumbo9, m->m_ext.ext_buf);
			break;
		case EXT_JUMBO16:
			ExFreeToNPagedLookasideList(&zone_jumbo16, m->m_ext.ext_buf);
			break;
		default:
			KASSERT(m->m_ext.ext_free != NULL,
			    ("%s: external free pointer not set", __func__));
			(*(m->m_ext.ext_free))(m->m_ext.ext_buf,
			    m->m_ext.ext_args);
		}
		m->m_ext.ext_buf = NULL;
	}
	ExFreeToNPagedLookasideList(&zone_mbuf, m);
}

/*
 * "Move" mbuf pkthdr from "from" to "to".
 * "from" must have M_PKTHDR set, and "to" must be empty.
 */
void
m_move_pkthdr(struct mbuf *to, struct mbuf *from)
{

#if 0
	/* see below for why these are not enabled */
	M_ASSERTPKTHDR(to);
	/* Note: with MAC, this may not be a good assertion. */
	KASSERT(SLIST_EMPTY(&to->m_pkthdr.tags),
	    ("m_move_pkthdr: to has tags"));
#endif
	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;		/* especially tags */
	from->m_flags &= ~M_PKTHDR;
}

/*
 * Duplicate "from"'s mbuf pkthdr in "to".
 * "from" must have M_PKTHDR set, and "to" must be empty.
 * In particular, this does a deep copy of the packet tags.
 */
int
m_dup_pkthdr(struct mbuf *to, struct mbuf *from, int how)
{

#if 0
	/*
	 * The mbuf allocator only initializes the pkthdr
	 * when the mbuf is allocated with MGETHDR. Many users
	 * (e.g. m_copy*, m_prepend) use MGET and then
	 * smash the pkthdr as needed causing these
	 * assertions to trip.  For now just disable them.
	 */
	M_ASSERTPKTHDR(to);
	/* Note: with MAC, this may not be a good assertion. */
	KASSERT(SLIST_EMPTY(&to->m_pkthdr.tags), ("m_dup_pkthdr: to has tags"));
#endif
	MBUF_CHECKSLEEP(how);
	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;
	return 0;
}

/*
 * Lesser-used path for M_PREPEND:
 * allocate new mbuf to prepend to chain,
 * copy junk along.
 */
struct mbuf *
m_prepend(struct mbuf *m, int len, int how)
{
	struct mbuf *mn;

	if (m->m_flags & M_PKTHDR)
		MGETHDR(mn, how, m->m_type);
	else
		MGET(mn, how, m->m_type);
	if (mn == NULL) {
		m_freem(m);
		return (NULL);
	}
	if (m->m_flags & M_PKTHDR)
		M_MOVE_PKTHDR(mn, m);
	mn->m_next = m;
	m = mn;
	if (len < MHLEN)
		MH_ALIGN(m, len);
	m->m_len = len;
	return (m);
}

/*
 * Make a copy of an mbuf chain starting "off0" bytes from the beginning,
 * continuing for "len" bytes.  If len is M_COPYALL, copy to end of mbuf.
 * The wait parameter is a choice of M_TRYWAIT/M_DONTWAIT from caller.
 * Note that the copy is read-only, because clusters are not copied,
 * only their reference counts are incremented.
 */
struct mbuf *
m_copym(struct mbuf *m, int off0, int len, int wait)
{
	struct mbuf *n, **np;
	int off = off0;
	struct mbuf *top;
	int copyhdr = 0;

	KASSERT(off >= 0, ("m_copym, negative off %d", off));
	KASSERT(len >= 0, ("m_copym, negative len %d", len));
	MBUF_CHECKSLEEP(wait);
	if (off == 0 && m->m_flags & M_PKTHDR)
		copyhdr = 1;
	while (off > 0) {
		KASSERT(m != NULL, ("m_copym, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	np = &top;
	top = 0;
	while (len > 0) {
		if (m == NULL) {
			KASSERT(len == M_COPYALL, 
			    ("m_copym, length > size of mbuf chain"));
			break;
		}
		if (copyhdr)
			MGETHDR(n, wait, m->m_type);
		else
			MGET(n, wait, m->m_type);
		*np = n;
		if (n == NULL)
			goto nospace;
		if (copyhdr) {
			if (!m_dup_pkthdr(n, m, wait))
				goto nospace;
			if (len == M_COPYALL)
				n->m_pkthdr.len -= off0;
			else
				n->m_pkthdr.len = len;
			copyhdr = 0;
		}
		n->m_len = min(len, m->m_len - off);
		if (m->m_flags & M_EXT) {
			n->m_data = m->m_data + off;
			n->m_ext = m->m_ext;
			n->m_flags |= M_EXT;
			MEXT_ADD_REF(m);
		} else
			bcopy(mtod(m, caddr_t)+off, mtod(n, caddr_t),
			    (u_int)n->m_len);
		if (len != M_COPYALL)
			len -= n->m_len;
		off = 0;
		m = m->m_next;
		np = &n->m_next;
	}
	if (top == NULL)
		mbstat.m_mcfail++;	/* XXX: No consistency. */

	return (top);
nospace:
	m_freem(top);
	mbstat.m_mcfail++;	/* XXX: No consistency. */
	return (NULL);
}

/*
 * Copy an entire packet, including header (which must be present).
 * An optimization of the common case `m_copym(m, 0, M_COPYALL, how)'.
 * Note that the copy is read-only, because clusters are not copied,
 * only their reference counts are incremented.
 * Preserve alignment of the first mbuf so if the creator has left
 * some room at the beginning (e.g. for inserting protocol headers)
 * the copies still have the room available.
 */
struct mbuf *
m_copypacket(struct mbuf *m, int how)
{
	struct mbuf *top, *n, *o;

	MBUF_CHECKSLEEP(how);
	MGET(n, how, m->m_type);
	top = n;
	if (n == NULL)
		goto nospace;

	if (!m_dup_pkthdr(n, m, how))
		goto nospace;
	n->m_len = m->m_len;
	if (m->m_flags & M_EXT) {
		n->m_data = m->m_data;
		n->m_ext = m->m_ext;
		n->m_flags |= M_EXT;
		MEXT_ADD_REF(m);
	} else {
		n->m_data = n->m_pktdat + (m->m_data - m->m_pktdat );
		bcopy(mtod(m, char *), mtod(n, char *), n->m_len);
	}

	m = m->m_next;
	while (m) {
		MGET(o, how, m->m_type);
		if (o == NULL)
			goto nospace;

		n->m_next = o;
		n = n->m_next;

		n->m_len = m->m_len;
		if (m->m_flags & M_EXT) {
			n->m_data = m->m_data;
			n->m_ext = m->m_ext;
			n->m_flags |= M_EXT;
			MEXT_ADD_REF(m);
		} else {
			bcopy(mtod(m, char *), mtod(n, char *), n->m_len);
		}

		m = m->m_next;
	}
	return top;
nospace:
	m_freem(top);
	mbstat.m_mcfail++;	/* XXX: No consistency. */ 
	return (NULL);
}

/*
 * Copy data from an mbuf chain starting "off" bytes from the beginning,
 * continuing for "len" bytes, into the indicated buffer.
 */
void
m_copydata(const struct mbuf *m, int off, int len, caddr_t cp)
{
	u_int count;

	KASSERT(off >= 0, ("m_copydata, negative off %d", off));
	KASSERT(len >= 0, ("m_copydata, negative len %d", len));
	while (off > 0) {
		KASSERT(m != NULL, ("m_copydata, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		KASSERT(m != NULL, ("m_copydata, length > size of mbuf chain"));
		count = min(m->m_len - off, len);
		bcopy(mtod(m, caddr_t) + off, cp, count);
		len -= count;
		cp += count;
		off = 0;
		m = m->m_next;
	}
}

/*
 * Copy a packet header mbuf chain into a completely new chain, including
 * copying any mbuf clusters.  Use this instead of m_copypacket() when
 * you need a writable copy of an mbuf chain.
 */
struct mbuf *
m_dup(struct mbuf *m, int how)
{
	struct mbuf **p, *top = NULL;
	int remain, moff, nsize;

	MBUF_CHECKSLEEP(how);
	/* Sanity check */
	if (m == NULL)
		return (NULL);
	M_ASSERTPKTHDR(m);

	/* While there's more data, get a new mbuf, tack it on, and fill it */
	remain = m->m_pkthdr.len;
	moff = 0;
	p = &top;
	while (remain > 0 || top == NULL) {	/* allow m->m_pkthdr.len == 0 */
		struct mbuf *n;

		/* Get the next new mbuf */
		if (remain >= MINCLSIZE) {
			n = m_getcl(how, m->m_type, 0);
			nsize = MCLBYTES;
		} else {
			n = m_get(how, m->m_type);
			nsize = MLEN;
		}
		if (n == NULL)
			goto nospace;

		if (top == NULL) {		/* First one, must be PKTHDR */
			if (!m_dup_pkthdr(n, m, how)) {
				m_free(n);
				goto nospace;
			}
			if ((n->m_flags & M_EXT) == 0)
				nsize = MHLEN;
		}
		n->m_len = 0;

		/* Link it into the new chain */
		*p = n;
		p = &n->m_next;

		/* Copy data from original mbuf(s) into new mbuf */
		while (n->m_len < nsize && m != NULL) {
			int chunk = min(nsize - n->m_len, m->m_len - moff);

			bcopy(m->m_data + moff, n->m_data + n->m_len, chunk);
			moff += chunk;
			n->m_len += chunk;
			remain -= chunk;
			if (moff == m->m_len) {
				m = m->m_next;
				moff = 0;
			}
		}

		/* Check correct total mbuf length */
		KASSERT((remain > 0 && m != NULL) || (remain == 0 && m == NULL),
		    	("%s: bogus m_pkthdr.len", __func__));
	}
	return (top);

nospace:
	m_freem(top);
	mbstat.m_mcfail++;	/* XXX: No consistency. */
	return (NULL);
}

/*
 * Concatenate mbuf chain n to m.
 * Both chains must be of the same type (e.g. MT_DATA).
 * Any m_pkthdr is not updated.
 */
void
m_cat(struct mbuf *m, struct mbuf *n)
{
	while (m->m_next)
		m = m->m_next;
	while (n) {
		if (m->m_flags & M_EXT ||
		    m->m_data + m->m_len + n->m_len >= &m->m_dat[MLEN]) {
			/* just join the two chains */
			m->m_next = n;
			return;
		}
		/* splat the data from one into the other */
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		    (u_int)n->m_len);
		m->m_len += n->m_len;
		n = m_free(n);
	}
}

void
m_adj(struct mbuf *mp, int req_len)
{
	int len = req_len;
	struct mbuf *m;
	int count;

	if ((m = mp) == NULL)
		return;
	if (len >= 0) {
		/*
		 * Trim from head.
		 */
		while (m != NULL && len > 0) {
			if (m->m_len <= len) {
				len -= m->m_len;
				m->m_len = 0;
				m = m->m_next;
			} else {
				m->m_len -= len;
				m->m_data += len;
				len = 0;
			}
		}
		m = mp;
		if (mp->m_flags & M_PKTHDR)
			m->m_pkthdr.len -= (req_len - len);
	} else {
		/*
		 * Trim from tail.  Scan the mbuf chain,
		 * calculating its length and finding the last mbuf.
		 * If the adjustment only affects this mbuf, then just
		 * adjust and return.  Otherwise, rescan and truncate
		 * after the remaining size.
		 */
		len = -len;
		count = 0;
		for (;;) {
			count += m->m_len;
			if (m->m_next == (struct mbuf *)0)
				break;
			m = m->m_next;
		}
		if (m->m_len >= len) {
			m->m_len -= len;
			if (mp->m_flags & M_PKTHDR)
				mp->m_pkthdr.len -= len;
			return;
		}
		count -= len;
		if (count < 0)
			count = 0;
		/*
		 * Correct length for chain is "count".
		 * Find the mbuf with last data, adjust its length,
		 * and toss data from remaining mbufs on chain.
		 */
		m = mp;
		if (m->m_flags & M_PKTHDR)
			m->m_pkthdr.len = count;
		for (; m; m = m->m_next) {
			if (m->m_len >= count) {
				m->m_len = count;
				if (m->m_next != NULL) {
					m_freem(m->m_next);
					m->m_next = NULL;
				}
				break;
			}
			count -= m->m_len;
		}
	}
}

/*
 * Rearange an mbuf chain so that len bytes are contiguous
 * and in the data area of an mbuf (so that mtod and dtom
 * will work for a structure of size len).  Returns the resulting
 * mbuf chain on success, frees it and returns null on failure.
 * If there is room, it will add up to max_protohdr-len extra bytes to the
 * contiguous region in an attempt to avoid being called next time.
 */
struct mbuf *
m_pullup(struct mbuf *n, int len)
{
	struct mbuf *m;
	int count;
	int space;

	/*
	 * If first mbuf has no cluster, and has room for len bytes
	 * without shifting current data, pullup into it,
	 * otherwise allocate a new mbuf to prepend to the chain.
	 */
	if ((n->m_flags & M_EXT) == 0 &&
	    n->m_data + len < &n->m_dat[MLEN] && n->m_next) {
		if (n->m_len >= len)
			return (n);
		m = n;
		n = n->m_next;
		len -= m->m_len;
	} else {
		if (len > MHLEN)
			goto bad;
		MGET(m, M_DONTWAIT, n->m_type);
		if (m == NULL)
			goto bad;
		m->m_len = 0;
		if (n->m_flags & M_PKTHDR)
			M_MOVE_PKTHDR(m, n);
	}
	space = &m->m_dat[MLEN] - (m->m_data + m->m_len);
	do {
		count = min(min(max(len, max_protohdr), space), n->m_len);
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		  (u_int)count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data += count;
		else
			n = m_free(n);
	} while (len > 0 && n);
	if (len > 0) {
		(void) m_free(m);
		goto bad;
	}
	m->m_next = n;
	return (m);
bad:
	m_freem(n);
	mbstat.m_mpfail++;	/* XXX: No consistency. */
	return (NULL);
}

/*
 * Like m_pullup(), except a new mbuf is always allocated, and we allow
 * the amount of empty space before the data in the new mbuf to be specified
 * (in the event that the caller expects to prepend later).
 */
int MSFail;

struct mbuf *
m_copyup(struct mbuf *n, int len, int dstoff)
{
	struct mbuf *m;
	int count, space;

	if (len > (int)(MHLEN - dstoff))
		goto bad;
	MGET(m, M_DONTWAIT, n->m_type);
	if (m == NULL)
		goto bad;
	m->m_len = 0;
	if (n->m_flags & M_PKTHDR)
		M_MOVE_PKTHDR(m, n);
	m->m_data += dstoff;
	space = &m->m_dat[MLEN] - (m->m_data + m->m_len);
	do {
		count = min(min(max(len, max_protohdr), space), n->m_len);
		memcpy(mtod(m, caddr_t) + m->m_len, mtod(n, caddr_t),
		    (unsigned)count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data += count;
		else
			n = m_free(n);
	} while (len > 0 && n);
	if (len > 0) {
		(void) m_free(m);
		goto bad;
	}
	m->m_next = n;
	return (m);
 bad:
	m_freem(n);
	MSFail++;
	return (NULL);
}

/*
 * Partition an mbuf chain in two pieces, returning the tail --
 * all but the first len0 bytes.  In case of failure, it returns NULL and
 * attempts to restore the chain to its original state.
 *
 * Note that the resulting mbufs might be read-only, because the new
 * mbuf can end up sharing an mbuf cluster with the original mbuf if
 * the "breaking point" happens to lie within a cluster mbuf. Use the
 * M_WRITABLE() macro to check for this case.
 */
struct mbuf *
m_split(struct mbuf *m0, int len0, int wait)
{
	struct mbuf *m, *n;
	u_int len = len0, remain;

	MBUF_CHECKSLEEP(wait);
	for (m = m0; m && (int)len > m->m_len; m = m->m_next)
		len -= m->m_len;
	if (m == NULL)
		return (NULL);
	remain = m->m_len - len;
	if (m0->m_flags & M_PKTHDR) {
		MGETHDR(n, wait, m0->m_type);
		if (n == NULL)
			return (NULL);
		n->m_pkthdr.rcvif = m0->m_pkthdr.rcvif;
		n->m_pkthdr.len = m0->m_pkthdr.len - len0;
		m0->m_pkthdr.len = len0;
		if (m->m_flags & M_EXT)
			goto extpacket;
		if (remain > MHLEN) {
			/* m can't be the lead packet */
			MH_ALIGN(n, 0);
			n->m_next = m_split(m, len, wait);
			if (n->m_next == NULL) {
				(void) m_free(n);
				return (NULL);
			} else {
				n->m_len = 0;
				return (n);
			}
		} else
			MH_ALIGN(n, remain);
	} else if (remain == 0) {
		n = m->m_next;
		m->m_next = NULL;
		return (n);
	} else {
		MGET(n, wait, m->m_type);
		if (n == NULL)
			return (NULL);
		M_ALIGN(n, remain);
	}
extpacket:
	if (m->m_flags & M_EXT) {
		n->m_flags |= M_EXT;
		n->m_ext = m->m_ext;
		MEXT_ADD_REF(m);
		n->m_data = m->m_data + len;
	} else {
		bcopy(mtod(m, caddr_t) + len, mtod(n, caddr_t), remain);
	}
	n->m_len = remain;
	m->m_len = len;
	n->m_next = m->m_next;
	m->m_next = NULL;
	return (n);
}
/*
 * Routine to copy from device local memory into mbufs.
 * Note that `off' argument is offset into first mbuf of target chain from
 * which to begin copying the data to.
 */
struct mbuf *
m_devget(char *buf, int totlen, int off, struct ifnet *ifp,
	 void (*copy)(char *from, caddr_t to, u_int len))
{
	struct mbuf *m;
	struct mbuf *top = NULL, **mp = &top;
	int len;

	if (off < 0 || off > MHLEN)
		return (NULL);

	while (totlen > 0) {
		if (top == NULL) {	/* First one, must be PKTHDR */
			if (totlen + off >= MINCLSIZE) {
				m = m_getcl(M_DONTWAIT, MT_DATA, M_PKTHDR);
				len = MCLBYTES;
			} else {
				m = m_gethdr(M_DONTWAIT, MT_DATA);
				len = MHLEN;

				/* Place initial small packet/header at end of mbuf */
				if (m && totlen + off + max_linkhdr <= MLEN) {
					m->m_data += max_linkhdr;
					len -= max_linkhdr;
				}
			}
			if (m == NULL)
				return NULL;
			m->m_pkthdr.rcvif = ifp;
			m->m_pkthdr.len = totlen;
		} else {
			if (totlen + off >= MINCLSIZE) {
				m = m_getcl(M_DONTWAIT, MT_DATA, 0);
				len = MCLBYTES;
			} else {
				m = m_get(M_DONTWAIT, MT_DATA);
				len = MLEN;
			}
			if (m == NULL) {
				m_freem(top);
				return NULL;
			}
		}
		if (off) {
			m->m_data += off;
			len -= off;
			off = 0;
		}
		m->m_len = len = min(totlen, len);
		if (copy)
			copy(buf, mtod(m, caddr_t), (u_int)len);
		else
			bcopy(buf, mtod(m, caddr_t), (u_int)len);
		buf += len;
		*mp = m;
		mp = &m->m_next;
		totlen -= len;
	}
	return (top);
}

/*
 * Copy data from a buffer back into the indicated mbuf chain,
 * starting "off" bytes from the beginning, extending the mbuf
 * chain if necessary.
 */
void
m_copyback(struct mbuf *m0, int off, int len, c_caddr_t cp)
{
	int mlen;
	struct mbuf *m = m0, *n;
	int totlen = 0;

	if (m0 == NULL)
		return;
	while (off > (mlen = m->m_len)) {
		off -= mlen;
		totlen += mlen;
		if (m->m_next == NULL) {
			n = m_get(M_DONTWAIT, m->m_type);
			if (n == NULL)
				goto out;
			bzero(mtod(n, caddr_t), MLEN);
			n->m_len = min(MLEN, len + off);
			m->m_next = n;
		}
		m = m->m_next;
	}
	while (len > 0) {
		mlen = min (m->m_len - off, len);
		bcopy(cp, off + mtod(m, caddr_t), (u_int)mlen);
		cp += mlen;
		len -= mlen;
		mlen += off;
		off = 0;
		totlen += mlen;
		if (len == 0)
			break;
		if (m->m_next == NULL) {
			n = m_get(M_DONTWAIT, m->m_type);
			if (n == NULL)
				break;
			n->m_len = min(MLEN, len);
			m->m_next = n;
		}
		m = m->m_next;
	}
out:	if (((m = m0)->m_flags & M_PKTHDR) && (m->m_pkthdr.len < totlen))
		m->m_pkthdr.len = totlen;
}

/*
 * Append the specified data to the indicated mbuf chain,
 * Extend the mbuf chain if the new data does not fit in
 * existing space.
 *
 * Return 1 if able to complete the job; otherwise 0.
 */
int
m_append(struct mbuf *m0, int len, c_caddr_t cp)
{
	struct mbuf *m, *n;
	int remainder, space;

	for (m = m0; m->m_next != NULL; m = m->m_next)
		;
	remainder = len;
	space = M_TRAILINGSPACE(m);
	if (space > 0) {
		/*
		 * Copy into available space.
		 */
		if (space > remainder)
			space = remainder;
		bcopy(cp, mtod(m, caddr_t) + m->m_len, space);
		m->m_len += space;
		cp += space, remainder -= space;
	}
	while (remainder > 0) {
		/*
		 * Allocate a new mbuf; could check space
		 * and allocate a cluster instead.
		 */
		n = m_get(M_DONTWAIT, m->m_type);
		if (n == NULL)
			break;
		n->m_len = min(MLEN, remainder);
		bcopy(cp, mtod(n, caddr_t), n->m_len);
		cp += n->m_len, remainder -= n->m_len;
		m->m_next = n;
		m = n;
	}
	if (m0->m_flags & M_PKTHDR)
		m0->m_pkthdr.len += len - remainder;
	return (remainder == 0);
}

/*
 * Apply function f to the data in an mbuf chain starting "off" bytes from
 * the beginning, continuing for "len" bytes.
 */
int
m_apply(struct mbuf *m, int off, int len,
    int (*f)(void *, void *, u_int), void *arg)
{
	u_int count;
	int rval;

	KASSERT(off >= 0, ("m_apply, negative off %d", off));
	KASSERT(len >= 0, ("m_apply, negative len %d", len));
	while (off > 0) {
		KASSERT(m != NULL, ("m_apply, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		KASSERT(m != NULL, ("m_apply, offset > size of mbuf chain"));
		count = min(m->m_len - off, len);
		rval = (*f)(arg, mtod(m, caddr_t) + off, count);
		if (rval)
			return (rval);
		len -= count;
		off = 0;
		m = m->m_next;
	}
	return (0);
}

/*
 * Return a pointer to mbuf/offset of location in mbuf chain.
 */
struct mbuf *
m_getptr(struct mbuf *m, int loc, int *off)
{

	while (loc >= 0) {
		/* Normal end of search. */
		if (m->m_len > loc) {
			*off = loc;
			return (m);
		} else {
			loc -= m->m_len;
			if (m->m_next == NULL) {
				if (loc == 0) {
					/* Point at the end of valid data. */
					*off = m->m_len;
					return (m);
				}
				return (NULL);
			}
			m = m->m_next;
		}
	}
	return (NULL);
}

void
m_print(const struct mbuf *m, int maxlen)
{
	int len;
	int pdata;
	const struct mbuf *m2;

	if (m->m_flags & M_PKTHDR)
		len = m->m_pkthdr.len;
	else
		len = -1;
	m2 = m;
	while (m2 != NULL && (len == -1 || len)) {
		pdata = m2->m_len;
		if (maxlen != -1 && pdata > maxlen)
			pdata = maxlen;
		printf("mbuf: %p len: %d, next: %p, %b%s", m2, m2->m_len,
		    m2->m_next, m2->m_flags, "\20\20freelist\17skipfw"
		    "\11proto5\10proto4\7proto3\6proto2\5proto1\4rdonly"
		    "\3eor\2pkthdr\1ext", pdata ? "" : "\n");
		if (pdata)
			printf(", %*D\n", m2->m_len, (u_char *)m2->m_data, "-");
		if (len != -1)
			len -= m2->m_len;
		m2 = m2->m_next;
	}
	if (len > 0)
		printf("%d bytes unaccounted for.\n", len);
	return;
}

u_int
m_fixhdr(struct mbuf *m0)
{
	u_int len;

	len = m_length(m0, NULL);
	m0->m_pkthdr.len = len;
	return (len);
}

u_int
m_length(struct mbuf *m0, struct mbuf **last)
{
	struct mbuf *m;
	u_int len;

	len = 0;
	for (m = m0; m != NULL; m = m->m_next) {
		len += m->m_len;
		if (m->m_next == NULL)
			break;
	}
	if (last != NULL)
		*last = m;
	return (len);
}

/*
 * Defragment a mbuf chain, returning the shortest possible
 * chain of mbufs and clusters.  If allocation fails and
 * this cannot be completed, NULL will be returned, but
 * the passed in chain will be unchanged.  Upon success,
 * the original chain will be freed, and the new chain
 * will be returned.
 *
 * If a non-packet header is passed in, the original
 * mbuf (chain?) will be returned unharmed.
 */
struct mbuf *
m_defrag(struct mbuf *m0, int how)
{
	struct mbuf *m_new = NULL, *m_final = NULL;
	int progress = 0, length;

	MBUF_CHECKSLEEP(how);
	if (!(m0->m_flags & M_PKTHDR))
		return (m0);

	m_fixhdr(m0); /* Needed sanity check */

#ifdef MBUF_STRESS_TEST
	if (m_defragrandomfailures) {
		int temp = arc4random() & 0xff;
		if (temp == 0xba)
			goto nospace;
	}
#endif
	
	if (m0->m_pkthdr.len > MHLEN)
		m_final = m_getcl(how, MT_DATA, M_PKTHDR);
	else
		m_final = m_gethdr(how, MT_DATA);

	if (m_final == NULL)
		goto nospace;

	if (m_dup_pkthdr(m_final, m0, how) == 0)
		goto nospace;

	m_new = m_final;

	while (progress < m0->m_pkthdr.len) {
		length = m0->m_pkthdr.len - progress;
		if (length > MCLBYTES)
			length = MCLBYTES;

		if (m_new == NULL) {
			if (length > MLEN)
				m_new = m_getcl(how, MT_DATA, 0);
			else
				m_new = m_get(how, MT_DATA);
			if (m_new == NULL)
				goto nospace;
		}

		m_copydata(m0, progress, length, mtod(m_new, caddr_t));
		progress += length;
		m_new->m_len = length;
		if (m_new != m_final)
			m_cat(m_final, m_new);
		m_new = NULL;
	}
#ifdef MBUF_STRESS_TEST
	if (m0->m_next == NULL)
		m_defraguseless++;
#endif
	m_freem(m0);
	m0 = m_final;
#ifdef MBUF_STRESS_TEST
	m_defragpackets++;
	m_defragbytes += m0->m_pkthdr.len;
#endif
	return (m0);
nospace:
#ifdef MBUF_STRESS_TEST
	m_defragfailure++;
#endif
	if (m_final)
		m_freem(m_final);
	return (NULL);
}

#ifdef MBUF_STRESS_TEST

/*
 * Fragment an mbuf chain.  There's no reason you'd ever want to do
 * this in normal usage, but it's great for stress testing various
 * mbuf consumers.
 *
 * If fragmentation is not possible, the original chain will be
 * returned.
 *
 * Possible length values:
 * 0	 no fragmentation will occur
 * > 0	each fragment will be of the specified length
 * -1	each fragment will be the same random value in length
 * -2	each fragment's length will be entirely random
 * (Random values range from 1 to 256)
 */
struct mbuf *
m_fragment(struct mbuf *m0, int how, int length)
{
	struct mbuf *m_new = NULL, *m_final = NULL;
	int progress = 0;

	if (!(m0->m_flags & M_PKTHDR))
		return (m0);
	
	if ((length == 0) || (length < -2))
		return (m0);

	m_fixhdr(m0); /* Needed sanity check */

	m_final = m_getcl(how, MT_DATA, M_PKTHDR);

	if (m_final == NULL)
		goto nospace;

	if (m_dup_pkthdr(m_final, m0, how) == 0)
		goto nospace;

	m_new = m_final;

	if (length == -1)
		length = 1 + (arc4random() & 255);

	while (progress < m0->m_pkthdr.len) {
		int fraglen;

		if (length > 0)
			fraglen = length;
		else
			fraglen = 1 + (arc4random() & 255);
		if (fraglen > m0->m_pkthdr.len - progress)
			fraglen = m0->m_pkthdr.len - progress;

		if (fraglen > MCLBYTES)
			fraglen = MCLBYTES;

		if (m_new == NULL) {
			m_new = m_getcl(how, MT_DATA, 0);
			if (m_new == NULL)
				goto nospace;
		}

		m_copydata(m0, progress, fraglen, mtod(m_new, caddr_t));
		progress += fraglen;
		m_new->m_len = fraglen;
		if (m_new != m_final)
			m_cat(m_final, m_new);
		m_new = NULL;
	}
	m_freem(m0);
	m0 = m_final;
	return (m0);
nospace:
	if (m_final)
		m_freem(m_final);
	/* Return the original chain on failure */
	return (m0);
}

#endif

#if 0
struct mbuf *
m_uiotombuf(struct uio *uio, int how, int len, int align)
{
	struct mbuf *m_new = NULL, *m_final = NULL;
	int progress = 0, error = 0, length, total;

	if (len > 0)
		total = min(uio->uio_resid, len);
	else
		total = uio->uio_resid;
	if (align >= MHLEN)
		goto nospace;
	if (total + align > MHLEN)
		m_final = m_getcl(how, MT_DATA, M_PKTHDR);
	else
		m_final = m_gethdr(how, MT_DATA);
	if (m_final == NULL)
		goto nospace;
	m_final->m_data += align;
	m_new = m_final;
	while (progress < total) {
		length = total - progress;
		if (length > MCLBYTES)
			length = MCLBYTES;
		if (m_new == NULL) {
			if (length > MLEN)
				m_new = m_getcl(how, MT_DATA, 0);
			else
				m_new = m_get(how, MT_DATA);
			if (m_new == NULL)
				goto nospace;
		}
		error = uiomove(mtod(m_new, void *), length, uio);
		if (error)
			goto nospace;
		progress += length;
		m_new->m_len = length;
		if (m_new != m_final)
			m_cat(m_final, m_new);
		m_new = NULL;
	}
	m_fixhdr(m_final);
	return (m_final);
nospace:
	if (m_new)
		m_free(m_new);
	if (m_final)
		m_freem(m_final);
	return (NULL);
}
#endif

/*
 * Set the m_data pointer of a newly-allocated mbuf
 * to place an object of the specified size at the
 * end of the mbuf, longword aligned.
 */
void
m_align(struct mbuf *m, int len)
{
	int adjust;

	if (m->m_flags & M_EXT)
		adjust = m->m_ext.ext_size - len;
	else if (m->m_flags & M_PKTHDR)
		adjust = MHLEN - len;
	else
		adjust = MLEN - len;
	m->m_data += adjust &~ (sizeof(long)-1);
}

/*
 * Create a writable copy of the mbuf chain.  While doing this
 * we compact the chain with a goal of producing a chain with
 * at most two mbufs.  The second mbuf in this chain is likely
 * to be a cluster.  The primary purpose of this work is to create
 * a writable packet for encryption, compression, etc.  The
 * secondary goal is to linearize the data so the data can be
 * passed to crypto hardware in the most efficient manner possible.
 */
struct mbuf *
m_unshare(struct mbuf *m0, int how)
{
	struct mbuf *m, *mprev;
	struct mbuf *n, *mfirst, *mlast;
	int len, off;

	mprev = NULL;
	for (m = m0; m != NULL; m = mprev->m_next) {
		/*
		 * Regular mbufs are ignored unless there's a cluster
		 * in front of it that we can use to coalesce.  We do
		 * the latter mainly so later clusters can be coalesced
		 * also w/o having to handle them specially (i.e. convert
		 * mbuf+cluster -> cluster).  This optimization is heavily
		 * influenced by the assumption that we're running over
		 * Ethernet where MCLBYTES is large enough that the max
		 * packet size will permit lots of coalescing into a
		 * single cluster.  This in turn permits efficient
		 * crypto operations, especially when using hardware.
		 */
		if ((m->m_flags & M_EXT) == 0) {
			if (mprev && (mprev->m_flags & M_EXT) &&
			    m->m_len <= M_TRAILINGSPACE(mprev)) {
				/* XXX: this ignores mbuf types */
				memcpy(mtod(mprev, caddr_t) + mprev->m_len,
				       mtod(m, caddr_t), m->m_len);
				mprev->m_len += m->m_len;
				mprev->m_next = m->m_next;	/* unlink from chain */
				m_free(m);			/* reclaim mbuf */
#if 0
				newipsecstat.ips_mbcoalesced++;
#endif
			} else {
				mprev = m;
			}
			continue;
		}
		/*
		 * Writable mbufs are left alone (for now).
		 */
		if (M_WRITABLE(m)) {
			mprev = m;
			continue;
		}

		/*
		 * Not writable, replace with a copy or coalesce with
		 * the previous mbuf if possible (since we have to copy
		 * it anyway, we try to reduce the number of mbufs and
		 * clusters so that future work is easier).
		 */
		KASSERT(m->m_flags & M_EXT, ("m_flags 0x%x", m->m_flags));
		/* NB: we only coalesce into a cluster or larger */
		if (mprev != NULL && (mprev->m_flags & M_EXT) &&
		    m->m_len <= M_TRAILINGSPACE(mprev)) {
			/* XXX: this ignores mbuf types */
			memcpy(mtod(mprev, caddr_t) + mprev->m_len,
			       mtod(m, caddr_t), m->m_len);
			mprev->m_len += m->m_len;
			mprev->m_next = m->m_next;	/* unlink from chain */
			m_free(m);			/* reclaim mbuf */
#if 0
			newipsecstat.ips_clcoalesced++;
#endif
			continue;
		}

		/*
		 * Allocate new space to hold the copy...
		 */
		/* XXX why can M_PKTHDR be set past the first mbuf? */
		if (mprev == NULL && (m->m_flags & M_PKTHDR)) {
			/*
			 * NB: if a packet header is present we must
			 * allocate the mbuf separately from any cluster
			 * because M_MOVE_PKTHDR will smash the data
			 * pointer and drop the M_EXT marker.
			 */
			MGETHDR(n, how, m->m_type);
			if (n == NULL) {
				m_freem(m0);
				return (NULL);
			}
			M_MOVE_PKTHDR(n, m);
			MCLGET(n, how);
			if ((n->m_flags & M_EXT) == 0) {
				m_free(n);
				m_freem(m0);
				return (NULL);
			}
		} else {
			n = m_getcl(how, m->m_type, m->m_flags);
			if (n == NULL) {
				m_freem(m0);
				return (NULL);
			}
		}
		/*
		 * ... and copy the data.  We deal with jumbo mbufs
		 * (i.e. m_len > MCLBYTES) by splitting them into
		 * clusters.  We could just malloc a buffer and make
		 * it external but too many device drivers don't know
		 * how to break up the non-contiguous memory when
		 * doing DMA.
		 */
		len = m->m_len;
		off = 0;
		mfirst = n;
		mlast = NULL;
		for (;;) {
			int cc = min(len, MCLBYTES);
			memcpy(mtod(n, caddr_t), mtod(m, caddr_t) + off, cc);
			n->m_len = cc;
			if (mlast != NULL)
				mlast->m_next = n;
			mlast = n;	
#if 0
			newipsecstat.ips_clcopied++;
#endif

			len -= cc;
			if (len <= 0)
				break;
			off += cc;

			n = m_getcl(how, m->m_type, m->m_flags);
			if (n == NULL) {
				m_freem(mfirst);
				m_freem(m0);
				return (NULL);
			}
		}
		n->m_next = m->m_next; 
		if (mprev == NULL)
			m0 = mfirst;		/* new head of chain */
		else
			mprev->m_next = mfirst;	/* replace old mbuf */
		m_free(m);			/* release old mbuf */
		mprev = mfirst;
	}
	return (m0);
}
