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
#include <sys/vdev_impl.h>
#include <sys/spa.h>
#include <zfs_comutil.h>

/*
 * Keeps stats on last N reads per spa_t, disabled by default.
 */
int zfs_read_history = 0;

/*
 * Include cache hits in history, disabled by default.
 */
int zfs_read_history_hits = 0;

/*
 * Keeps stats on the last N txgs, disabled by default.
 */
int zfs_txg_history = 0;

/*
 * Keeps stats on the last N MMP updates, disabled by default.
 */
int zfs_multihost_history = 0;

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
	(void) snprintf(buf, size, "%-8s %-16s %-8s %-8s %-8s %-8s %-8s "
	    "%-24s %-8s %-16s\n", "UID", "start", "objset", "object",
	    "level", "blkid", "aflags", "origin", "pid", "process");

	return (0);
}

static int
spa_read_history_data(char *buf, size_t size, void *data)
{
	spa_read_history_t *srh = (spa_read_history_t *)data;

	(void) snprintf(buf, size, "%-8llu %-16llu 0x%-6llx "
	    "%-8lli %-8lli %-8lli 0x%-6x %-24s %-8i %-16s\n",
	    (u_longlong_t)srh->uid, srh->start,
	    (longlong_t)srh->objset, (longlong_t)srh->object,
	    (longlong_t)srh->level, (longlong_t)srh->blkid,
	    srh->aflags, srh->origin, srh->pid, srh->comm);

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
 * When the kstat is written discard all spa_read_history_t entries.  The
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
			kmem_free(srh, sizeof (spa_read_history_t));
		}

		ASSERT3U(ssh->size, ==, 0);
	}

	ksp->ks_ndata = ssh->size;
	ksp->ks_data_size = ssh->size * sizeof (spa_read_history_t);

	return (0);
}

static void
spa_read_history_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;
	char *name;
	kstat_t *ksp;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ssh->list, sizeof (spa_read_history_t),
	    offsetof(spa_read_history_t, srh_link));

	ssh->count = 0;
	ssh->size = 0;
	ssh->private = NULL;

	name = kmem_asprintf("zfs/%s", spa_name(spa));

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
	strfree(name);
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
		kmem_free(srh, sizeof (spa_read_history_t));
	}

	ASSERT3U(ssh->size, ==, 0);
	list_destroy(&ssh->list);
	mutex_exit(&ssh->lock);

	mutex_destroy(&ssh->lock);
}

void
spa_read_history_add(spa_t *spa, const zbookmark_phys_t *zb, uint32_t aflags)
{
	spa_stats_history_t *ssh = &spa->spa_stats.read_history;
	spa_read_history_t *srh, *rm;

	ASSERT3P(spa, !=, NULL);
	ASSERT3P(zb,  !=, NULL);

	if (zfs_read_history == 0 && ssh->size == 0)
		return;

	if (zfs_read_history_hits == 0 && (aflags & ARC_FLAG_CACHED))
		return;

	srh = kmem_zalloc(sizeof (spa_read_history_t), KM_SLEEP);
	strlcpy(srh->comm, getcomm(), sizeof (srh->comm));
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
		kmem_free(rm, sizeof (spa_read_history_t));
	}

	mutex_exit(&ssh->lock);
}

/*
 * ==========================================================================
 * SPA TXG History Routines
 * ==========================================================================
 */

/*
 * Txg statistics - Information exported regarding each txg sync
 */

typedef struct spa_txg_history {
	uint64_t	txg;		/* txg id */
	txg_state_t	state;		/* active txg state */
	uint64_t	nread;		/* number of bytes read */
	uint64_t	nwritten;	/* number of bytes written */
	uint64_t	reads;		/* number of read operations */
	uint64_t	writes;		/* number of write operations */
	uint64_t	ndirty;		/* number of dirty bytes */
	hrtime_t	times[TXG_STATE_COMMITTED]; /* completion times */
	list_node_t	sth_link;
} spa_txg_history_t;

static int
spa_txg_history_headers(char *buf, size_t size)
{
	(void) snprintf(buf, size, "%-8s %-16s %-5s %-12s %-12s %-12s "
	    "%-8s %-8s %-12s %-12s %-12s %-12s\n", "txg", "birth", "state",
	    "ndirty", "nread", "nwritten", "reads", "writes",
	    "otime", "qtime", "wtime", "stime");

	return (0);
}

static int
spa_txg_history_data(char *buf, size_t size, void *data)
{
	spa_txg_history_t *sth = (spa_txg_history_t *)data;
	uint64_t open = 0, quiesce = 0, wait = 0, sync = 0;
	char state;

	switch (sth->state) {
		case TXG_STATE_BIRTH:		state = 'B';	break;
		case TXG_STATE_OPEN:		state = 'O';	break;
		case TXG_STATE_QUIESCED:	state = 'Q';	break;
		case TXG_STATE_WAIT_FOR_SYNC:	state = 'W';	break;
		case TXG_STATE_SYNCED:		state = 'S';	break;
		case TXG_STATE_COMMITTED:	state = 'C';	break;
		default:			state = '?';	break;
	}

	if (sth->times[TXG_STATE_OPEN])
		open = sth->times[TXG_STATE_OPEN] -
		    sth->times[TXG_STATE_BIRTH];

	if (sth->times[TXG_STATE_QUIESCED])
		quiesce = sth->times[TXG_STATE_QUIESCED] -
		    sth->times[TXG_STATE_OPEN];

	if (sth->times[TXG_STATE_WAIT_FOR_SYNC])
		wait = sth->times[TXG_STATE_WAIT_FOR_SYNC] -
		    sth->times[TXG_STATE_QUIESCED];

	if (sth->times[TXG_STATE_SYNCED])
		sync = sth->times[TXG_STATE_SYNCED] -
		    sth->times[TXG_STATE_WAIT_FOR_SYNC];

	(void) snprintf(buf, size, "%-8llu %-16llu %-5c %-12llu "
	    "%-12llu %-12llu %-8llu %-8llu %-12llu %-12llu %-12llu %-12llu\n",
	    (longlong_t)sth->txg, sth->times[TXG_STATE_BIRTH], state,
	    (u_longlong_t)sth->ndirty,
	    (u_longlong_t)sth->nread, (u_longlong_t)sth->nwritten,
	    (u_longlong_t)sth->reads, (u_longlong_t)sth->writes,
	    (u_longlong_t)open, (u_longlong_t)quiesce, (u_longlong_t)wait,
	    (u_longlong_t)sync);

	return (0);
}

/*
 * Calculate the address for the next spa_stats_history_t entry.  The
 * ssh->lock will be held until ksp->ks_ndata entries are processed.
 */
static void *
spa_txg_history_addr(kstat_t *ksp, loff_t n)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;

	ASSERT(MUTEX_HELD(&ssh->lock));

	if (n == 0)
		ssh->private = list_tail(&ssh->list);
	else if (ssh->private)
		ssh->private = list_prev(&ssh->list, ssh->private);

	return (ssh->private);
}

/*
 * When the kstat is written discard all spa_txg_history_t entries.  The
 * ssh->lock will be held until ksp->ks_ndata entries are processed.
 */
static int
spa_txg_history_update(kstat_t *ksp, int rw)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;

	ASSERT(MUTEX_HELD(&ssh->lock));

	if (rw == KSTAT_WRITE) {
		spa_txg_history_t *sth;

		while ((sth = list_remove_head(&ssh->list))) {
			ssh->size--;
			kmem_free(sth, sizeof (spa_txg_history_t));
		}

		ASSERT3U(ssh->size, ==, 0);
	}

	ksp->ks_ndata = ssh->size;
	ksp->ks_data_size = ssh->size * sizeof (spa_txg_history_t);

	return (0);
}

static void
spa_txg_history_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;
	char *name;
	kstat_t *ksp;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ssh->list, sizeof (spa_txg_history_t),
	    offsetof(spa_txg_history_t, sth_link));

	ssh->count = 0;
	ssh->size = 0;
	ssh->private = NULL;

	name = kmem_asprintf("zfs/%s", spa_name(spa));

	ksp = kstat_create(name, 0, "txgs", "misc",
	    KSTAT_TYPE_RAW, 0, KSTAT_FLAG_VIRTUAL);
	ssh->kstat = ksp;

	if (ksp) {
		ksp->ks_lock = &ssh->lock;
		ksp->ks_data = NULL;
		ksp->ks_private = spa;
		ksp->ks_update = spa_txg_history_update;
		kstat_set_raw_ops(ksp, spa_txg_history_headers,
		    spa_txg_history_data, spa_txg_history_addr);
		kstat_install(ksp);
	}
	strfree(name);
}

static void
spa_txg_history_destroy(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;
	spa_txg_history_t *sth;
	kstat_t *ksp;

	ksp = ssh->kstat;
	if (ksp)
		kstat_delete(ksp);

	mutex_enter(&ssh->lock);
	while ((sth = list_remove_head(&ssh->list))) {
		ssh->size--;
		kmem_free(sth, sizeof (spa_txg_history_t));
	}

	ASSERT3U(ssh->size, ==, 0);
	list_destroy(&ssh->list);
	mutex_exit(&ssh->lock);

	mutex_destroy(&ssh->lock);
}

/*
 * Add a new txg to historical record.
 */
void
spa_txg_history_add(spa_t *spa, uint64_t txg, hrtime_t birth_time)
{
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;
	spa_txg_history_t *sth, *rm;

	if (zfs_txg_history == 0 && ssh->size == 0)
		return;

	sth = kmem_zalloc(sizeof (spa_txg_history_t), KM_SLEEP);
	sth->txg = txg;
	sth->state = TXG_STATE_OPEN;
	sth->times[TXG_STATE_BIRTH] = birth_time;

	mutex_enter(&ssh->lock);

	list_insert_head(&ssh->list, sth);
	ssh->size++;

	while (ssh->size > zfs_txg_history) {
		ssh->size--;
		rm = list_remove_tail(&ssh->list);
		kmem_free(rm, sizeof (spa_txg_history_t));
	}

	mutex_exit(&ssh->lock);
}

/*
 * Set txg state completion time and increment current state.
 */
int
spa_txg_history_set(spa_t *spa, uint64_t txg, txg_state_t completed_state,
    hrtime_t completed_time)
{
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;
	spa_txg_history_t *sth;
	int error = ENOENT;

	if (zfs_txg_history == 0)
		return (0);

	mutex_enter(&ssh->lock);
	for (sth = list_head(&ssh->list); sth != NULL;
	    sth = list_next(&ssh->list, sth)) {
		if (sth->txg == txg) {
			sth->times[completed_state] = completed_time;
			sth->state++;
			error = 0;
			break;
		}
	}
	mutex_exit(&ssh->lock);

	return (error);
}

/*
 * Set txg IO stats.
 */
static int
spa_txg_history_set_io(spa_t *spa, uint64_t txg, uint64_t nread,
    uint64_t nwritten, uint64_t reads, uint64_t writes, uint64_t ndirty)
{
	spa_stats_history_t *ssh = &spa->spa_stats.txg_history;
	spa_txg_history_t *sth;
	int error = ENOENT;

	if (zfs_txg_history == 0)
		return (0);

	mutex_enter(&ssh->lock);
	for (sth = list_head(&ssh->list); sth != NULL;
	    sth = list_next(&ssh->list, sth)) {
		if (sth->txg == txg) {
			sth->nread = nread;
			sth->nwritten = nwritten;
			sth->reads = reads;
			sth->writes = writes;
			sth->ndirty = ndirty;
			error = 0;
			break;
		}
	}
	mutex_exit(&ssh->lock);

	return (error);
}

txg_stat_t *
spa_txg_history_init_io(spa_t *spa, uint64_t txg, dsl_pool_t *dp)
{
	txg_stat_t *ts;

	if (zfs_txg_history == 0)
		return (NULL);

	ts = kmem_alloc(sizeof (txg_stat_t), KM_SLEEP);

	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	vdev_get_stats(spa->spa_root_vdev, &ts->vs1);
	spa_config_exit(spa, SCL_ALL, FTAG);

	ts->txg = txg;
	ts->ndirty = dp->dp_dirty_pertxg[txg & TXG_MASK];

	spa_txg_history_set(spa, txg, TXG_STATE_WAIT_FOR_SYNC, gethrtime());

	return (ts);
}

void
spa_txg_history_fini_io(spa_t *spa, txg_stat_t *ts)
{
	if (ts == NULL)
		return;

	if (zfs_txg_history == 0) {
		kmem_free(ts, sizeof (txg_stat_t));
		return;
	}

	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	vdev_get_stats(spa->spa_root_vdev, &ts->vs2);
	spa_config_exit(spa, SCL_ALL, FTAG);

	spa_txg_history_set(spa, ts->txg, TXG_STATE_SYNCED, gethrtime());
	spa_txg_history_set_io(spa, ts->txg,
	    ts->vs2.vs_bytes[ZIO_TYPE_READ] - ts->vs1.vs_bytes[ZIO_TYPE_READ],
	    ts->vs2.vs_bytes[ZIO_TYPE_WRITE] - ts->vs1.vs_bytes[ZIO_TYPE_WRITE],
	    ts->vs2.vs_ops[ZIO_TYPE_READ] - ts->vs1.vs_ops[ZIO_TYPE_READ],
	    ts->vs2.vs_ops[ZIO_TYPE_WRITE] - ts->vs1.vs_ops[ZIO_TYPE_WRITE],
	    ts->ndirty);

	kmem_free(ts, sizeof (txg_stat_t));
}

/*
 * ==========================================================================
 * SPA TX Assign Histogram Routines
 * ==========================================================================
 */

/*
 * Tx statistics - Information exported regarding dmu_tx_assign time.
 */

/*
 * When the kstat is written zero all buckets.  When the kstat is read
 * count the number of trailing buckets set to zero and update ks_ndata
 * such that they are not output.
 */
static int
spa_tx_assign_update(kstat_t *ksp, int rw)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.tx_assign_histogram;
	int i;

	if (rw == KSTAT_WRITE) {
		for (i = 0; i < ssh->count; i++)
			((kstat_named_t *)ssh->private)[i].value.ui64 = 0;
	}

	for (i = ssh->count; i > 0; i--)
		if (((kstat_named_t *)ssh->private)[i-1].value.ui64 != 0)
			break;

	ksp->ks_ndata = i;
	ksp->ks_data_size = i * sizeof (kstat_named_t);

	return (0);
}

static void
spa_tx_assign_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.tx_assign_histogram;
	char *name;
	kstat_named_t *ks;
	kstat_t *ksp;
	int i;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);

	ssh->count = 42; /* power of two buckets for 1ns to 2,199s */
	ssh->size = ssh->count * sizeof (kstat_named_t);
	ssh->private = kmem_alloc(ssh->size, KM_SLEEP);

	name = kmem_asprintf("zfs/%s", spa_name(spa));

	for (i = 0; i < ssh->count; i++) {
		ks = &((kstat_named_t *)ssh->private)[i];
		ks->data_type = KSTAT_DATA_UINT64;
		ks->value.ui64 = 0;
		(void) snprintf(ks->name, KSTAT_STRLEN, "%llu ns",
		    (u_longlong_t)1 << i);
	}

	ksp = kstat_create(name, 0, "dmu_tx_assign", "misc",
	    KSTAT_TYPE_NAMED, 0, KSTAT_FLAG_VIRTUAL);
	ssh->kstat = ksp;

	if (ksp) {
		ksp->ks_lock = &ssh->lock;
		ksp->ks_data = ssh->private;
		ksp->ks_ndata = ssh->count;
		ksp->ks_data_size = ssh->size;
		ksp->ks_private = spa;
		ksp->ks_update = spa_tx_assign_update;
		kstat_install(ksp);
	}
	strfree(name);
}

static void
spa_tx_assign_destroy(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.tx_assign_histogram;
	kstat_t *ksp;

	ksp = ssh->kstat;
	if (ksp)
		kstat_delete(ksp);

	kmem_free(ssh->private, ssh->size);
	mutex_destroy(&ssh->lock);
}

void
spa_tx_assign_add_nsecs(spa_t *spa, uint64_t nsecs)
{
	spa_stats_history_t *ssh = &spa->spa_stats.tx_assign_histogram;
	uint64_t idx = 0;

	while (((1ULL << idx) < nsecs) && (idx < ssh->size - 1))
		idx++;

	atomic_inc_64(&((kstat_named_t *)ssh->private)[idx].value.ui64);
}

/*
 * ==========================================================================
 * SPA IO History Routines
 * ==========================================================================
 */
static int
spa_io_history_update(kstat_t *ksp, int rw)
{
	if (rw == KSTAT_WRITE)
		memset(ksp->ks_data, 0, ksp->ks_data_size);

	return (0);
}

static void
spa_io_history_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.io_history;
	char *name;
	kstat_t *ksp;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);

	name = kmem_asprintf("zfs/%s", spa_name(spa));

	ksp = kstat_create(name, 0, "io", "disk", KSTAT_TYPE_IO, 1, 0);
	ssh->kstat = ksp;

	if (ksp) {
		ksp->ks_lock = &ssh->lock;
		ksp->ks_private = spa;
		ksp->ks_update = spa_io_history_update;
		kstat_install(ksp);
	}
	strfree(name);
}

static void
spa_io_history_destroy(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.io_history;

	if (ssh->kstat)
		kstat_delete(ssh->kstat);

	mutex_destroy(&ssh->lock);
}

/*
 * ==========================================================================
 * SPA MMP History Routines
 * ==========================================================================
 */

/*
 * MMP statistics - Information exported regarding attempted MMP writes
 *   For MMP writes issued, fields used as per comments below.
 *   For MMP writes skipped, an entry represents a span of time when
 *      writes were skipped for same reason (error from mmp_random_leaf).
 *      Differences are:
 *      timestamp	time first write skipped, if >1 skipped in a row
 *      mmp_delay	delay value at timestamp
 *      vdev_guid	number of writes skipped
 *      io_error	one of enum mmp_error
 *      duration	time span (ns) of skipped writes
 */

typedef struct spa_mmp_history {
	uint64_t	mmp_kstat_id;	/* unique # for updates */
	uint64_t	txg;		/* txg of last sync */
	uint64_t	timestamp;	/* UTC time MMP write issued */
	uint64_t	mmp_delay;	/* mmp_thread.mmp_delay at timestamp */
	uint64_t	vdev_guid;	/* unique ID of leaf vdev */
	char		*vdev_path;
	int		vdev_label;	/* vdev label */
	int		io_error;	/* error status of MMP write */
	hrtime_t	error_start;	/* hrtime of start of error period */
	hrtime_t	duration;	/* time from submission to completion */
	list_node_t	smh_link;
} spa_mmp_history_t;

static int
spa_mmp_history_headers(char *buf, size_t size)
{
	(void) snprintf(buf, size, "%-10s %-10s %-10s %-6s %-10s %-12s %-24s "
	    "%-10s %s\n", "id", "txg", "timestamp", "error", "duration",
	    "mmp_delay", "vdev_guid", "vdev_label", "vdev_path");
	return (0);
}

static int
spa_mmp_history_data(char *buf, size_t size, void *data)
{
	spa_mmp_history_t *smh = (spa_mmp_history_t *)data;
	char skip_fmt[] = "%-10llu %-10llu %10llu %#6llx %10lld %12llu %-24llu "
	    "%-10lld %s\n";
	char write_fmt[] = "%-10llu %-10llu %10llu %6lld %10lld %12llu %-24llu "
	    "%-10lld %s\n";

	(void) snprintf(buf, size, (smh->error_start ? skip_fmt : write_fmt),
	    (u_longlong_t)smh->mmp_kstat_id, (u_longlong_t)smh->txg,
	    (u_longlong_t)smh->timestamp, (longlong_t)smh->io_error,
	    (longlong_t)smh->duration, (u_longlong_t)smh->mmp_delay,
	    (u_longlong_t)smh->vdev_guid, (u_longlong_t)smh->vdev_label,
	    (smh->vdev_path ? smh->vdev_path : "-"));

	return (0);
}

/*
 * Calculate the address for the next spa_stats_history_t entry.  The
 * ssh->lock will be held until ksp->ks_ndata entries are processed.
 */
static void *
spa_mmp_history_addr(kstat_t *ksp, loff_t n)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;

	ASSERT(MUTEX_HELD(&ssh->lock));

	if (n == 0)
		ssh->private = list_tail(&ssh->list);
	else if (ssh->private)
		ssh->private = list_prev(&ssh->list, ssh->private);

	return (ssh->private);
}

/*
 * When the kstat is written discard all spa_mmp_history_t entries.  The
 * ssh->lock will be held until ksp->ks_ndata entries are processed.
 */
static int
spa_mmp_history_update(kstat_t *ksp, int rw)
{
	spa_t *spa = ksp->ks_private;
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;

	ASSERT(MUTEX_HELD(&ssh->lock));

	if (rw == KSTAT_WRITE) {
		spa_mmp_history_t *smh;

		while ((smh = list_remove_head(&ssh->list))) {
			ssh->size--;
			if (smh->vdev_path)
				strfree(smh->vdev_path);
			kmem_free(smh, sizeof (spa_mmp_history_t));
		}

		ASSERT3U(ssh->size, ==, 0);
	}

	ksp->ks_ndata = ssh->size;
	ksp->ks_data_size = ssh->size * sizeof (spa_mmp_history_t);

	return (0);
}

static void
spa_mmp_history_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;
	char *name;
	kstat_t *ksp;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&ssh->list, sizeof (spa_mmp_history_t),
	    offsetof(spa_mmp_history_t, smh_link));

	ssh->count = 0;
	ssh->size = 0;
	ssh->private = NULL;

	name = kmem_asprintf("zfs/%s", spa_name(spa));

	ksp = kstat_create(name, 0, "multihost", "misc",
	    KSTAT_TYPE_RAW, 0, KSTAT_FLAG_VIRTUAL);
	ssh->kstat = ksp;

	if (ksp) {
		ksp->ks_lock = &ssh->lock;
		ksp->ks_data = NULL;
		ksp->ks_private = spa;
		ksp->ks_update = spa_mmp_history_update;
		kstat_set_raw_ops(ksp, spa_mmp_history_headers,
		    spa_mmp_history_data, spa_mmp_history_addr);
		kstat_install(ksp);
	}
	strfree(name);
}

static void
spa_mmp_history_destroy(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;
	spa_mmp_history_t *smh;
	kstat_t *ksp;

	ksp = ssh->kstat;
	if (ksp)
		kstat_delete(ksp);

	mutex_enter(&ssh->lock);
	while ((smh = list_remove_head(&ssh->list))) {
		ssh->size--;
		if (smh->vdev_path)
			strfree(smh->vdev_path);
		kmem_free(smh, sizeof (spa_mmp_history_t));
	}

	ASSERT3U(ssh->size, ==, 0);
	list_destroy(&ssh->list);
	mutex_exit(&ssh->lock);

	mutex_destroy(&ssh->lock);
}

/*
 * Set duration in existing "skip" record to how long we have waited for a leaf
 * vdev to become available.
 *
 * Important that we start search at the head of the list where new
 * records are inserted, so this is normally an O(1) operation.
 */
int
spa_mmp_history_set_skip(spa_t *spa, uint64_t mmp_kstat_id)
{
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;
	spa_mmp_history_t *smh;
	int error = ENOENT;

	if (zfs_multihost_history == 0 && ssh->size == 0)
		return (0);

	mutex_enter(&ssh->lock);
	for (smh = list_head(&ssh->list); smh != NULL;
	    smh = list_next(&ssh->list, smh)) {
		if (smh->mmp_kstat_id == mmp_kstat_id) {
			ASSERT3U(smh->io_error, !=, 0);
			smh->duration = gethrtime() - smh->error_start;
			smh->vdev_guid++;
			error = 0;
			break;
		}
	}
	mutex_exit(&ssh->lock);

	return (error);
}

/*
 * Set MMP write duration and error status in existing record.
 * See comment re: search order above spa_mmp_history_set_skip().
 */
int
spa_mmp_history_set(spa_t *spa, uint64_t mmp_kstat_id, int io_error,
    hrtime_t duration)
{
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;
	spa_mmp_history_t *smh;
	int error = ENOENT;

	if (zfs_multihost_history == 0 && ssh->size == 0)
		return (0);

	mutex_enter(&ssh->lock);
	for (smh = list_head(&ssh->list); smh != NULL;
	    smh = list_next(&ssh->list, smh)) {
		if (smh->mmp_kstat_id == mmp_kstat_id) {
			ASSERT(smh->io_error == 0);
			smh->io_error = io_error;
			smh->duration = duration;
			error = 0;
			break;
		}
	}
	mutex_exit(&ssh->lock);

	return (error);
}

/*
 * Add a new MMP historical record.
 * error == 0 : a write was issued.
 * error != 0 : a write was not issued because no leaves were found.
 */
void *
spa_mmp_history_add(spa_t *spa, uint64_t txg, uint64_t timestamp,
    uint64_t mmp_delay, vdev_t *vd, int label, uint64_t mmp_kstat_id,
    int error)
{
	spa_stats_history_t *ssh = &spa->spa_stats.mmp_history;
	spa_mmp_history_t *smh, *rm;

	if (zfs_multihost_history == 0 && ssh->size == 0)
		return (NULL);

	smh = kmem_zalloc(sizeof (spa_mmp_history_t), KM_SLEEP);
	smh->txg = txg;
	smh->timestamp = timestamp;
	smh->mmp_delay = mmp_delay;
	if (vd) {
		smh->vdev_guid = vd->vdev_guid;
		if (vd->vdev_path)
			smh->vdev_path = strdup(vd->vdev_path);
	}
	smh->vdev_label = label;
	smh->mmp_kstat_id = mmp_kstat_id;

	if (error) {
		smh->io_error = error;
		smh->error_start = gethrtime();
		smh->vdev_guid = 1;
	}

	mutex_enter(&ssh->lock);

	list_insert_head(&ssh->list, smh);
	ssh->size++;

	while (ssh->size > zfs_multihost_history) {
		ssh->size--;
		rm = list_remove_tail(&ssh->list);
		if (rm->vdev_path)
			strfree(rm->vdev_path);
		kmem_free(rm, sizeof (spa_mmp_history_t));
	}

	mutex_exit(&ssh->lock);
	return ((void *)smh);
}

static void *
spa_state_addr(kstat_t *ksp, loff_t n)
{
	return (ksp->ks_private);	/* return the spa_t */
}

static int
spa_state_data(char *buf, size_t size, void *data)
{
	spa_t *spa = (spa_t *)data;
	(void) snprintf(buf, size, "%s\n", spa_state_to_name(spa));
	return (0);
}

/*
 * Return the state of the pool in /proc/spl/kstat/zfs/<pool>/state.
 *
 * This is a lock-less read of the pool's state (unlike using 'zpool', which
 * can potentially block for seconds).  Because it doesn't block, it can useful
 * as a pool heartbeat value.
 */
static void
spa_state_init(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.state;
	char *name;
	kstat_t *ksp;

	mutex_init(&ssh->lock, NULL, MUTEX_DEFAULT, NULL);

	name = kmem_asprintf("zfs/%s", spa_name(spa));
	ksp = kstat_create(name, 0, "state", "misc",
	    KSTAT_TYPE_RAW, 0, KSTAT_FLAG_VIRTUAL);

	ssh->kstat = ksp;
	if (ksp) {
		ksp->ks_lock = &ssh->lock;
		ksp->ks_data = NULL;
		ksp->ks_private = spa;
		ksp->ks_flags |= KSTAT_FLAG_NO_HEADERS;
		kstat_set_raw_ops(ksp, NULL, spa_state_data, spa_state_addr);
		kstat_install(ksp);
	}

	strfree(name);
}

static void
spa_health_destroy(spa_t *spa)
{
	spa_stats_history_t *ssh = &spa->spa_stats.state;
	kstat_t *ksp = ssh->kstat;
	if (ksp)
		kstat_delete(ksp);

	mutex_destroy(&ssh->lock);
}

void
spa_stats_init(spa_t *spa)
{
	spa_read_history_init(spa);
	spa_txg_history_init(spa);
	spa_tx_assign_init(spa);
	spa_io_history_init(spa);
	spa_mmp_history_init(spa);
	spa_state_init(spa);
}

void
spa_stats_destroy(spa_t *spa)
{
	spa_health_destroy(spa);
	spa_tx_assign_destroy(spa);
	spa_txg_history_destroy(spa);
	spa_read_history_destroy(spa);
	spa_io_history_destroy(spa);
	spa_mmp_history_destroy(spa);
}

#if defined(_KERNEL) && defined(HAVE_SPL)
/* CSTYLED */
module_param(zfs_read_history, int, 0644);
MODULE_PARM_DESC(zfs_read_history,
	"Historical statistics for the last N reads");

module_param(zfs_read_history_hits, int, 0644);
MODULE_PARM_DESC(zfs_read_history_hits,
	"Include cache hits in read history");

module_param(zfs_txg_history, int, 0644);
MODULE_PARM_DESC(zfs_txg_history,
	"Historical statistics for the last N txgs");

module_param(zfs_multihost_history, int, 0644);
MODULE_PARM_DESC(zfs_multihost_history,
	"Historical statistics for last N multihost writes");
/* END CSTYLED */
#endif
