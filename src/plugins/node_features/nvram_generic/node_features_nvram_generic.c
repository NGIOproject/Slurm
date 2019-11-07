/*****************************************************************************\
 *  node_features_nvram_generic.c - Plugin for managing NVRAM state
 *  information on a generic Linux cluster
 *****************************************************************************
 *  Copyright (C) 2019 EPCC
 *  Written by Iakovos Panourgias <i.panourgias@epcc.ed.ac.uk>
 *  This software was developed as part of the EC H2020 funded project NEXTGenIO (Project ID: 671951) www.nextgenio.eu
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
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
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <ctype.h>
#include <fcntl.h>
#ifdef HAVE_NUMA
#undef NUMA_VERSION1_COMPATIBILITY
#include <numa.h>
#endif
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__NetBSD__)
#define POLLRDHUP POLLHUP
#endif

#include "slurm/slurm.h"

#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/fd.h"
#include "src/common/gres.h"
#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/read_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/node_scheduler.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"
#include "src/slurmd/slurmd/req.h"

/* Maximum poll wait time for child processes, in milliseconds */
#define MAX_POLL_WAIT   500
#define DEFAULT_IPMCTL_TIMEOUT 1000

/* NVRAM Configuration Modes */
#define NVRAM_MODE_CNT 2
#define NVRAM_MODE_FLAG	0x00ff
#define NVRAM_MODE_1LM  0x0001
#define NVRAM_MODE_2LM  0x0002

#ifndef DEFAULT_NVRAM_SIZE
#define DEFAULT_NVRAM_SIZE ((uint64_t) 16 * 1024 * 1024 * 1024)
#endif

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
slurmctld_config_t slurmctld_config __attribute__((weak_import));
#else
slurmctld_config_t slurmctld_config;
#endif

/*
 * These variables are required by the node features plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "node_features" for Slurm node_features) and <method> is a
 * description of how this plugin satisfies that application.  Slurm will only
 * load a node_features plugin if the plugin_type string has a prefix of
 * "node_features/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "node_features nvram_generic plugin";
const char plugin_type[]        = "node_features/nvram_generic";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/* Configuration Parameters */
static uint16_t allow_nvram_mode = NVRAM_MODE_FLAG;
static uid_t *allowed_uid = NULL;
static int allowed_uid_cnt = 0;
static uint32_t boot_time = (25 * 60);	/* 25 minute estimated boot time */
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool debug_flag = false;
static uint16_t default_nvram_mode = NVRAM_MODE_1LM;
static uint32_t node_reboot_weight = (INFINITE - 1);
static uint32_t ipmctl_timeout = 0;
static bool reconfig = false;
static time_t shutdown_time = 0;
static int ipmctl_found = -1;
static char *ipmctl_path = NULL;
static char *bmc_password = NULL;
static char *bmc_ip_address = NULL;
static uint32_t force_load = 0;
static int hw_is_nvram = -1;
static bitstr_t *nvram_node_bitmap = NULL;	// NVRAM nodes found by ipmctl
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

static void _free_command_argv(char **script_argv);
static s_p_hashtbl_t *_config_make_tbl(char *filename);
static int _nvram_mode_bits_cnt(uint16_t mode_num);
static uint16_t _nvram_mode_parse(char *mode_str, char *sep);
static char *_nvram_mode_str(uint16_t mode_num);
static uint16_t _nvram_mode_token(char *token);
static int  _tot_wait (struct timeval *start_time);
static void _log_script_argv(char **script_argv, char *resp_msg);
static char *_run_script(char *cmd_path, char **script_argv, int *status);
static int _change_platform_mode(char* active_features);
static int _check_platform_mode();


static void _free_command_argv(char **script_argv)
{
	int i;

	for (i = 0; script_argv[i]; i++)
	        xfree(script_argv[i]);
	xfree(script_argv);
}

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

/*
 * Return the count of NVRAM mode bits set
 */
static int _nvram_mode_bits_cnt(uint16_t mode_num)
{
	int cnt = 0, i;
	uint16_t tmp = 1;

	for (i = 0; i < 16; i++) {
		if ((mode_num & NVRAM_MODE_FLAG) & tmp)
			cnt++;
		tmp = tmp << 1;
	}
	return cnt;
}

/*
 * Translate NVRAM Mode string to equivalent numeric value
 * mode_str IN - String to scan
 * sep IN - token separator to search for
 * RET NVRAM Mode numeric value
 */
static uint16_t _nvram_mode_parse(char *mode_str, char *sep)
{
	char *save_ptr = NULL, *tmp, *tok;
	uint16_t mode_num = 0;

	if (!mode_str)
		return mode_num;

	tmp = xstrdup(mode_str);
	tok = strtok_r(tmp, sep, &save_ptr);
	while (tok) {
		mode_num |= _nvram_mode_token(tok);
		tok = strtok_r(NULL, sep, &save_ptr);
	}
	xfree(tmp);

	return mode_num;
}

/*
 * Translate NVRAM mode number to equivalent string value
 * Caller must free return value
 */
static char *_nvram_mode_str(uint16_t mode_num)
{
	char *mode_str = NULL, *sep = "";

	if (mode_num & NVRAM_MODE_1LM) {
		xstrfmtcat(mode_str, "%s1LM", sep);
		sep = ",";
	}
	if (mode_num & NVRAM_MODE_2LM) {
		xstrfmtcat(mode_str, "%s2LM", sep);
		sep = ",";
	}

	return mode_str;

}

/*
 * Given a NVRAM mode token, return its equivalent numeric value
 * token IN - String to scan
 * RET NVRAM mode numeric value
 */
static uint16_t _nvram_mode_token(char *token)
{
	uint16_t mode_num = 0;

	if (!xstrcasecmp(token, "1LM"))
		mode_num |= NVRAM_MODE_1LM;
	else if (!xstrcasecmp(token, "2LM"))
		mode_num |= NVRAM_MODE_2LM;

	return mode_num;
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

/* Log a command's arguments. */
static void _log_script_argv(char **script_argv, char *resp_msg)
{
	char *cmd_line = NULL;
	int i;

	if (!debug_flag)
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

static void _make_uid_array(char *uid_str)
{
	char *save_ptr = NULL, *tmp_str, *tok;
	int i, uid_cnt = 0;

	if (!uid_str)
		return;

	/* Count the number of users */
	for (i = 0; uid_str[i]; i++) {
		if (uid_str[i] == ',')
			uid_cnt++;
	}
	uid_cnt++;

	allowed_uid = xmalloc(sizeof(uid_t) * uid_cnt);
	allowed_uid_cnt = 0;
	tmp_str = xstrdup(uid_str);
	tok = strtok_r(tmp_str, ",", &save_ptr);
	while (tok) {
		if (uid_from_string(tok, &allowed_uid[allowed_uid_cnt++]) < 0)
			error("nvram_generic.conf: Invalid AllowUserBoot: %s", tok);
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	xfree(tmp_str);
}

static char *_make_uid_str(uid_t *uid_array, int uid_cnt)
{
	char *sep = "", *tmp_str = NULL, *uid_str = NULL;
	int i;

	if (allowed_uid_cnt == 0) {
		uid_str = xstrdup("ALL");
		return uid_str;
	}

	for (i = 0; i < uid_cnt; i++) {
		tmp_str = uid_to_string(uid_array[i]);
		xstrfmtcat(uid_str, "%s%s(%d)", sep, tmp_str, uid_array[i]);
		xfree(tmp_str);
		sep = ",";
	}

	return uid_str;
}

/* Load configuration */
extern int init(void)
{
	char *allow_nvram_mode_str, *default_nvram_mode_str, *allow_user_str;
	char *nvram_conf_file, *tmp_str = NULL, *resume_program;
	s_p_hashtbl_t *tbl;
	struct stat stat_buf;
	int rc = SLURM_SUCCESS;
	char *ndctl_path = "/dev/ndctl0";
	FILE *ndctl_file;

	/* Set default values */
	allow_nvram_mode = NVRAM_MODE_FLAG;
	xfree(ipmctl_path);
	ipmctl_timeout = DEFAULT_IPMCTL_TIMEOUT;
	debug_flag = false;
	default_nvram_mode = NVRAM_MODE_1LM;
	xfree(allowed_uid);
	allowed_uid_cnt = 0;
	xfree(bmc_ip_address);
	xfree(bmc_password);

	if (slurm_get_debug_flags() & DEBUG_FLAG_NODE_FEATURES)
		debug_flag = true;

	nvram_conf_file = get_extra_conf_path("nvram_generic.conf");
	if ((stat(nvram_conf_file, &stat_buf) == 0) &&
	    (tbl = _config_make_tbl(nvram_conf_file))) {
		if (s_p_get_string(&tmp_str, "AllowNVRAMMode", tbl)) {
			allow_nvram_mode = _nvram_mode_parse(tmp_str, ",");
			if (_nvram_mode_bits_cnt(allow_nvram_mode) < 1) {
				fatal("nvram_generic.conf: Invalid AllowNVRAMMode=%s",
				      tmp_str);
			}
			xfree(tmp_str);
		}
		if (s_p_get_string(&tmp_str, "AllowUserBoot", tbl)) {
			_make_uid_array(tmp_str);
			xfree(tmp_str);
		}

		(void) s_p_get_uint32(&boot_time, "BootTime", tbl);

		if (s_p_get_string(&tmp_str, "DefaultNVRAMMode", tbl)) {
			default_nvram_mode = _nvram_mode_parse(tmp_str, ",");
			if (_nvram_mode_bits_cnt(default_nvram_mode) != 1) {
				fatal("nvram_generic.conf: Invalid DefaultNVRAMMode=%s",
				      tmp_str);
			}
			xfree(tmp_str);
		}
		(void) s_p_get_uint32(&force_load, "Force", tbl);

		(void) s_p_get_uint32(&node_reboot_weight, "NodeRebootWeight",
				      tbl);

		(void) s_p_get_string(&ipmctl_path, "IpmctlPath", tbl);

		(void) s_p_get_uint32(&ipmctl_timeout, "IpmctlTimeout", tbl);

		(void) s_p_get_string(&bmc_ip_address, "BMCIP", tbl);

		(void) s_p_get_string(&bmc_password, "BMCPASSWORD", tbl);

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

	hw_is_nvram = 0;
	ndctl_file = fopen(ndctl_path, "r");
	if (ndctl_file == NULL) {
		error("Error opening/reading %s: %m", ndctl_path);
	} else {
		hw_is_nvram = 1;
		fclose(ndctl_file);
	}

	if ((resume_program = slurm_get_resume_program())) {
		error("Use of ResumeProgram with %s not currently supported",
		      plugin_name);
		xfree(resume_program);
		rc = SLURM_ERROR;
	}

	if (slurm_get_debug_flags() & DEBUG_FLAG_NODE_FEATURES) {
		allow_nvram_mode_str = _nvram_mode_str(allow_nvram_mode);
		default_nvram_mode_str = _nvram_mode_str(default_nvram_mode);
		allow_user_str = _make_uid_str(allowed_uid, allowed_uid_cnt);
		info("AllowNVRAMMode=%s DefaultNVRAMMode=%s",
				allow_nvram_mode_str, default_nvram_mode_str);
		info("AllowUserBoot=%s", allow_user_str);
		info("BootTIme=%u", boot_time);
		info("Force=%u", force_load);
		info("NodeRebootWeight=%u", node_reboot_weight);
		info("IpmctlPath=%s (Found=%d)", ipmctl_path, ipmctl_found);
		info("IpmctlTimeout=%u msec", ipmctl_timeout);
		info("BMCIP=%s", bmc_ip_address);
		info("BMCPASSWORD=%s", bmc_password);
		xfree(allow_nvram_mode_str);
		xfree(default_nvram_mode_str);
		xfree(allow_user_str);
	}
	gres_plugin_add("nvmem");

	return rc;
}

/* Release allocated memory */
extern int fini(void)
{
	shutdown_time = time(NULL);

	debug_flag = false;
	xfree(ipmctl_path);
	xfree(allowed_uid);
	allowed_uid_cnt = 0;
	FREE_NULL_BITMAP(nvram_node_bitmap);

	return SLURM_SUCCESS;
}

/* Reload configuration */
extern int node_features_p_reconfig(void)
{
	slurm_mutex_lock(&config_mutex);
	reconfig = true;
	slurm_mutex_unlock(&config_mutex);
	return SLURM_SUCCESS;
}

/* Update active and available features on specified nodes,
 * sets features on all nodes if node_list is NULL */
extern int node_features_p_get_node(char *node_list)
{
	slurm_mutex_lock(&config_mutex);
	if (reconfig) {
		(void) init();
		reconfig = false;
	}
	slurm_mutex_unlock(&config_mutex);
	return SLURM_SUCCESS;
}

/* Get this node's current and available NVRAM settings from BIOS.
 * avail_modes IN/OUT - append available modes, must be xfreed
 * current_mode IN/OUT - append current modes, must be xfreed
 *
 * NOTES about syscfg (from Intel):
 * To display the BIOS Parameters:
 * >> syscfg /d biossettings <"BIOS variable Name">
 *
 * To Set the BIOS Parameters:
 * >> syscfg /bcs <AdminPw> <"BIOS variable Name"> <Value>
 * Note: If AdminPw is not set use ""
 */
extern void node_features_p_node_state(char **avail_modes, char **current_mode)
{
	char *avail_states = NULL, *cur_state = NULL;
	char *resp_msg, *argv[10], *avail_sep = "", *cur_sep = "", *tok;
	char *tok1, *save_ptr = NULL, *tmp = NULL;
	int status = 0, len = 0;
	bool set_capacity = false, set_memory_capacity = false, set_appdirect_capacity = false, set_inaccessible_capacity = false;
	uint32_t capacity = 0, memory_capacity = 0, appdirect_capacity = 0, inaccessible_capacity = 0;

	if (!ipmctl_path || !avail_modes || !current_mode)
		return;
	if ((ipmctl_found == 0) || (!hw_is_nvram && !force_load)) {
		/* This node on cluster lacks ipmctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			info("%s: ipmctl program not found or node isn't NVRAM, can not get NVRAM modes",
			     __func__);
			log_event = false;
		}
		*avail_modes = NULL;
		*current_mode = NULL;
		return;
	}

	// Get system capabilities : CurrentVolatileMode=1LM / Memory Mode
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
		_log_script_argv(argv, resp_msg);
		tok = strstr(resp_msg, "CurrentVolatileMode=");
		len = 20;

		if (tok) {
			tok += len;
			if (!xstrncasecmp(tok, "1LM", 3)) {
				cur_state = xstrdup("1LM");
				nvram_mode = 1;
				cur_sep = ",";
			} else if (!xstrncasecmp(tok, "Memory Mode", 3)) {
				cur_state = xstrdup("2LM");
				nvram_mode = 2;
				cur_sep = ",";
			}
		}
	}
	xfree(resp_msg);

	// Get memory resources : Capacity / MemoryCapacity / AppDirectCapacity
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
		tok1 = NULL;
		_log_script_argv(argv, resp_msg);
		tok1 = strtok_r(resp_msg, "\n", &save_ptr);

		while (tok1) {
			if ( !xstrncasecmp(tok1, "Capacity=",9)) {
				/*                    123456789       */
				len = 9;
				tok1 += len;
				tmp = xstrndup(tok1,4);
				capacity = atoi(tmp);
				xfree(tmp);
				set_capacity = true;
			} else if ( !xstrncasecmp(tok1, "MemoryCapacity=",15)) {
				/*                           123456789012345       */
				len = 15;
				tok1 += len;
				tmp = xstrndup(tok1,4);
				memory_capacity = atoi(tmp);
				xfree(tmp);
				set_memory_capacity = true;
			} else if ( !xstrncasecmp(tok1, "AppDirectCapacity=",18)) {
				/*                           123456789012345678       */
				len = 18;
				tok1 += len;
				tmp = xstrndup(tok1,4);
				appdirect_capacity = atoi(tmp);
				xfree(tmp);
				set_appdirect_capacity = true;
			} else if ( !xstrncasecmp(tok1, "InaccessibleCapacity=",21)) {
				/*                           123456789012345678901       */
				len = 21;
				tok1 += len;
				tmp = xstrndup(tok1,4);
				inaccessible_capacity = atoi(tmp);
				xfree(tmp);
				set_inaccessible_capacity = true;
			}
			tok1 = strtok_r(NULL, "\n", &save_ptr);
		}
		xfree(resp_msg);

		if ( !(set_capacity && set_memory_capacity && set_appdirect_capacity && set_inaccessible_capacity) ) {
			error("%s: ipmctl did not get NVRAM memories (Capacity:%u, Memory Capacity:%u, AppDirect Capacity:%u, InaccessibleCapacity:%u)\n",
					      __func__, set_capacity, set_memory_capacity, set_appdirect_capacity, set_inaccessible_capacity);
			return;
		}
		debug2("%s: Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, Inaccessible Capacity:%d",
				__func__, capacity, memory_capacity, appdirect_capacity, inaccessible_capacity);
	}

	if ( nvram_mode == 1 ) {
		if ( (capacity - appdirect_capacity < 100) ) {
			// Pure 1LM
			debug2("%s: Pure 1LM node. Setting 1LM as available feature.", __func__);
			xstrfmtcat(avail_states, "%s%s", avail_sep, "1LM");
			avail_sep = ",";
		} else if ( inaccessible_capacity > 100 ) {
			// Mixed mode
			debug2("%s: Mixed mode node. Setting 1LM and 2LM as available features.", __func__);
			xstrfmtcat(avail_states, "%s%s", avail_sep, "1LM");
			avail_sep = ",";
			xstrfmtcat(avail_states, "%s%s", avail_sep, "2LM");
			avail_sep = ",";
		} else {
			error("%s: Unknown state (1): Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, Inaccessible Capacity:%d, NVRAM Mode:%d",
					__func__, capacity, memory_capacity, appdirect_capacity, inaccessible_capacity, nvram_mode);

			return;
		}
	} else if ( nvram_mode == 2 ) {
		if ( (capacity - memory_capacity < 100) ) {
			// Pure 2LM
			debug2("%s: Pure 2LM node. Setting 2LM as available feature.", __func__);
			xstrfmtcat(avail_states, "%s%s", avail_sep, "2LM");
			avail_sep = ",";
		} else if ( appdirect_capacity > 100 ) {
			// Mixed mode
			debug2("%s: Mixed mode node. Setting 1LM and 2LM as available features.", __func__);
			xstrfmtcat(avail_states, "%s%s", avail_sep, "1LM");
			avail_sep = ",";
			xstrfmtcat(avail_states, "%s%s", avail_sep, "2LM");
			avail_sep = ",";
		} else {
			error("%s: Unknown state (2): Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, Inaccessible Capacity:%d, NVRAM Mode:%d",
					__func__, capacity, memory_capacity, appdirect_capacity, inaccessible_capacity, nvram_mode);

			return;
		}
	} else {
		error("%s: Unknown state (3): Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, Inaccessible Capacity:%d, NVRAM Mode:%d",
				__func__, capacity, memory_capacity, appdirect_capacity, inaccessible_capacity, nvram_mode);

		return;
	}
/*
	if ( (memory_capacity > 1) && (appdirect_capacity > 1) ) {
		// Mixed Mode
		debug2("%s: Mixed mode node. Setting 1LM and 2LM as available features.", __func__);
		xstrfmtcat(avail_states, "%s%s", avail_sep, "1LM");
		avail_sep = ",";
		xstrfmtcat(avail_states, "%s%s", avail_sep, "2LM");
		avail_sep = ",";
	} else if ( memory_capacity > 1 ) {
		if ( nvram_mode == 2 ) {
			// Pure 2LM mode
			debug2("%s: Pure 2LM node. Setting 2LM as available feature.", __func__);
			xstrfmtcat(avail_states, "%s%s", avail_sep, "2LM");
			avail_sep = ",";
		} else {
			error("%s: Unknown state: Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, NVRAM Mode:%d",
					__func__, capacity, memory_capacity, appdirect_capacity, nvram_mode);

			return;
		}
	} else if ( appdirect_capacity > 1 ) {
		if ( nvram_mode == 1 ) {
			// Pure 1LM mode
			debug2("%s: Pure 1LM node. Setting 1LM as available feature.", __func__);
			xstrfmtcat(avail_states, "%s%s", avail_sep, "1LM");
			avail_sep = ",";
		} else {
			error("%s: Unknown state: Capacity:%d, Memory Capacity:%d, AppDirect Capacity:%d, NVRAM Mode:%d",
					__func__, capacity, memory_capacity, appdirect_capacity, nvram_mode);

			return;
		}
	}
*/

	if ( avail_states == NULL ) {
		error("%s: Could not get available Platform Modes.", __func__);
		return;
	}

	if (*avail_modes) {	/* Append for multiple node_features plugins */
		if (*avail_modes[0])
			avail_sep = ",";
		else
			avail_sep = "";
		xstrfmtcat(*avail_modes, "%s%s", avail_sep, avail_states);
		xfree(avail_states);
	} else {
		*avail_modes = avail_states;
	}

	if (*current_mode) {	/* Append for multiple node_features plugins */
		if (*current_mode[0])
			cur_sep = ",";
		else
			cur_sep = "";
		xstrfmtcat(*current_mode, "%s%s", cur_sep, cur_state);
		xfree(cur_state);
	} else {
		*current_mode = cur_state;
	}

}

/* Test if a job's feature specification is valid */
extern int node_features_p_job_valid(char *job_features)
{
	uint16_t job_nvram_mode;
	int nvram_mode_cnt;
	int last_nvram_mode_cnt = 0;
	int rc = SLURM_SUCCESS;
	char *tmp, *tok, *save_ptr = NULL;

	if ((job_features == NULL) || (job_features[0] == '\0'))
		return SLURM_SUCCESS;

	tmp = xstrdup(job_features);
	tok = strtok_r(tmp, "", &save_ptr);
	while (tok) {
		job_nvram_mode = _nvram_mode_parse(tok, ",");
		nvram_mode_cnt = _nvram_mode_bits_cnt(job_nvram_mode) + last_nvram_mode_cnt;
		if (nvram_mode_cnt > 1) {	/* Multiple ANDed NVRAM options */
			rc = ESLURM_INVALID_NVRAM;
			break;
		}
		tok = strtok_r(NULL, "", &save_ptr);
	}
	xfree(tmp);

	return rc;
}

/*
 * Translate a job's feature request to the node features needed at boot time.
 * IN job_features - job's --constraint specification
 * RET features required on node reboot. Must xfree to release memory
 */
extern char *node_features_p_job_xlate(char *job_features)
{
	char *node_features = NULL;
	char *tmp, *save_ptr = NULL, *mult, *sep = "", *tok;
	bool has_nvram = false;

	if ((job_features == NULL) || (job_features[0] ==  '\0'))
		return node_features;

	tmp = xstrdup(job_features);
	tok = strtok_r(tmp, "[]()|&", &save_ptr);
	while (tok) {
		bool nvram_opt = false;
		if ((mult = strchr(tok, '*')))
			mult[0] = '\0';
		if (_nvram_mode_token(tok)) {
			if (!has_nvram) {
				has_nvram = true;
				nvram_opt = true;
			}
		}
		if (nvram_opt) {
			xstrfmtcat(node_features, "%s%s", sep, tok);
			sep = ",";
		}
		tok = strtok_r(NULL, "[]()|&", &save_ptr);
	}
	xfree(tmp);

	return node_features;
}

static int _change_platform_mode(char* active_features)
{
	char **curl_argv;
	char *curl_resp_msg;
	int status = SLURM_SUCCESS, i = 0;

	curl_argv = xmalloc(sizeof(char *) * 30);

	if (strstr(active_features, "1LM"))
		curl_argv[0] = xstrdup("/opt/software/slurm/pdshchangeplatformto1LM.sh");
	else if (strstr(active_features, "2LM"))
		curl_argv[0] = xstrdup("/opt/software/slurm/pdshchangeplatformto2LM.sh");
	else {
		error("%s: Invalid active feature: %s", __func__, active_features);
		_free_command_argv(curl_argv);
		return SLURM_ERROR;
	}
	curl_argv[1] = NULL;

	debug3("%s: I will run:", __func__);
	for ( i = 0; i < 1 ; i++ )
		debug3("   %2d : %s", i, curl_argv[i]);
	if (strstr(active_features, "1LM"))
		curl_resp_msg = _run_script("/opt/software/slurm/pdshchangeplatformto1LM.sh", curl_argv, &status);
	else if (strstr(active_features, "2LM"))
		curl_resp_msg = _run_script("/opt/software/slurm/pdshchangeplatformto2LM.sh", curl_argv, &status);
	else {
		error("%s: Invalid active feature: %s", __func__, active_features);
		_free_command_argv(curl_argv);
		return SLURM_ERROR;
	}
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: curl (set %s mode) status:%u response:%s",
			  __func__, active_features, status, curl_resp_msg);
		status = SLURM_ERROR;
	}  else {
		_log_script_argv(curl_argv, curl_resp_msg);
	}
	_free_command_argv(curl_argv);
	xfree(curl_resp_msg);

	return status;
}


static int _check_platform_mode()
{
	char **curl_argv;
	char *curl_resp_msg, *tok;
	char *user_password = NULL, *ip_address = NULL;
	int status = SLURM_ERROR, i = 0;
	bool found_match = false;

	curl_argv = xmalloc(sizeof(char *) * 20);

	curl_argv[0] = xstrdup("/usr/bin/curl");
	curl_argv[1] = xstrdup("--globoff");
	curl_argv[2] = xstrdup("--insecure");
	curl_argv[3] = xstrdup("--user");
	user_password = xstrdup("admin:");
	xstrcat(user_password, bmc_password);
	curl_argv[4] = xstrdup(user_password);
	curl_argv[5] = xstrdup("--header");
	curl_argv[6] = xstrdup("\"Accept: application/json\"");
	curl_argv[7] = xstrdup("--header");
	curl_argv[8] = xstrdup("\"Content-type: application/json\"");
	curl_argv[9] = xstrdup("--location");
	curl_argv[10] = xstrdup("--request");
	curl_argv[11] = xstrdup("GET");
	curl_argv[12] = xstrdup("--include");
	ip_address = xstrdup("https://");
	xstrcat(ip_address, bmc_ip_address);
	xstrcat(ip_address, "/sessionInformation/1/status");
	curl_argv[13] = xstrdup(ip_address);
	curl_argv[14] = NULL;

	debug3("%s: I will run:", __func__);
	for ( i = 0; i < 14 ; i++ )
		debug3("   %2d : %s", i, curl_argv[i]);
	curl_resp_msg = _run_script("/usr/bin/curl", curl_argv, &status);
	xfree(user_password);
	xfree(ip_address);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: curl (get mode) status:%u response:%s",
			  __func__, status, curl_resp_msg);
		_free_command_argv(curl_argv);
		xfree(curl_resp_msg);
		return SLURM_ERROR;
	}
	if (curl_resp_msg == NULL) {
		info("%s: curl (get mode) returned no information", __func__);
		status = SLURM_ERROR;
	} else {
		tok = NULL;
		_log_script_argv(curl_argv, curl_resp_msg);
		tok = strstr(curl_resp_msg, "terminated regularly");

		if (tok) {
			debug2("%s: Found string. Platform mode successfully set.", __func__);
			status = SLURM_SUCCESS;
			found_match = true;
		}
	}

	if ( !(found_match) ) {
		error("%s: did not find string \"terminated regularly\". Error.", __func__);
		status = SLURM_ERROR;
	}
	_free_command_argv(curl_argv);
	xfree(curl_resp_msg);

	return status;
}


/* Set's the node's active features based upon job constraints.
 * NOTE: Executed by the slurmd daemon.
 * IN active_features - New active features
 * RET error code */
extern int node_features_p_node_set(char *active_features)
{
	int error_code = SLURM_SUCCESS, status = SLURM_SUCCESS, i = 0;

	if ((active_features == NULL) || (active_features[0] == '\0'))
		return SLURM_SUCCESS;

	if (!ipmctl_path) {
		error("%s: IpmctlPath not configured", __func__);
		return SLURM_ERROR;
	}
	if ((ipmctl_found == 0) || (!hw_is_nvram && !force_load)) {
		/* This node on cluster lacks ipmctl; should not be NVRAM */
		static bool log_event = true;
		if (log_event) {
			error("%s: ipmctl program not found or node isn't NVRAM; can not set NVRAM modes",
			      __func__);
			log_event = false;
		}
		return SLURM_ERROR;
	}

	status = _change_platform_mode(active_features);
	if ( status != SLURM_SUCCESS ) {
		error("%s: Failed to update platform mode.", __func__);
		return SLURM_ERROR;
	}


	for ( i = 0; i < 20 ; i++ ) {
		sleep(2);
		status = _check_platform_mode();
		if ( status != SLURM_SUCCESS ) {
			error("%s: Failed to check platform mode (%d).", __func__, i);
		} else if ( status == SLURM_SUCCESS )
			i = 20;
	}

	if ( status != SLURM_SUCCESS ) {
		error("%s: Failed to update platform mode.", __func__);
		return SLURM_ERROR;
	}

	/* Clear features, do not pass as argument to reboot program
	 * (assuming we are calling /sbin/reboot). */
	active_features[0] = '\0';

	return error_code;
}

/* Return bitmap of NVRAM nodes, NULL if none identified */
extern bitstr_t *node_features_p_get_node_bitmap(void)
{
	if (nvram_node_bitmap)
		return bit_copy(nvram_node_bitmap);
	return NULL;
}

/* Return count of overlaping bits in active_bitmap and nvram_node_bitmap */
extern int node_features_p_overlap(bitstr_t *active_bitmap)
{
	int cnt = 0;

	if (!nvram_node_bitmap || !active_bitmap ||
	    !(cnt = bit_overlap(active_bitmap, nvram_node_bitmap)))
		return 0;

	return cnt;
}
/* Return true if the plugin requires PowerSave mode for booting nodes */
extern bool node_features_p_node_power(void)
{
	return false;
}

/*
 * Note the active features associated with a set of nodes have been updated.
 * Specifically update the node's "nvram" GRES values as needed.
 * IN active_features - New active features
 * IN node_bitmap - bitmap of nodes changed
 * RET error code
 */
extern int node_features_p_node_update(char *active_features,
				       bitstr_t *node_bitmap)
{
	int i, i_first, i_last;
	int rc = SLURM_SUCCESS;
	uint64_t nvram_size;
	struct node_record *node_ptr;
	//char *save_ptr = NULL, *tmp, *tok;

	xassert(node_bitmap);
	i_first = bit_ffs(node_bitmap);
	if (i_first >= 0)
		i_last = bit_fls(node_bitmap);
	else
		i_last = i_first - 1;
	for (i = i_first; i <= i_last; i++) {
		if (!bit_test(node_bitmap, i))
			continue;
		if (i >= node_record_count) {
			error("%s: Invalid node index (%d >= %d)",
			      __func__, i, node_record_count);
			rc = SLURM_ERROR;
			break;
		}
		node_ptr = node_record_table_ptr + i;
		nvram_size = 0;
		if (!node_ptr->gres)
			node_ptr->gres = xstrdup(node_ptr->config_ptr->gres);
			gres_plugin_node_feature(node_ptr->name, "nvmem",
					nvram_size, &node_ptr->gres,
						 &node_ptr->gres_list);
	}

	return rc;
}

/*
 * Return TRUE if the specified node update request is valid with respect
 * to features changes (i.e. don't permit a non-NVRAM node to set NVRAM features).
 *
 * arg IN - Pointer to struct node_record record
 * update_node_msg IN - Pointer to update request
 */
extern bool node_features_p_node_update_valid(void *arg,
					update_node_msg_t *update_node_msg)
{
	struct node_record *node_ptr = (struct node_record *) arg;
	char *tmp, *save_ptr = NULL, *tok;
	bool is_nvram = false, invalid_feature = false;

	/* No feature changes */
	if (!update_node_msg->features && !update_node_msg->features_act)
		return true;

	/* Determine if this is NVRAM node based upon current features */
	if (node_ptr->features && node_ptr->features[0]) {
		tmp = xstrdup(node_ptr->features);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if (_nvram_mode_token(tok)) {
				is_nvram = true;
				break;
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
	}
	if (is_nvram)
		return true;

	/* Validate that AvailableFeatures update request has no NVRAM modes */
	if (update_node_msg->features) {
		tmp = xstrdup(update_node_msg->features);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if (_nvram_mode_token(tok)) {
				invalid_feature = true;
				break;
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
		if (invalid_feature) {
			info("Invalid AvailableFeatures update request (%s) for non-NVRAM node %s",
			     update_node_msg->features, node_ptr->name);
			return false;
		}
	}

	/* Validate that ActiveFeatures update request has no NVRAM modes */
	if (update_node_msg->features_act) {
		tmp = xstrdup(update_node_msg->features_act);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if (_nvram_mode_token(tok)) {
				invalid_feature = true;
				break;
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
		if (invalid_feature) {
			info("Invalid ActiveFeatures update request (%s) for non-NVRAM node %s",
			     update_node_msg->features_act, node_ptr->name);
			return false;
		}
	}

	/*
	 * For non-NVRAM node, active and available features must match
	 */
	if (!update_node_msg->features) {
		update_node_msg->features =
			xstrdup(update_node_msg->features_act);
	} else if (!update_node_msg->features_act) {
		update_node_msg->features_act =
			xstrdup(update_node_msg->features);
	} else if (xstrcmp(update_node_msg->features,
			   update_node_msg->features_act)) {
		info("Invalid ActiveFeatures != AvailableFeatures (%s != %s) for non-NVRAM node %s",
		     update_node_msg->features, update_node_msg->features_act,
		     node_ptr->name);
		return false;
	}

	return true;
}

/* Return TRUE if this (one) feature name is under this plugin's control */
extern bool node_features_p_changeable_feature(char *feature)
{
	if (_nvram_mode_token(feature))
		return true;
	return false;
}

/*
 * Translate a node's feature specification by replacing any features associated
 *	with this plugin in the original value with the new values, preserving
 *	any features that are not associated with this plugin
 * IN new_features - newly active features
 * IN orig_features - original active features
 * IN avail_features - original available features
 * IN node_inx - index of node in node table
 * RET node's new merged features, must be xfreed
 */
extern char *node_features_p_node_xlate(char *new_features, char *orig_features,
					char *avail_features, int node_inx)
{
	char *node_features = NULL;
	char *tmp, *save_ptr = NULL, *sep = "", *tok;
	uint16_t new_nvram_mode = 0;
	uint16_t tmp_nvram_mode;
	bool is_nvram = false;

	if (avail_features) {
		tmp = xstrdup(avail_features);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if (_nvram_mode_token(tok)) {
				is_nvram = true;
			} else {
				xstrfmtcat(node_features, "%s%s", sep, tok);
				sep = ",";
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
		if (!is_nvram) {
			xfree(node_features);
			sep = "";
		}
	}

	if (new_features) {
		/* Copy non-NVRAM features */
		if (!is_nvram && new_features) {
			tmp = xstrdup(new_features);
			tok = strtok_r(tmp, ",", &save_ptr);
			while (tok) {
				if ((_nvram_mode_token(tok) == 0)) {
					xstrfmtcat(node_features, "%s%s", sep,
						   tok);
					sep = ",";
				}
				tok = strtok_r(NULL, ",", &save_ptr);
			}
			xfree(tmp);
		}

		/* Copy new NVRAM features in order */
		tmp = xstrdup(new_features);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if ((tmp_nvram_mode = _nvram_mode_token(tok)))
				new_nvram_mode |= tmp_nvram_mode;
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);

		if (is_nvram && (new_nvram_mode == 0)) {
			/*
			 * New active features lacks current NVRAM mode,
			 * copy values from original
			 */
			tmp = xstrdup(orig_features);
			tok = strtok_r(tmp, ",", &save_ptr);
			while (tok) {
				if ((new_nvram_mode == 0) &&
				    (tmp_nvram_mode = _nvram_mode_token(tok)))
					new_nvram_mode |= tmp_nvram_mode;
				tok = strtok_r(NULL, ",", &save_ptr);
			}
			xfree(tmp);
		}
		if (new_nvram_mode) {
			tmp = _nvram_mode_str(new_nvram_mode);
			xstrfmtcat(node_features, "%s%s", sep, tmp);
			xfree(tmp);
			sep = ",";
		}
	}

	if (is_nvram) {
		if (!nvram_node_bitmap)
			nvram_node_bitmap = bit_alloc(node_record_count);
		bit_set(nvram_node_bitmap, node_inx);
	}

	return node_features;
}

/* Translate a node's new feature specification into a "standard" ordering
 * RET node's new merged features, must be xfreed */
extern char *node_features_p_node_xlate2(char *new_features)
{
	char *node_features = NULL;
	char *tmp, *save_ptr = NULL, *sep = "", *tok;
	uint16_t new_nvram_mode = 0;
	uint16_t tmp_nvram_mode;

	if (new_features && *new_features) {
		tmp = xstrdup(new_features);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if ((tmp_nvram_mode = _nvram_mode_token(tok))) {
				new_nvram_mode |= tmp_nvram_mode;
			} else {
				xstrfmtcat(node_features, "%s%s", sep, tok);
				sep = ",";
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
		if (new_nvram_mode) {
			tmp = _nvram_mode_str(new_nvram_mode);
			xstrfmtcat(node_features, "%s%s", sep, tmp);
			xfree(tmp);
			sep = ",";
		}
	}

	return node_features;
}

/* Perform set up for step launch
 * mem_sort IN - Trigger sort of memory pages (KNL zonesort)
 * numa_bitmap IN - NUMA nodes allocated to this job */
extern void node_features_p_step_config(bool mem_sort, bitstr_t *numa_bitmap)
{
#ifdef HAVE_NUMA
  return;
#endif
}

/* Determine if the specified user can modify the currently available node
 * features */
extern bool node_features_p_user_update(uid_t uid)
{
	static int reboot_allowed = -1;
	int i;

	if (reboot_allowed == -1) {
		char *reboot_program = slurm_get_reboot_program();
		if (reboot_program && reboot_program[0])
			reboot_allowed = 1;
		else
			reboot_allowed = 0;
		xfree(reboot_program);
	}

	if (reboot_allowed != 1) {
		info("Change in NVRAM mode not supported. No RebootProgram configured");
		return false;
	}

	if (allowed_uid_cnt == 0)   /* Default is ALL users allowed to update */
		return true;

	for (i = 0; i < allowed_uid_cnt; i++) {
		if (allowed_uid[i] == uid)
			return true;
	}

	return false;
}

/* Return estimated reboot time, in seconds */
extern uint32_t node_features_p_boot_time(void)
{
	return boot_time;
}

/* Get node features plugin configuration */
extern void node_features_p_get_config(config_plugin_params_t *p)
{
	config_key_pair_t *key_pair;
	List data;

	xassert(p);
	xstrcat(p->name, plugin_type);
	data = p->key_pairs;

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("AllowNVRAMMode");
	key_pair->value = _nvram_mode_str(allow_nvram_mode);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("BootTime");
	key_pair->value = xstrdup_printf("%u", boot_time);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("DefaultNVRAMMode");
	key_pair->value = _nvram_mode_str(default_nvram_mode);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("AllowUserBoot");
	key_pair->value = _make_uid_str(allowed_uid, allowed_uid_cnt);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("Force");
	key_pair->value = xstrdup_printf("%u", force_load);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("NodeRebootWeight");
	key_pair->value = xstrdup_printf("%u", node_reboot_weight);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("IpmctlPath");
	key_pair->value = xstrdup(ipmctl_path);
	list_append(data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("IpmctlTimeout");
	key_pair->value = xstrdup_printf("%u", ipmctl_timeout);
	list_append(data, key_pair);

	list_sort(data, (ListCmpF) sort_key_pairs);

	return;
}

/*
 * Return node "weight" field if reboot required to change mode
 */
extern uint32_t node_features_p_reboot_weight(void)
{
	return node_reboot_weight;
}
