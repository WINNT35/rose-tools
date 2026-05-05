/*
 * Copyright (C) 2026 WINNT35
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 */
#ifndef _ROSE_CONFIG_H_
#define _ROSE_CONFIG_H_

#include "vmware/tools/rose_plugin.h"

/*
 * Default path for the rose-tools configuration file.
 * On Windows XP (the minimum target) there is no %ProgramData%, so we
 * use a fixed path under the rose-tools install directory.
 */
#define ROSE_CONF_PATH  "C:\\Program Files\\VMware\\Rose Tools\\tools.conf"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RoseConfig_Load --
 *
 * Parses an INI-format config file into dst.
 * dst is cleared before parsing begins - any existing content is lost.
 *
 * Returns non-zero on success. Returns zero if the file cannot be
 * opened; in that case dst is left cleared (all defaults apply).
 * A missing file is not an error - tools.conf is optional.
 *
 * @param[out] dst      Config struct to populate.
 * @param[in]  path     Path to the config file (e.g. "C:\\tools.conf").
 *
 * @return  Non-zero on success, zero if file could not be opened.
 */
int RoseConfig_Load(RoseConfig *dst, const char *path);


/**
 * RoseConfig_Clear --
 *
 * Resets a RoseConfig to empty. All section and key counts set to zero,
 * all strings zeroed. Safe to call on an already-empty struct.
 *
 * @param[in,out] cfg   Config struct to clear.
 */
void RoseConfig_Clear(RoseConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* _ROSE_CONFIG_H_ */
