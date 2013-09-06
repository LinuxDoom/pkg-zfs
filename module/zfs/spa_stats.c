/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#include <sys/zfs_context.h>
#include <sys/spa_impl.h>

/*
 * Keeps stats on last N reads per spa_t, disabled by default.
 */
int zfs_read_history = 0;

/*
 * Include cache hits in history, disabled by default.
 */
int zfs_read_history_hits = 0;

/*
 * ==========================================================================
 * SPA Read History Routines
 * ==========================================================================
 */

/*
 * Read statistics - Information exported regarding each arc_read call
 */
typedef struct spa_read_history {
	uint64_t	uid;		/* unique identifier */
	hrtime_t	start;		/* time read completed */
	uint64_t	objset;		/* read from this objset */
	uint64_t	object;		/* read of this object number */
	uint64_t	level;		/* block's indirection level */
	uint64_t	blkid;		/* read of this block id */
	char		origin[24];	/* read originated from here */
	uint32_t	aflags;		/* ARC flags (cached, prefetch, etc.) */
	pid_t		pid;		/* PID of task doing read */
	char		comm[16];	/* process name of task doing read */
	list_node_t	srh_link;
} spa_read_history_t;

static int
spa_read_history_headers(char *buf, size_t size)
{
	size = snprintf(buf, size - 1, "%-8s %-16s %-8s %-8s %-8s %-8s %-8s "
	    "%-24s %-8s %-16s\n", "UID", "start", "objset", "object",
	    "level", "blkid", "aflags", "origin", "pid", "process");
	buf[size] = '\0';

	return (0);
}

static int
spa_read_history_data(char *buf, size_t size, void *data)
{
	spa_read_history_t *srh = (spa_read_history_t *)data;

	size = snprintf(buf, size - 1, "%-8llu %-16llu 0x%-6llx "
	    "%-8lli %-8lli %-8lli 0x%-6x %-24s %-8i %-16s\n",
	    (u_longlong_t)srh->uid, srh->start,
	    (longlong_t)srh->objset, (longlong_t)srh->object,
	    (longlong_t)srh->level, (longlong_t)srh->blkid,
	    srh->aflags, srh->origin, srh->pid, srh->comm);
	buf[size] = '\0';

	return (0);
}

/*
 * Calculate the address for the next spa_stats_history_t entry.  The
 * ssh->lock will be held until ksp->ks_ndata entries are processed.
 */
static void *
spa_read_history_addr(kstat_t *ksp, loff_t n)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;

	ASSERT(MUTEX_HELD(&ssh->lock));

	if (n == 0)
		ssh->private = list_tail(&ssh->list);
	else if (ssh->private)
		ssh->private = list_prev(&ssh->list, ssh->private);

	return (ssh->private);
}

/*
 * When the kstat is written discard all spa_read_history_t entires.  The
 * ssh->lock will be held until ksp->ks_ndata entries are processed.
 */
static int
spa_read_history_update(kstat_t *ksp, int rw)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;

	if (rw == KSTAT_WRITE) {
		spa_read_history_t *srh;

		while ((srh = list_remove_head(&ssh->list))) {
			ssh->size--;
			kmem_free(srh, sizeof(spa_read_history_t));
		}

		ASSERT3U(ssh->size, ==, 0);
	}

	ksp->ks_ndata = ssh->size;
	ksp->ks_data_size = ssh->size * sizeof(spa_read_history_t);

	return (0);
}

static void
spa_read_history_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;
	char name[KSTAT_STRLEN];
	kstat_t *ksp;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ssh->list, sizeof (spa_read_history_t),
	    offsetof(spa_read_history_t, srh_link));

	ssh->count = 0;
	ssh->size = 0;
	ssh->private = NULL;

	(void) snprintf(name, KSTAT_STRLEN, "zfs/%s", spa_name(spa));
	name[KSTAT_STRLEN-1] = '\0';

	ksp = kstat_create(name, 0, "reads", "misc",
	    KSTAT_TYPE_RAW, 0, KSTAT_FLAG_VIRTUAL);
	ssh->kstat = ksp;

	if (ksp) {
		ksp->ks_lock = &ssh->lock;
		ksp->ks_data = NULL;
		ksp->ks_private = spa;
		ksp->ks_update = spa_read_history_update;
		kstat_set_raw_ops(ksp, spa_read_history_headers,
		    spa_read_history_data, spa_read_history_addr);
		kstat_install(ksp);
	}
}

static void
spa_read_history_destroy(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;
	spa_read_history_t *srh;
	kstat_t *ksp;

	ksp = ssh->kstat;
	if (ksp)
		kstat_delete(ksp);

	mutex_enter(&ssh->lock);
	while ((srh = list_remove_head(&ssh->list))) {
		ssh->size--;
		kmem_free(srh, sizeof(spa_read_history_t));
	}

	ASSERT3U(ssh->size, ==, 0);
	list_destroy(&ssh->list);
	mutex_exit(&ssh->lock);

	mutex_destroy(&ssh->lock);
}

void
spa_read_history_add(spa_t *spa, const zbookmark_t *zb, uint32_t aflags)
{
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;
	spa_read_history_t *srh, *rm;

	ASSERT3P(spa, !=, NULL);
	ASSERT3P(zb,  !=, NULL);

	if (zfs_read_history == 0 && ssh->size == 0)
		return;

	if (zfs_read_history_hits == 0 && (aflags & ARC_CACHED))
		return;

	srh = kmem_zalloc(sizeof(spa_read_history_t), KM_PUSHPAGE);
	strlcpy(srh->origin, zb->zb_func, sizeof(srh->origin));
	strlcpy(srh->comm, getcomm(), sizeof(srh->comm));
	srh->start  = gethrtime();
	srh->objset = zb->zb_objset;
	srh->object = zb->zb_object;
	srh->level  = zb->zb_level;
	srh->blkid  = zb->zb_blkid;
	srh->aflags = aflags;
	srh->pid    = getpid();

	mutex_enter(&ssh->lock);

	srh->uid = ssh->count++;
	list_insert_head(&ssh->list, srh);
	ssh->size++;

	while (ssh->size > zfs_read_history) {
		ssh->size--;
		rm = list_remove_tail(&ssh->list);
		kmem_free(rm, sizeof(spa_read_history_t));
	}

	mutex_exit(&ssh->lock);
}

void
spa_stats_init(spa_t *spa)
{
	spa_read_history_init(spa);
}

void
spa_stats_destroy(spa_t *spa)
{
	spa_read_history_destroy(spa);
}

#if defined(_KERNEL) && defined(HAVE_SPL)
module_param(zfs_read_history, int, 0644);
MODULE_PARM_DESC(zfs_read_history, "Historic statistics for the last N reads");

module_param(zfs_read_history_hits, int, 0644);
MODULE_PARM_DESC(zfs_read_history_hits, "Include cache hits in read history");
#endif
