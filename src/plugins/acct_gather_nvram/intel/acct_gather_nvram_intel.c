/*****************************************************************************\
 *  acct_gather_nvram_intel.c -slurm NVRAM accounting plugin for Intel
 *****************************************************************************
 *  Copyright (C) 2017
 *  Written by EPCC - Iakovos Panourgias <i.panourgias@epcc.ed.ac.uk>
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
 *
 *  This file is patterned after jobcomp_linux.c, written by Morris Jette and
 *  Copyright (C) 2002 The Regents of the University of California.
\*****************************************************************************/

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/common/slurm_xlator.h"
#include "src/common/slurm_acct_gather_nvram.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/slurmd/common/proctrack.h"
#include "src/common/slurm_acct_gather_profile.h"

#include "src/slurmd/slurmd/slurmd.h"


/***************************************************************/



#define _DEBUG 1
#define _DEBUG_NVRAM 1
#define NVRAM_DEFAULT_PORT 1

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for SLURM job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  SLURM will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */

const char plugin_name[] = "AcctGatherNVRAM Intel plugin";
const char plugin_type[] = "acct_gather_nvram/intel";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;

typedef struct {
	time_t last_update_time;
	time_t update_time;
	uint64_t nvram_nb_writes;
	uint64_t nvram_nb_reads;
	uint64_t all_nvram_nb_writes;
	uint64_t all_nvram_nb_reads;
	uint64_t nvram_write_bytes;
	uint64_t nvram_read_bytes;
	uint64_t all_nvram_write_bytes;
	uint64_t all_nvram_read_bytes;
} nvram_sens_t;

static nvram_sens_t nvram_se = {0,0,0,0,0,0,0,0};
static nvram_sens_t nvram_se_prev = {0,0,0,0,0,0,0,0,0,0};

static uint64_t debug_flags = 0;
static pthread_mutex_t nvram_lock = PTHREAD_MUTEX_INITIALIZER;

/* Default path to NVRAM stats */
const char proc_base_path[] = "/NEXTGenIO/NVDIMM";

/**
 *  is NVRAM supported
 **/
static int _check_nvram(void)
{
	static bool set = false;
	static int rc = SLURM_SUCCESS;

	debug4("Entering %s", __func__);

	if (!set) {
		uint32_t profile = 0;
		char nvram_directory[BUFSIZ];
		DIR *proc_dir;

		set = true;
		acct_gather_profile_g_get(ACCT_GATHER_PROFILE_RUNNING,
					  &profile);
		if ((profile & ACCT_GATHER_PROFILE_NVRAM)) {
			sprintf(nvram_directory, "%s/nvram_stats", proc_base_path);
			proc_dir = opendir(proc_base_path);
			if (!proc_dir) {
				error("%s: not able to read %s %m",
				      __func__, nvram_directory);
				rc = SLURM_FAILURE;
			} else {
				closedir(proc_dir);
			}
		} else
			rc = SLURM_ERROR;
	}

	debug4("Exiting %s", __func__);

	return rc;
}

/* _read_nvram_counters()
 * Read counters from all mounted NVRAM
 * from the file stats under the directories:
 *
 * /proc/fs/NVRAM/llite/NVRAM-xxxx
 *
 * From the file stat we use 2 entries:
 *
 * read_bytes          17996 samples [bytes] 0 4194304 30994606834
 * write_bytes         9007 samples [bytes] 2 4194304 31008331389
 *
 */
static int _read_nvram_counters(void)
{
	char nvram_dir[PATH_MAX];
	char path_stats[PATH_MAX];
	DIR *proc_dir;
	struct dirent *entry;
	FILE *fff;
	char buffer[BUFSIZ];

	debug4("Entering %s", __func__);

	sprintf(nvram_dir, "%s/nvram_stats", proc_base_path);

	proc_dir = opendir(nvram_dir);
	if (proc_dir == NULL) {
		error("%s: Cannot open %s %m", __func__, nvram_dir);
		return SLURM_FAILURE;
	}

	while ((entry = readdir(proc_dir))) {
		bool bread;
		bool bwrote;

		if (xstrcmp(entry->d_name, ".") == 0
		    || xstrcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(path_stats, PATH_MAX, "%s/%s/stats", nvram_dir,
			 entry->d_name);
		debug3("%s: Found file %s", __func__, path_stats);

		fff = fopen(path_stats, "r");
		if (fff == NULL) {
			error("%s: Cannot open %s %m", __func__, path_stats);
			continue;
		}

		bread = bwrote = false;
		while (fgets(buffer, BUFSIZ, fff)) {

			if (bread && bwrote)
				break;

			if (strstr(buffer, "write_bytes")) {
				sscanf(buffer,
				       "%*s %"PRIu64" %*s %*s "
				       "%*d %*d %"PRIu64"",
				       &nvram_se.nvram_nb_writes,
				       &nvram_se.nvram_write_bytes);
				debug3("%s "
				       "%"PRIu64" "
				       "write_bytes %"PRIu64" "
				       "writes",
				       __func__,
				       nvram_se.nvram_write_bytes,
				       nvram_se.nvram_nb_writes);
				bwrote = true;
			}

			if (strstr(buffer, "read_bytes")) {
				sscanf(buffer,
				       "%*s %"PRIu64" %*s %*s "
				       "%*d %*d %"PRIu64"",
				       &nvram_se.nvram_nb_reads,
				       &nvram_se.nvram_read_bytes);
				debug3("%s "
				       "%"PRIu64" "
				       "read_bytes %"PRIu64" "
				       "reads",
				       __func__,
				       nvram_se.nvram_read_bytes,
				       nvram_se.nvram_nb_reads);
				bread = true;
			}
		}
		fclose(fff);

		nvram_se.all_nvram_write_bytes +=
			nvram_se.nvram_write_bytes;
		nvram_se.all_nvram_read_bytes += nvram_se.nvram_read_bytes;
		nvram_se.all_nvram_nb_writes += nvram_se.nvram_nb_writes;
		nvram_se.all_nvram_nb_reads += nvram_se.nvram_nb_reads;
		debug3("%s: all_nvram_write_bytes %"PRIu64" "
		       "all_nvram_read_bytes %"PRIu64"",
		       __func__, nvram_se.all_nvram_write_bytes,
		       nvram_se.all_nvram_read_bytes);
		debug3("%s: all_nvram_nb_writes %"PRIu64" "
		       "all_nvram_nb_reads %"PRIu64"",
		       __func__, nvram_se.all_nvram_nb_writes,
		       nvram_se.all_nvram_nb_reads);

	} /* while ((entry = readdir(proc_dir)))  */
	closedir(proc_dir);

	nvram_se.last_update_time = nvram_se.update_time;
	nvram_se.update_time = time(NULL);

	debug4("Exiting %s", __func__);

	return SLURM_SUCCESS;
}


/*
 * _update_node_nvram calls _read_ipmi_values and updates all values
 * for node consumption
 */
static int _update_node_nvram(void)
{
	static int dataset_id = -1;
	static bool first = true;

	debug4("Entering %s", __func__);

	enum {
		FIELD_READ,
		FIELD_READMB,
		FIELD_WRITE,
		FIELD_WRITEMB,
		FIELD_CNT
	};

	acct_gather_profile_dataset_t dataset[] = {
		{ "Reads", PROFILE_FIELD_UINT64 },
		{ "ReadMB", PROFILE_FIELD_DOUBLE },
		{ "Writes", PROFILE_FIELD_UINT64 },
		{ "WriteMB", PROFILE_FIELD_DOUBLE },
		{ NULL, PROFILE_FIELD_NOT_SET }
	};

	union {
		double d;
		uint64_t u64;
	} data[FIELD_CNT];

	slurm_mutex_lock(&nvram_lock);

	if (_read_nvram_counters() != SLURM_SUCCESS) {
		error("%s: Cannot read NVRAM counters", __func__);
		slurm_mutex_unlock(&nvram_lock);
		return SLURM_FAILURE;
	}

	if (first) {
		dataset_id = acct_gather_profile_g_create_dataset(
			"NVRAM", NO_PARENT, dataset);
		if (dataset_id == SLURM_ERROR) {
			error("NVRAM: Failed to create the dataset "
			      "for Intel");
			slurm_mutex_unlock(&nvram_lock);
			return SLURM_ERROR;
		}

		first = false;
	}

	if (dataset_id < 0) {
		slurm_mutex_unlock(&nvram_lock);
		return SLURM_ERROR;
	}

	/* Compute the current values read from all lustre-xxxx directories */
	data[FIELD_READ].u64 = nvram_se.all_nvram_nb_reads -
		nvram_se_prev.all_nvram_nb_reads;
	data[FIELD_READMB].d =
		(double)(nvram_se.all_nvram_read_bytes -
			 nvram_se_prev.all_nvram_read_bytes) / (1 << 20);
	data[FIELD_WRITE].u64 = nvram_se.all_nvram_nb_writes -
		nvram_se_prev.all_nvram_nb_writes;
	data[FIELD_WRITEMB].d =
		(double)(nvram_se.all_nvram_write_bytes -
			 nvram_se_prev.all_nvram_write_bytes)	/ (1 << 20);

	/* record sample */
	if (debug_flags & DEBUG_FLAG_PROFILE) {
		char str[256];
		info("PROFILE-NVRAM: %s", acct_gather_profile_dataset_str(
			     dataset, data, str, sizeof(str)));
	}
	acct_gather_profile_g_add_sample_data(dataset_id, (void *)data,
					      nvram_se.update_time);

	/* Save current as previous */
	memcpy(&nvram_se_prev, &nvram_se, sizeof(nvram_sens_t));

	slurm_mutex_unlock(&nvram_lock);

	debug4("Exiting %s", __func__);

	return SLURM_SUCCESS;
}

static bool _run_in_daemon(void)
{
	static bool set = false;
	static bool run = false;

	debug4("Entering %s", __func__);

	if (!set) {
		set = 1;
		run = run_in_daemon("slurmstepd");
	}

	debug4("Exiting %s", __func__);

	return run;
}


/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	debug4("Entering %s", __func__);

	debug_flags = slurm_get_debug_flags();

	debug4("Exiting %s", __func__);

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	debug4("Entering %s", __func__);

	if (!_run_in_daemon())
		return SLURM_SUCCESS;

	if (debug_flags & DEBUG_FLAG_NVRAM)
		info("NVRAM: Intel: ended");

	return SLURM_SUCCESS;
}

extern int acct_gather_nvram_p_node_update(void)
{
	debug4("Entering %s", __func__);

	if (_run_in_daemon() && (_check_nvram() == SLURM_SUCCESS))
		_update_node_nvram();

	debug4("Exiting %s", __func__);

	return SLURM_SUCCESS;
}


extern void acct_gather_nvram_p_conf_set(s_p_hashtbl_t *tbl)
{
	debug4("Entering %s", __func__);

	if (!_run_in_daemon())
		return;

	debug("%s loaded", plugin_name);
}

extern void acct_gather_nvram_p_conf_options(s_p_options_t **full_options,
						  int *full_options_cnt)
{
	debug4("Entering %s", __func__);

	debug4("Exiting %s", __func__);

	return;
}

extern void acct_gather_nvram_p_conf_values(List *data)
{
	debug4("Entering %s", __func__);

	debug4("Exiting %s", __func__);

	return;
}
