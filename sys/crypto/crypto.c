/*	$OpenBSD: crypto.c,v 1.39 2002/07/16 06:12:46 angelos Exp $	*/
/*
 * The author of this code is Angelos D. Keromytis (angelos@cis.upenn.edu)
 *
 * This code was written by Angelos D. Keromytis in Athens, Greece, in
 * February 2000. Network Security Technologies Inc. (NSTI) kindly
 * supported the development of this code.
 *
 * Copyright (c) 2000, 2001 Angelos D. Keromytis
 *
 * Permission to use, copy, and modify this software with or without fee
 * is hereby granted, provided that this entire notice is included in
 * all source code copies of any software which is or includes a copy or
 * modification of this software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTY. IN PARTICULAR, NONE OF THE AUTHORS MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE
 * MERCHANTABILITY OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR
 * PURPOSE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/pool.h>
#include <crypto/cryptodev.h>

struct cryptocap *crypto_drivers = NULL;
int crypto_drivers_num = 0;

struct pool cryptop_pool;
struct pool cryptodesc_pool;
int crypto_pool_initialized = 0;

struct cryptop *crp_req_queue = NULL;
struct cryptop **crp_req_queue_tail = NULL;

struct cryptkop *krp_req_queue = NULL;
struct cryptkop **krp_req_queue_tail = NULL;

/*
 * Create a new session.
 */
int
crypto_newsession(u_int64_t *sid, struct cryptoini *cri, int hard)
{
	struct cryptoini *cr;
	u_int32_t hid, lid;
	int err, s;

	if (crypto_drivers == NULL)
		return EINVAL;

	s = splimp();

	/*
	 * The algorithm we use here is pretty stupid; just use the
	 * first driver that supports all the algorithms we need.
	 *
	 * XXX We need more smarts here (in real life too, but that's
	 * XXX another story altogether).
	 */

	for (hid = 0; hid < crypto_drivers_num; hid++) {
		/*
		 * If it's not initialized or has remaining sessions
		 * referencing it, skip.
		 */
		if (crypto_drivers[hid].cc_newsession == NULL ||
		    (crypto_drivers[hid].cc_flags & CRYPTOCAP_F_CLEANUP))
			continue;

		/* Hardware requested -- ignore software drivers. */
		if (hard &&
		    (crypto_drivers[hid].cc_flags & CRYPTOCAP_F_SOFTWARE))
			continue;

		/* See if all the algorithms are supported. */
		for (cr = cri; cr; cr = cr->cri_next)
			if (crypto_drivers[hid].cc_alg[cr->cri_alg] == 0)
				break;

		/* Ok, all algorithms are supported. */
		if (cr == NULL)
			break;
	}

	/*
	 * Can't do everything in one session.
	 *
	 * XXX Fix this. We need to inject a "virtual" session layer right
	 * XXX about here.
	 */

	if (hid == crypto_drivers_num) {
		splx(s);
		return EINVAL;
	}

	/* Call the driver initialization routine. */
	lid = hid; /* Pass the driver ID. */
	err = crypto_drivers[hid].cc_newsession(&lid, cri);
	if (err == 0) {
		(*sid) = hid;
		(*sid) <<= 32;
		(*sid) |= (lid & 0xffffffff);
		crypto_drivers[hid].cc_sessions++;
	}

	splx(s);
	return err;
}

/*
 * Delete an existing session (or a reserved session on an unregistered
 * driver).
 */
int
crypto_freesession(u_int64_t sid)
{
	int err = 0, s;
	u_int32_t hid;

	if (crypto_drivers == NULL)
		return EINVAL;

	/* Determine two IDs. */
	hid = (sid >> 32) & 0xffffffff;

	if (hid >= crypto_drivers_num)
		return ENOENT;

	s = splimp();

	if (crypto_drivers[hid].cc_sessions)
		crypto_drivers[hid].cc_sessions--;

	/* Call the driver cleanup routine, if available. */
	if (crypto_drivers[hid].cc_freesession)
		err = crypto_drivers[hid].cc_freesession(sid);

	/*
	 * If this was the last session of a driver marked as invalid,
	 * make the entry available for reuse.
	 */
	if ((crypto_drivers[hid].cc_flags & CRYPTOCAP_F_CLEANUP) &&
	    crypto_drivers[hid].cc_sessions == 0)
		bzero(&crypto_drivers[hid], sizeof(struct cryptocap));

	splx(s);
	return err;
}

/*
 * Find an empty slot.
 */
int32_t
crypto_get_driverid(u_int8_t flags)
{
	struct cryptocap *newdrv;
	int i, s = splimp();

	if (crypto_drivers_num == 0) {
		crypto_drivers_num = CRYPTO_DRIVERS_INITIAL;
		crypto_drivers = malloc(crypto_drivers_num *
		    sizeof(struct cryptocap), M_CRYPTO_DATA, M_NOWAIT);
		if (crypto_drivers == NULL) {
			splx(s);
			crypto_drivers_num = 0;
			return -1;
		}

		bzero(crypto_drivers, crypto_drivers_num *
		    sizeof(struct cryptocap));
	}

	for (i = 0; i < crypto_drivers_num; i++) {
		if (crypto_drivers[i].cc_process == NULL &&
		    !(crypto_drivers[i].cc_flags & CRYPTOCAP_F_CLEANUP) &&
		    crypto_drivers[i].cc_sessions == 0) {
			crypto_drivers[i].cc_sessions = 1; /* Mark */
			crypto_drivers[i].cc_flags = flags;
			splx(s);
			return i;
		}
	}

	/* Out of entries, allocate some more. */
	if (i == crypto_drivers_num) {
		/* Be careful about wrap-around. */
		if (2 * crypto_drivers_num <= crypto_drivers_num) {
			splx(s);
			return -1;
		}

		newdrv = malloc(2 * crypto_drivers_num *
		    sizeof(struct cryptocap), M_CRYPTO_DATA, M_NOWAIT);
		if (newdrv == NULL) {
			splx(s);
			return -1;
		}

		bcopy(crypto_drivers, newdrv,
		    crypto_drivers_num * sizeof(struct cryptocap));
		bzero(&newdrv[crypto_drivers_num],
		    crypto_drivers_num * sizeof(struct cryptocap));

		newdrv[i].cc_sessions = 1; /* Mark */
		newdrv[i].cc_flags = flags;
		crypto_drivers_num *= 2;

		free(crypto_drivers, M_CRYPTO_DATA);
		crypto_drivers = newdrv;
		splx(s);
		return i;
	}

	/* Shouldn't really get here... */
	splx(s);
	return -1;
}

/*
 * Register a crypto driver. It should be called once for each algorithm
 * supported by the driver.
 */
int
crypto_kregister(u_int32_t driverid, int kalg, u_int32_t flags,
    int (*kprocess)(struct cryptkop *))
{
	int s;

	if (driverid >= crypto_drivers_num || kalg < 0 ||
	    kalg > CRK_ALGORITHM_MAX || crypto_drivers == NULL)
		return EINVAL;

	s = splimp();

	/*
	 * XXX Do some performance testing to determine placing.
	 * XXX We probably need an auxiliary data structure that describes
	 * XXX relative performances.
	 */

	crypto_drivers[driverid].cc_kalg[kalg] =
	    flags | CRYPTO_ALG_FLAG_SUPPORTED;

	if (crypto_drivers[driverid].cc_kprocess == NULL)
		crypto_drivers[driverid].cc_kprocess = kprocess;

	splx(s);
	return 0;
}

/*
 * Register a crypto driver. It should be called once for each algorithm
 * supported by the driver.
 */
int
crypto_register(u_int32_t driverid, int alg, u_int16_t maxoplen,
    u_int32_t flags,
    int (*newses)(u_int32_t *, struct cryptoini *),
    int (*freeses)(u_int64_t), int (*process)(struct cryptop *))
{
	int s;

	if (driverid >= crypto_drivers_num || alg <= 0 ||
	    alg > CRYPTO_ALGORITHM_MAX || crypto_drivers == NULL)
		return EINVAL;

	s = splimp();

	/*
	 * XXX Do some performance testing to determine placing.
	 * XXX We probably need an auxiliary data structure that describes
	 * XXX relative performances.
	 */

	crypto_drivers[driverid].cc_alg[alg] =
	    flags | CRYPTO_ALG_FLAG_SUPPORTED;

	crypto_drivers[driverid].cc_max_op_len[alg] = maxoplen;

	if (crypto_drivers[driverid].cc_process == NULL) {
		crypto_drivers[driverid].cc_newsession = newses;
		crypto_drivers[driverid].cc_process = process;
		crypto_drivers[driverid].cc_freesession = freeses;
		crypto_drivers[driverid].cc_sessions = 0; /* Unmark */
	}

	splx(s);
	return 0;
}

/*
 * Unregister a crypto driver. If there are pending sessions using it,
 * leave enough information around so that subsequent calls using those
 * sessions will correctly detect the driver being unregistered and reroute
 * the request.
 */
int
crypto_unregister(u_int32_t driverid, int alg)
{
	int i = CRYPTO_ALGORITHM_MAX + 1, s = splimp();
	u_int32_t ses;

	/* Sanity checks */
	if (driverid >= crypto_drivers_num || crypto_drivers == NULL ||
	    ((alg <= 0 || alg > CRYPTO_ALGORITHM_MAX) &&
		alg != CRYPTO_ALGORITHM_ALL) ||
	    crypto_drivers[driverid].cc_alg[alg] == 0) {
		splx(s);
		return EINVAL;
	}

	if (alg != CRYPTO_ALGORITHM_ALL) {
		crypto_drivers[driverid].cc_alg[alg] = 0;
		crypto_drivers[driverid].cc_max_op_len[alg] = 0;

		/* Was this the last algorithm ? */
		for (i = 1; i <= CRYPTO_ALGORITHM_MAX; i++)
			if (crypto_drivers[driverid].cc_alg[i] != 0)
				break;
	}

	/*
	 * If a driver unregistered its last algorithm or all of them
	 * (alg == CRYPTO_ALGORITHM_ALL), cleanup its entry.
	 */
	if (i == CRYPTO_ALGORITHM_MAX + 1 || alg == CRYPTO_ALGORITHM_ALL) {
		ses = crypto_drivers[driverid].cc_sessions;
		bzero(&crypto_drivers[driverid], sizeof(struct cryptocap));
		if (ses != 0) {
			/*
			 * If there are pending sessions, just mark as invalid.
			 */
			crypto_drivers[driverid].cc_flags |= CRYPTOCAP_F_CLEANUP;
			crypto_drivers[driverid].cc_sessions = ses;
		}
	}
	splx(s);
	return 0;
}

/*
 * Add crypto request to a queue, to be processed by a kernel thread.
 */
int
crypto_dispatch(struct cryptop *crp)
{
	int s = splimp();

	if (crp_req_queue == NULL) {
		crp_req_queue = crp;
		crp_req_queue_tail = &(crp->crp_next);
		splx(s);
		wakeup((caddr_t) &crp_req_queue);
	} else {
		*crp_req_queue_tail = crp;
		crp_req_queue_tail = &(crp->crp_next);
		splx(s);
	}
	return 0;
}

int
crypto_kdispatch(struct cryptkop *krp)
{
	int s = splimp();

	if (krp_req_queue == NULL) {
		krp_req_queue = krp;
		krp_req_queue_tail = &(krp->krp_next);
		splx(s);
		wakeup((caddr_t) &crp_req_queue);	/* shared wait channel */
	} else {
		*krp_req_queue_tail = krp;
		krp_req_queue_tail = &(krp->krp_next);
		splx(s);
	}
	return 0;
}

/*
 * Dispatch an asymmetric crypto request to the appropriate crypto devices.
 */
int
crypto_kinvoke(struct cryptkop *krp)
{
	extern int cryptodevallowsoft;
	u_int32_t hid;
	int error;

	/* Sanity checks. */
	if (krp == NULL || krp->krp_callback == NULL)
		return (EINVAL);

	for (hid = 0; hid < crypto_drivers_num; hid++) {
		if ((crypto_drivers[hid].cc_flags & CRYPTOCAP_F_SOFTWARE) &&
		    cryptodevallowsoft == 0)
			continue;
		if (crypto_drivers[hid].cc_kprocess == NULL)
			continue;
		if ((crypto_drivers[hid].cc_kalg[krp->krp_op] &
		    CRYPTO_ALG_FLAG_SUPPORTED) == 0)
			continue;
		break;
	}
	if (hid == crypto_drivers_num) {
		krp->krp_status = ENODEV;
		crypto_kdone(krp);
		return (0);
	}
	krp->krp_hid = hid;
	error = crypto_drivers[hid].cc_kprocess(krp);
	if (error) {
		krp->krp_status = error;
		crypto_kdone(krp);
	}
	return (0);
}

/*
 * Dispatch a crypto request to the appropriate crypto devices.
 */
int
crypto_invoke(struct cryptop *crp)
{
	struct cryptodesc *crd;
	u_int64_t nid;
	u_int32_t hid;
	int error;

	/* Sanity checks. */
	if (crp == NULL || crp->crp_callback == NULL)
		return EINVAL;

	if (crp->crp_desc == NULL || crypto_drivers == NULL) {
		crp->crp_etype = EINVAL;
		crypto_done(crp);
		return 0;
	}

	hid = (crp->crp_sid >> 32) & 0xffffffff;
	if (hid >= crypto_drivers_num)
		goto migrate;

	if (crypto_drivers[hid].cc_flags & CRYPTOCAP_F_CLEANUP)
		crypto_freesession(crp->crp_sid);

	if (crypto_drivers[hid].cc_process == NULL)
		goto migrate;

	error = crypto_drivers[hid].cc_process(crp);
	if (error) {
		if (error == ERESTART) {
			/* Unregister driver and migrate session. */
			crypto_unregister(hid, CRYPTO_ALGORITHM_ALL);
			goto migrate;
		} else {
			crp->crp_etype = error;
			crypto_done(crp);
		}
	}

	return 0;

 migrate:
	/* Migrate session. */
	for (crd = crp->crp_desc; crd->crd_next; crd = crd->crd_next)
		crd->CRD_INI.cri_next = &(crd->crd_next->CRD_INI);

	if (crypto_newsession(&nid, &(crp->crp_desc->CRD_INI), 0) == 0)
		crp->crp_sid = nid;

	crp->crp_etype = EAGAIN;
	crypto_done(crp);
	return 0;
}

/*
 * Release a set of crypto descriptors.
 */
void
crypto_freereq(struct cryptop *crp)
{
	struct cryptodesc *crd;
	int s;

	if (crp == NULL)
		return;

	s = splimp();

	while ((crd = crp->crp_desc) != NULL) {
		crp->crp_desc = crd->crd_next;
		pool_put(&cryptodesc_pool, crd);
	}

	pool_put(&cryptop_pool, crp);
	splx(s);
}

/*
 * Acquire a set of crypto descriptors.
 */
struct cryptop *
crypto_getreq(int num)
{
	struct cryptodesc *crd;
	struct cryptop *crp;
	int s = splimp();

	if (crypto_pool_initialized == 0) {
		pool_init(&cryptop_pool, sizeof(struct cryptop), 0, 0,
		    0, "cryptop", NULL);
		pool_init(&cryptodesc_pool, sizeof(struct cryptodesc), 0, 0,
		    0, "cryptodesc", NULL);
		crypto_pool_initialized = 1;
	}

	crp = pool_get(&cryptop_pool, 0);
	if (crp == NULL) {
		splx(s);
		return NULL;
	}
	bzero(crp, sizeof(struct cryptop));

	while (num--) {
		crd = pool_get(&cryptodesc_pool, 0);
		if (crd == NULL) {
			splx(s);
			crypto_freereq(crp);
			return NULL;
		}

		bzero(crd, sizeof(struct cryptodesc));
		crd->crd_next = crp->crp_desc;
		crp->crp_desc = crd;
	}

	splx(s);
	return crp;
}

/*
 * Crypto thread, runs as a kernel thread to process crypto requests.
 */
void
crypto_thread(void)
{
	struct cryptop *crp;
	struct cryptkop *krp;
	int s;

	s = splimp();

	for (;;) {
		crp = crp_req_queue;
		krp = krp_req_queue;
		if (crp == NULL && krp == NULL) {
			(void) tsleep(&crp_req_queue, PLOCK, "crypto_wait", 0);
			continue;
		}

		if (crp) {
			/* Remove from the queue. */
			crp_req_queue = crp->crp_next;
			crypto_invoke(crp);
		}
		if (krp) {
			/* Remove from the queue. */
			krp_req_queue = krp->krp_next;
			crypto_kinvoke(krp);
		}
	}
}

/*
 * Invoke the callback on behalf of the driver.
 */
void
crypto_done(struct cryptop *crp)
{
	crp->crp_callback(crp);
}

/*
 * Invoke the callback on behalf of the driver.
 */
void
crypto_kdone(struct cryptkop *krp)
{
	krp->krp_callback(krp);
}

int
crypto_getfeat(int *featp)
{
	extern int cryptodevallowsoft, userasymcrypto;
	int hid, kalg, feat = 0;

	if (userasymcrypto == 0)
		goto out;	  
	for (hid = 0; hid < crypto_drivers_num; hid++) {
		if ((crypto_drivers[hid].cc_flags & CRYPTOCAP_F_SOFTWARE) &&
		    cryptodevallowsoft == 0) {
			continue;
		}
		if (crypto_drivers[hid].cc_kprocess == NULL)
			continue;
		for (kalg = 0; kalg < CRK_ALGORITHM_MAX; kalg++)
			if ((crypto_drivers[hid].cc_kalg[kalg] &
			    CRYPTO_ALG_FLAG_SUPPORTED) != 0)
				feat |=  1 << kalg;
	}
out:
	*featp = feat;
	return (0);
}
