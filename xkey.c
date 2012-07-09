/* Copyright 2001 Vadim Vygonets.  No rights reserved.
 * Use of this source code is governed by WTFPL v2
 * that can be found in the LICENSE file.  */

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
	char		*string;
	char		*cmd;
	int		keycode;
	unsigned long	req;
	int		pid;
};

static struct binding	*keys = NULL;
static int		nkeys;
static Display		*dpy;
static Window		root;

#ifdef __GNUC__
static void	xerror(char *s, int code) __attribute__((noreturn));
#endif

static void
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
eh(Display *dpy, XErrorEvent *e)
{
	int	i;

	for (i = 0; i < nkeys; i++) {
		if (keys[i].req == e->serial)
			xerror(keys[i].string, e->error_code);
	}
	xerror(NULL, e->error_code);
	/* NOTREACHED */
	return 0;
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
		keys[i].string = *argv++;
		keys[i].cmd = *argv++;
		sym = XStringToKeysym(keys[i].string);
		if (sym == NoSymbol) {
			errx(1, "%s: keysym not found", keys[i].string);
			continue;
		}
		keys[i].keycode = XKeysymToKeycode(dpy, sym);
		if (keys[i].keycode == 0) {
			errx(1, "%s: keycode for %#x not found",
			    keys[i].string, (unsigned)sym);
			continue;
		}
		keys[i].req = NextRequest(dpy);
		if (XGrabKey(dpy, keys[i].keycode, AnyModifier, root, True,
		    GrabModeAsync, GrabModeAsync) == BadAccess) {
			xerror(keys[i].string, BadAccess);
			break;
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
	switch ((key->pid = fork())) {
	case 0:
		execl(_PATH_BSHELL, _PATH_BSHELL, "-c", key->cmd, NULL);
		err(1, "exec failed");
		/* NOTREACHED */
	case -1:
		warn("fork failed");
		/* FALLTHROUGH */
	default:
		warnx("pid %d", key->pid);
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
			warnx("waited for an unknown pid %d", pid);
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
			if (keys[i].keycode == ev.xkey.keycode) {
				if (keys[i].pid == 0)
					run(&keys[i]);
				else
					warnx("handler for %s still running, pid %d",
					    keys[i].string, keys[i].pid);
				break;
			}
		}
		if (i == nkeys)
			warnx("swallowed keycode %d", ev.xkey.keycode);
	}
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
	root = DefaultRootWindow(dpy);
	XSetErrorHandler(eh);
	sigaction(SIGCHLD, &(struct sigaction){.sa_handler = sigchld}, NULL);
	initkeys(argc - 1, argv + 1);
	mainloop();
	freekeys();
	XCloseDisplay(dpy);
	return 0;
}
