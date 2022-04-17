/*
 * Copyright 2012, 2014, 2022 Vadim Vygonets <vadik@vygo.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <unistd.h>
#include <signal.h>
#include <paths.h>
#include <sys/wait.h>
#include <X11/Xlib.h>

#ifndef __dead
#ifdef __GNUC__
#define __dead __attribute__((noreturn))
#else
#define __dead
#endif
#endif

#ifdef __GNUC__
#define BARRIER()	asm("":::"memory")
#else
#define BARRIER()	do { } while(0)
#endif

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof(*(a)))

#define MODMASK	(ShiftMask | ControlMask | Mod1Mask | Mod4Mask | Mod5Mask)
static const char	modifiers[] = "S-CM--45";

// LockMask = Caps Lock, Mod2Mask = Num Lock, Mod3Mask = Scroll Lock
static const unsigned char	lockmasks[] = {
	0,
	LockMask,
	Mod2Mask,
	LockMask | Mod2Mask,
	Mod3Mask,
	LockMask | Mod3Mask,
	Mod2Mask | Mod3Mask,
	LockMask | Mod2Mask | Mod3Mask,
};

struct binding {
	char		*symbol;
	char		*cmd;
	unsigned int	 keycode;
	unsigned int	 modifiers;
	unsigned long	 req[ARRAY_SIZE(lockmasks)];
	volatile pid_t	 pid;
};

static struct binding	*keys;
static int		 nkeys;
static Display		*dpy;
static Window		 root;

static int __dead
eh(Display *dpy, XErrorEvent *e)
{
	char	buf[BUFSIZ];
	int	i, j;

	XGetErrorText(dpy, e->error_code, buf, sizeof(buf));
	for (i = 0; i < nkeys; i++) {
		for (j = 0; j < ARRAY_SIZE(keys[i].req); j++) {
			if (keys[i].req[j] == e->serial) {
				errx(1, "Cannot bind %s: %s", keys[i].symbol,
				    buf);
			}
		}
	}
	errx(1, "X error: %s", buf);
	/* NOTREACHED */
}

static void
grabkey(struct binding *key)
{
	int	i;

	for (i = 0; i < ARRAY_SIZE(lockmasks); i++) {
		key->req[i] = NextRequest(dpy);
		XGrabKey(dpy, key->keycode, key->modifiers | lockmasks[i],
		    root, True, GrabModeAsync, GrabModeAsync);
	}
}

static void
initkeys(int argc, char **argv)
{
	int	 i;
	KeySym	 sym;
	char	*s, *m;

	nkeys = argc / 2;
	if ((keys = malloc(sizeof(struct binding) * nkeys)) == NULL)
		err(1, "malloc");
	memset(keys, 0, sizeof(struct binding) * nkeys);
	for (i = 0; i < nkeys; i++) {
		keys[i].symbol = *argv++;
		keys[i].cmd = *argv++;
		if ((s = strchr(keys[i].symbol, '-')) != NULL) {
			for (s = keys[i].symbol; *s != '-'; s++) {
				if ((m = strchr(modifiers, *s)) == NULL) {
					errx(1, "%s: invalid modifier %c",
					    keys[i].symbol, *s);
				}
				keys[i].modifiers |= 1 << (m - modifiers);
			}
			s++;
		} else {
			s = keys[i].symbol;
		}
		sym = XStringToKeysym(s);
		if (sym == NoSymbol)
			errx(1, "%s: keysym not found", s);
		keys[i].keycode = XKeysymToKeycode(dpy, sym);
		if (keys[i].keycode == 0) {
			errx(1, "%s: keycode for %#x not found",
			    keys[i].symbol, (unsigned)sym);
		}
		grabkey(keys + i);
	}
}

static void
run(struct binding *key)
{
	sigset_t	set;
	pid_t		pid;

	if (key->pid != 0) {
		warnx("handler for %s already running, pid %d",
		    key->symbol, (int)key->pid);
		return;
	}
	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &set, NULL) == -1)
		err(1, "sigprocmask failed");
	key->pid = pid = fork();
	BARRIER();
	if (sigprocmask(SIG_UNBLOCK, &set, NULL) == -1)
		err(1, "sigprocmask failed");
	switch (pid) {
	case 0:
		execl(_PATH_BSHELL, _PATH_BSHELL, "-c", key->cmd, NULL);
		err(1, "exec failed");
		/* NOTREACHED */
	case -1:
		key->pid = 0;
		warn("fork failed");
		break;
	default:
		break;
	}
}

static void
sigchld(int sig)
{
	pid_t	pid;
	int	status, i;

	while ((pid = wait4(-1, &status, WNOHANG, NULL)) > 0) {
		if (!WIFEXITED(status) && !WIFSIGNALED(status))
			continue;
		for (i = 0; i < nkeys; i++) {
			if (keys[i].pid == pid) {
				keys[i].pid = 0;
				break;
			}
		}
		if (i == nkeys)
			warnx("waited for an unknown pid %d", (int)pid);
	}
}

static void __dead
mainloop()
{
	XEvent	ev;
	int	i;

	for (;;) {
Next:
		XNextEvent(dpy, &ev);
		if (ev.type != KeyPress)
			continue;
		for (i = 0; i < nkeys; i++) {
			if (ev.xkey.keycode == keys[i].keycode &&
			    (ev.xkey.state & MODMASK) == keys[i].modifiers) {
				run(&keys[i]);
				goto Next;
			}
		}
		warnx("swallowed keycode %d", ev.xkey.keycode);
	}
}

int
main(int argc, char **argv)
{
	if (argc < 3 || !(argc & 1)) {
		errx(2, "Usage: %s key command [key command ...]\n",
		    argv[0]);
	}
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "no display");
	root = DefaultRootWindow(dpy);
	XSetErrorHandler(eh);
	if (sigaction(SIGCHLD, &(struct sigaction){.sa_handler = sigchld},
            NULL) == -1) {
		err(1, "sigaction failed");
	}
	initkeys(argc - 1, argv + 1);
	mainloop();
	/* NOTREACHED */
}
