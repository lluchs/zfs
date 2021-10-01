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
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2012 Cyril Plisko. All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright (c) 2013, 2017 by Delphix. All rights reserved.
 */

/*
 * Print intent log header and statistics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/dmu.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/zil_lwb.h>
#include <sys/zil_impl.h>
#include <sys/spa_impl.h>
#include <sys/abd.h>

#include "zdb.h"

extern uint8_t dump_opt[256];

static char tab_prefix[4] = "\t\t\t";

static void
print_log_bp(const blkptr_t *bp, const char *prefix)
{
	char blkbuf[BP_SPRINTF_LEN];

	snprintf_blkptr(blkbuf, sizeof (blkbuf), bp);
	(void) printf("%s%s\n", prefix, blkbuf);
}

typedef struct {
	spa_t *pra_spa;
	objset_t *pra_os;
	uint64_t pra_claim_txg;
} print_record_arg_t;

/* ARGSUSED */
static void
zil_prt_rec_create(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_create_t *lr = (const lr_create_t *)lrc;
	time_t crtime = lr->lr_crtime[0];
	char *name, *link;
	lr_attr_t *lrattr;

	name = (char *)(lr + 1);

	if (lr->lr_common.lrc_txtype == TX_CREATE_ATTR ||
	    lr->lr_common.lrc_txtype == TX_MKDIR_ATTR) {
		lrattr = (lr_attr_t *)(lr + 1);
		name += ZIL_XVAT_SIZE(lrattr->lr_attr_masksize);
	}

	if (txtype == TX_SYMLINK) {
		link = name + strlen(name) + 1;
		(void) printf("%s%s -> %s\n", tab_prefix, name, link);
	} else if (txtype != TX_MKXATTR) {
		(void) printf("%s%s\n", tab_prefix, name);
	}

	(void) printf("%s%s", tab_prefix, ctime(&crtime));
	(void) printf("%sdoid %llu, foid %llu, slots %llu, mode %llo\n",
	    tab_prefix, (u_longlong_t)lr->lr_doid,
	    (u_longlong_t)LR_FOID_GET_OBJ(lr->lr_foid),
	    (u_longlong_t)LR_FOID_GET_SLOTS(lr->lr_foid),
	    (longlong_t)lr->lr_mode);
	(void) printf("%suid %llu, gid %llu, gen %llu, rdev 0x%llx\n",
	    tab_prefix,
	    (u_longlong_t)lr->lr_uid, (u_longlong_t)lr->lr_gid,
	    (u_longlong_t)lr->lr_gen, (u_longlong_t)lr->lr_rdev);
}

/* ARGSUSED */
static void
zil_prt_rec_remove(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_remove_t *lr = (const lr_remove_t *)lrc;

	(void) printf("%sdoid %llu, name %s\n", tab_prefix,
	    (u_longlong_t)lr->lr_doid, (char *)(lr + 1));
}

/* ARGSUSED */
static void
zil_prt_rec_link(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_link_t *lr = (const lr_link_t *)lrc;

	(void) printf("%sdoid %llu, link_obj %llu, name %s\n", tab_prefix,
	    (u_longlong_t)lr->lr_doid, (u_longlong_t)lr->lr_link_obj,
	    (char *)(lr + 1));
}

/* ARGSUSED */
static void
zil_prt_rec_rename(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_rename_t *lr = (const lr_rename_t *)lrc;
	char *snm = (char *)(lr + 1);
	char *tnm = snm + strlen(snm) + 1;

	(void) printf("%ssdoid %llu, tdoid %llu\n", tab_prefix,
	    (u_longlong_t)lr->lr_sdoid, (u_longlong_t)lr->lr_tdoid);
	(void) printf("%ssrc %s tgt %s\n", tab_prefix, snm, tnm);
}

/* ARGSUSED */
static int
zil_prt_rec_write_cb(void *data, size_t len, void *unused)
{
	char *cdata = data;

	for (size_t i = 0; i < len; i++) {
		if (isprint(*cdata))
			(void) printf("%c ", *cdata);
		else
			(void) printf("%2X", *cdata);
		cdata++;
	}
	return (0);
}

/* ARGSUSED */
static void
zil_prt_rec_write(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_write_t *lr = (const lr_write_t *)lrc;
	abd_t *data;
	const blkptr_t *bp = &lr->lr_blkptr;
	zbookmark_phys_t zb;
	int verbose = MAX(dump_opt['d'], dump_opt['i']);
	int error;

	(void) printf("%sfoid %llu, offset %llx, length %llx\n", tab_prefix,
	    (u_longlong_t)lr->lr_foid, (u_longlong_t)lr->lr_offset,
	    (u_longlong_t)lr->lr_length);

	if (txtype == TX_WRITE2 || verbose < 5)
		return;

	if (lr->lr_common.lrc_reclen == sizeof (lr_write_t)) {
		(void) printf("%shas blkptr, %s\n", tab_prefix,
		    !BP_IS_HOLE(bp) &&
		    bp->blk_birth >= spa_min_claim_txg(arg->pra_spa) ?
		    "will claim" : "won't claim");
		print_log_bp(bp, tab_prefix);

		if (BP_IS_HOLE(bp)) {
			(void) printf("\t\t\tLSIZE 0x%llx\n",
			    (u_longlong_t)BP_GET_LSIZE(bp));
			(void) printf("%s<hole>\n", tab_prefix);
			return;
		}
		if (bp->blk_birth < arg->pra_claim_txg) {
			(void) printf("%s<block already committed>\n",
			    tab_prefix);
			return;
		}

		SET_BOOKMARK(&zb, dmu_objset_id(arg->pra_os),
		    lr->lr_foid, ZB_ZIL_LEVEL,
		    lr->lr_offset / BP_GET_LSIZE(bp));

		data = abd_alloc(BP_GET_LSIZE(bp), B_FALSE);
		error = zio_wait(zio_read(NULL, arg->pra_spa,
		    bp, data, BP_GET_LSIZE(bp), NULL, NULL,
		    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL, &zb));
		if (error)
			goto out;
	} else {
		/* data is stored after the end of the lr_write record */
		data = abd_alloc(lr->lr_length, B_FALSE);
		abd_copy_from_buf(data, lr + 1, lr->lr_length);
	}

	(void) printf("%s", tab_prefix);
	(void) abd_iterate_func(data,
	    0, MIN(lr->lr_length, (verbose < 6 ? 20 : SPA_MAXBLOCKSIZE)),
	    zil_prt_rec_write_cb, NULL);
	(void) printf("\n");

out:
	abd_free(data);
}

/* ARGSUSED */
static void
zil_prt_rec_truncate(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_truncate_t *lr = (const lr_truncate_t *)lrc;

	(void) printf("%sfoid %llu, offset 0x%llx, length 0x%llx\n", tab_prefix,
	    (u_longlong_t)lr->lr_foid, (longlong_t)lr->lr_offset,
	    (u_longlong_t)lr->lr_length);
}

/* ARGSUSED */
static void
zil_prt_rec_setattr(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_setattr_t *lr = (const lr_setattr_t *)lrc;
	time_t atime = (time_t)lr->lr_atime[0];
	time_t mtime = (time_t)lr->lr_mtime[0];

	(void) printf("%sfoid %llu, mask 0x%llx\n", tab_prefix,
	    (u_longlong_t)lr->lr_foid, (u_longlong_t)lr->lr_mask);

	if (lr->lr_mask & AT_MODE) {
		(void) printf("%sAT_MODE  %llo\n", tab_prefix,
		    (longlong_t)lr->lr_mode);
	}

	if (lr->lr_mask & AT_UID) {
		(void) printf("%sAT_UID   %llu\n", tab_prefix,
		    (u_longlong_t)lr->lr_uid);
	}

	if (lr->lr_mask & AT_GID) {
		(void) printf("%sAT_GID   %llu\n", tab_prefix,
		    (u_longlong_t)lr->lr_gid);
	}

	if (lr->lr_mask & AT_SIZE) {
		(void) printf("%sAT_SIZE  %llu\n", tab_prefix,
		    (u_longlong_t)lr->lr_size);
	}

	if (lr->lr_mask & AT_ATIME) {
		(void) printf("%sAT_ATIME %llu.%09llu %s", tab_prefix,
		    (u_longlong_t)lr->lr_atime[0],
		    (u_longlong_t)lr->lr_atime[1],
		    ctime(&atime));
	}

	if (lr->lr_mask & AT_MTIME) {
		(void) printf("%sAT_MTIME %llu.%09llu %s", tab_prefix,
		    (u_longlong_t)lr->lr_mtime[0],
		    (u_longlong_t)lr->lr_mtime[1],
		    ctime(&mtime));
	}
}

/* ARGSUSED */
static void
zil_prt_rec_acl(const print_record_arg_t *arg, int txtype, const lr_t *lrc)
{
	const lr_acl_t *lr = (const lr_acl_t *)lrc;

	(void) printf("%sfoid %llu, aclcnt %llu\n", tab_prefix,
	    (u_longlong_t)lr->lr_foid, (u_longlong_t)lr->lr_aclcnt);
}

typedef void (*zil_prt_rec_func_t)(const print_record_arg_t *, int,
    const lr_t *);
typedef struct zil_rec_info {
	zil_prt_rec_func_t	zri_print;
	const char		*zri_name;
	uint64_t		zri_count;
} zil_rec_info_t;

static zil_rec_info_t zil_rec_info[TX_MAX_TYPE] = {
	{.zri_print = NULL,		    .zri_name = "Total              "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_CREATE          "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_MKDIR           "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_MKXATTR         "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_SYMLINK         "},
	{.zri_print = zil_prt_rec_remove,   .zri_name = "TX_REMOVE          "},
	{.zri_print = zil_prt_rec_remove,   .zri_name = "TX_RMDIR           "},
	{.zri_print = zil_prt_rec_link,	    .zri_name = "TX_LINK            "},
	{.zri_print = zil_prt_rec_rename,   .zri_name = "TX_RENAME          "},
	{.zri_print = zil_prt_rec_write,    .zri_name = "TX_WRITE           "},
	{.zri_print = zil_prt_rec_truncate, .zri_name = "TX_TRUNCATE        "},
	{.zri_print = zil_prt_rec_setattr,  .zri_name = "TX_SETATTR         "},
	{.zri_print = zil_prt_rec_acl,	    .zri_name = "TX_ACL_V0          "},
	{.zri_print = zil_prt_rec_acl,	    .zri_name = "TX_ACL_ACL         "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_CREATE_ACL      "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_CREATE_ATTR     "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_CREATE_ACL_ATTR "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_MKDIR_ACL       "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_MKDIR_ATTR      "},
	{.zri_print = zil_prt_rec_create,   .zri_name = "TX_MKDIR_ACL_ATTR  "},
	{.zri_print = zil_prt_rec_write,    .zri_name = "TX_WRITE2          "},
};

/* ARGSUSED */
static int
print_log_record(const lr_t *lr, void *varg)
{
	print_record_arg_t *arg = varg;

	int txtype;
	int verbose = MAX(dump_opt['d'], dump_opt['i']);

	/* reduce size of txtype to strip off TX_CI bit */
	txtype = lr->lrc_txtype;

	ASSERT(txtype != 0 && (uint_t)txtype < TX_MAX_TYPE);
	ASSERT(lr->lrc_txg);

	(void) printf("\t\t%s%s len %6llu, txg %llu, seq %llu\n",
	    (lr->lrc_txtype & TX_CI) ? "CI-" : "",
	    zil_rec_info[txtype].zri_name,
	    (u_longlong_t)lr->lrc_reclen,
	    (u_longlong_t)lr->lrc_txg,
	    (u_longlong_t)lr->lrc_seq);

	if (txtype && verbose >= 3) {
		if (!arg->pra_os->os_encrypted) {
			zil_rec_info[txtype].zri_print(arg, txtype, lr);
		} else {
			(void) printf("%s(encrypted)\n", tab_prefix);
		}
	}

	zil_rec_info[txtype].zri_count++;
	zil_rec_info[0].zri_count++;

	return (0);
}

/* ARGSUSED */
static int
print_log_block(const blkptr_t *bp, void *varg)
{
	print_record_arg_t *arg = varg;

	char blkbuf[BP_SPRINTF_LEN + 10];
	int verbose = MAX(dump_opt['d'], dump_opt['i']);
	const char *claim;

	if (verbose <= 3)
		return (0);

	if (verbose >= 5) {
		(void) strcpy(blkbuf, ", ");
		snprintf_blkptr(blkbuf + strlen(blkbuf),
		    sizeof (blkbuf) - strlen(blkbuf), bp);
	} else {
		blkbuf[0] = '\0';
	}

	if (arg->pra_claim_txg != 0)
		claim = "already claimed";
	else if (bp->blk_birth >= spa_min_claim_txg(arg->pra_spa))
		claim = "will claim";
	else
		claim = "won't claim";

	(void) printf("\tBlock seqno %llu, %s%s\n",
	    (u_longlong_t)bp->blk_cksum.zc_word[ZILLWB_ZC_SEQ], claim, blkbuf);

	return (0);
}

static void
print_log_stats(int verbose)
{
	unsigned i, w, p10;

	if (verbose > 3)
		(void) printf("\n");

	if (zil_rec_info[0].zri_count == 0)
		return;

	for (w = 1, p10 = 10; zil_rec_info[0].zri_count >= p10; p10 *= 10)
		w++;

	for (i = 0; i < TX_MAX_TYPE; i++)
		if (zil_rec_info[i].zri_count || verbose >= 3)
			(void) printf("\t\t%s %*llu\n",
			    zil_rec_info[i].zri_name, w,
			    (u_longlong_t)zil_rec_info[i].zri_count);
	(void) printf("\n");
}

#include <libnvpair.h>

/* ARGSUSED */
void
dump_intent_log(zilog_t *super)
{
	if (super->zl_vtable != &zillwb_vtable) {
		/*
		 * TODO refactor this as a vfunc that only exists when compiling
		 * zdb.
		 */
		zh_kind_t kind = 0;
		VERIFY0(zil_kind_specific_data_from_header(super->zl_spa, super->zl_header, NULL, NULL, NULL, &kind));
		(void) printf("\n    ZIL kind %u dump not supported by zdb\n", kind);
		return;
	}
	zilog_lwb_t *zilog = zillwb_downcast(super);

	const zil_header_lwb_t *zh =
	    zillwb_zil_header_const(zilog);
	int verbose = MAX(dump_opt['d'], dump_opt['i']);
	int i;

	if (BP_IS_HOLE(&zh->zh_log) || verbose < 1)
		return;

	(void) printf("\n    ZIL header: claim_txg %llu, "
	    "claim_blk_seq %llu, claim_lr_seq %llu",
	    (u_longlong_t)zh->zh_claim_txg,
	    (u_longlong_t)zh->zh_claim_blk_seq,
	    (u_longlong_t)zh->zh_claim_lr_seq);
	(void) printf(" replay_seq %llu, flags 0x%llx\n",
	    (u_longlong_t)zh->zh_replay_seq, (u_longlong_t)zh->zh_flags);

	for (i = 0; i < TX_MAX_TYPE; i++)
		zil_rec_info[i].zri_count = 0;

	/* see comment in zil_claim() or zil_check_log_chain() */
	if (zilog->zl_super.zl_spa->spa_uberblock.ub_checkpoint_txg != 0 &&
	    zh->zh_claim_txg == 0)
		return;

	if (verbose >= 2) {
		(void) printf("\n");
		print_record_arg_t arg = {
			.pra_os = zilog->zl_super.zl_os,
			.pra_spa = zilog->zl_super.zl_spa,
			.pra_claim_txg = zillwb_zil_header_const(zilog)->zh_claim_txg,
		};
		(void) zillwb_parse_phys(zilog->zl_super.zl_spa, zh,
		    print_log_block, print_log_record, &arg, B_FALSE,
		    ZIO_PRIORITY_SYNC_READ, NULL);
		print_log_stats(verbose);
	}
}
