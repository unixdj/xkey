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
#include <X11/XF86keysym.h>

struct binding {
	char		*symbol;
	char		*cmd;
	int		keycode;
	unsigned long	req;
	volatile pid_t	pid;
};

static struct binding	*keys;
static int		nkeys;
static Display		*dpy;
static Window		root;

static void
#ifdef __GNUC__
__attribute__((noreturn))
#endif
xerror(char *s, int code)
{
	char	buf[BUFSIZ];

	XGetErrorText(dpy, code, buf, sizeof(buf));
	if (s != NULL)
		errx(1, "Cannot bind %s: %s", s, buf);
	errx(1, "X error: %s", buf);
	/* NOTREACHED */
}

static int
#ifdef __GNUC__
__attribute__((noreturn))
#endif
eh(Display *dpy, XErrorEvent *e)
{
	int	i;

	for (i = 0; i < nkeys; i++) {
		if (keys[i].req == e->serial)
			xerror(keys[i].symbol, e->error_code);
	}
	xerror(NULL, e->error_code);
	/* NOTREACHED */
}

static void
initkeys(int argc, char **argv)
{
	int	i;
	KeySym	sym;

	nkeys = argc / 2;
	if ((keys = malloc(sizeof(struct binding) * nkeys)) == NULL)
		return;
	memset(keys, 0, sizeof(struct binding) * nkeys);
	for (i = 0; i < nkeys; i++) {
		keys[i].symbol = *argv++;
		keys[i].cmd = *argv++;
		sym = XStringToKeysym(keys[i].symbol);
		if (sym == NoSymbol)
			errx(1, "%s: keysym not found", keys[i].symbol);
		keys[i].keycode = XKeysymToKeycode(dpy, sym);
		if (keys[i].keycode == 0) {
			errx(1, "%s: keycode for %#x not found",
			    keys[i].symbol, (unsigned)sym);
		}
		keys[i].req = NextRequest(dpy);
		if (XGrabKey(dpy, keys[i].keycode, AnyModifier, root, True,
		    GrabModeAsync, GrabModeAsync) == BadAccess) {
			xerror(keys[i].symbol, BadAccess);
		}
	}
}

static void
freekeys()
{
	while (nkeys) {
		if (keys[--nkeys].keycode)
			XUngrabKey(dpy, keys[nkeys].keycode, AnyModifier, root);
	}
	free(keys);
	keys = NULL;
}

static void
run(struct binding *key)
{
	sigset_t	set;
	pid_t		pid;

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &set, NULL) == -1)
		err(1, "sigprocmask failed");
	key->pid = pid = fork();
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
#if 0
	if (pid == -1)
		warn("wait4 failed");
#endif
}

static void
mainloop()
{
	XEvent	ev;
	int	i;

	for (;;) {
		XNextEvent(dpy, &ev);
		if (ev.type != KeyPress)
			continue;
		for (i = 0; i < nkeys; i++) {
			if (keys[i].keycode != ev.xkey.keycode)
				continue;
			if (keys[i].pid == 0)
				run(&keys[i]);
			else {
				warnx("handler for %s already running, pid %d",
				    keys[i].symbol, (int)keys[i].pid);
			}
			break;
		}
		if (i == nkeys)
			warnx("swallowed keycode %d", ev.xkey.keycode);
	}
}

static void
closedisplay()
{
	XCloseDisplay(dpy);
}

int
main(int argc, char **argv)
{
	if (argc < 3 || !(argc & 1)) {
		errx(1, "Usage: %s keysym command [keysym command ...]\n",
		    argv[0]);
	}
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "no display");
	atexit(closedisplay);
	root = DefaultRootWindow(dpy);
	XSetErrorHandler(eh);
	if (sigaction(SIGCHLD, &(struct sigaction){.sa_handler = sigchld},
            NULL) == -1) {
		err(1, "sigaction failed");
	}
	initkeys(argc - 1, argv + 1);
	atexit(freekeys);
	mainloop(); /* this never returns, actually */
	return 0;
}
