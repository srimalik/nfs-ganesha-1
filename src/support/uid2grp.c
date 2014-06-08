/*
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @addtogroup uid2grp
 * @{
 */

/**
 * @file uid2grp.c
 * @brief Uid to group list conversion
 */

#include "config.h"
#include "ganesha_rpc.h"
#include "nfs_core.h"
#include <unistd.h>		/* for using gethostname */
#include <stdlib.h>		/* for using exit */
#include <strings.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <stdint.h>
#include <stdbool.h>
#include "common_utils.h"
#include "uid2grp.h"

/* group_data has a reference counter. If it goes to zero, it implies
 * that it is out of the cache (AVL trees) and should be freed. The
 * reference count is 1 when we put it into AVL trees. We decrement when
 * we take it out of AVL trees. Also incremented when we pass this to
 * out siders (uid2grp and friends) and decremented when they are done
 * (in uid2grp_unref()).
 *
 * When a group_data needs to be removed or expired after a certain
 * timeout, we take it out of the cache (AVL trees). When everyone using
 * the group_data are done, the refcount will go to zero at which point
 * we free group_data as well as the buffer holding supplementary
 * groups.
 */
void uid2grp_hold_group_data(struct group_data *gdata)
{
	pthread_mutex_lock(&gdata->lock);
	gdata->refcount++;
	pthread_mutex_unlock(&gdata->lock);
}

void uid2grp_release_group_data(struct group_data *gdata)
{
	unsigned int refcount;

	pthread_mutex_lock(&gdata->lock);
	refcount = --gdata->refcount;
	pthread_mutex_unlock(&gdata->lock);

	if (refcount == 0) {
		gsh_free(gdata->groups);
		gsh_free(gdata);
	} else if (refcount == (unsigned int)-1) {
		LogAlways(COMPONENT_IDMAPPER, "negative refcount on gdata: %p",
			  gdata);
	}
}

/* Allocate supplementary groups buffer */
static bool my_getgrouplist_alloc(char *user,
				  gid_t gid,
				  struct group_data *gdata)
{
	int nbgrp = 0;
	gid_t *groups = NULL;

	/* Step 1 : call getgrouplist with a 0 size
	 * getgrouplist() will return the needed size.
	 * This call WILL fail (see manpages), but errno
	 * won't be set and nbgrp will contain the right value */
	getgrouplist(user, gid, NULL, &nbgrp);
	if (errno != 0) {
		/* This case is actually an error */
		LogEvent(COMPONENT_IDMAPPER, "getgrouplist %s failed",
			 user);
		return false;
	}
	gdata->nbgroups = nbgrp;

	/* Step 2: allocate gdata->groups with the right size then
	 * call getgrouplist() a second time to get the actual group list */
	groups = (gid_t *) gsh_malloc(nbgrp * sizeof(gid_t));
	if (groups == NULL)
		return false;

	if (getgrouplist
	    (user, gid, groups, &gdata->nbgroups) == -1) {
		LogEvent(COMPONENT_IDMAPPER, "getgrouplist %s failed", user);
		gsh_free(groups);
		return false;
	}

	gdata->groups = groups;

	return true;
}

/* Allocate and fill in group_data structure */
static struct group_data *uid2grp_allocate_by_name(
		const struct gsh_buffdesc *name)
{
	struct passwd p;
	struct passwd *pp;
	char *namebuff = alloca(name->len + 1);
	struct group_data *gdata = NULL;
	char *buff;
	long buff_size;

	memcpy(namebuff, name->addr, name->len);
	*(namebuff + name->len) = '\0';

	buff_size = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (buff_size == -1) {
		LogMajor(COMPONENT_IDMAPPER, "sysconf failure: %d", errno);
		return NULL;
	}

	buff = alloca(buff_size);
	if ((getpwnam_r(namebuff, &p, buff, buff_size, &pp) != 0)
	    || (pp == NULL)) {
		LogEvent(COMPONENT_IDMAPPER, "getpwnam_r %s failed", namebuff);
		return gdata;
	}

	gdata = gsh_malloc(sizeof(struct group_data) + strlen(p.pw_name));
	if (gdata == NULL) {
		LogEvent(COMPONENT_IDMAPPER, "failed to allocate group data");
		return gdata;
	}

	gdata->uname.len = strlen(p.pw_name);
	gdata->uname.addr = (char *)gdata + sizeof(struct group_data);
	memcpy(gdata->uname.addr, p.pw_name, gdata->uname.len);
	gdata->uid = p.pw_uid;
	gdata->gid = p.pw_gid;
	if (!my_getgrouplist_alloc(p.pw_name, p.pw_gid, gdata)) {
		gsh_free(gdata);
		return NULL;
	}

	pthread_mutex_init(&gdata->lock, NULL);
	gdata->epoch = time(NULL);
	gdata->refcount = 0;
	return gdata;
}

/* Allocate and fill in group_data structure */
static struct group_data *uid2grp_allocate_by_uid(uid_t uid)
{
	struct passwd p;
	struct passwd *pp;
	struct group_data *gdata = NULL;
	char *buff;
	long buff_size;

	buff_size = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (buff_size == -1) {
		LogMajor(COMPONENT_IDMAPPER, "sysconf failure: %d", errno);
		return NULL;
	}

	buff = alloca(buff_size);
	if ((getpwuid_r(uid, &p, buff, buff_size, &pp) != 0) || (pp == NULL)) {
		LogEvent(COMPONENT_IDMAPPER, "getpwuid_r %u failed", uid);
		return gdata;
	}

	gdata = gsh_malloc(sizeof(struct group_data) + strlen(p.pw_name));
	if (gdata == NULL) {
		LogEvent(COMPONENT_IDMAPPER, "failed to allocate group data");
		return gdata;
	}

	gdata->uname.len = strlen(p.pw_name);
	gdata->uname.addr = (char *)gdata + sizeof(struct group_data);
	memcpy(gdata->uname.addr, p.pw_name, gdata->uname.len);
	gdata->uid = p.pw_uid;
	gdata->gid = p.pw_gid;
	if (!my_getgrouplist_alloc(p.pw_name, p.pw_gid, gdata)) {
		gsh_free(gdata);
		return NULL;
	}

	pthread_mutex_init(&gdata->lock, NULL);
	gdata->epoch = time(NULL);
	gdata->refcount = 0;
	return gdata;
}

/**
 * @brief Get supplementary groups given uname
 *
 * @param[in]  name  The name of the user
 * @param[out]  group_data
 *
 * @return true if successful, false otherwise
 */
#define uid2grp_expired(gdata) (time(NULL) - (gdata)->epoch > \
		nfs_param.core_param.manage_gids_expiration)
bool name2grp(const struct gsh_buffdesc *name, struct group_data **gdata)
{
	bool success = false;
	uid_t uid = -1;

	pthread_rwlock_rdlock(&uid2grp_user_lock);
	success = uid2grp_lookup_by_uname(name, &uid, gdata);

	/* Handle common case first */
	if (success && !uid2grp_expired(*gdata)) {
		uid2grp_hold_group_data(*gdata);
		pthread_rwlock_unlock(&uid2grp_user_lock);
		return success;
	}
	pthread_rwlock_unlock(&uid2grp_user_lock);

	if (success) {
		/* Cache entry is expired */
		pthread_rwlock_wrlock(&uid2grp_user_lock);
		uid2grp_remove_by_uname(name);
		pthread_rwlock_unlock(&uid2grp_user_lock);
	}

	*gdata = uid2grp_allocate_by_name(name);
	pthread_rwlock_wrlock(&uid2grp_user_lock);
	if (*gdata)
		uid2grp_add_user(*gdata);
	success = uid2grp_lookup_by_uname(name, &uid, gdata);
	if (success)
		uid2grp_hold_group_data(*gdata);
	pthread_rwlock_unlock(&uid2grp_user_lock);

	return success;
}

/**
 * @brief Get supplementary groups given uid
 *
 * @param[in]  uid  The uid of the user
 * @param[out]  group_data
 *
 * @return true if successful, false otherwise
 */
bool uid2grp(uid_t uid, struct group_data **gdata)
{
	bool success = false;

	pthread_rwlock_rdlock(&uid2grp_user_lock);
	success = uid2grp_lookup_by_uid(uid, gdata);

	/* Handle common case first */
	if (success && !uid2grp_expired(*gdata)) {
		uid2grp_hold_group_data(*gdata);
		pthread_rwlock_unlock(&uid2grp_user_lock);
		return success;
	}
	pthread_rwlock_unlock(&uid2grp_user_lock);

	if (success) {
		/* Cache entry is expired */
		pthread_rwlock_wrlock(&uid2grp_user_lock);
		uid2grp_remove_by_uid(uid);
		pthread_rwlock_unlock(&uid2grp_user_lock);
	}

	*gdata = uid2grp_allocate_by_uid(uid);
	pthread_rwlock_wrlock(&uid2grp_user_lock);
	if (*gdata)
		uid2grp_add_user(*gdata);
	success = uid2grp_lookup_by_uid(uid, gdata);
	if (success)
		uid2grp_hold_group_data(*gdata);
	pthread_rwlock_unlock(&uid2grp_user_lock);

	return success;
}

/*
 * All callers of uid2grp() and uname2grp must call this
 * when they are done accessing supplementary groups
 */
void uid2grp_unref(struct group_data *gdata)
{
	uid2grp_release_group_data(gdata);
}

/** @} */
