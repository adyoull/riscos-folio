/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* tbx: the thin Toolbox layer (see tbx.h). */
#include "tbx.h"
#include <string.h>
#include <swis.h>

_kernel_oserror *tbx_init(const char *dir, const int *messages, tbx_idblock *idb,
		int *task, void **sprite_area)
{
	static int mfd[4];
	static const int all_events[1] = { 0 };   /* a lone 0: all Toolbox events */
	int wimp_version, t;
	void *area;
	_kernel_oserror *e = _swix(Toolbox_Initialise, _INR(0, 6) | _OUTR(0, 2),
			0, 310, messages, all_events, dir, mfd, idb,
			&wimp_version, &t, &area);
	if (!e) {
		if (task)
			*task = t;
		if (sprite_area)
			*sprite_area = area;
	}
	return e;
}

_kernel_oserror *tbx_create(int class_id, int version, int flags, void *body,
		int body_size, ObjectId *id)
{
	tbx_header h;
	memset(&h, 0, sizeof h);
	h.class_id = class_id;
	h.flags = flags;
	h.version = version;
	h.total_size = (int)sizeof h + body_size;
	h.body = body;
	h.body_size = body_size;
	/* R0 bit 0: R1 is a template in memory, not a name. */
	return _swix(Toolbox_CreateObject, _INR(0, 1) | _OUT(0), 1, &h, id);
}

_kernel_oserror *tbx_delete(ObjectId id, int flags)
{
	return _swix(Toolbox_DeleteObject, _INR(0, 1), flags, id);
}

_kernel_oserror *tbx_show(ObjectId id, int flags, int type, const void *data,
		ObjectId parent, ComponentId parent_component)
{
	return _swix(Toolbox_ShowObject, _INR(0, 5), flags, id, type, data, parent, parent_component);
}

_kernel_oserror *tbx_hide(ObjectId id)
{
	return _swix(Toolbox_HideObject, _INR(0, 1), 0, id);
}

_kernel_oserror *tbx_set_handle(ObjectId id, void *handle)
{
	return _swix(Toolbox_SetClientHandle, _INR(0, 2), 0, id, handle);
}

_kernel_oserror *tbx_get_handle(ObjectId id, void **handle)
{
	return _swix(Toolbox_GetClientHandle, _INR(0, 1) | _OUT(0), 0, id, handle);
}

_kernel_oserror *tbx_method(ObjectId id, int method, int r3, int r4, int r5)
{
	return _swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, id, method, r3, r4, r5);
}

_kernel_oserror *tbx_method_out(ObjectId id, int method, int r3, int *r0_out)
{
	return _swix(Toolbox_ObjectMiscOp, _INR(0, 3) | _OUT(0), 0, id, method, r3, r0_out);
}

_kernel_oserror *window_get_wimp_handle(ObjectId w, int *handle)
{
	return tbx_method_out(w, 0, 0, handle);
}

void tbx_report_error(const _kernel_oserror *e)
{
	/* Flags: OK box (bit 0); "Error from Folio" in the title. */
	_swix(Wimp_ReportError, _INR(0, 2), e, 1, "Folio");
}

void tbx_report(const char *message)
{
	_kernel_oserror e;
	e.errnum = 0;
	strncpy(e.errmess, message, sizeof e.errmess - 1);
	e.errmess[sizeof e.errmess - 1] = 0;
	tbx_report_error(&e);
}
