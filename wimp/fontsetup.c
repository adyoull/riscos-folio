/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Which fontconfig setup Folio uses (core/fonts.c), for this program only:
 * - with PackMan's fontconfig installed (UnixFC, which sets UnixFC$Dir and
 *   has fonts/fonts.conf), UnixFC's: the system's fonts, mapping and cache,
 *   as other fontconfig programs; Folio's own Fonts folder is added to it;
 * - without it, !Folio.Fonts.fonts/conf: the fonts in !Folio.Fonts, with
 *   the cache in <Choices$Write>.Folio.FontCache.
 * Set with setenv() (a name without '$' only changes this program's
 * environment, never the system's) before fontconfig starts, which is when
 * the first font is asked for. Paths are UnixLib paths, /<Var$Dir>/...
 * (As the Warzone 2100 port does: riscos_fontconfig.c.) */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <swis.h>

#define UNIXFC_CONF "/<UnixFC$Dir>/fonts/fonts.conf"

void fontsetup(void)
{
	const char *unixfc = getenv("UnixFC$Dir");
	const char *cw = getenv("Choices$Write");
	if (cw && *cw) {
		char dir[256];
		snprintf(dir, sizeof dir, "%s.Folio", cw);
		_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
	}
	if (unixfc && *unixfc && access(UNIXFC_CONF, R_OK) == 0) {
		setenv("FONTCONFIG_FILE", UNIXFC_CONF, 1);
		setenv("FONTCONFIG_PATH", "/<UnixFC$Dir>/fonts", 1);
		setenv("FOLIO_APPFONTS", "/<Folio$Dir>/Fonts", 1);
		app_log("fonts: PackMan's fontconfig setup (UnixFC)");
	} else {
		setenv("FONTCONFIG_FILE", "/<Folio$Dir>/Fonts/fonts.conf", 1);
		unsetenv("FONTCONFIG_PATH");
		unsetenv("FOLIO_APPFONTS");
		app_log("fonts: Folio's own fontconfig setup (!Folio.Fonts)");
	}
	unsetenv("FONTCONFIG_SYSROOT");
}
