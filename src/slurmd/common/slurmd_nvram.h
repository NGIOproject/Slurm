/***************************************************************************** \
 *  slurmd_nvram.h - slurmd NVRAM management
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

#ifndef _SLURMD_NVRAM_H
#define _SLURMD_NVRAM_H

/* Clean up memory */
extern void fini_system_nvram(void);

/* Get number of NVRAMs */
extern int get_nvram_number(uint16_t *has_nvram);

/* Get NVRAM availabilty */
extern int get_nvram_availability(uint16_t *has_nvram);

/* Get NVRAM memories */
extern int get_nvram_memory(uint32_t *capacity, uint32_t *memory_capacity, uint32_t *appdirect_capacity, bool print_debug);

/* Get number of NVRAM partitions */
extern int get_nvram_partition_number(uint16_t *nvram_partition_no);

/* Get NVRAM partitions */
extern int get_namespaces(uint16_t *nvram_number_of_namespaces);

/* Get Free NVRAM Memory in GiB */
extern int get_free_mem_nvram(uint32_t *free_mem_nvram);

/* Get Free NVRAM space in GiB */
extern int get_free_space_nvram(uint32_t *free_space_nvram_0, uint32_t *free_space_nvram_1);


#endif	/* _SLURMD_NVRAM_H */
