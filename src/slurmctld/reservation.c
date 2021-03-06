/*****************************************************************************\
 *  reservation.c - resource reservation management
 *****************************************************************************
 *  Copyright (C) 2009-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov> et. al.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://computing.llnl.gov/linux/slurm/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif				/* WITH_PTHREADS */

#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/hostlist.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/node_select.h"
#include "src/common/pack.h"
#include "src/common/parse_time.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/uid.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmctld/licenses.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"

#define ONE_YEAR	(365 * 24 * 60 * 60)
#define RESV_MAGIC	0x3b82

/* Change RESV_STATE_VERSION value when changing the state save format
 * Add logic to permit reading of the previous version's state in order
 * to avoid losing reservations between releases major SLURM updates. */
#define RESV_STATE_VERSION      "VER003"

time_t    last_resv_update = (time_t) 0;
List      resv_list = (List) NULL;
uint32_t  resv_over_run;
uint32_t  top_suffix = 0;
#ifdef HAVE_BG
uint32_t  cnodes_per_bp = 0;
#endif

static void _advance_resv_time(slurmctld_resv_t *resv_ptr);
static void _advance_time(time_t *res_time, int day_cnt);
static int  _build_account_list(char *accounts, int *account_cnt,
				char ***account_list);
static int  _build_uid_list(char *users, int *user_cnt, uid_t **user_list);
static void _clear_job_resv(slurmctld_resv_t *resv_ptr);
static slurmctld_resv_t *_copy_resv(slurmctld_resv_t *resv_orig_ptr);
static void _del_resv_rec(void *x);
static void _dump_resv_req(resv_desc_msg_t *resv_ptr, char *mode);
static int  _find_resv_id(void *x, void *key);
static int  _find_resv_name(void *x, void *key);
static void _generate_resv_id(void);
static void _generate_resv_name(resv_desc_msg_t *resv_ptr);
static uint32_t _get_job_duration(struct job_record *job_ptr);
static bool _is_account_valid(char *account);
static bool _is_resv_used(slurmctld_resv_t *resv_ptr);
static bool _job_overlap(time_t start_time, uint16_t flags,
			 bitstr_t *node_bitmap);
static List _list_dup(List license_list);
static int  _open_resv_state_file(char **state_file);
static void _pack_resv(slurmctld_resv_t *resv_ptr, Buf buffer,
		       bool internal);
static bitstr_t *_pick_idle_nodes(bitstr_t *avail_nodes,
				  resv_desc_msg_t *resv_desc_ptr);
static int  _post_resv_create(slurmctld_resv_t *resv_ptr);
static int  _post_resv_delete(slurmctld_resv_t *resv_ptr);
static int  _post_resv_update(slurmctld_resv_t *resv_ptr,
			      slurmctld_resv_t *old_resv_ptr);
static int  _resize_resv(slurmctld_resv_t *resv_ptr, uint32_t node_cnt);
static bool _resv_overlap(time_t start_time, time_t end_time,
			  uint16_t flags, bitstr_t *node_bitmap,
			  slurmctld_resv_t *this_resv_ptr);
static int  _select_nodes(resv_desc_msg_t *resv_desc_ptr,
			  struct part_record **part_ptr,
			  bitstr_t **resv_bitmap);
static int  _set_assoc_list(slurmctld_resv_t *resv_ptr);
static void _set_cpu_cnt(slurmctld_resv_t *resv_ptr);
static void _set_nodes_maint(slurmctld_resv_t *resv_ptr, time_t now);
static void _swap_resv(slurmctld_resv_t *resv_backup,
		       slurmctld_resv_t *resv_ptr);
static int  _update_account_list(slurmctld_resv_t *resv_ptr,
				 char *accounts);
static int  _update_uid_list(slurmctld_resv_t *resv_ptr, char *users);
static void _validate_all_reservations(void);
static int  _valid_job_access_resv(struct job_record *job_ptr,
				   slurmctld_resv_t *resv_ptr);
static bool _validate_one_reservation(slurmctld_resv_t *resv_ptr);
static void _validate_node_choice(slurmctld_resv_t *resv_ptr);

/* Advance res_time by the specified day count,
 * account for daylight savings time */
static void _advance_time(time_t *res_time, int day_cnt)
{
	time_t save_time = *res_time;
	struct tm time_tm;

	localtime_r(res_time, &time_tm);
	time_tm.tm_isdst = -1;
	time_tm.tm_mday += day_cnt;
	*res_time = mktime(&time_tm);
	if (*res_time == (time_t)(-1)) {
		error("Could not compute reservation time %lu",
		      (long unsigned int) save_time);
		*res_time = save_time + (24 * 60 * 60);
	}
}

static List _list_dup(List license_list)
{
	ListIterator iter;
	licenses_t *license_src, *license_dest;
	List lic_list = (List) NULL;

	if (!license_list)
		return lic_list;

	lic_list = list_create(license_free_rec);
	if (lic_list == NULL)
		fatal("list_create malloc failure");
	iter = list_iterator_create(license_list);
	if (!iter)
		fatal("list_interator_create malloc failure");
	while ((license_src = (licenses_t *) list_next(iter))) {
		license_dest = xmalloc(sizeof(licenses_t));
		license_dest->name = xstrdup(license_src->name);
		license_dest->used = license_src->used;
		list_push(lic_list, license_dest);
	}
	list_iterator_destroy(iter);
	return lic_list;
}

static slurmctld_resv_t *_copy_resv(slurmctld_resv_t *resv_orig_ptr)
{
	slurmctld_resv_t *resv_copy_ptr;
	int i;

	xassert(resv_orig_ptr->magic == RESV_MAGIC);
	resv_copy_ptr = xmalloc(sizeof(slurmctld_resv_t));
	resv_copy_ptr->accounts = xstrdup(resv_orig_ptr->accounts);
	resv_copy_ptr->account_cnt = resv_orig_ptr->account_cnt;
	resv_copy_ptr->account_list = xmalloc(sizeof(char *) *
					      resv_orig_ptr->account_cnt);
	for (i=0; i<resv_copy_ptr->account_cnt; i++) {
		resv_copy_ptr->account_list[i] =
				xstrdup(resv_orig_ptr->account_list[i]);
	}
	resv_copy_ptr->assoc_list = xstrdup(resv_orig_ptr->assoc_list);
	resv_copy_ptr->cpu_cnt = resv_orig_ptr->cpu_cnt;
	resv_copy_ptr->end_time = resv_orig_ptr->end_time;
	resv_copy_ptr->features = xstrdup(resv_orig_ptr->features);
	resv_copy_ptr->flags = resv_orig_ptr->flags;
	resv_copy_ptr->job_pend_cnt = resv_orig_ptr->job_pend_cnt;
	resv_copy_ptr->job_run_cnt = resv_orig_ptr->job_run_cnt;
	resv_copy_ptr->licenses = xstrdup(resv_orig_ptr->licenses);
	resv_copy_ptr->license_list = _list_dup(resv_orig_ptr->
						license_list);
	resv_copy_ptr->magic = resv_orig_ptr->magic;
	resv_copy_ptr->name = xstrdup(resv_orig_ptr->name);
	resv_copy_ptr->node_bitmap = bit_copy(resv_orig_ptr->node_bitmap);
	resv_copy_ptr->node_cnt = resv_orig_ptr->node_cnt;
	resv_copy_ptr->node_list = xstrdup(resv_orig_ptr->node_list);
	resv_copy_ptr->partition = xstrdup(resv_orig_ptr->partition);
	resv_copy_ptr->part_ptr = resv_orig_ptr->part_ptr;
	resv_copy_ptr->resv_id = resv_orig_ptr->resv_id;
	resv_copy_ptr->start_time = resv_orig_ptr->start_time;
	resv_copy_ptr->start_time_first = resv_orig_ptr->start_time_first;
	resv_copy_ptr->start_time_prev = resv_orig_ptr->start_time_prev;
	resv_copy_ptr->users = xstrdup(resv_orig_ptr->users);
	resv_copy_ptr->user_cnt = resv_orig_ptr->user_cnt;
	resv_copy_ptr->user_list = xmalloc(sizeof(uid_t) *
					   resv_orig_ptr->user_cnt);
	for (i=0; i<resv_copy_ptr->user_cnt; i++)
		resv_copy_ptr->user_list[i] = resv_orig_ptr->user_list[i];

	return resv_copy_ptr;
}

/* Swaping the contents of two reservation records */
static void _swap_resv(slurmctld_resv_t *resv_backup,
		       slurmctld_resv_t *resv_ptr)
{
	resv_desc_msg_t *resv_copy_ptr;

	xassert(resv_backup->magic == RESV_MAGIC);
	xassert(resv_ptr->magic    == RESV_MAGIC);
	resv_copy_ptr = xmalloc(sizeof(slurmctld_resv_t));
	memcpy(resv_copy_ptr, resv_backup, sizeof(slurmctld_resv_t));
	memcpy(resv_backup, resv_ptr, sizeof(slurmctld_resv_t));
	memcpy(resv_ptr, resv_copy_ptr, sizeof(slurmctld_resv_t));
	xfree(resv_copy_ptr);
}

static void _del_resv_rec(void *x)
{
	int i;
	slurmctld_resv_t *resv_ptr = (slurmctld_resv_t *) x;

	if (resv_ptr) {
		xassert(resv_ptr->magic == RESV_MAGIC);
		resv_ptr->magic = 0;
		xfree(resv_ptr->accounts);
		for (i=0; i<resv_ptr->account_cnt; i++)
			xfree(resv_ptr->account_list[i]);
		xfree(resv_ptr->account_list);
		xfree(resv_ptr->assoc_list);
		xfree(resv_ptr->features);
		if (resv_ptr->license_list)
			list_destroy(resv_ptr->license_list);
		xfree(resv_ptr->licenses);
		xfree(resv_ptr->name);
		FREE_NULL_BITMAP(resv_ptr->node_bitmap);
		xfree(resv_ptr->node_list);
		xfree(resv_ptr->partition);
		xfree(resv_ptr->users);
		xfree(resv_ptr->user_list);
		xfree(resv_ptr);
	}
}

static int _find_resv_id(void *x, void *key)
{
	slurmctld_resv_t *resv_ptr = (slurmctld_resv_t *) x;
	uint32_t *resv_id = (uint32_t *) key;

	xassert(resv_ptr->magic == RESV_MAGIC);

	if (resv_ptr->resv_id != *resv_id)
		return 0;
	else
		return 1;	/* match */
}

static int _find_resv_name(void *x, void *key)
{
	slurmctld_resv_t *resv_ptr = (slurmctld_resv_t *) x;

	xassert(resv_ptr->magic == RESV_MAGIC);

	if (strcmp(resv_ptr->name, (char *) key))
		return 0;
	else
		return 1;	/* match */
}

static void _dump_resv_req(resv_desc_msg_t *resv_ptr, char *mode)
{

	char start_str[32] = "-1", end_str[32] = "-1", *flag_str = NULL;
	int duration;

	if (!(slurm_get_debug_flags() & DEBUG_FLAG_RESERVATION))
		return;

	if (resv_ptr->start_time != (time_t) NO_VAL) {
		slurm_make_time_str(&resv_ptr->start_time,
				    start_str, sizeof(start_str));
	}
	if (resv_ptr->end_time != (time_t) NO_VAL) {
		slurm_make_time_str(&resv_ptr->end_time,
				    end_str,  sizeof(end_str));
	}
	if (resv_ptr->flags != (uint16_t) NO_VAL)
		flag_str = reservation_flags_string(resv_ptr->flags);

	if (resv_ptr->duration == NO_VAL)
		duration = -1;
	else
		duration = resv_ptr->duration;

	info("%s: Name=%s StartTime=%s EndTime=%s Duration=%d "
	     "Flags=%s NodeCnt=%d NodeList=%s Features=%s "
	     "PartitionName=%s Users=%s Accounts=%s Licenses=%s",
	     mode, resv_ptr->name, start_str, end_str, duration,
	     flag_str, resv_ptr->node_cnt, resv_ptr->node_list,
	     resv_ptr->features, resv_ptr->partition,
	     resv_ptr->users, resv_ptr->accounts, resv_ptr->licenses);

	xfree(flag_str);
}

static void _generate_resv_id(void)
{
	while (1) {
		if (top_suffix >= 9999)
			top_suffix = 1;		/* wrap around */
		else
			top_suffix++;
		if (!list_find_first(resv_list, _find_resv_id, &top_suffix))
			break;
	}
}

static void _generate_resv_name(resv_desc_msg_t *resv_ptr)
{
	char *key, *name, *sep;
	int len;

	/* Generate name prefix, based upon the first account
	 * name if provided otherwise first user name */
	if (resv_ptr->accounts && resv_ptr->accounts[0])
		key = resv_ptr->accounts;
	else
		key = resv_ptr->users;
	sep = strchr(key, ',');
	if (sep)
		len = sep - key;
	else
		len = strlen(key);
	name = xmalloc(len + 16);
	strncpy(name, key, len);

	xstrfmtcat(name, "_%d", top_suffix);
	len++;

	resv_ptr->name = name;
}

/* Validate an account name */
static bool _is_account_valid(char *account)
{
	slurmdb_association_rec_t assoc_rec, *assoc_ptr;

	if (!(accounting_enforce & ACCOUNTING_ENFORCE_ASSOCS))
		return true;	/* don't worry about account validity */

	memset(&assoc_rec, 0, sizeof(slurmdb_association_rec_t));
	assoc_rec.uid       = NO_VAL;
	assoc_rec.acct      = account;

	if (assoc_mgr_fill_in_assoc(acct_db_conn, &assoc_rec,
				    accounting_enforce, &assoc_ptr)) {
		return false;
	}
	return true;
}

static int _append_assoc_list(List assoc_list, slurmdb_association_rec_t *assoc)
{
	int rc = ESLURM_INVALID_ACCOUNT;
	slurmdb_association_rec_t *assoc_ptr = NULL;
	if (assoc_mgr_fill_in_assoc(
		    acct_db_conn, assoc,
		    accounting_enforce,
		    &assoc_ptr)) {
		if(accounting_enforce & ACCOUNTING_ENFORCE_ASSOCS) {
			error("No association for user %u and account %s",
			      assoc->uid, assoc->acct);
		} else {
			verbose("No association for user %u and account %s",
				assoc->uid, assoc->acct);
			rc = SLURM_SUCCESS;
		}

	}
	if (assoc_ptr) {
		list_append(assoc_list, assoc_ptr);
		rc = SLURM_SUCCESS;
	}

	return rc;
}
/* Set a association list based upon accounts and users */
static int _set_assoc_list(slurmctld_resv_t *resv_ptr)
{
	int rc = SLURM_SUCCESS, i = 0, j = 0;
	List assoc_list = NULL;
	slurmdb_association_rec_t assoc, *assoc_ptr = NULL;

	/* no need to do this if we can't ;) */
	if (!association_based_accounting)
		return rc;

	assoc_list = list_create(NULL);

	memset(&assoc, 0, sizeof(slurmdb_association_rec_t));
	xfree(resv_ptr->assoc_list);

	if (resv_ptr->user_cnt) {
		for (i=0; i < resv_ptr->user_cnt; i++) {
			if (resv_ptr->account_cnt) {
				for (j=0; j < resv_ptr->account_cnt; j++) {
					memset(&assoc, 0,
					       sizeof(slurmdb_association_rec_t));
					assoc.uid = resv_ptr->user_list[i];
					assoc.acct = resv_ptr->account_list[j];
					rc = _append_assoc_list(assoc_list,
								&assoc);
					if (rc != SLURM_SUCCESS)
						goto end_it;
				}
			} else {
				memset(&assoc, 0,
				       sizeof(slurmdb_association_rec_t));
				assoc.uid = resv_ptr->user_list[i];
				rc = assoc_mgr_get_user_assocs(
					    acct_db_conn, &assoc,
					    accounting_enforce, assoc_list);
				if (rc != SLURM_SUCCESS) {
					rc = ESLURM_INVALID_ACCOUNT;
					goto end_it;
				}
			}
		}
	} else if (resv_ptr->account_cnt) {
		for (i=0; i < resv_ptr->account_cnt; i++) {
			memset(&assoc, 0,
			       sizeof(slurmdb_association_rec_t));
			assoc.uid = (uint32_t)NO_VAL;
			assoc.acct = resv_ptr->account_list[i];
			if ((rc = _append_assoc_list(assoc_list, &assoc))
			    != SLURM_SUCCESS) {
				goto end_it;
			}
		}
	} else if(accounting_enforce & ACCOUNTING_ENFORCE_ASSOCS) {
		error("We need at least 1 user or 1 account to "
		      "create a reservtion.");
		rc = SLURM_ERROR;
	}

	if (list_count(assoc_list)) {
		ListIterator itr = list_iterator_create(assoc_list);
		if (!itr)
			fatal("malloc: list_iterator_create");
		xfree(resv_ptr->assoc_list);	/* clear for modify */
		while ((assoc_ptr = list_next(itr))) {
			if (resv_ptr->assoc_list) {
				xstrfmtcat(resv_ptr->assoc_list, "%u,",
					   assoc_ptr->id);
			} else {
				xstrfmtcat(resv_ptr->assoc_list, ",%u,",
					   assoc_ptr->id);
			}
		}
		list_iterator_destroy(itr);
	}

end_it:
	list_destroy(assoc_list);
	return rc;
}

/* Post reservation create */
static int _post_resv_create(slurmctld_resv_t *resv_ptr)
{
	int rc = SLURM_SUCCESS;
	slurmdb_reservation_rec_t resv;
	char temp_bit[BUF_SIZE];

	memset(&resv, 0, sizeof(slurmdb_reservation_rec_t));

	resv.assocs = resv_ptr->assoc_list;
	resv.cluster = slurmctld_cluster_name;
	resv.cpus = resv_ptr->cpu_cnt;
	resv.flags = resv_ptr->flags;
	resv.id = resv_ptr->resv_id;
	resv.name = resv_ptr->name;
	resv.nodes = resv_ptr->node_list;
	if (resv_ptr->node_bitmap) {
		resv.node_inx = bit_fmt(temp_bit, sizeof(temp_bit),
					resv_ptr->node_bitmap);
	}

	resv.time_end = resv_ptr->end_time;
	resv.time_start = resv_ptr->start_time;

	rc = acct_storage_g_add_reservation(acct_db_conn, &resv);

	return rc;
}

/* Note that a reservation has been deleted */
static int _post_resv_delete(slurmctld_resv_t *resv_ptr)
{
	int rc = SLURM_SUCCESS;
	slurmdb_reservation_rec_t resv;
	memset(&resv, 0, sizeof(slurmdb_reservation_rec_t));

	resv.cluster = slurmctld_cluster_name;
	resv.id = resv_ptr->resv_id;
	resv.name = resv_ptr->name;
	resv.time_start = resv_ptr->start_time;
	/* This is just a time stamp here to delete if the reservation
	 * hasn't started yet so we don't get trash records in the
	 * database if said database isn't up right now */
	resv.time_start_prev = time(NULL);
	rc = acct_storage_g_remove_reservation(acct_db_conn, &resv);

	return rc;
}

/* Note that a reservation has been updated */
static int _post_resv_update(slurmctld_resv_t *resv_ptr,
			     slurmctld_resv_t *old_resv_ptr)
{
	int rc = SLURM_SUCCESS;
	slurmdb_reservation_rec_t resv;
	char temp_bit[BUF_SIZE];

	memset(&resv, 0, sizeof(slurmdb_reservation_rec_t));

	resv.cluster = slurmctld_cluster_name;
	resv.id = resv_ptr->resv_id;
	resv.time_end = resv_ptr->end_time;

	if(!old_resv_ptr) {
		resv.assocs = resv_ptr->assoc_list;
		resv.cpus = resv_ptr->cpu_cnt;
		resv.flags = resv_ptr->flags;
		resv.nodes = resv_ptr->node_list;
	} else {
		time_t now = time(NULL);

		if(old_resv_ptr->assoc_list && resv_ptr->assoc_list) {
			if(strcmp(old_resv_ptr->assoc_list,
				  resv_ptr->assoc_list))
				resv.assocs = resv_ptr->assoc_list;
		} else if(resv_ptr->assoc_list)
			resv.assocs = resv_ptr->assoc_list;

		if(old_resv_ptr->cpu_cnt != resv_ptr->cpu_cnt)
			resv.cpus = resv_ptr->cpu_cnt;
		else
			resv.cpus = (uint32_t)NO_VAL;

		if(old_resv_ptr->flags != resv_ptr->flags)
			resv.flags = resv_ptr->flags;
		else
			resv.flags = (uint16_t)NO_VAL;

		if(old_resv_ptr->node_list && resv_ptr->node_list) {
			if(strcmp(old_resv_ptr->node_list,
				  resv_ptr->node_list))
				resv.nodes = resv_ptr->node_list;
		} else if(resv_ptr->node_list)
			resv.nodes = resv_ptr->node_list;

		/* Here if the reservation has started already we need
		 * to mark a new start time for it if certain
		 * variables are needed in accounting.  Right now if
		 * the assocs, nodes, flags or cpu count changes we need a
		 * new start time of now. */
		if((resv_ptr->start_time < now)
		   && (resv.assocs
		       || resv.nodes
		       || (resv.flags != (uint16_t)NO_VAL)
		       || (resv.cpus != (uint32_t)NO_VAL))) {
			resv_ptr->start_time_prev = resv_ptr->start_time;
			resv_ptr->start_time = now;
		}
	}
	/* now set the (maybe new) start_times */
	resv.time_start = resv_ptr->start_time;
	resv.time_start_prev = resv_ptr->start_time_prev;

	if (resv.nodes && resv_ptr->node_bitmap) {
		resv.node_inx = bit_fmt(temp_bit, sizeof(temp_bit),
					resv_ptr->node_bitmap);
	}

	rc = acct_storage_g_modify_reservation(acct_db_conn, &resv);

	return rc;
}

/*
 * Validate a comma delimited list of account names and build an array of
 *	them
 * IN account       - a list of account names
 * OUT account_cnt  - number of accounts in the list
 * OUT account_list - list of the account names,
 *		      CALLER MUST XFREE this plus each individual record
 * RETURN 0 on success
 */
static int _build_account_list(char *accounts, int *account_cnt,
			       char ***account_list)
{
	char *last = NULL, *tmp, *tok;
	int ac_cnt = 0, i;
	char **ac_list;

	*account_cnt = 0;
	*account_list = (char **) NULL;

	if (!accounts)
		return ESLURM_INVALID_ACCOUNT;

	i = strlen(accounts);
	ac_list = xmalloc(sizeof(char *) * (i + 2));
	tmp = xstrdup(accounts);
	tok = strtok_r(tmp, ",", &last);
	while (tok) {
		if (!_is_account_valid(tok)) {
			info("Reservation request has invalid account %s",
			     tok);
			goto inval;
		}
		ac_list[ac_cnt++] = xstrdup(tok);
		tok = strtok_r(NULL, ",", &last);
	}
	*account_cnt  = ac_cnt;
	*account_list = ac_list;
	xfree(tmp);
	return SLURM_SUCCESS;

 inval:	for (i=0; i<ac_cnt; i++)
		xfree(ac_list[i]);
	xfree(ac_list);
	xfree(tmp);
	return ESLURM_INVALID_ACCOUNT;
}

/*
 * Update a account list for an existing reservation based upon an
 *	update comma delimited specification of accounts to add (+name),
 *	remove (-name), or set value of
 * IN/OUT resv_ptr - pointer to reservation structure being updated
 * IN accounts     - a list of account names, to set, add, or remove
 * RETURN 0 on success
 */
static int  _update_account_list(slurmctld_resv_t *resv_ptr,
				 char *accounts)
{
	char *last = NULL, *ac_cpy, *tok;
	int ac_cnt = 0, i, j, k;
	int *ac_type, minus_account = 0, plus_account = 0;
	char **ac_list;
	bool found_it;

	if (!accounts)
		return ESLURM_INVALID_ACCOUNT;

	i = strlen(accounts);
	ac_list = xmalloc(sizeof(char *) * (i + 2));
	ac_type = xmalloc(sizeof(int)    * (i + 2));
	ac_cpy = xstrdup(accounts);
	tok = strtok_r(ac_cpy, ",", &last);
	while (tok) {
		if (tok[0] == '-') {
			ac_type[ac_cnt] = 1;	/* minus */
			minus_account = 1;
			tok++;
		} else if (tok[0] == '+') {
			ac_type[ac_cnt] = 2;	/* plus */
			plus_account = 1;
			tok++;
		} else if (tok[0] == '\0') {
			continue;
		} else if (plus_account || minus_account) {
			info("Reservation account expression invalid %s",
			     accounts);
			goto inval;
		} else
			ac_type[ac_cnt] = 3;	/* set */
		if (!_is_account_valid(tok)) {
			info("Reservation request has invalid account %s",
			     tok);
			goto inval;
		}
		ac_list[ac_cnt++] = xstrdup(tok);
		tok = strtok_r(NULL, ",", &last);
	}

	if ((plus_account == 0) && (minus_account == 0)) {
		/* Just a reset of account list */
		xfree(resv_ptr->accounts);
		if (accounts[0] != '\0')
			resv_ptr->accounts = xstrdup(accounts);
		xfree(resv_ptr->account_list);
		resv_ptr->account_list = ac_list;
		resv_ptr->account_cnt = ac_cnt;
		xfree(ac_cpy);
		xfree(ac_type);
		return SLURM_SUCCESS;
	}

	/* Modification of existing account list */
	if (minus_account) {
		if (resv_ptr->account_cnt == 0)
			goto inval;
		for (i=0; i<ac_cnt; i++) {
			if (ac_type[i] != 1)
				continue;
			found_it = false;
			for (j=0; j<resv_ptr->account_cnt; j++) {
				if (strcmp(resv_ptr->account_list[j],
					   ac_list[i])) {
					continue;
				}
				found_it = true;
				xfree(resv_ptr->account_list[j]);
				resv_ptr->account_cnt--;
				for (k=j; k<resv_ptr->account_cnt; k++) {
					resv_ptr->account_list[k] =
						resv_ptr->account_list[k+1];
				}
				break;
			}
			if (!found_it)
				goto inval;
		}
		xfree(resv_ptr->accounts);
		for (i=0; i<resv_ptr->account_cnt; i++) {
			if (i == 0) {
				resv_ptr->accounts = xstrdup(resv_ptr->
							     account_list[i]);
			} else {
				xstrcat(resv_ptr->accounts, ",");
				xstrcat(resv_ptr->accounts,
					resv_ptr->account_list[i]);
			}
		}
	}

	if (plus_account) {
		for (i=0; i<ac_cnt; i++) {
			if (ac_type[i] != 2)
				continue;
			found_it = false;
			for (j=0; j<resv_ptr->account_cnt; j++) {
				if (strcmp(resv_ptr->account_list[j],
					   ac_list[i])) {
					continue;
				}
				found_it = true;
				break;
			}
			if (found_it)
				continue;	/* duplicate entry */
			xrealloc(resv_ptr->account_list,
				 sizeof(char *) * (resv_ptr->account_cnt + 1));
			resv_ptr->account_list[resv_ptr->account_cnt++] =
					xstrdup(ac_list[i]);
		}
		xfree(resv_ptr->accounts);
		for (i=0; i<resv_ptr->account_cnt; i++) {
			if (i == 0) {
				resv_ptr->accounts = xstrdup(resv_ptr->
							     account_list[i]);
			} else {
				xstrcat(resv_ptr->accounts, ",");
				xstrcat(resv_ptr->accounts,
					resv_ptr->account_list[i]);
			}
		}
	}

	for (i=0; i<ac_cnt; i++)
		xfree(ac_list[i]);
	xfree(ac_list);
	xfree(ac_type);
	xfree(ac_cpy);
	return SLURM_SUCCESS;

 inval:	for (i=0; i<ac_cnt; i++)
		xfree(ac_list[i]);
	xfree(ac_list);
	xfree(ac_type);
	xfree(ac_cpy);
	return ESLURM_INVALID_ACCOUNT;
}

/*
 * Validate a comma delimited list of user names and build an array of
 *	their UIDs
 * IN users      - a list of user names
 * OUT user_cnt  - number of UIDs in the list
 * OUT user_list - list of the user's uid, CALLER MUST XFREE;
 * RETURN 0 on success
 */
static int _build_uid_list(char *users, int *user_cnt, uid_t **user_list)
{
	char *last = NULL, *tmp = NULL, *tok;
	int u_cnt = 0, i;
	uid_t *u_list, u_tmp;

	*user_cnt = 0;
	*user_list = (uid_t *) NULL;

	if (!users)
		return ESLURM_USER_ID_MISSING;

	i = strlen(users);
	u_list = xmalloc(sizeof(uid_t) * (i + 2));
	tmp = xstrdup(users);
	tok = strtok_r(tmp, ",", &last);
	while (tok) {
		if (uid_from_string (tok, &u_tmp) < 0) {
			info("Reservation request has invalid user %s", tok);
			goto inval;
		}
		u_list[u_cnt++] = u_tmp;
		tok = strtok_r(NULL, ",", &last);
	}
	*user_cnt  = u_cnt;
	*user_list = u_list;
	xfree(tmp);
	return SLURM_SUCCESS;

 inval:	xfree(tmp);
	xfree(u_list);
	return ESLURM_USER_ID_MISSING;
}

/*
 * Update a user/uid list for an existing reservation based upon an
 *	update comma delimited specification of users to add (+name),
 *	remove (-name), or set value of
 * IN/OUT resv_ptr - pointer to reservation structure being updated
 * IN users        - a list of user names, to set, add, or remove
 * RETURN 0 on success
 */
static int _update_uid_list(slurmctld_resv_t *resv_ptr, char *users)
{
	char *last = NULL, *u_cpy = NULL, *tmp = NULL, *tok;
	int u_cnt = 0, i, j, k;
	uid_t *u_list, u_tmp;
	int *u_type, minus_user = 0, plus_user = 0;
	char **u_name;
	bool found_it;

	if (!users)
		return ESLURM_USER_ID_MISSING;

	/* Parse the incoming user expression */
	i = strlen(users);
	u_list = xmalloc(sizeof(uid_t)  * (i + 2));
	u_name = xmalloc(sizeof(char *) * (i + 2));
	u_type = xmalloc(sizeof(int)    * (i + 2));
	u_cpy = xstrdup(users);
	tok = strtok_r(u_cpy, ",", &last);
	while (tok) {
		if (tok[0] == '-') {
			u_type[u_cnt] = 1;	/* minus */
			minus_user = 1;
			tok++;
		} else if (tok[0] == '+') {
			u_type[u_cnt] = 2;	/* plus */
			plus_user = 1;
			tok++;
		} else if (tok[0] == '\0') {
			continue;
		} else if (plus_user || minus_user) {
			info("Reservation user expression invalid %s", users);
			goto inval;
		} else
			u_type[u_cnt] = 3;	/* set */

		if (uid_from_string (tok, &u_tmp) < 0) {
			info("Reservation request has invalid user %s", tok);
			goto inval;
		}

		u_name[u_cnt] = tok;
		u_list[u_cnt++] = u_tmp;
		tok = strtok_r(NULL, ",", &last);
	}

	if ((plus_user == 0) && (minus_user == 0)) {
		/* Just a reset of user list */
		xfree(resv_ptr->users);
		xfree(resv_ptr->user_list);
		if (users[0] != '\0')
			resv_ptr->users = xstrdup(users);
		resv_ptr->user_cnt  = u_cnt;
		resv_ptr->user_list = u_list;
		xfree(u_cpy);
		xfree(u_name);
		xfree(u_type);
		return SLURM_SUCCESS;
	}

	/* Modification of existing user list */
	if (minus_user) {
		for (i=0; i<u_cnt; i++) {
			if (u_type[i] != 1)
				continue;
			found_it = false;
			for (j=0; j<resv_ptr->user_cnt; j++) {
				if (resv_ptr->user_list[j] != u_list[i])
					continue;
				found_it = true;
				resv_ptr->user_cnt--;
				for (k=j; k<resv_ptr->user_cnt; k++) {
					resv_ptr->user_list[k] =
						resv_ptr->user_list[k+1];
				}
				break;
			}
			if (!found_it)
				goto inval;
			/* Now we need to remove from users string */
			k = strlen(u_name[i]);
			tmp = resv_ptr->users;
			while ((tok = strstr(tmp, u_name[i]))) {
				if (((tok != resv_ptr->users) &&
				     (tok[-1] != ',')) ||
				    ((tok[k] != '\0') && (tok[k] != ','))) {
					tmp = tok + 1;
					continue;
				}
				if (tok[-1] == ',') {
					tok--;
					k++;
				} else if (tok[k] == ',')
					k++;
				for (j=0; ; j++) {
					tok[j] = tok[j+k];
					if (tok[j] == '\0')
						break;
				}
			}
		}
	}

	if (plus_user) {
		for (i=0; i<u_cnt; i++) {
			if (u_type[i] != 2)
				continue;
			found_it = false;
			for (j=0; j<resv_ptr->user_cnt; j++) {
				if (resv_ptr->user_list[j] != u_list[i])
					continue;
				found_it = true;
				break;
			}
			if (found_it)
				continue;	/* duplicate entry */
			if (resv_ptr->users && resv_ptr->users[0])
				xstrcat(resv_ptr->users, ",");
			xstrcat(resv_ptr->users, u_name[i]);
			xrealloc(resv_ptr->user_list,
				 sizeof(uid_t) * (resv_ptr->user_cnt + 1));
			resv_ptr->user_list[resv_ptr->user_cnt++] =
				u_list[i];
		}
	}
	xfree(u_cpy);
	xfree(u_list);
	xfree(u_name);
	xfree(u_type);
	return SLURM_SUCCESS;

 inval:	xfree(u_cpy);
	xfree(u_list);
	xfree(u_name);
	xfree(u_type);
	return ESLURM_USER_ID_MISSING;
}

/*
 * _pack_resv - dump configuration information about a specific reservation
 *	in machine independent form (for network transmission or state save)
 * IN resv_ptr - pointer to reservation for which information is requested
 * IN/OUT buffer - buffer in which data is placed, pointers automatically
 *	updated
 * IN internal   - true if for internal save state, false for xmit to users
 * NOTE: if you make any changes here be sure to make the corresponding
 *	to _unpack_reserve_info_members() in common/slurm_protocol_pack.c
 *	plus load_all_resv_state() below.
 */
static void _pack_resv(slurmctld_resv_t *resv_ptr, Buf buffer,
		       bool internal)
{
#ifdef HAVE_BG
	uint32_t cnode_cnt;
#endif

	packstr(resv_ptr->accounts,	buffer);
	pack_time(resv_ptr->end_time,	buffer);
	packstr(resv_ptr->features,	buffer);
	packstr(resv_ptr->licenses,	buffer);
	packstr(resv_ptr->name,		buffer);
#ifdef HAVE_BG
	if (!cnodes_per_bp) {
		select_g_alter_node_cnt(SELECT_GET_NODE_SCALING,
					&cnodes_per_bp);
	}
	cnode_cnt = resv_ptr->node_cnt;
	if (cnodes_per_bp && !internal)
		cnode_cnt *= cnodes_per_bp;
	pack32(cnode_cnt,	        buffer);
#else
	pack32(resv_ptr->node_cnt,	buffer);
#endif
	packstr(resv_ptr->node_list,	buffer);
	packstr(resv_ptr->partition,	buffer);
	pack_time(resv_ptr->start_time_first,	buffer);
	pack16(resv_ptr->flags,		buffer);
	packstr(resv_ptr->users,	buffer);

	if (internal) {
		packstr(resv_ptr->assoc_list,	buffer);
		pack32(resv_ptr->cpu_cnt,	buffer);
		pack32(resv_ptr->resv_id,	buffer);
		pack_time(resv_ptr->start_time_prev,	buffer);
		pack_time(resv_ptr->start_time,	buffer);
		pack32(resv_ptr->duration,	buffer);
	} else {
		pack_bit_fmt(resv_ptr->node_bitmap, buffer);
	}
}

/*
 * Test if a new/updated reservation request will overlap running jobs
 * RET true if overlap
 */
static bool _job_overlap(time_t start_time, uint16_t flags,
			 bitstr_t *node_bitmap)
{
	ListIterator job_iterator;
	struct job_record *job_ptr;
	bool overlap = false;

	if (flags & RESERVE_FLAG_IGN_JOBS)	/* ignore job overlap */
		return overlap;

	job_iterator = list_iterator_create(job_list);
	if (!job_iterator)
		fatal("malloc: list_iterator_create");
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (IS_JOB_RUNNING(job_ptr)		&&
		    (job_ptr->end_time > start_time)	&&
		    (bit_overlap(job_ptr->node_bitmap, node_bitmap) > 0)) {
			overlap = true;
			break;
		}
	}
	list_iterator_destroy(job_iterator);

	return overlap;
}

/*
 * Test if a new/updated reservation request overlaps an existing
 *	reservation
 * RET true if overlap
 */
static bool _resv_overlap(time_t start_time, time_t end_time,
			  uint16_t flags, bitstr_t *node_bitmap,
			  slurmctld_resv_t *this_resv_ptr)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	bool rc = false;
	int i, j;
	time_t s_time1, s_time2, e_time1, e_time2;

	if ((flags & RESERVE_FLAG_MAINT)   ||
	    (flags & RESERVE_FLAG_OVERLAP) ||
	    (!node_bitmap))
		return rc;

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");

	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if (resv_ptr == this_resv_ptr)
			continue;	/* skip self */
		if (resv_ptr->node_bitmap == NULL)
			continue;	/* no specific nodes in reservation */
		if (!bit_overlap(resv_ptr->node_bitmap, node_bitmap))
			continue;	/* no overlap */

		for (i=0; ((i<7) && (!rc)); i++) {  /* look forward one week */
			s_time1 = start_time;
			e_time1 = end_time;
			_advance_time(&s_time1, i);
			_advance_time(&e_time1, i);
			for (j=0; ((j<7) && (!rc)); j++) {
				s_time2 = resv_ptr->start_time;
				e_time2 = resv_ptr->end_time;
				_advance_time(&s_time2, j);
				_advance_time(&e_time2, j);
				if ((s_time1 < e_time2) &&
				    (e_time1 > s_time2)) {
					verbose("Reservation overlap with %s",
						resv_ptr->name);
					rc = true;
					break;
				}
				if (!(resv_ptr->flags & RESERVE_FLAG_DAILY))
					break;
			}
			if ((flags & RESERVE_FLAG_DAILY) == 0)
				break;
		}
	}
	list_iterator_destroy(iter);

	return rc;
}

/* Set a reservation's CPU count. Requires that the reservation's
 *	node_bitmap be set. */
static void _set_cpu_cnt(slurmctld_resv_t *resv_ptr)
{
	int i;
	uint32_t cpu_cnt = 0;
	struct node_record *node_ptr = node_record_table_ptr;

	if (!resv_ptr->node_bitmap)
		return;

	for (i=0; i<node_record_count; i++, node_ptr++) {
		if (!bit_test(resv_ptr->node_bitmap, i))
			continue;
		if (slurmctld_conf.fast_schedule)
			cpu_cnt += node_ptr->config_ptr->cpus;
		else
			cpu_cnt += node_ptr->cpus;
	}
	resv_ptr->cpu_cnt = cpu_cnt;
}

/* Create a resource reservation */
extern int create_resv(resv_desc_msg_t *resv_desc_ptr)
{
	int i, rc = SLURM_SUCCESS;
	time_t now = time(NULL);
	struct part_record *part_ptr = NULL;
	bitstr_t *node_bitmap = NULL;
	slurmctld_resv_t *resv_ptr;
	int account_cnt = 0, user_cnt = 0;
	char **account_list = NULL;
	uid_t *user_list = NULL;
	char start_time[32], end_time[32];
	List license_list = (List) NULL;
	char *name1, *name2, *val1, *val2;

	if (!resv_list)
		resv_list = list_create(_del_resv_rec);
	_dump_resv_req(resv_desc_ptr, "create_resv");

	/* Validate the request */
	if (resv_desc_ptr->start_time != (time_t) NO_VAL) {
		if (resv_desc_ptr->start_time < (now - 60)) {
			info("Reservation request has invalid start time");
			rc = ESLURM_INVALID_TIME_VALUE;
			goto bad_parse;
		}
	} else
		resv_desc_ptr->start_time = now;

	if (resv_desc_ptr->end_time != (time_t) NO_VAL) {
		if (resv_desc_ptr->end_time < (now - 60)) {
			info("Reservation request has invalid end time");
			rc = ESLURM_INVALID_TIME_VALUE;
			goto bad_parse;
		}
	} else if (resv_desc_ptr->duration == INFINITE) {
		resv_desc_ptr->end_time = resv_desc_ptr->start_time + ONE_YEAR;
	} else if (resv_desc_ptr->duration) {
		resv_desc_ptr->end_time = resv_desc_ptr->start_time +
					  (resv_desc_ptr->duration * 60);
	} else
		resv_desc_ptr->end_time = INFINITE;
	if (resv_desc_ptr->flags == (uint16_t) NO_VAL)
		resv_desc_ptr->flags = 0;
	else {
		resv_desc_ptr->flags &= RESERVE_FLAG_MAINT    |
					RESERVE_FLAG_OVERLAP  |
					RESERVE_FLAG_IGN_JOBS |
					RESERVE_FLAG_DAILY    |
					RESERVE_FLAG_WEEKLY   |
					RESERVE_FLAG_LIC_ONLY;
	}
	if (resv_desc_ptr->partition) {
		part_ptr = find_part_record(resv_desc_ptr->partition);
		if (!part_ptr) {
			info("Reservation request has invalid partition %s",
			     resv_desc_ptr->partition);
			rc = ESLURM_INVALID_PARTITION_NAME;
			goto bad_parse;
		}
	}
	if ((resv_desc_ptr->accounts == NULL) &&
	    (resv_desc_ptr->users == NULL)) {
		info("Reservation request lacks users or accounts");
		rc = ESLURM_INVALID_ACCOUNT;
		goto bad_parse;
	}
	if (resv_desc_ptr->accounts) {
		rc = _build_account_list(resv_desc_ptr->accounts,
					 &account_cnt, &account_list);
		if (rc)
			goto bad_parse;
	}
	if (resv_desc_ptr->users) {
		rc = _build_uid_list(resv_desc_ptr->users,
				     &user_cnt, &user_list);
		if (rc)
			goto bad_parse;
	}
	if (resv_desc_ptr->licenses) {
		bool valid;
		license_list = license_validate(resv_desc_ptr->licenses,
						&valid);
		if (!valid) {
			info("Reservation request has invalid licenses %s",
			     resv_desc_ptr->licenses);
			rc = ESLURM_INVALID_LICENSES;
			goto bad_parse;
		}
		if ((resv_desc_ptr->node_cnt == NO_VAL) &&
		    (resv_desc_ptr->node_list == NULL))
			resv_desc_ptr->node_cnt = 0;
	}

#ifdef HAVE_BG
	if (!cnodes_per_bp) {
		select_g_alter_node_cnt(SELECT_GET_NODE_SCALING,
					&cnodes_per_bp);
	}
	if ((resv_desc_ptr->node_cnt != NO_VAL) && cnodes_per_bp) {
		/* Convert c-node count to midplane count */
		resv_desc_ptr->node_cnt = (resv_desc_ptr->node_cnt + 
					   cnodes_per_bp - 1) / cnodes_per_bp;
	}
#endif

	if (resv_desc_ptr->node_list) {
		resv_desc_ptr->flags |= RESERVE_FLAG_SPEC_NODES;
		if (strcasecmp(resv_desc_ptr->node_list, "ALL") == 0) {
			node_bitmap = bit_alloc(node_record_count);
			bit_nset(node_bitmap, 0, (node_record_count - 1));
			xfree(resv_desc_ptr->node_list);
			resv_desc_ptr->node_list =
				bitmap2node_name(node_bitmap);
		} else if (node_name2bitmap(resv_desc_ptr->node_list,
					    false, &node_bitmap)) {
			rc = ESLURM_INVALID_NODE_NAME;
			goto bad_parse;
		}
		if (resv_desc_ptr->node_cnt == NO_VAL)
			resv_desc_ptr->node_cnt = 0;
		if (!(resv_desc_ptr->flags & RESERVE_FLAG_OVERLAP) &&
		    _resv_overlap(resv_desc_ptr->start_time,
				  resv_desc_ptr->end_time,
				  resv_desc_ptr->flags, node_bitmap,
				  NULL)) {
			info("Reservation request overlaps another");
			rc = ESLURM_RESERVATION_OVERLAP;
			goto bad_parse;
		}
		resv_desc_ptr->node_cnt = bit_set_count(node_bitmap);
		if (!(resv_desc_ptr->flags & RESERVE_FLAG_IGN_JOBS) &&
		    _job_overlap(resv_desc_ptr->start_time,
				 resv_desc_ptr->flags, node_bitmap)) {
			info("Reservation request overlaps jobs");
			rc = ESLURM_NODES_BUSY;
			goto bad_parse;
		}
	} else if (resv_desc_ptr->node_cnt == NO_VAL) {
		info("Reservation request lacks node specification");
		rc = ESLURM_INVALID_NODE_NAME;
		goto bad_parse;
	} else if ((rc = _select_nodes(resv_desc_ptr, &part_ptr, &node_bitmap))
		   != SLURM_SUCCESS) {
		goto bad_parse;
	}

	_generate_resv_id();
	if (resv_desc_ptr->name) {
		resv_ptr = (slurmctld_resv_t *) list_find_first (resv_list,
				_find_resv_name, resv_desc_ptr->name);
		if (resv_ptr) {
			info("Reservation request name duplication (%s)",
			     resv_desc_ptr->name);
			rc = ESLURM_RESERVATION_INVALID;
			goto bad_parse;
		}
	} else {
		while (1) {
			_generate_resv_name(resv_desc_ptr);
			resv_ptr = (slurmctld_resv_t *)
					list_find_first (resv_list,
					_find_resv_name, resv_desc_ptr->name);
			if (!resv_ptr)
				break;
			_generate_resv_id();	/* makes new suffix */
			/* Same as previously created name, retry */
		}
	}

	/* Create a new reservation record */
	resv_ptr = xmalloc(sizeof(slurmctld_resv_t));
	resv_ptr->accounts	= resv_desc_ptr->accounts;
	resv_desc_ptr->accounts = NULL;		/* Nothing left to free */
	resv_ptr->account_cnt	= account_cnt;
	resv_ptr->account_list	= account_list;
	resv_ptr->duration      = resv_desc_ptr->duration;
	resv_ptr->end_time	= resv_desc_ptr->end_time;
	resv_ptr->features	= resv_desc_ptr->features;
	resv_desc_ptr->features = NULL;		/* Nothing left to free */
	resv_ptr->licenses	= resv_desc_ptr->licenses;
	resv_desc_ptr->licenses = NULL;		/* Nothing left to free */
	resv_ptr->license_list	= license_list;
	resv_ptr->resv_id       = top_suffix;
	xassert(resv_ptr->magic = RESV_MAGIC);	/* Sets value */
	resv_ptr->name		= xstrdup(resv_desc_ptr->name);
	resv_ptr->node_cnt	= resv_desc_ptr->node_cnt;
	resv_ptr->node_list	= resv_desc_ptr->node_list;
	resv_desc_ptr->node_list = NULL;	/* Nothing left to free */
	resv_ptr->node_bitmap	= node_bitmap;	/* May be unset */
	resv_ptr->partition	= resv_desc_ptr->partition;
	resv_desc_ptr->partition = NULL;	/* Nothing left to free */
	resv_ptr->part_ptr	= part_ptr;
	resv_ptr->start_time	= resv_desc_ptr->start_time;
	resv_ptr->start_time_first = resv_ptr->start_time;
	resv_ptr->start_time_prev = resv_ptr->start_time;
	resv_ptr->flags		= resv_desc_ptr->flags;
	resv_ptr->users		= resv_desc_ptr->users;
	resv_ptr->user_cnt	= user_cnt;
	resv_ptr->user_list	= user_list;
	resv_desc_ptr->users 	= NULL;		/* Nothing left to free */
	_set_cpu_cnt(resv_ptr);
	if((rc = _set_assoc_list(resv_ptr)) != SLURM_SUCCESS)
		goto bad_parse;

	/* This needs to be done after all other setup is done. */
	_post_resv_create(resv_ptr);

	slurm_make_time_str(&resv_ptr->start_time, start_time,
			    sizeof(start_time));
	slurm_make_time_str(&resv_ptr->end_time, end_time, sizeof(end_time));
	if (resv_ptr->accounts) {
		name1 = " accounts=";
		val1  = resv_ptr->accounts;
	} else
		name1 = val1 = "";
	if (resv_ptr->users) {
		name2 = " users=";
		val2  = resv_ptr->users;
	} else
		name2 = val2 = "";
	info("sched: Created reservation %s%s%s%s%s nodes=%s start=%s end=%s",
	     resv_ptr->name, name1, val1, name2, val2,
	     resv_ptr->node_list, start_time, end_time);
	list_append(resv_list, resv_ptr);
	last_resv_update = now;
	schedule_resv_save();

	return SLURM_SUCCESS;

 bad_parse:
	for (i=0; i<account_cnt; i++)
		xfree(account_list[i]);
	xfree(account_list);
	if (license_list)
		list_destroy(license_list);
	FREE_NULL_BITMAP(node_bitmap);
	xfree(user_list);
	return rc;
}

/* Purge all reservation data structures */
extern void resv_fini(void)
{
	if (resv_list) {
		list_destroy(resv_list);
		resv_list = (List) NULL;
	}
}

/* Update an exiting resource reservation */
extern int update_resv(resv_desc_msg_t *resv_desc_ptr)
{
	time_t now = time(NULL);
	slurmctld_resv_t *resv_backup, *resv_ptr;
	int error_code = SLURM_SUCCESS, rc;
	char start_time[32], end_time[32];
	char *name1, *name2, *val1, *val2;

	if (!resv_list)
		resv_list = list_create(_del_resv_rec);
	_dump_resv_req(resv_desc_ptr, "update_resv");

	/* Find the specified reservation */
	if ((resv_desc_ptr->name == NULL))
		return ESLURM_RESERVATION_INVALID;
	resv_ptr = (slurmctld_resv_t *) list_find_first (resv_list,
			_find_resv_name, resv_desc_ptr->name);
	if (!resv_ptr)
		return ESLURM_RESERVATION_INVALID;

	/* Make backup to restore state in case of failure */
	resv_backup = _copy_resv(resv_ptr);

	/* Process the request */
	if (resv_desc_ptr->flags != (uint16_t) NO_VAL) {
		if (resv_desc_ptr->flags & RESERVE_FLAG_MAINT)
			resv_ptr->flags |= RESERVE_FLAG_MAINT;
		if (resv_desc_ptr->flags & RESERVE_FLAG_NO_MAINT)
			resv_ptr->flags &= (~RESERVE_FLAG_MAINT);
		if (resv_desc_ptr->flags & RESERVE_FLAG_OVERLAP)
			resv_ptr->flags |= RESERVE_FLAG_OVERLAP;
		if (resv_desc_ptr->flags & RESERVE_FLAG_IGN_JOBS)
			resv_ptr->flags |= RESERVE_FLAG_IGN_JOBS;
		if (resv_desc_ptr->flags & RESERVE_FLAG_NO_IGN_JOB)
			resv_ptr->flags &= (~RESERVE_FLAG_IGN_JOBS);
		if (resv_desc_ptr->flags & RESERVE_FLAG_DAILY)
			resv_ptr->flags |= RESERVE_FLAG_DAILY;
		if (resv_desc_ptr->flags & RESERVE_FLAG_NO_DAILY)
			resv_ptr->flags &= (~RESERVE_FLAG_DAILY);
		if (resv_desc_ptr->flags & RESERVE_FLAG_WEEKLY)
			resv_ptr->flags |= RESERVE_FLAG_WEEKLY;
		if (resv_desc_ptr->flags & RESERVE_FLAG_NO_WEEKLY)
			resv_ptr->flags &= (~RESERVE_FLAG_WEEKLY);
		if (resv_desc_ptr->flags & RESERVE_FLAG_LIC_ONLY)
			resv_ptr->flags |= RESERVE_FLAG_LIC_ONLY;
		if (resv_desc_ptr->flags & RESERVE_FLAG_NO_LIC_ONLY)
			resv_ptr->flags &= (~RESERVE_FLAG_LIC_ONLY);
	}
	if (resv_desc_ptr->partition && (resv_desc_ptr->partition[0] == '\0')){
		/* Clear the partition */
		xfree(resv_desc_ptr->partition);
		xfree(resv_ptr->partition);
		resv_ptr->part_ptr = NULL;
	}
	if (resv_desc_ptr->partition) {
		struct part_record *part_ptr = NULL;
		part_ptr = find_part_record(resv_desc_ptr->partition);
		if (!part_ptr) {
			info("Reservation request has invalid partition (%s)",
			     resv_desc_ptr->partition);
			error_code = ESLURM_INVALID_PARTITION_NAME;
			goto update_failure;
		}
		xfree(resv_ptr->partition);
		resv_ptr->partition	= resv_desc_ptr->partition;
		resv_desc_ptr->partition = NULL; /* Nothing left to free */
		resv_ptr->part_ptr	= part_ptr;
	}
	if (resv_desc_ptr->accounts) {
		rc = _update_account_list(resv_ptr, resv_desc_ptr->accounts);
		if (rc) {
			error_code = rc;
			goto update_failure;
		}
	}
	if (resv_desc_ptr->licenses && (resv_desc_ptr->licenses[0] == '\0')) {
		if ((resv_desc_ptr->node_cnt == 0) ||
		    ((resv_desc_ptr->node_cnt == NO_VAL) &&
		     (resv_ptr->node_cnt == 0))) {
			info("Reservation attempt to clear licenses with "
			     "NodeCount=0");
			rc = ESLURM_INVALID_LICENSES;
			goto update_failure;
		}
		xfree(resv_desc_ptr->licenses);	/* clear licenses */
		xfree(resv_ptr->licenses);
		if (resv_ptr->license_list)
			list_destroy(resv_ptr->license_list);
	}

	if (resv_desc_ptr->licenses) {
		bool valid = true;
		List license_list;
		license_list = license_validate(resv_desc_ptr->licenses,
						&valid);
		if (!valid) {
			info("Reservation invalid license update (%s)",
			     resv_desc_ptr->licenses);
			error_code = ESLURM_INVALID_LICENSES;
			goto update_failure;
		}
		xfree(resv_ptr->licenses);
		resv_ptr->licenses	= resv_desc_ptr->licenses;
		resv_desc_ptr->licenses = NULL; /* Nothing left to free */
		if (resv_ptr->license_list)
			list_destroy(resv_ptr->license_list);
		resv_ptr->license_list  = license_list;
	}
	if (resv_desc_ptr->features && (resv_desc_ptr->features[0] == '\0')) {
		xfree(resv_desc_ptr->features);	/* clear features */
		xfree(resv_ptr->features);
	}
	if (resv_desc_ptr->features) {
		/* To support in the future, the reservation resources would
		 * need to be selected again. For now, administrator can
		 * delete this reservation and create a new one. */
		info("Attempt to change features of reservation %s",
		     resv_desc_ptr->name);
		error_code = ESLURM_NOT_SUPPORTED;
		goto update_failure;
	}
	if (resv_desc_ptr->users) {
		rc = _update_uid_list(resv_ptr, resv_desc_ptr->users);
		if (rc) {
			error_code = rc;
			goto update_failure;
		}
	}
	if ((resv_ptr->users == NULL) && (resv_ptr->accounts == NULL)) {
		info("Reservation request lacks users or accounts");
		error_code = ESLURM_INVALID_ACCOUNT;
		goto update_failure;
	}

	if (resv_desc_ptr->start_time != (time_t) NO_VAL) {
		if (resv_desc_ptr->start_time < (now - 60)) {
			info("Reservation request has invalid start time");
			error_code = ESLURM_INVALID_TIME_VALUE;
			goto update_failure;
		}
		resv_ptr->start_time_prev = resv_ptr->start_time;
		resv_ptr->start_time = resv_desc_ptr->start_time;
		resv_ptr->start_time_first = resv_desc_ptr->start_time;
		if(resv_ptr->duration) {
			resv_ptr->end_time = resv_ptr->start_time_first +
				(resv_ptr->duration * 60);
		}
	}
	if (resv_desc_ptr->end_time != (time_t) NO_VAL) {
		if (resv_desc_ptr->end_time < (now - 60)) {
			info("Reservation request has invalid end time");
			error_code = ESLURM_INVALID_TIME_VALUE;
			goto update_failure;
		}
		resv_ptr->end_time = resv_desc_ptr->end_time;
		resv_ptr->duration = 0;
	}
	if (resv_desc_ptr->duration != NO_VAL) {
		resv_ptr->duration = resv_desc_ptr->duration;
		resv_ptr->end_time = resv_ptr->start_time_first +
				     (resv_desc_ptr->duration * 60);
	}

	if (resv_ptr->start_time >= resv_ptr->end_time) {
		error_code = ESLURM_INVALID_TIME_VALUE;
		goto update_failure;
	}
	if (resv_desc_ptr->node_list &&
	    (resv_desc_ptr->node_list[0] == '\0')) {	/* Clear bitmap */
		resv_ptr->flags &= (~RESERVE_FLAG_SPEC_NODES);
		xfree(resv_desc_ptr->node_list);
		xfree(resv_ptr->node_list);
		FREE_NULL_BITMAP(resv_ptr->node_bitmap);
		resv_ptr->node_bitmap = bit_alloc(node_record_count);
		if (resv_desc_ptr->node_cnt == NO_VAL)
			resv_desc_ptr->node_cnt = resv_ptr->node_cnt;
		resv_ptr->node_cnt = 0;
	}
	if (resv_desc_ptr->node_list) {		/* Change bitmap last */
		bitstr_t *node_bitmap;
		resv_ptr->flags |= RESERVE_FLAG_SPEC_NODES;
		if (strcasecmp(resv_desc_ptr->node_list, "ALL") == 0) {
			node_bitmap = bit_alloc(node_record_count);
			bit_nset(node_bitmap, 0, (node_record_count - 1));
		} else if (node_name2bitmap(resv_desc_ptr->node_list,
					    false, &node_bitmap)) {
			error_code = ESLURM_INVALID_NODE_NAME;
			goto update_failure;
		}
		xfree(resv_ptr->node_list);
		resv_ptr->node_list = resv_desc_ptr->node_list;
		resv_desc_ptr->node_list = NULL;  /* Nothing left to free */
		FREE_NULL_BITMAP(resv_ptr->node_bitmap);
		resv_ptr->node_bitmap = node_bitmap;
		resv_ptr->node_cnt = bit_set_count(resv_ptr->node_bitmap);
	}
	if (resv_desc_ptr->node_cnt != NO_VAL) {
#ifdef HAVE_BG
		if (!cnodes_per_bp) {
			select_g_alter_node_cnt(SELECT_GET_NODE_SCALING,
						&cnodes_per_bp);
		}
		if (cnodes_per_bp) {
			/* Convert c-node count to midplane count */
			resv_desc_ptr->node_cnt = (resv_desc_ptr->node_cnt + 
						   cnodes_per_bp - 1) /
						   cnodes_per_bp;
		}
#endif
		rc = _resize_resv(resv_ptr, resv_desc_ptr->node_cnt);
		if (rc) {
			error_code = rc;
			goto update_failure;
		}
		resv_ptr->node_cnt = bit_set_count(resv_ptr->node_bitmap);
	}
	if (_resv_overlap(resv_ptr->start_time, resv_ptr->end_time,
			  resv_ptr->flags, resv_ptr->node_bitmap, resv_ptr)) {
		info("Reservation request overlaps another");
		error_code = ESLURM_RESERVATION_OVERLAP;
		goto update_failure;
	}
	if (_job_overlap(resv_ptr->start_time, resv_ptr->flags,
			 resv_ptr->node_bitmap)) {
		info("Reservation request overlaps jobs");
		error_code = ESLURM_NODES_BUSY;
		goto update_failure;
	}
	_set_cpu_cnt(resv_ptr);

	/* This needs to be after checks for both account and user
	   changes */
	if ((error_code = _set_assoc_list(resv_ptr)) != SLURM_SUCCESS)
		goto update_failure;

	slurm_make_time_str(&resv_ptr->start_time, start_time,
			    sizeof(start_time));
	slurm_make_time_str(&resv_ptr->end_time, end_time, sizeof(end_time));
	if (resv_ptr->accounts) {
		name1 = " accounts=";
		val1  = resv_ptr->accounts;
	} else
		name1 = val1 = "";
	if (resv_ptr->users) {
		name2 = " users=";
		val2  = resv_ptr->users;
	} else
		name2 = val2 = "";
	info("sched: Updated reservation %s%s%s%s%s nodes=%s licenses=%s "
	     "start=%s end=%s",
	     resv_ptr->name, name1, val1, name2, val2,
	     resv_ptr->node_list, resv_ptr->licenses, start_time, end_time);

	_post_resv_update(resv_ptr, resv_backup);
	_del_resv_rec(resv_backup);
	last_resv_update = now;
	schedule_resv_save();
	return error_code;

update_failure:
	_swap_resv(resv_backup, resv_ptr);
	_del_resv_rec(resv_backup);
	return error_code;
}

/* Determine if a running or pending job is using a reservation */
static bool _is_resv_used(slurmctld_resv_t *resv_ptr)
{
	ListIterator job_iterator;
	struct job_record *job_ptr;
	bool match = false;

	job_iterator = list_iterator_create(job_list);
	if (!job_iterator)
		fatal("malloc: list_iterator_create");
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if ((!IS_JOB_FINISHED(job_ptr)) &&
		    (job_ptr->resv_id == resv_ptr->resv_id)) {
			match = true;
			break;
		}
	}
	list_iterator_destroy(job_iterator);

	return match;
}

/* Clear the reservation points for jobs referencing a defunct reservation */
static void _clear_job_resv(slurmctld_resv_t *resv_ptr)
{
	ListIterator job_iterator;
	struct job_record *job_ptr;

	job_iterator = list_iterator_create(job_list);
	if (!job_iterator)
		fatal("malloc: list_iterator_create");
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (job_ptr->resv_ptr != resv_ptr)
			continue;
		if (!IS_JOB_FINISHED(job_ptr)) {
			info("Job %u linked to defunct reservation %s, "
			     "clearing that reservation",
			     job_ptr->job_id, job_ptr->resv_name);
		}
		job_ptr->resv_id = 0;
		job_ptr->resv_ptr = NULL;
		xfree(job_ptr->resv_name);
	}
	list_iterator_destroy(job_iterator);
}

/* Delete an exiting resource reservation */
extern int delete_resv(reservation_name_msg_t *resv_desc_ptr)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	int rc = SLURM_SUCCESS;
	time_t now = time(NULL);

	if (slurm_get_debug_flags() & DEBUG_FLAG_RESERVATION)
		info("delete_resv: Name=%s", resv_desc_ptr->name);

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if (strcmp(resv_ptr->name, resv_desc_ptr->name))
			continue;
		if (_is_resv_used(resv_ptr)) {
			rc = ESLURM_RESERVATION_BUSY;
			break;
		}

		if (resv_ptr->maint_set_node) {
			resv_ptr->maint_set_node = false;
			_set_nodes_maint(resv_ptr, now);
			last_node_update = now;
		}

		rc = _post_resv_delete(resv_ptr);
		_clear_job_resv(resv_ptr);
		list_delete_item(iter);
		break;
	}
	list_iterator_destroy(iter);

	if (!resv_ptr) {
		info("Reservation %s not found for deletion",
		     resv_desc_ptr->name);
		return ESLURM_RESERVATION_INVALID;
	}

	last_resv_update = time(NULL);
	schedule_resv_save();
	return rc;
}

/* Dump the reservation records to a buffer */
extern void show_resv(char **buffer_ptr, int *buffer_size, uid_t uid)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	uint32_t resv_packed;
	int tmp_offset;
	Buf buffer;
	time_t now = time(NULL);
	DEF_TIMERS;

	START_TIMER;
	if (!resv_list)
		resv_list = list_create(_del_resv_rec);

	buffer_ptr[0] = NULL;
	*buffer_size = 0;

	buffer = init_buf(BUF_SIZE);

	/* write header: version and time */
	resv_packed = 0;
	pack32(resv_packed, buffer);
	pack_time(now, buffer);

	/* write individual reservation records */
	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if ((slurmctld_conf.private_data & PRIVATE_DATA_RESERVATIONS)
		    && !validate_slurm_user(uid)) {
			int i = 0;
			for (i=0; i<resv_ptr->user_cnt; i++) {
				if (resv_ptr->user_list[i] == uid)
					break;
			}

			if (i >= resv_ptr->user_cnt)
				continue;
		}

		_pack_resv(resv_ptr, buffer, false);
		resv_packed++;
	}
	list_iterator_destroy(iter);

	/* put the real record count in the message body header */
	tmp_offset = get_buf_offset(buffer);
	set_buf_offset(buffer, 0);
	pack32(resv_packed, buffer);
	set_buf_offset(buffer, tmp_offset);

	*buffer_size = get_buf_offset(buffer);
	buffer_ptr[0] = xfer_buf_data(buffer);
	END_TIMER2("show_resv");
}

/* Save the state of all reservations to file */
extern int dump_all_resv_state(void)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	int error_code = 0, log_fd;
	char *old_file, *new_file, *reg_file;
	/* Locks: Read node */
	slurmctld_lock_t resv_read_lock =
	    { READ_LOCK, NO_LOCK, READ_LOCK, NO_LOCK };
	Buf buffer = init_buf(BUF_SIZE);
	DEF_TIMERS;

	START_TIMER;
	if (!resv_list)
		resv_list = list_create(_del_resv_rec);

	/* write header: time */
	packstr(RESV_STATE_VERSION, buffer);
	pack_time(time(NULL), buffer);
	pack32(top_suffix, buffer);

	/* write reservation records to buffer */
	lock_slurmctld(resv_read_lock);
	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter)))
		_pack_resv(resv_ptr, buffer, true);
	list_iterator_destroy(iter);

	old_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(old_file, "/resv_state.old");
	reg_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(reg_file, "/resv_state");
	new_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(new_file, "/resv_state.new");
	unlock_slurmctld(resv_read_lock);

	/* write the buffer to file */
	lock_state_files();
	log_fd = creat(new_file, 0600);
	if (log_fd < 0) {
		error("Can't save state, error creating file %s, %m",
		      new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount, rc;
		char *data = (char *)get_buf_data(buffer);

		while (nwrite > 0) {
			amount = write(log_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				error_code = errno;
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}
		rc = fsync_and_close(log_fd, "reservation");
		if (rc && !error_code)
			error_code = rc;
	}
	if (error_code)
		(void) unlink(new_file);
	else {			/* file shuffle */
		(void) unlink(old_file);
		if(link(reg_file, old_file))
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		(void) unlink(reg_file);
		if(link(new_file, reg_file))
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		(void) unlink(new_file);
	}
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);
	unlock_state_files();

	free_buf(buffer);
	END_TIMER2("dump_all_resv_state");
	return 0;
}

/* Validate one reservation record, return true if good */
static bool _validate_one_reservation(slurmctld_resv_t *resv_ptr)
{
	if ((resv_ptr->name == NULL) || (resv_ptr->name[0] == '\0')) {
		error("Read reservation without name");
		return false;
	}
	if (resv_ptr->partition) {
		struct part_record *part_ptr = NULL;
		part_ptr = find_part_record(resv_ptr->partition);
		if (!part_ptr) {
			error("Reservation %s has invalid partition (%s)",
			      resv_ptr->name, resv_ptr->partition);
			return false;
		}
		resv_ptr->part_ptr	= part_ptr;
	}
	if (resv_ptr->accounts) {
		int account_cnt = 0, i, rc;
		char **account_list;
		rc = _build_account_list(resv_ptr->accounts,
					 &account_cnt, &account_list);
		if (rc) {
			error("Reservation %s has invalid accounts (%s)",
			      resv_ptr->name, resv_ptr->accounts);
			return false;
		}
		for (i=0; i<resv_ptr->account_cnt; i++)
			xfree(resv_ptr->account_list[i]);
		xfree(resv_ptr->account_list);
		resv_ptr->account_cnt  = account_cnt;
		resv_ptr->account_list = account_list;
	}
	if (resv_ptr->licenses) {
		bool valid;
		if (resv_ptr->license_list)
			list_destroy(resv_ptr->license_list);
		resv_ptr->license_list = license_validate(resv_ptr->licenses,
							  &valid);
		if (!valid) {
			error("Reservation %s has invalid licenses (%s)",
			      resv_ptr->name, resv_ptr->licenses);
			return false;
		}
	}
	if (resv_ptr->users) {
		int rc, user_cnt = 0;
		uid_t *user_list = NULL;
		rc = _build_uid_list(resv_ptr->users,
				     &user_cnt, &user_list);
		if (rc) {
			error("Reservation %s has invalid users (%s)",
			      resv_ptr->name, resv_ptr->users);
			return false;
		}
		xfree(resv_ptr->user_list);
		resv_ptr->user_cnt  = user_cnt;
		resv_ptr->user_list = user_list;
	}
	if (resv_ptr->node_list) {		/* Change bitmap last */
		bitstr_t *node_bitmap;
		if (strcasecmp(resv_ptr->node_list, "ALL") == 0) {
			node_bitmap = bit_alloc(node_record_count);
			bit_nset(node_bitmap, 0, (node_record_count - 1));
		} else if (node_name2bitmap(resv_ptr->node_list,
					    false, &node_bitmap)) {
			error("Reservation %s has invalid nodes (%s)",
			      resv_ptr->name, resv_ptr->node_list);
			return false;
		}
		FREE_NULL_BITMAP(resv_ptr->node_bitmap);
		resv_ptr->node_bitmap = node_bitmap;
	}
	return true;
}

/*
 * Validate all reservation records, reset bitmaps, etc.
 * Purge any invalid reservation.
 */
static void _validate_all_reservations(void)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	struct job_record *job_ptr;
	char *tmp;
	uint32_t res_num;

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if (!_validate_one_reservation(resv_ptr)) {
			error("Purging invalid reservation record %s",
			      resv_ptr->name);
			_post_resv_delete(resv_ptr);
			_clear_job_resv(resv_ptr);
			list_delete_item(iter);
		} else {
			_set_assoc_list(resv_ptr);
			tmp = strrchr(resv_ptr->name, '_');
			if (tmp) {
				res_num = atoi(tmp + 1);
				top_suffix = MAX(top_suffix, res_num);
			}
		}
	}
	list_iterator_destroy(iter);

	/* Validate all job reservation pointers */
	iter = list_iterator_create(job_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((job_ptr = (struct job_record *) list_next(iter))) {
		if (job_ptr->resv_name == NULL)
			continue;

		if ((job_ptr->resv_ptr == NULL) ||
		    (job_ptr->resv_ptr->magic != RESV_MAGIC)) {
			job_ptr->resv_ptr = (slurmctld_resv_t *)
					list_find_first(resv_list,
							_find_resv_name,
							job_ptr->resv_name);
		}
		if (!job_ptr->resv_ptr) {
			error("JobId %u linked to defunct reservation %s",
			       job_ptr->job_id, job_ptr->resv_name);
			job_ptr->resv_id = 0;
			xfree(job_ptr->resv_name);
		}
	}
	list_iterator_destroy(iter);

}

/*
 * Validate that the reserved nodes are not DOWN or DRAINED and
 *	select different nodes as needed.
 */
static void _validate_node_choice(slurmctld_resv_t *resv_ptr)
{
	bitstr_t *tmp_bitmap = NULL;
	int i;
	resv_desc_msg_t resv_desc;

	if (resv_ptr->flags & RESERVE_FLAG_SPEC_NODES)
		return;

	i = bit_overlap(resv_ptr->node_bitmap, avail_node_bitmap);
	if (i == resv_ptr->node_cnt)
		return;

	/* Reservation includes DOWN, DRAINED/DRAINING, FAILING or
	 * NO_RESPOND nodes. Generate new request using _select_nodes()
	 * in attempt to replace this nodes */
	memset(&resv_desc, 0, sizeof(resv_desc_msg_t));
	resv_desc.start_time = resv_ptr->start_time;
	resv_desc.end_time   = resv_ptr->end_time;
	resv_desc.features   = resv_ptr->features;
	resv_desc.node_cnt   = resv_ptr->node_cnt - i;
	i = _select_nodes(&resv_desc, &resv_ptr->part_ptr, &tmp_bitmap);
	xfree(resv_desc.node_list);
	xfree(resv_desc.partition);
	if (i == SLURM_SUCCESS) {
		bit_and(resv_ptr->node_bitmap, avail_node_bitmap);
		bit_or(resv_ptr->node_bitmap, tmp_bitmap);
		FREE_NULL_BITMAP(tmp_bitmap);
		xfree(resv_ptr->node_list);
		resv_ptr->node_list = bitmap2node_name(resv_ptr->node_bitmap);
		info("modified reservation %s due to unusable nodes, "
		     "new nodes: %s", resv_ptr->name, resv_ptr->node_list);
	} else if (difftime(resv_ptr->start_time, time(NULL)) < 600) {
		info("reservation %s contains unusable nodes, "
		     "can't reallocate now", resv_ptr->name);
	} else {
		debug("reservation %s contains unusable nodes, "
		      "can't reallocate now", resv_ptr->name);
	}
}

/* Open the reservation state save file, or backup if necessary.
 * state_file IN - the name of the state save file used
 * RET the file description to read from or error code
 */
static int _open_resv_state_file(char **state_file)
{
	int state_fd;
	struct stat stat_buf;

	*state_file = xstrdup(slurmctld_conf.state_save_location);
	xstrcat(*state_file, "/resv_state");
	state_fd = open(*state_file, O_RDONLY);
	if (state_fd < 0) {
		error("Could not open reservation state file %s: %m",
		      *state_file);
	} else if (fstat(state_fd, &stat_buf) < 0) {
		error("Could not stat reservation state file %s: %m",
		      *state_file);
		(void) close(state_fd);
	} else if (stat_buf.st_size < 10) {
		error("Reservation state file %s too small", *state_file);
		(void) close(state_fd);
	} else 	/* Success */
		return state_fd;

	error("NOTE: Trying backup state save file. Reservations may be lost");
	xstrcat(*state_file, ".old");
	state_fd = open(*state_file, O_RDONLY);
	return state_fd;
}

/*
 * Load the reservation state from file, recover on slurmctld restart.
 *	Reset reservation pointers for all jobs.
 *	Execute this after loading the configuration file data.
 * IN recover - 0 = validate current reservations ONLY if already recovered,
 *                  otherwise recover from disk
 *              1+ = recover all reservation state from disk
 * RET SLURM_SUCCESS or error code
 * NOTE: READ lock_slurmctld config before entry
 */
extern int load_all_resv_state(int recover)
{
	char *state_file, *data = NULL, *ver_str = NULL;
	time_t now;
	uint32_t data_size = 0, uint32_tmp;
	int data_allocated, data_read = 0, error_code = 0, state_fd;
	Buf buffer;
	slurmctld_resv_t *resv_ptr = NULL;

	last_resv_update = time(NULL);
	if ((recover == 0) && resv_list) {
		_validate_all_reservations();
		return SLURM_SUCCESS;
	}

	/* Read state file and validate */
	if (resv_list)
		list_flush(resv_list);
	else
		resv_list = list_create(_del_resv_rec);

	/* read the file */
	lock_state_files();
	state_fd = _open_resv_state_file(&state_file);
	if (state_fd < 0) {
		info("No reservation state file (%s) to recover",
		     state_file);
		error_code = ENOENT;
	} else {
		data_allocated = BUF_SIZE;
		data = xmalloc(data_allocated);
		while (1) {
			data_read = read(state_fd, &data[data_size],
					BUF_SIZE);
			if (data_read < 0) {
				if  (errno == EINTR)
					continue;
				else {
					error("Read error on %s: %m",
						state_file);
					break;
				}
			} else if (data_read == 0)     /* eof */
				break;
			data_size      += data_read;
			data_allocated += data_read;
			xrealloc(data, data_allocated);
		}
		close(state_fd);
	}
	xfree(state_file);
	unlock_state_files();

	buffer = create_buf(data, data_size);

	safe_unpackstr_xmalloc( &ver_str, &uint32_tmp, buffer);
	debug3("Version string in resv_state header is %s", ver_str);
	if ((!ver_str) || (strcmp(ver_str, RESV_STATE_VERSION) != 0)) {
		error("************************************************************");
		error("Can not recover reservation state, data version incompatible");
		error("************************************************************");
		xfree(ver_str);
		free_buf(buffer);
		schedule_resv_save();	/* Schedule save with new format */
		return EFAULT;
	}
	xfree(ver_str);
	safe_unpack_time(&now, buffer);
	safe_unpack32(&top_suffix, buffer);

	while (remaining_buf(buffer) > 0) {
		resv_ptr = xmalloc(sizeof(slurmctld_resv_t));
		xassert(resv_ptr->magic = RESV_MAGIC);	/* Sets value */
		safe_unpackstr_xmalloc(&resv_ptr->accounts,
				       &uint32_tmp,	buffer);
		safe_unpack_time(&resv_ptr->end_time,	buffer);
		safe_unpackstr_xmalloc(&resv_ptr->features,
				       &uint32_tmp, 	buffer);
		safe_unpackstr_xmalloc(&resv_ptr->licenses,
				       &uint32_tmp, 	buffer);
		safe_unpackstr_xmalloc(&resv_ptr->name,	&uint32_tmp, buffer);
		safe_unpack32(&resv_ptr->node_cnt,	buffer);
		safe_unpackstr_xmalloc(&resv_ptr->node_list,
				       &uint32_tmp,	buffer);
		safe_unpackstr_xmalloc(&resv_ptr->partition,
				       &uint32_tmp, 	buffer);
		safe_unpack_time(&resv_ptr->start_time_first,	buffer);
		safe_unpack16(&resv_ptr->flags,		buffer);
		safe_unpackstr_xmalloc(&resv_ptr->users,&uint32_tmp, buffer);

		/* Fields saved for internal use only (save state) */
		safe_unpackstr_xmalloc(&resv_ptr->assoc_list,
				       &uint32_tmp,	buffer);
		safe_unpack32(&resv_ptr->cpu_cnt,	buffer);
		safe_unpack32(&resv_ptr->resv_id,	buffer);
		safe_unpack_time(&resv_ptr->start_time_prev, buffer);
		safe_unpack_time(&resv_ptr->start_time, buffer);
		safe_unpack32(&resv_ptr->duration,	buffer);

		list_append(resv_list, resv_ptr);
		info("Recovered state of reservation %s", resv_ptr->name);
	}

	_validate_all_reservations();
	info("Recovered state of %d reservations", list_count(resv_list));
	free_buf(buffer);
	return error_code;

      unpack_error:
	_validate_all_reservations();
	if (state_fd >= 0)
		error("Incomplete reservation data checkpoint file");
	info("Recovered state of %d reservations", list_count(resv_list));
	if (resv_ptr)
		_del_resv_rec(resv_ptr);
	free_buf(buffer);
	return EFAULT;
}

/*
 * Determine if a job request can use the specified reservations
 *
 * IN/OUT job_ptr - job to validate, set its resv_id and resv_flags
 * RET SLURM_SUCCESS or error code (not found or access denied)
 */
extern int validate_job_resv(struct job_record *job_ptr)
{
	slurmctld_resv_t *resv_ptr = NULL;
	int rc;

	xassert(job_ptr);

	if ((job_ptr->resv_name == NULL) || (job_ptr->resv_name[0] == '\0')) {
		xfree(job_ptr->resv_name);
		job_ptr->resv_id    = 0;
		job_ptr->resv_flags = 0;
		job_ptr->resv_ptr   = NULL;
		return SLURM_SUCCESS;
	}

	if (!resv_list)
		return ESLURM_RESERVATION_INVALID;

	/* Find the named reservation */
	resv_ptr = (slurmctld_resv_t *) list_find_first (resv_list,
			_find_resv_name, job_ptr->resv_name);
	if (!resv_ptr) {
		info("Reservation name not found (%s)", job_ptr->resv_name);
		return ESLURM_RESERVATION_INVALID;
	}

	rc = _valid_job_access_resv(job_ptr, resv_ptr);
	if (rc == SLURM_SUCCESS) {
		job_ptr->resv_id    = resv_ptr->resv_id;
		job_ptr->resv_flags = resv_ptr->flags;
		job_ptr->resv_ptr   = resv_ptr;
	}
	return rc;
}

static int  _resize_resv(slurmctld_resv_t *resv_ptr, uint32_t node_cnt)
{
	bitstr_t *tmp1_bitmap = NULL, *tmp2_bitmap = NULL;
	int delta_node_cnt, i;
	resv_desc_msg_t resv_desc;

	delta_node_cnt = resv_ptr->node_cnt - node_cnt;
	if (delta_node_cnt == 0)	/* Already correct node count */
		return SLURM_SUCCESS;

	if (delta_node_cnt > 0) {	/* Must decrease node count */
		if (bit_overlap(resv_ptr->node_bitmap, idle_node_bitmap)) {
			/* Start by eliminating idle nodes from reservation */
			tmp1_bitmap = bit_copy(resv_ptr->node_bitmap);
			bit_and(tmp1_bitmap, idle_node_bitmap);
			i = bit_set_count(tmp1_bitmap);
			if (i > delta_node_cnt) {
				tmp2_bitmap = bit_pick_cnt(tmp1_bitmap,
							   delta_node_cnt);
				bit_not(tmp2_bitmap);
				bit_and(resv_ptr->node_bitmap, tmp2_bitmap);
				FREE_NULL_BITMAP(tmp1_bitmap);
				FREE_NULL_BITMAP(tmp2_bitmap);
				delta_node_cnt = 0;	/* ALL DONE */
			} else if (i) {
				bit_not(idle_node_bitmap);
				bit_and(resv_ptr->node_bitmap,
					idle_node_bitmap);
				bit_not(idle_node_bitmap);
				resv_ptr->node_cnt = bit_set_count(
						resv_ptr->node_bitmap);
				delta_node_cnt = resv_ptr->node_cnt -
						 node_cnt;
			}
			FREE_NULL_BITMAP(tmp1_bitmap);
		}
		if (delta_node_cnt > 0) {
			/* Now eliminate allocated nodes from reservation */
			tmp1_bitmap = bit_pick_cnt(resv_ptr->node_bitmap,
						   node_cnt);
			FREE_NULL_BITMAP(resv_ptr->node_bitmap);
			resv_ptr->node_bitmap = tmp1_bitmap;
		}
		xfree(resv_ptr->node_list);
		resv_ptr->node_list = bitmap2node_name(resv_ptr->node_bitmap);
		resv_ptr->node_cnt = node_cnt;
		return SLURM_SUCCESS;
	}

	/* Must increase node count. Make this look like new request so
	 * we can use _select_nodes() for selecting the nodes */
	memset(&resv_desc, 0, sizeof(resv_desc_msg_t));
	resv_desc.start_time = resv_ptr->start_time;
	resv_desc.end_time   = resv_ptr->end_time;
	resv_desc.features   = resv_ptr->features;
	resv_desc.flags      = resv_ptr->flags;
	resv_desc.node_cnt   = 0 - delta_node_cnt;
	i = _select_nodes(&resv_desc, &resv_ptr->part_ptr, &tmp1_bitmap);
	xfree(resv_desc.node_list);
	xfree(resv_desc.partition);
	if (i == SLURM_SUCCESS) {
		bit_or(resv_ptr->node_bitmap, tmp1_bitmap);
		FREE_NULL_BITMAP(tmp1_bitmap);
		xfree(resv_ptr->node_list);
		resv_ptr->node_list = bitmap2node_name(resv_ptr->node_bitmap);
		resv_ptr->node_cnt = node_cnt;
	}
	return i;
}

/* Given a reservation create request, select appropriate nodes for use */
static int  _select_nodes(resv_desc_msg_t *resv_desc_ptr,
			  struct part_record **part_ptr,
			  bitstr_t **resv_bitmap)
{
	slurmctld_resv_t *resv_ptr;
	bitstr_t *node_bitmap;
	ListIterator iter;
	int i, rc = SLURM_SUCCESS;
	time_t now = time(NULL);

	if (*part_ptr == NULL) {
		*part_ptr = default_part_loc;
		if (*part_ptr == NULL)
			return ESLURM_DEFAULT_PARTITION_NOT_SET;
		xfree(resv_desc_ptr->partition);	/* should be no-op */
		resv_desc_ptr->partition = xstrdup((*part_ptr)->name);
	}

	/* Start with all nodes in the partition */
	node_bitmap = bit_copy((*part_ptr)->node_bitmap);

	/* Don't use node already reserved */
	if (!(resv_desc_ptr->flags & RESERVE_FLAG_OVERLAP)) {
		iter = list_iterator_create(resv_list);
		if (!iter)
			fatal("malloc: list_iterator_create");
		while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
			if (resv_ptr->end_time <= now)
				_advance_resv_time(resv_ptr);
			if ((resv_ptr->node_bitmap == NULL) ||
			    (resv_ptr->start_time >= resv_desc_ptr->end_time) ||
			    (resv_ptr->end_time   <= resv_desc_ptr->start_time))
				continue;
			bit_not(resv_ptr->node_bitmap);
			bit_and(node_bitmap, resv_ptr->node_bitmap);
			bit_not(resv_ptr->node_bitmap);
		}
		list_iterator_destroy(iter);
	}

	/* Satisfy feature specification */
	if (resv_desc_ptr->features) {
		int   op_code = FEATURE_OP_AND, last_op_code = FEATURE_OP_AND;
		char *features = xstrdup(resv_desc_ptr->features);
		char *sep_ptr, *token = features;
		bitstr_t *feature_bitmap = bit_copy(node_bitmap);
		struct features_record *feature_ptr;
		ListIterator feature_iter;
		bool match;

		if (feature_bitmap == NULL)
			fatal("bit_copy malloc failure");

		while (1) {
			for (i=0; ; i++) {
				if (token[i] == '\0') {
					sep_ptr = NULL;
					break;
				} else if (token[i] == '|') {
					op_code = FEATURE_OP_OR;
					token[i] = '\0';
					sep_ptr = &token[i];
					break;
				} else if ((token[i] == '&') ||
					   (token[i] == ',')) {
					op_code = FEATURE_OP_AND;
					token[i] = '\0';
					sep_ptr = &token[i];
					break;
				}
			}

			match = false;
			feature_iter = list_iterator_create(feature_list);
			if (feature_iter == NULL)
				fatal("list_iterator_create malloc failure");
			while ((feature_ptr = (struct features_record *)
					list_next(feature_iter))) {
				if (strcmp(token, feature_ptr->name))
					continue;
				if (last_op_code == FEATURE_OP_OR) {
					bit_or(feature_bitmap,
					       feature_ptr->node_bitmap);
				} else {
					bit_and(feature_bitmap,
						feature_ptr->node_bitmap);
				}
				match = true;
				break;
			}
			list_iterator_destroy(feature_iter);
			if (!match) {
				info("reservation feature invalid: %s", token);
				rc = ESLURM_INVALID_FEATURE;
				bit_nclear(feature_bitmap, 0,
					   (node_record_count - 1));
				break;
			}
			if (sep_ptr == NULL)
				break;
			token = sep_ptr + 1;
			last_op_code = op_code;
		}
		xfree(features);
		bit_and(node_bitmap, feature_bitmap);
		FREE_NULL_BITMAP(feature_bitmap);
	}

	if ((resv_desc_ptr->flags & RESERVE_FLAG_MAINT) == 0) {
		/* Nodes must be available */
		bit_and(node_bitmap, avail_node_bitmap);
	}

	*resv_bitmap = NULL;
	if (rc == SLURM_SUCCESS)
		*resv_bitmap = _pick_idle_nodes(node_bitmap,
						resv_desc_ptr);
	FREE_NULL_BITMAP(node_bitmap);
	if (*resv_bitmap == NULL) {
		if (rc == SLURM_SUCCESS)
			rc = ESLURM_NODES_BUSY;
		return rc;
	}

	resv_desc_ptr->node_list = bitmap2node_name(*resv_bitmap);
	return SLURM_SUCCESS;
}

static bitstr_t *_pick_idle_nodes(bitstr_t *avail_bitmap,
				  resv_desc_msg_t *resv_desc_ptr)
{
	ListIterator job_iterator;
	struct job_record *job_ptr;
	bitstr_t *save_bitmap, *ret_bitmap, *tmp_bitmap;

	if (bit_set_count(avail_bitmap) < resv_desc_ptr->node_cnt) {
		verbose("reservation requests more nodes than are available");
		return NULL;
	}

	save_bitmap = bit_copy(avail_bitmap);
	/* First: Try to reserve nodes that are currently IDLE */
	if (bit_overlap(avail_bitmap, idle_node_bitmap) >=
	    resv_desc_ptr->node_cnt) {
		bit_and(avail_bitmap, idle_node_bitmap);
		ret_bitmap = select_g_resv_test(avail_bitmap,
						resv_desc_ptr->node_cnt);
		if (ret_bitmap)
			goto fini;
	}

	/* Second: Try to reserve nodes that are will be IDLE */
	bit_or(avail_bitmap, save_bitmap);	/* restore avail_bitmap */
	job_iterator = list_iterator_create(job_list);
	if (job_iterator == NULL)
		fatal("list_iterator_create: malloc failure");
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (!IS_JOB_RUNNING(job_ptr) && !IS_JOB_SUSPENDED(job_ptr))
			continue;
		if (job_ptr->end_time < resv_desc_ptr->start_time)
			continue;
		bit_not(job_ptr->node_bitmap);
		bit_and(avail_bitmap, job_ptr->node_bitmap);
		bit_not(job_ptr->node_bitmap);
	}
	list_iterator_destroy(job_iterator);
	ret_bitmap = select_g_resv_test(avail_bitmap, resv_desc_ptr->node_cnt);
	if (ret_bitmap)
		goto fini;

	/* Third: Try to reserve nodes that will be allocated to a limited
	 * number of running jobs. We could sort the jobs by priority, QOS,
	 * size or other criterion if desired. Right now we just go down
	 * the unsorted job list. */
	if (resv_desc_ptr->flags & RESERVE_FLAG_IGN_JOBS) {
		job_iterator = list_iterator_create(job_list);
		if (!job_iterator)
			fatal("list_iterator_create: malloc failure");
		while ((job_ptr = (struct job_record *)
			list_next(job_iterator))) {
			if (!IS_JOB_RUNNING(job_ptr) &&
			    !IS_JOB_SUSPENDED(job_ptr))
				continue;
			if (job_ptr->end_time < resv_desc_ptr->start_time)
				continue;
			tmp_bitmap = bit_copy(save_bitmap);
			bit_and(tmp_bitmap, job_ptr->node_bitmap);
			if (bit_set_count(tmp_bitmap) > 0) {
				bit_or(avail_bitmap, tmp_bitmap);
				ret_bitmap = select_g_resv_test(avail_bitmap,
								resv_desc_ptr->
								node_cnt);
			}
			FREE_NULL_BITMAP(tmp_bitmap);
			if (ret_bitmap)
				break;
		}
		list_iterator_destroy(job_iterator);
	}

fini:	FREE_NULL_BITMAP(save_bitmap);
	return ret_bitmap;
}

/* Determine if a job has access to a reservation
 * RET SLURM_SUCCESS if true, ESLURM_RESERVATION_ACCESS otherwise */
static int _valid_job_access_resv(struct job_record *job_ptr,
				  slurmctld_resv_t *resv_ptr)
{
	int i;

	/* Determine if we have access */
	if (accounting_enforce & ACCOUNTING_ENFORCE_ASSOCS) {
		char tmp_char[30];
		slurmdb_association_rec_t *assoc;
		if (!resv_ptr->assoc_list) {
			error("Reservation %s has no association list. "
			      "Checking user/account lists",
			      resv_ptr->name);
			goto no_assocs;
		}

		if (!job_ptr->assoc_ptr) {
			slurmdb_association_rec_t assoc_rec;
			/* This should never be called, but just to be
			 * safe we will try to fill it in. */
			memset(&assoc_rec, 0,
			       sizeof(slurmdb_association_rec_t));
			assoc_rec.id = job_ptr->assoc_id;
			if (assoc_mgr_fill_in_assoc(
				    acct_db_conn, &assoc_rec,
				    accounting_enforce,
				    (slurmdb_association_rec_t **)
				    &job_ptr->assoc_ptr))
				goto end_it;
		}

		/* Check to see if the association is here or the parent
		 * association is listed in the valid associations. */
		assoc = job_ptr->assoc_ptr;
		while (assoc) {
			snprintf(tmp_char, sizeof(tmp_char), ",%u,", assoc->id);
			if (strstr(resv_ptr->assoc_list, tmp_char))
				return SLURM_SUCCESS;
			assoc = assoc->usage->parent_assoc_ptr;
		}
	} else {
no_assocs:	for (i=0; i<resv_ptr->user_cnt; i++) {
			if (job_ptr->user_id == resv_ptr->user_list[i])
				return SLURM_SUCCESS;
		}
		for (i=0; (i<resv_ptr->account_cnt) && job_ptr->account; i++) {
			if (resv_ptr->account_list[i] &&
			    (strcmp(job_ptr->account,
				    resv_ptr->account_list[i]) == 0)) {
				return SLURM_SUCCESS;
			}
		}
	}

end_it:
	info("Security violation, uid=%u attempt to use reservation %s",
	     job_ptr->user_id, resv_ptr->name);
	return ESLURM_RESERVATION_ACCESS;
}

/*
 * Determine if a job can start now based only upon reservations
 *
 * IN job_ptr      - job to test
 * RET	SLURM_SUCCESS if runable now, otherwise an error code
 */
extern int job_test_resv_now(struct job_record *job_ptr)
{
	slurmctld_resv_t * resv_ptr;
	time_t now;

	if (job_ptr->resv_name == NULL)
		return SLURM_SUCCESS;

	resv_ptr = (slurmctld_resv_t *) list_find_first (resv_list,
			_find_resv_name, job_ptr->resv_name);
	job_ptr->resv_ptr = resv_ptr;
	if (!resv_ptr)
		return ESLURM_RESERVATION_INVALID;

	if (_valid_job_access_resv(job_ptr, resv_ptr) != SLURM_SUCCESS)
		return ESLURM_RESERVATION_ACCESS;
	now = time(NULL);
	if (now < resv_ptr->start_time) {
		/* reservation starts later */
		return ESLURM_INVALID_TIME_VALUE;
	}
	if (now > resv_ptr->end_time) {
		/* reservation ended earlier */
		return ESLURM_RESERVATION_INVALID;
	}
	if ((resv_ptr->node_cnt == 0) &&
	    !(resv_ptr->flags & RESERVE_FLAG_LIC_ONLY)) {
		/* empty reservation treated like it will start later */
		return ESLURM_INVALID_TIME_VALUE;
	}

	return SLURM_SUCCESS;
}

/* Adjust a job's time_limit and end_time as needed to avoid using
 *	reserved resources. Don't go below job's time_min value. */
extern void job_time_adj_resv(struct job_record *job_ptr)
{
	ListIterator iter;
	slurmctld_resv_t * resv_ptr;
	time_t now = time(NULL);
	int32_t resv_begin_time;

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if (resv_ptr->end_time <= now)
			_advance_resv_time(resv_ptr);
		if (job_ptr->resv_ptr == resv_ptr)
			continue;	/* authorized user of reservation */
		if (resv_ptr->start_time <= now)
			continue;	/* already validated */
		if (resv_ptr->start_time >= job_ptr->end_time)
			continue;	/* reservation starts after job ends */
		if (!license_list_overlap(job_ptr->license_list,
					  resv_ptr->license_list) &&
		    ((resv_ptr->node_bitmap == NULL) ||
		     (bit_overlap(resv_ptr->node_bitmap,
				  job_ptr->node_bitmap) == 0)))
			continue;	/* disjoint resources */
		resv_begin_time = difftime(resv_ptr->start_time, now) / 60;
		job_ptr->time_limit = MIN(job_ptr->time_limit,resv_begin_time);
	}
	list_iterator_destroy(iter);
	job_ptr->time_limit = MAX(job_ptr->time_limit, job_ptr->time_min);
	job_ptr->end_time = job_ptr->start_time + (job_ptr->time_limit * 60);
}

/* For a given license_list, return the total count of licenses of the
 *	specified name */
static int _license_cnt(List license_list, char *lic_name)
{
	int lic_cnt = 0;
	ListIterator iter;
	licenses_t *license_ptr;

	if (license_list == NULL)
		return lic_cnt;

	iter = list_iterator_create(license_list);
	if (!iter)
		fatal("list_interator_create malloc failure");
	while ((license_ptr = list_next(iter))) {
		if (strcmp(license_ptr->name, lic_name) == 0)
			lic_cnt += license_ptr->total;
	}
	list_iterator_destroy(iter);

	return lic_cnt;
}

static uint32_t _get_job_duration(struct job_record *job_ptr)
{
	uint32_t duration;
	uint16_t time_slices = 1;

	if (job_ptr->time_limit == INFINITE)
		duration = ONE_YEAR;
	else if (job_ptr->time_limit != NO_VAL)
		duration = (job_ptr->time_limit * 60);
	else {	/* partition time limit */
		if (job_ptr->part_ptr->max_time == INFINITE)
			duration = ONE_YEAR;
		else
			duration = (job_ptr->part_ptr->max_time * 60);
	}
	if (job_ptr->part_ptr)
		time_slices = job_ptr->part_ptr->max_share & ~SHARED_FORCE;
	if ((duration != ONE_YEAR) && (time_slices > 1) &&
	    (slurm_get_preempt_mode() & PREEMPT_MODE_GANG)) {
		/* FIXME: Ideally we figure out how many jobs are actually
		 * time-slicing on each node rather than using the maximum
		 * value. */
		duration *= time_slices;
	}
	return duration;
}

/*
 * Determine how many licenses of the give type the specified job is
 *	prevented from using due to reservations
 *
 * IN job_ptr   - job to test
 * IN lic_name  - name of license
 * IN when      - when the job is expected to start
 * RET number of licenses of this type the job is prevented from using
 */
extern int job_test_lic_resv(struct job_record *job_ptr, char *lic_name,
			     time_t when)
{
	slurmctld_resv_t * resv_ptr;
	time_t job_start_time, job_end_time, now = time(NULL);
	ListIterator iter;
	int resv_cnt = 0;

	job_start_time = when;
	job_end_time   = when + _get_job_duration(job_ptr);
	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if (resv_ptr->end_time <= now)
			_advance_resv_time(resv_ptr);
		if ((resv_ptr->start_time >= job_end_time) ||
		    (resv_ptr->end_time   <= job_start_time))
			continue;	/* reservation at different time */

		if (job_ptr->resv_name &&
		    (strcmp(job_ptr->resv_name, resv_ptr->name) == 0))
			continue;	/* job can use this reservation */

		resv_cnt += _license_cnt(resv_ptr->license_list, lic_name);
	}
	list_iterator_destroy(iter);

	/* info("job %u blocked from %d licenses of type %s",
	     job_ptr->job_id, resv_cnt, lic_name); */
	return resv_cnt;
}

/*
 * Determine which nodes a job can use based upon reservations
 * IN job_ptr      - job to test
 * IN/OUT when     - when we want the job to start (IN)
 *                   when the reservation is available (OUT)
 * IN move_time    - if true, then permit the start time to advance from
 *                   "when" as needed IF job has no reservervation
 * OUT node_bitmap - nodes which the job can use, caller must free unless error
 * RET	SLURM_SUCCESS if runable now
 *	ESLURM_RESERVATION_ACCESS access to reservation denied
 *	ESLURM_RESERVATION_INVALID reservation invalid
 *	ESLURM_INVALID_TIME_VALUE reservation invalid at time "when"
 *	ESLURM_NODES_BUSY job has no reservation, but required nodes are
 *			  reserved
 */
extern int job_test_resv(struct job_record *job_ptr, time_t *when,
			 bool move_time, bitstr_t **node_bitmap)
{
	slurmctld_resv_t * resv_ptr, *res2_ptr;
	time_t job_start_time, job_end_time, lic_resv_time;
	time_t now = time(NULL);
	ListIterator iter;
	int i, rc = SLURM_SUCCESS;

	job_start_time = *when;
	job_end_time   = *when + _get_job_duration(job_ptr);
	*node_bitmap = (bitstr_t *) NULL;

	if (job_ptr->resv_name) {
		bool overlap_resv = false;
		resv_ptr = (slurmctld_resv_t *) list_find_first (resv_list,
				_find_resv_name, job_ptr->resv_name);
		job_ptr->resv_ptr = resv_ptr;
		if (!resv_ptr)
			return ESLURM_RESERVATION_INVALID;
		if (_valid_job_access_resv(job_ptr, resv_ptr) != SLURM_SUCCESS)
			return ESLURM_RESERVATION_ACCESS;
		if (resv_ptr->end_time <= now)
			_advance_resv_time(resv_ptr);
		if (*when < resv_ptr->start_time) {
			/* reservation starts later */
			*when = resv_ptr->start_time;
			return ESLURM_INVALID_TIME_VALUE;
		}
		if ((resv_ptr->node_cnt == 0) &&
		    (!(resv_ptr->flags & RESERVE_FLAG_LIC_ONLY))) {
			/* empty reservation treated like it will start later */
			*when = now + 600;
			return ESLURM_INVALID_TIME_VALUE;
		}
		if (*when > resv_ptr->end_time) {
			/* reservation ended earlier */
			*when = resv_ptr->end_time;
			job_ptr->priority = 0;	/* administrative hold */
			return ESLURM_RESERVATION_INVALID;
		}
		if (job_ptr->details->req_node_bitmap &&
		    (!(resv_ptr->flags & RESERVE_FLAG_LIC_ONLY)) &&
		    !bit_super_set(job_ptr->details->req_node_bitmap,
				   resv_ptr->node_bitmap)) {
			return ESLURM_RESERVATION_INVALID;
		}
		if (resv_ptr->flags & RESERVE_FLAG_LIC_ONLY) {
			*node_bitmap = bit_alloc(node_record_count);
			if (*node_bitmap == NULL)
				fatal("bit_alloc: malloc failure");
			bit_nset(*node_bitmap, 0, (node_record_count - 1));
		} else
			*node_bitmap = bit_copy(resv_ptr->node_bitmap);

		/* if there are any overlapping reservations, we need to
		 * prevent the job from using those nodes (e.g. MAINT nodes) */
		iter = list_iterator_create(resv_list);
		if (!iter)
			fatal("malloc: list_iterator_create");
		while ((res2_ptr = (slurmctld_resv_t *) list_next(iter))) {
			if ((resv_ptr->flags & RESERVE_FLAG_MAINT) ||
			    (resv_ptr->flags & RESERVE_FLAG_OVERLAP) ||
			    (res2_ptr == resv_ptr) ||
			    (res2_ptr->node_bitmap == NULL) ||
			    (res2_ptr->start_time >= job_end_time) ||
			    (res2_ptr->end_time   <= job_start_time))
				continue;
			bit_not(res2_ptr->node_bitmap);
			bit_and(*node_bitmap, res2_ptr->node_bitmap);
			bit_not(res2_ptr->node_bitmap);
			overlap_resv = true;
		}
		list_iterator_destroy(iter);

		if (slurm_get_debug_flags() & DEBUG_FLAG_RESERVATION) {
			char *nodes=bitmap2node_name(*node_bitmap);
			info("job_test_resv: job:%u reservation:%s nodes:%s",
			     job_ptr->job_id, nodes, job_ptr->resv_name);
			xfree(nodes);
		}
		return SLURM_SUCCESS;
	}

	job_ptr->resv_ptr = NULL;	/* should be redundant */
	*node_bitmap = bit_alloc(node_record_count);
	if (*node_bitmap == NULL)
		fatal("bit_alloc: malloc failure");
	bit_nset(*node_bitmap, 0, (node_record_count - 1));
	if (list_count(resv_list) == 0)
		return SLURM_SUCCESS;

	/* Job has no reservation, try to find time when this can
	 * run and get it's required nodes (if any) */
	for (i=0; ; i++) {
		lic_resv_time = (time_t) 0;

		iter = list_iterator_create(resv_list);
		if (!iter)
			fatal("malloc: list_iterator_create");
		while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
			if (resv_ptr->end_time <= now)
				_advance_resv_time(resv_ptr);
			if ((resv_ptr->node_bitmap == NULL) ||
			    (resv_ptr->start_time >= job_end_time) ||
			    (resv_ptr->end_time   <= job_start_time))
				continue;
			if (job_ptr->details->req_node_bitmap &&
			    bit_overlap(job_ptr->details->req_node_bitmap,
					resv_ptr->node_bitmap)) {
				*when = resv_ptr->end_time;
				rc = ESLURM_NODES_BUSY;
				break;
			}
			/* FIXME: This only tracks when ANY licenses required
			 * by the job are freed by any reservation without
			 * counting them, so the results are not accurate. */
			if (license_list_overlap(job_ptr->license_list,
						 resv_ptr->license_list)) {
				if ((lic_resv_time == (time_t) 0) ||
				    (lic_resv_time > resv_ptr->end_time))
					lic_resv_time = resv_ptr->end_time;
			}
			bit_not(resv_ptr->node_bitmap);
			bit_and(*node_bitmap, resv_ptr->node_bitmap);
			bit_not(resv_ptr->node_bitmap);
		}
		list_iterator_destroy(iter);

		if ((rc == SLURM_SUCCESS) && move_time) {
			if (license_job_test(job_ptr, job_start_time)
			    == EAGAIN) {
				/* Need to postpone for licenses. Time returned
				 * is best case; first reservation with those
				 * licenses ends. */
				rc = ESLURM_NODES_BUSY;
				*when = lic_resv_time;
			}
		}
		if (rc == SLURM_SUCCESS)
			break;
		/* rc == ESLURM_NODES_BUSY here from above break */
		if (move_time && (i<10)) {  /* Retry for later start time */
			bit_nset(*node_bitmap, 0, (node_record_count - 1));
			rc = SLURM_SUCCESS;
			continue;
		}
		FREE_NULL_BITMAP(*node_bitmap);
		break;	/* Give up */
	}

	return rc;
}

/* Begin scan of all jobs for valid reservations */
extern void begin_job_resv_check(void)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	slurm_ctl_conf_t *conf;

	if (!resv_list)
		return;

	conf = slurm_conf_lock();
	resv_over_run = conf->resv_over_run;
	slurm_conf_unlock();
	if (resv_over_run == (uint16_t) INFINITE)
		resv_over_run = ONE_YEAR;
	else
		resv_over_run *= 60;

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		resv_ptr->job_pend_cnt = 0;
		resv_ptr->job_run_cnt  = 0;
	}
	list_iterator_destroy(iter);
}

/* Test a particular job for valid reservation
 *
 * RET ESLURM_INVALID_TIME_VALUE if reservation is terminated
 *     SLURM_SUCCESS if reservation is still valid
 */
extern int job_resv_check(struct job_record *job_ptr)
{
	bool run_flag = false;

	if (!job_ptr->resv_name)
		return SLURM_SUCCESS;

	if (IS_JOB_RUNNING(job_ptr) || IS_JOB_SUSPENDED(job_ptr))
		run_flag = true;
	else if (IS_JOB_PENDING(job_ptr))
		run_flag = false;
	else
		return SLURM_SUCCESS;

	xassert(job_ptr->resv_ptr->magic == RESV_MAGIC);
	if (run_flag)
		job_ptr->resv_ptr->job_run_cnt++;
	else
		job_ptr->resv_ptr->job_pend_cnt++;

	if ((job_ptr->resv_ptr->end_time + resv_over_run) < time(NULL))
		return ESLURM_INVALID_TIME_VALUE;
	return SLURM_SUCCESS;
}

/* Advance a expired reservation's time stamps one day or one week 
 * as appropriate. */
static void _advance_resv_time(slurmctld_resv_t *resv_ptr)
{
	int day_cnt = 0;
	char *interval = "";

	if (resv_ptr->flags & RESERVE_FLAG_DAILY) {
		day_cnt = 1;
		interval = "day";
	} else if (resv_ptr->flags & RESERVE_FLAG_WEEKLY) {
		day_cnt = 7;
		interval = "week";
	}

	if (day_cnt) {
		verbose("Advance reservation %s one %s", resv_ptr->name,
			interval);
		resv_ptr->start_time = resv_ptr->start_time_first;
		_advance_time(&resv_ptr->start_time, day_cnt);
		resv_ptr->start_time_prev = resv_ptr->start_time;
		resv_ptr->start_time_first = resv_ptr->start_time;
		_advance_time(&resv_ptr->end_time, day_cnt);
		_post_resv_create(resv_ptr);
		last_resv_update = time(NULL);
		schedule_resv_save();
	}
}

/* Finish scan of all jobs for valid reservations
 *
 * Purge vestigial reservation records.
 * Advance daily or weekly reservations that are no longer
 *	being actively used.
 */
extern void fini_job_resv_check(void)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	time_t now = time(NULL);

	if (!resv_list)
		return;

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if (resv_ptr->end_time > now) { /* reservation not over */
			_validate_node_choice(resv_ptr);
			continue;
		}
		_advance_resv_time(resv_ptr);
		if ((resv_ptr->job_pend_cnt   == 0) &&
		    (resv_ptr->job_run_cnt    == 0) &&
		    (resv_ptr->maint_set_node == 0) &&
		    ((resv_ptr->flags & RESERVE_FLAG_DAILY ) == 0) &&
		    ((resv_ptr->flags & RESERVE_FLAG_WEEKLY) == 0)) {
			debug("Purging vestigial reservation record %s",
			      resv_ptr->name);
			_clear_job_resv(resv_ptr);
			list_delete_item(iter);
			last_resv_update = now;
			schedule_resv_save();
		}

	}
	list_iterator_destroy(iter);
}

/* send all reservations to accounting.  Only needed at
 * first registration
 */
extern int send_resvs_to_accounting(void)
{
	ListIterator itr = NULL;
	slurmctld_resv_t *resv_ptr;

	if(!resv_list)
		return SLURM_SUCCESS;

	itr = list_iterator_create(resv_list);
	if (!itr)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = list_next(itr))) {
		_post_resv_create(resv_ptr);
	}
	list_iterator_destroy(itr);

	return SLURM_SUCCESS;
}


/* Set or clear NODE_STATE_MAINT for node_state as needed */
extern void set_node_maint_mode(void)
{
	ListIterator iter;
	slurmctld_resv_t *resv_ptr;
	time_t now = time(NULL);

	if (!resv_list)
		return;

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = (slurmctld_resv_t *) list_next(iter))) {
		if ((resv_ptr->flags & RESERVE_FLAG_MAINT) == 0)
			continue;
		if ((now >= resv_ptr->start_time) &&
		    (now <  resv_ptr->end_time  )) {
			if (!resv_ptr->maint_set_node) {
				resv_ptr->maint_set_node = true;
				_set_nodes_maint(resv_ptr, now);
				last_node_update = now;
			}
		} else if (resv_ptr->maint_set_node) {
			resv_ptr->maint_set_node = false;
			_set_nodes_maint(resv_ptr, now);
			last_node_update = now;
		}
	}
	list_iterator_destroy(iter);
}

extern void update_assocs_in_resvs(void)
{
	slurmctld_resv_t *resv_ptr = NULL;
	ListIterator  iter = NULL;

	if (!resv_list)
		error("No reservation list given for updating associations");

	iter = list_iterator_create(resv_list);
	if (!iter)
		fatal("malloc: list_iterator_create");
	while ((resv_ptr = list_next(iter))) {
		_set_assoc_list(resv_ptr);
	}
	list_iterator_destroy(iter);
}

static void _set_nodes_maint(slurmctld_resv_t *resv_ptr, time_t now)
{
	int i, i_first, i_last;
	struct node_record *node_ptr;

	if (!resv_ptr->node_bitmap) {
		error("reservation %s lacks a bitmap", resv_ptr->name);
		return;
	}

	i_first = bit_ffs(resv_ptr->node_bitmap);
	i_last  = bit_fls(resv_ptr->node_bitmap);
	for (i=i_first; i<=i_last; i++) {
		if (!bit_test(resv_ptr->node_bitmap, i))
			continue;

		node_ptr = node_record_table_ptr + i;
		if (resv_ptr->maint_set_node)
			node_ptr->node_state |= NODE_STATE_MAINT;
		else
			node_ptr->node_state &= (~NODE_STATE_MAINT);
		/* mark that this node is now down and in maint mode
		 * or was removed from maint mode */
		if (IS_NODE_DOWN(node_ptr) || IS_NODE_DRAIN(node_ptr) ||
		    IS_NODE_FAIL(node_ptr)) {
			clusteracct_storage_g_node_down(
				acct_db_conn,
				node_ptr, now, NULL,
				slurm_get_slurm_user_id());
		}
	}
}
