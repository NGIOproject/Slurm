/***************************************************************************** \
 *  slurmd_nvram.c - slurmd NVRAM management
 *****************************************************************************
 *  Copyright (C) 2019 EPCC
 *		EPCC, Iakovos Panourgias <i.panourgias@epcc.ed.ac.uk>
 *
 *  Written by Iakovos Panourgias <i.panourgias@epcc.ed.ac.uk>
 *  This software was developed as part of the EC H2020 funded project NEXTGenIO (Project ID: 671951) www.nextgenio.eu 
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com>.
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
\****************************************************************************/

#include "config.h"

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>

#include "src/common/fd.h"
#include "src/common/macros.h"
#include "slurm/slurm_errno.h"
#include "slurm/slurm.h"
#include "src/common/bitstring.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/xstring.h"
#include "src/common/xmalloc.h"
#include "src/slurmd/common/slurmd_nvram.h"
#include "src/slurmd/slurmd/slurmd.h"
#include "src/slurmctld/slurmctld.h"

/* Maximum poll wait time for child processes, in milliseconds */
#define MAX_POLL_WAIT   500

#if defined (__APPLE__)
slurmctld_config_t slurmctld_config __attribute__((weak_import));
#else
slurmctld_config_t slurmctld_config;
#endif

/* Default path to NVRAM stats */
const char nvram_base_path[] = "/NEXTGenIO/NVDIMM";
const char ndctl_path[] = "/dev/ndctl0";

static s_p_hashtbl_t *_config_make_tbl(char *filename);
/* Log a command's arguments. */
static void _log_script_argv(char **script_argv, char *resp_msg, bool print_debug);
/* Return time in msec since "start time" */
static int  _tot_wait (struct timeval *start_time);
/* Run a script and return its stdout plus exit status */
static char *_run_script(char *cmd_path, char **script_argv, int *status);
static int _get_current_mode();
static bool checked_for_nvram_memory = false; 	/* check for NVRAM Capacity */
static bool checked_for_nvram_mode = false; 	/* check for NVRAM Platform Mode */
static uint32_t static_capacity = 0;
static uint32_t static_memory_capacity = 0;
static uint32_t static_appdirect_capacity = 0;
static uint32_t static_inaccessible_capacity = 0;

static uint32_t ipmctl_timeout = 0;
static int ipmctl_found = -1;
static int ndctl_found = -1;
static char *ipmctl_path = NULL;
static uint16_t nvram_mode = 0;

static s_p_options_t nvram_conf_file_options[] = {
	{"AllowNVRAMMode", S_P_STRING},
	{"DefaultNVRAMMode", S_P_STRING},
	{"AllowUserBoot", S_P_STRING},
	{"IpmctlPath", S_P_STRING},
	{"IpmctlTimeout", S_P_UINT32},
	{"Force", S_P_UINT32},
	{"BootTime", S_P_UINT32},
	{"NodeRebootWeight", S_P_UINT32},
	{"BMCIP", S_P_STRING},
	{"BMCPASSWORD", S_P_STRING},
	{NULL}
};

static s_p_hashtbl_t *_config_make_tbl(char *filename)
{
	s_p_hashtbl_t *tbl = NULL;

	xassert(filename);

	if (!(tbl = s_p_hashtbl_create(nvram_conf_file_options))) {
		error("nvram_generic.conf: %s: s_p_hashtbl_create error: %m", __func__);
		return tbl;
	}

	if (s_p_parse_file(tbl, NULL, filename, false) == SLURM_ERROR) {
		error("nvram_generic.conf: %s: s_p_parse_file error: %m", __func__);
		s_p_hashtbl_destroy(tbl);
		tbl = NULL;
	}

	return tbl;
}

/* Log a command's arguments. */
static void _log_script_argv(char **script_argv, char *resp_msg, bool print_debug)
{
	char *cmd_line = NULL;
	int i;

	if (!print_debug)
		return;

	for (i = 0; script_argv[i]; i++) {
		if (i)
			xstrcat(cmd_line, " ");
		xstrcat(cmd_line, script_argv[i]);
	}
	info("%s", cmd_line);
	if (resp_msg && resp_msg[0])
		info("%s", resp_msg);
	xfree(cmd_line);
}

/*
 * Return time in msec since "start time"
 */
static int _tot_wait (struct timeval *start_time)
{
	struct timeval end_time;
	int msec_delay;

	gettimeofday(&end_time, NULL);
	msec_delay =   (end_time.tv_sec  - start_time->tv_sec ) * 1000;
	msec_delay += ((end_time.tv_usec - start_time->tv_usec + 500) / 1000);
	return msec_delay;
}

/* Run a script and return its stdout plus exit status */
static char *_run_script(char *cmd_path, char **script_argv, int *status)
{
	int cc, i, new_wait, resp_size = 0, resp_offset = 0;
	pid_t cpid;
	char *resp = NULL;
	int pfd[2] = { -1, -1 };

	if (access(cmd_path, R_OK | X_OK) < 0) {
		error("%s: %s can not be executed: %m", __func__, cmd_path);
		*status = 127;
		resp = xstrdup("Slurm node_features/nvram_generic configuration error");
		return resp;
	}
	if (pipe(pfd) != 0) {
		error("%s: pipe(): %m", __func__);
		*status = 127;
		resp = xstrdup("System error");
		return resp;
	}

	if ((cpid = fork()) == 0) {
		cc = sysconf(_SC_OPEN_MAX);
		dup2(pfd[1], STDERR_FILENO);
		dup2(pfd[1], STDOUT_FILENO);
		for (i = 0; i < cc; i++) {
			if ((i != STDERR_FILENO) && (i != STDOUT_FILENO))
				close(i);
		}
		setpgid(0, 0);
		execv(cmd_path, script_argv);
		error("%s: execv(%s): %m", __func__, cmd_path);
		exit(127);
	} else if (cpid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		error("%s: fork(): %m", __func__);
	} else {
		struct pollfd fds;
		struct timeval tstart;
		resp_size = 1024;
		resp = xmalloc(resp_size);
		close(pfd[1]);
		gettimeofday(&tstart, NULL);
		while (1) {
			if (slurmctld_config.shutdown_time) {
				error("%s: killing %s operation on shutdown",
				      __func__, script_argv[1]);
				break;
			}
			fds.fd = pfd[0];
			fds.events = POLLIN | POLLHUP | POLLRDHUP;
			fds.revents = 0;
			new_wait = ipmctl_timeout - _tot_wait(&tstart);
			if (new_wait <= 0) {
				error("%s: %s poll timeout @ %d msec",
				      __func__, script_argv[1], ipmctl_timeout);
				break;
			}
			new_wait = MIN(new_wait, MAX_POLL_WAIT);
			i = poll(&fds, 1, new_wait);
			if (i == 0) {
				continue;
			} else if (i < 0) {
				error("%s: %s poll:%m", __func__,
				      script_argv[1]);
				break;
			}
			if ((fds.revents & POLLIN) == 0)
				break;
			i = read(pfd[0], resp + resp_offset,
				 resp_size - resp_offset);
			if (i == 0) {
				break;
			} else if (i < 0) {
				if (errno == EAGAIN)
					continue;
				error("%s: read(%s): %m", __func__, ipmctl_path);
				break;
			} else {
				resp_offset += i;
				if (resp_offset + 1024 >= resp_size) {
					resp_size *= 2;
					resp = xrealloc(resp, resp_size);
				}
			}
		}
		killpg(cpid, SIGTERM);
		usleep(10000);
		killpg(cpid, SIGKILL);
		waitpid(cpid, status, 0);
		close(pfd[0]);
	}
	return resp;
}


extern void fini_system_nvram(void)
{
	xfree(ipmctl_path);
}

/* Get number of NVRAMs */
extern int get_nvram_number(uint16_t *has_nvram)
{
	char nvram_dir[PATH_MAX];
	char path_stats[PATH_MAX];
	DIR *proc_dir;
	FILE *fff;
	char buffer[BUFSIZ];
	bool found = false;

	debug4("Entering %s", __func__);

	sprintf(nvram_dir, "%s/", nvram_base_path);

	proc_dir = opendir(nvram_dir);
	if (proc_dir == NULL) {
		error("%s: Cannot open %s %m", __func__, nvram_dir);
		*has_nvram = 0;
		return SLURM_SUCCESS;
	}

	snprintf(path_stats, PATH_MAX, "%s/nvram", nvram_dir);
	debug3("%s: Found file %s", __func__, path_stats);

	fff = fopen(path_stats, "r");
	if (fff == NULL) {
		error("%s: Cannot open %s %m", __func__, path_stats);
		*has_nvram = 0;
		return SLURM_SUCCESS;
	}

	found = false;
	while (fgets(buffer, BUFSIZ, fff)) {

		if (found)
			break;

		if (strstr(buffer, "NVRAM Online")) {
			*has_nvram = 1;
			debug3("%s NVRAM:%u", __func__, *has_nvram);
			found = true;
		} else if (strstr(buffer, "NVRAM Offline")) {
			*has_nvram = 0;
			debug3("%s NVRAM:%u", __func__, *has_nvram);
			found = true;
		}
	}
	fclose(fff);
	closedir(proc_dir);

	debug4("Exiting %s", __func__);

	return SLURM_SUCCESS;
}

/* Get NVRAM availabilty */
extern int get_nvram_availability(uint16_t *has_nvram)
{
	FILE *ndctl_file;
	char *nvram_conf_file;
	struct stat stat_buf;
	s_p_hashtbl_t *tbl;
	int rc = SLURM_SUCCESS;

	nvram_conf_file = get_extra_conf_path("nvram_generic.conf");
	if ((stat(nvram_conf_file, &stat_buf) == 0) &&
	    (tbl = _config_make_tbl(nvram_conf_file))) {
		(void) s_p_get_string(&ipmctl_path, "IpmctlPath", tbl);

		(void) s_p_get_uint32(&ipmctl_timeout, "IpmctlTimeout", tbl);

		s_p_hashtbl_destroy(tbl);
	} else if (errno != ENOENT) {
		error("Error opening/reading nvram_generic.conf: %m");
		rc = SLURM_ERROR;
	}
	xfree(nvram_conf_file);

	if (!ipmctl_path)
		ipmctl_path = xstrdup("/usr/bin/ipmctl");
	if (access(ipmctl_path, X_OK) == 0)
		ipmctl_found = 1;
	else
		ipmctl_found = 0;

	if (access("/usr/bin/ndctl", X_OK) == 0)
		ndctl_found = 1;
	else
		ndctl_found = 0;

	ndctl_file = fopen(ndctl_path, "r");
    if (ndctl_file == NULL) {
    	error("Error opening/reading %s: %m", ndctl_path);
		*has_nvram = 0;
		return rc;
    } else {
		*has_nvram = 1;
        fclose(ndctl_file);
    }

    _get_current_mode();

	return rc;
}

static int _get_current_mode()
{
	char *resp_msg, *argv[10], *tok;
	int status = 0;
	int len = 0, rc = SLURM_SUCCESS;

	if (!ipmctl_path) {
		error("Error opening/reading nvram_generic.conf: %m");
		rc = SLURM_ERROR;
		return rc;
	}
	if (ipmctl_found == 0) {
		/* This node on cluster lacks ipmctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ipmctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		rc = SLURM_ERROR;
		return rc;
	}

	if ( checked_for_nvram_mode == true ) {
		debug2("%s: Returning Mode from last run: NVRAM Mode:%d", __func__, nvram_mode);

		return rc;
	}

	argv[0] = "ipmctl";
	argv[1] = "show";
	argv[2] = "-system";
	argv[3] = "-capabilities";
	argv[4] = NULL;

	resp_msg = _run_script(ipmctl_path, argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: ipmctl (get cluster mode) status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: ipmctl returned no information", __func__);
	} else {
		tok = NULL;
		_log_script_argv(argv, resp_msg, false);
		tok = strstr(resp_msg, "CurrentVolatileMode=");
		len = 20;

		if (tok) {
			tok += len;
			if (!xstrncasecmp(tok, "1LM", 3)) {
				/*                  123       */
				nvram_mode = 1;
			} else if (!xstrncasecmp(tok, "Memory Mode", 11)) {
				/*                         12345678901       */
				nvram_mode = 2;
			}
		xfree(resp_msg);
		}

		debug2("%s: Setting nvram_mode to %d", __func__, nvram_mode);
	}

	if ( checked_for_nvram_mode == false ) {
		checked_for_nvram_mode = true;
		debug3("%s: Setting checked_for_nvram_mode to true.", __func__);
	}

	return rc;
}

/* Get NVRAM memories */
extern int get_nvram_memory(uint32_t *capacity,
		uint32_t *memory_capacity, uint32_t *appdirect_capacity,
		bool print_debug)
{
	char *resp_msg, *save_ptr = NULL, *argv[10], *tok, *tmp = NULL;
	int status = 0;
	int len = 0, rc = SLURM_SUCCESS;
	uint32_t inaccessible_capacity = 0;
	bool set_capacity = false, set_memory_capacity = false, set_appdirect_capacity = false, set_inaccessible_capacity = false;

	if (!ipmctl_path) {
		error("Error opening/reading nvram_generic.conf: %m");
		rc = SLURM_ERROR;
		return rc;
	}
	if (ipmctl_found == 0) {
		/* This node on cluster lacks ipmctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ipmctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		rc = SLURM_ERROR;
		return rc;
	}

	if ( checked_for_nvram_memory == true ) {
		*capacity = static_capacity;
		*memory_capacity = static_memory_capacity;
		*appdirect_capacity = static_appdirect_capacity;

		debug2("%s: Setting Capacities from last run: Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, Inaccessible Capacity:%d",
					__func__, *capacity, *memory_capacity, *appdirect_capacity, static_inaccessible_capacity);

		return rc;
	}

	argv[0] = "ipmctl";
	argv[1] = "show";
	argv[2] = "-memoryresources";
	argv[3] = NULL;

	resp_msg = _run_script(ipmctl_path, argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: ipmctl (get cluster mode) status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: ipmctl returned no information", __func__);
	} else {
		tok = NULL;
		_log_script_argv(argv, resp_msg, print_debug);
		tok = strtok_r(resp_msg, "\n", &save_ptr);

		while (tok) {
			if ( !xstrncasecmp(tok, "Capacity=",9)) {
				/*                   123456789       */
				len = 9;
				tok += len;
				tmp = xstrndup(tok,4);
				*capacity = atoi(tmp);
				xfree(tmp);
				set_capacity = true;
			} else if ( !xstrncasecmp(tok, "MemoryCapacity=",15)) {
				/*                          123456789012345       */
				len = 15;
				tok += len;
				tmp = xstrndup(tok,4);
				*memory_capacity = atoi(tmp);
				xfree(tmp);
				set_memory_capacity = true;
			} else if ( !xstrncasecmp(tok, "AppDirectCapacity=",18)) {
				/*                          123456789012345678       */
				len = 18;
				tok += len;
				tmp = xstrndup(tok,4);
				*appdirect_capacity = atoi(tmp);
				xfree(tmp);
				set_appdirect_capacity = true;
			} else if ( !xstrncasecmp(tok, "InaccessibleCapacity=",21)) {
				/*                          123456789012345678901       */
				len = 21;
				tok += len;
				tmp = xstrndup(tok,4);
				inaccessible_capacity = atoi(tmp);
				xfree(tmp);
				set_inaccessible_capacity = true;
			}
			tok = strtok_r(NULL, "\n", &save_ptr);
		}
		xfree(resp_msg);

		if ( !(set_capacity && set_memory_capacity && set_appdirect_capacity && set_inaccessible_capacity) ) {
			error("%s: ipmctl did not get NVRAM memories (Capacity:%u, Memory Capacity:%u, AppDirect Capacity:%u, Inaccessible Capacity:%u)\n",
					      __func__, set_capacity, set_memory_capacity, set_appdirect_capacity, set_inaccessible_capacity);
			rc = SLURM_ERROR;
		}
		debug2("%s: Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, Inaccessible Capacity:%d",
					__func__, *capacity, *memory_capacity, *appdirect_capacity, inaccessible_capacity);
	}

	if ( rc == SLURM_SUCCESS ) {
		checked_for_nvram_memory = true;
		static_capacity = *capacity;
		static_memory_capacity = *memory_capacity;
		static_appdirect_capacity = *appdirect_capacity;
		static_inaccessible_capacity = inaccessible_capacity;
	}

	return rc;
}

/* Get NVRAM availabilty */
extern int get_free_nvram(uint32_t *free_nvram, bool print_debug)
{
	char *resp_msg, *argv[10], *tok, *tmp = NULL;
	int status = 0;
	int len = 0, rc = SLURM_SUCCESS;

	if (!ipmctl_path) {
		error("Error opening/reading nvram_generic.conf: %m");
		rc = SLURM_ERROR;
		return rc;
	}
	if (ipmctl_found == 0) {
		/* This node on cluster lacks ipmctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ipmctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		rc = SLURM_ERROR;
		return rc;
	}

	argv[0] = "ipmctl";
	argv[1] = "show";
	argv[2] = "-memoryresources";
	argv[3] = NULL;

	resp_msg = _run_script(ipmctl_path, argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: ipmctl (get cluster mode) status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: ipmctl returned no information", __func__);
	} else {
		tok = NULL;
		_log_script_argv(argv, resp_msg, print_debug);
		if ( nvram_mode == 1 ) {
			tok = strstr(resp_msg, "AppDirectCapacity=");
			/*                      123456789012345678       */
			len = 18;
		} else if ( nvram_mode == 2 ) {
			tok = strstr(resp_msg, "MemoryCapacity=");
			/*                      123456789012345      */
			len = 15;
		} else {
			error("%s: Unknown nvram_mode :%u",
					  __func__, nvram_mode);
		}

		if (tok) {
			tok += len;
			tmp = xmalloc(4);
			tmp = xstrndup(tok,4);
			*free_nvram = atoi(tmp);
			xfree(tmp);
			xfree(resp_msg);
		}
	}

	return rc;
}

/* Get number of NVRAM partitions */
extern int get_nvram_partition_number(uint16_t *nvram_partition_no)
{
	char nvram_dir[PATH_MAX];
	char path_stats[PATH_MAX];
	DIR *proc_dir;
	FILE *fff;
	char buffer[BUFSIZ];
	bool found = false;

	debug4("Entering %s", __func__);

	sprintf(nvram_dir, "%s/", nvram_base_path);

	proc_dir = opendir(nvram_dir);
	if (proc_dir == NULL) {
		error("%s: Cannot open %s %m", __func__, nvram_dir);
		*nvram_partition_no = 0;
		return SLURM_SUCCESS;
	}

	snprintf(path_stats, PATH_MAX, "%s/nvram_partitions", nvram_dir);
	debug3("%s: Found file %s", __func__, path_stats);

	fff = fopen(path_stats, "r");
	if (fff == NULL) {
		error("%s: Cannot open %s %m", __func__, path_stats);
		*nvram_partition_no = 0;
		return SLURM_SUCCESS;
	}

	found = false;
	while (fgets(buffer, BUFSIZ, fff)) {

		if (found)
			break;

		if (strstr(buffer, "NumberOfPartitions")) {
			sscanf(buffer,
			       "%*s %hu", nvram_partition_no);
			debug3("%s NVRAM:%hu", __func__, *nvram_partition_no);
			found = true;
		}
	}
	fclose(fff);
	closedir(proc_dir);

	debug4("Exiting %s", __func__);

	return SLURM_SUCCESS;

}

extern int get_namespaces(uint16_t *nvram_number_of_namespaces)
{
	char *resp_msg, *save_ptr = NULL, *argv[10], *tok;
	int status = 0, no_namespaces = 0;
	int rc = SLURM_SUCCESS;

	if (ndctl_found == 0) {
		/* This node on cluster lacks ndctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ndctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		rc = SLURM_ERROR;
		return rc;
	}

	argv[0] = "ndctl";
	argv[1] = "list";
	argv[2] = "--namespaces";
	argv[3] = NULL;

	resp_msg = _run_script("/usr/bin/ndctl", argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: ndctl (get cluster mode) status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: ndctl returned no information", __func__);
	} else {
		tok = NULL;
		_log_script_argv(argv, resp_msg, true);
		tok = strtok_r(resp_msg, "\n", &save_ptr);

		while (tok) {
			if ( xstrcasestr(tok, "sector_size")) {
				no_namespaces++;
			}
			tok = strtok_r(NULL, "\n", &save_ptr);
		}
		xfree(resp_msg);
	}

	*nvram_number_of_namespaces = no_namespaces;

	debug2("%s: Found %d namespaces", __func__, no_namespaces);

	return rc;
}

extern int get_free_mem_nvram(uint32_t *free_mem_nvram)
{
	char *resp_msg, *save_ptr = NULL, *argv[10], *tok;
	int status = 0, free_mem = 0;
	int rc = SLURM_SUCCESS;
	char temp[60], unit[10];

	if (ndctl_found == 0) {
		/* This node on cluster lacks ndctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ndctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		rc = SLURM_ERROR;
		return rc;
	}

	argv[0] = "cat";
	argv[1] = "/proc/meminfo";
	argv[2] = NULL;

	resp_msg = _run_script("/usr/bin/cat", argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: meminfo status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: meminfo returned no information", __func__);
	} else {
		tok = NULL;
		_log_script_argv(argv, resp_msg, true);
		tok = strtok_r(resp_msg, "\n", &save_ptr);

		while (tok) {
			debug2("%s: tok:%s.", __func__, tok);
			if ( !xstrncasecmp(tok, "MemFree", 6)) {
				/*                   1234567   */
				debug2("%s:          tok:%s.", __func__, tok);
				sscanf(tok, "%s %d %s", temp, &free_mem, unit);
			}
			tok = strtok_r(NULL, "\n", &save_ptr);
		}
		xfree(resp_msg);
	}

	*free_mem_nvram = free_mem / 1024 / 1024;

	debug3("%s: Found free RAM:%d GBs ", __func__, *free_mem_nvram);

	return rc;
}

extern int get_free_space_nvram(uint32_t *free_space_nvram_0, uint32_t *free_space_nvram_1)
{
	char *resp_msg, *save_ptr = NULL, *argv[10], *tok, *tmp = NULL;
	int status = 0, free_space_0 = 0, free_space_1 = 0;
	int rc = SLURM_SUCCESS;
	char dev_path[60], total_size[20], used_size[10], free_space[10], percentage[10], mount_path[50];

	if (ndctl_found == 0) {
		/* This node on cluster lacks ndctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ndctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		rc = SLURM_ERROR;
		return rc;
	}

	argv[0] = "df";
	argv[1] = "-h";
	argv[2] = NULL;

	resp_msg = _run_script("/usr/bin/df", argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: disk free status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: disk free returned no information", __func__);
	} else {
		tok = NULL;
		_log_script_argv(argv, resp_msg, true);
		tok = strtok_r(resp_msg, "\n", &save_ptr);

		while (tok) {
			debug2("%s: tok:%s.", __func__, tok);
			if ( !xstrncasecmp(tok, "/dev/pmem12", 11)) {
			//if ( !xstrncasecmp(tok, "/dev/sda3", 9)) {
				/*                   12345678901   */
				debug2("%s:          tok:%s.", __func__, tok);
				sscanf(tok, "%s %s %s %s %s %s",
						dev_path, total_size, used_size, free_space, percentage, mount_path);
				tmp = xstrdup(free_space);
				if ( xstrstr(tmp, "G") ) {
					xstrsubstituteall(tmp, "G", "");
					*free_space_nvram_0 = atoi(tmp);
				} else if ( xstrstr(tmp, "M") ) {
					xstrsubstituteall(tmp, "M", "");
					free_space_0 = atoi(tmp);
					*free_space_nvram_0 = free_space_0 / 1024;
				} else if ( xstrstr(tmp, "K") ) {
					xstrsubstituteall(tmp, "K", "");
					free_space_0 = atoi(tmp);
					*free_space_nvram_0 = free_space_0 / 1024 / 1024;
				} else if ( xstrstr(tmp, "T") ) {
					xstrsubstituteall(tmp, "T", "");
					free_space_0 = atoi(tmp);
					*free_space_nvram_0 = free_space_0 * 1024;
				} else {
					error("%s: Unknown unit:%s", __func__, tmp);
				}
				xfree(tmp);
			} else if ( !xstrncasecmp(tok, "/dev/pmem13", 11)) {
			//} else if ( !xstrncasecmp(tok, "/dev/sda1", 9)) {
				/*                          12345678901   */
				debug2("%s:          tok:%s.", __func__, tok);
				sscanf(tok, "%s %s %s %s %s %s",
						dev_path, total_size, used_size, free_space, percentage, mount_path);
				tmp = xstrdup(free_space);
				if ( xstrstr(tmp, "G") ) {
					xstrsubstituteall(tmp, "G", "");
					*free_space_nvram_1 = atoi(tmp);
				} else if ( xstrstr(tmp, "M") ) {
					xstrsubstituteall(tmp, "M", "");
					free_space_1 = atoi(tmp);
					*free_space_nvram_1 = free_space_1 / 1024;
				} else if ( xstrstr(tmp, "K") ) {
					xstrsubstituteall(tmp, "K", "");
					free_space_1 = atoi(tmp);
					*free_space_nvram_1 = free_space_1 / 1024 / 1024;
				} else if ( xstrstr(tmp, "T") ) {
					xstrsubstituteall(tmp, "T", "");
					free_space_1 = atoi(tmp);
					*free_space_nvram_1 = free_space_1 * 1024;
				} else {
					error("%s: Unknown unit:%s", __func__, free_space);
				}
				xfree(tmp);
			}
			tok = strtok_r(NULL, "\n", &save_ptr);
		}
		xfree(resp_msg);
	}

	debug3("%s: Found free space 0:%d, 1:%d GBs ",
			__func__, *free_space_nvram_0, *free_space_nvram_1);

	return rc;
}
