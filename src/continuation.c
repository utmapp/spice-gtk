/*
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.0 of the License, or (at your option) any later version.
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
#include "config.h"

/* keep this above system headers, but below config.h */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif

#include <errno.h>
#include <glib.h>

#include "continuation.h"

#ifdef HAVE_LIBUCONTEXT
#include <libucontext/libucontext.h>
#define ucontext_t libucontext_ucontext_t
#define getcontext libucontext_getcontext
#define setcontext libucontext_setcontext
#define swapcontext libucontext_swapcontext
#define makecontext libucontext_makecontext
#else
#include <ucontext.h>
#endif

/*
 * va_args to makecontext() must be type 'int', so passing
 * the pointer we need may require several int args. This
 * union is a quick hack to let us do that
 */
union cc_arg {
	int i[2];
	void *p;
};

static void continuation_trampoline(int i0, int i1)
{
	const union cc_arg arg = {{ i0, i1 }};
	struct continuation *const cc = arg.p;

	if (_setjmp(cc->jmp) != 0) {
		cc->entry(cc);
		cc->exited = 1;
		_longjmp(*((jmp_buf *) cc->last), 1);
	}

	/* Here you would be tempted to use use uc_link and avoid the usage of
	 * setcontext; don't do it, it would potentially corruct stack returning.
	 * The return would "release" part of the stack which could be
	 * overridden for instance by a signal handler.
	 * This could corrupt some variables allocated on the stack.
	 * Although this function has very few variables which potentially
	 * will be allocated on registers the union and the call to
	 * _setjmp could reduce optimizations causing variables to be
	 * allocated on the stack.
	 */
	setcontext((ucontext_t *) cc->last);
	g_error("setcontext() failed: %s", g_strerror(errno));
}

void cc_init(struct continuation *cc)
{
	volatile union cc_arg arg;
	ucontext_t uc, uc_ret;
	arg.p = cc;
	if (getcontext(&uc) == -1)
		g_error("getcontext() failed: %s", g_strerror(errno));
	cc->exited = 0;
	uc.uc_link = NULL;
	uc.uc_stack.ss_sp = cc->stack;
	uc.uc_stack.ss_size = cc->stack_size;
	uc.uc_stack.ss_flags = 0;
	cc->last = &uc_ret;

	makecontext(&uc, (void *)continuation_trampoline, 2, arg.i[0], arg.i[1]);
	swapcontext(&uc_ret, &uc);
}

int cc_release(struct continuation *cc)
{
	if (cc->release)
		return cc->release(cc);

	return 0;
}

int cc_swap(struct continuation *from, struct continuation *to)
{
	if (!to->exited) {
		to->last = &from->jmp;
		if (_setjmp(from->jmp) == 0) {
			_longjmp(to->jmp, 1);
		}

		return to->exited;
	}
	g_error("continuation routine already exited");
}
/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
