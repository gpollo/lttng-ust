/*
 * Copyright (C) 2019 Gabriel-Andrew Pollo-Guilbert
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1 of
 * the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <lttng/ust-dlfcn.h>
#include <lttng/ust.h>

int setuid(uid_t uid)
{
	static int (*plibc_func)(uid_t) = NULL;
	int retval;
	uid_t before_uid;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, __func__);
		if (plibc_func == NULL) {
			fprintf(stderr, "libustsetuid: unable to find \"%s\" symbol\n", __func__);
			errno = ENOSYS;
			return -1;
		}
	}

	before_uid = getuid();

	retval = plibc_func(uid);
	if (retval < 0)
		return retval;

	if (before_uid != getuid())
		ust_after_setuid();

	return retval;
}

int setreuid(uid_t ruid, uid_t euid)
{
	static int (*plibc_func)(uid_t, uid_t) = NULL;
	int retval;
	uid_t before_uid;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, __func__);
		if (plibc_func == NULL) {
			fprintf(stderr, "libustsetuid: unable to find \"%s\" symbol\n", __func__);
			errno = ENOSYS;
			return -1;
		}
	}

	before_uid = getuid();

	retval = plibc_func(ruid, euid);
	if (retval < 0)
		return retval;

	if (before_uid != getuid())
		ust_after_setuid();

	return retval;
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	static int (*plibc_func)(uid_t, uid_t, uid_t) = NULL;
	int retval;
	uid_t before_uid;

	if (plibc_func == NULL) {
		plibc_func = dlsym(RTLD_NEXT, __func__);
		if (plibc_func == NULL) {
			fprintf(stderr, "libustsetuid: unable to find \"%s\" symbol\n", __func__);
			errno = ENOSYS;
			return -1;
		}
	}

	before_uid = getuid();

	retval = plibc_func(ruid, euid, suid);
	if (retval < 0)
		return retval;

	if (before_uid != getuid())
		ust_after_setuid();

	return retval;
}
