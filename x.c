/* See LICENSE for license details. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <locale.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xrender.h>
#include <png.h>

char *argv0;
#include "arg.h"
#include "st.h"
#include "win.h"
#include "lua_config.h"

/* Maximum depth for pane tree traversal stack */
#define MAX_PANE_DEPTH 64

/* types used in config.h */
typedef struct {
	uint mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

typedef struct {
	uint mod;
	uint button;
	void (*func)(const Arg *);
	const Arg arg;
	uint  release;
} MouseShortcut;

typedef struct {
	KeySym k;
	uint mask;
	char *s;
	/* three-valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;    /* application keypad */
	signed char appcursor; /* application cursor */
} Key;

/* X modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13|1<<14)

/* function definitions used in config.h */
static void clipcopy(const Arg *);
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
void zoom(const Arg *);
static void zoomabs(const Arg *);
void zoomreset(const Arg *);
static void ttysend(const Arg *);
static void tabnew(const Arg *);
static void tabnext(const Arg *);
static void tabprev(const Arg *);
static void tabclose(const Arg *);
static void winrename(const Arg *);
static void tabrename(const Arg *);
static void splitv(const Arg *);
static void splith(const Arg *);
static void focuspane(const Arg *);
static void configreload(const Arg *);
static void closepane(const Arg *);
static void killpane(const Arg *);
static void newwindow(const Arg *);
static void openurl(const char *);
static void togglelog(const Arg *);
static void xseticon(void);
void xnotify(const char *);

/* config.h for applying patches and the configuration. */
#include "config.h"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* macros */
#define IS_SET(flag)		((win.mode & (flag)) != 0)
/* Check if current terminal has mouse mode (per-terminal, not global) */
#define TERM_HAS_MOUSE()	(g_term && (g_term->term.mode & MODE_MOUSE))
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

typedef XftDraw *Draw;
typedef XftColor Color;
typedef XftGlyphFontSpec GlyphFontSpec;

/* Purely graphic info */
typedef struct {
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	int mode; /* window state/mode flags */
	int cursor; /* cursor style */
} TermWindow;

typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	GlyphFontSpec *specbuf; /* font spec buffer used for rendering */
	Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid, netwmicon;
	struct {
		XIM xim;
		XIC xic;
		XPoint spot;
		XVaNestedList spotlist;
	} ime;
	Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed; /* is fixed geometry? */
	int l, t; /* left and top offset */
	int gm; /* geometry mask */
} XWindow;

typedef struct {
	Atom xtarget;
	char *primary, *clipboard;
	struct timespec tclick1;
	struct timespec tclick2;
} XSelection;

/* Font structure */
#define Font Font_
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	int badslant;
	int badweight;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcPattern *pattern;
} Font;

/* Drawing Context */
typedef struct {
	Color *col;
	size_t collen;
	Font font, bfont, ifont, ibfont;
	GC gc;
} DC;

static inline ushort sixd_to_16bit(int);
static int xmakeglyphfontspecs(XftGlyphFontSpec *, const Glyph *, int, int, int);
static void xdrawglyphfontspecs(const XftGlyphFontSpec *, Glyph, int, int, int);
static void xdrawglyph(Glyph, int, int);
static void xclear(int, int, int, int);
static int xgeommasktogravity(int);
static int ximopen(Display *);
static void ximinstantiate(Display *, XPointer, XPointer);
static void ximdestroy(XIM, XPointer, XPointer);
static int xicdestroy(XIC, XPointer, XPointer);
static void xinit(int, int);
void cresize(int, int);
static void xresize(int, int);
static void xhints(void);
static int xloadcolor(int, const char *, Color *);
static int xloadfont(Font *, FcPattern *);
void xloadfonts(const char *, double);
static void xunloadfont(Font *);
void xunloadfonts(void);
static void xsetenv(void);
static void xseturgency(int);
static int evcol(XEvent *);
static int evrow(XEvent *);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static uint buttonmask(uint);
static int mouseaction(XEvent *, uint);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear_(XEvent *);
static void selrequest(XEvent *);
static void setsel(char *, Time);
static void mousesel(XEvent *, int);
static void mousereport(XEvent *);
static char *kmap(KeySym, uint);
static int match(uint, uint);

static void run(void);
static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
/*
 * Uncomment if you want the selection to disappear when you select something
 * different in another window.
 */
/*	[SelectionClear] = selclear_, */
	[SelectionNotify] = selnotify,
/*
 * PropertyNotify is only turned on when there is some INCR transfer happening
 * for the selection retrieval.
 */
	[PropertyNotify] = propnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static DC dc;
static XWindow xw;
static XSelection xsel;
static TermWindow win;

/* Window icon settings */
uint32_t g_icon_emoji = 0x1F5A5;  /* fallback emoji (desktop computer 🖥️) */
static int g_icon_use_emoji = 0;  /* 0 = use PNG (default), 1 = use emoji from Lua */

/* Pane drawing offsets (set before drawing each pane) */
static int pane_ox = 0;  /* pixel offset x */
static int pane_oy = 0;  /* pixel offset y */

/* Notification overlay */
static struct {
	char text[128];
	struct timespec time;
	int active;
} notification = {0};

/* Cursor blinking */
static int cursorblink_on = 1;           /* cursor visible state */
static struct timespec cursorblink_last; /* last toggle time */
unsigned int cursorblinktimeout = 800;   /* ms, 0 = no blink, configurable via Lua */

/* Active pane border */
unsigned int activepaneborder = 1;       /* pixels, 0 = no border, configurable via Lua */
unsigned int activepanebordercolor = 14; /* color index, default bright cyan, configurable via Lua */

/* Background image */
static struct {
	unsigned char *data;   /* raw RGBA pixels (original size) */
	int orig_w, orig_h;    /* original image dimensions */
	Pixmap pix;            /* scaled pixmap for current window size */
	int pix_w, pix_h;      /* current scaled dimensions */
	float opacity;         /* 0.0 = no transparency, 1.0 = fully transparent bg */
	int loaded;
	char path[512];
} bgimg;

static void bgimg_scale(void);
static void bgimg_paint(int, int, int, int);

/* URL open suppresses focus events briefly to avoid sending escape sequences to PTY */
static struct timespec url_open_time = {0};
static int url_open_suppress_focus = 0;

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	int flags;
	Rune unicodep;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache *frc = NULL;
static int frclen = 0;
static int frccap = 0;
char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

static char *opt_class = NULL;
static char **opt_cmd  = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *opt_title = NULL;

static uint buttons; /* bit field of pressed buttons */

/*
 * Scroll-select state: while Button1 is held outside the active pane the main
 * loop keeps scrolling on a timer, so scrolling continues even when the
 * pointer sits still and generates no motion events.
 */
static struct {
	int dir;              /* -1 scroll up, +1 scroll down, 0 inactive */
	int x, y;             /* last pointer position, window pixel coords */
	uint state;           /* modifier state of the ongoing drag */
	struct timespec last; /* when the last step ran */
} autoscroll;

void
clipcopy(const Arg *dummy)
{
	Atom clipboard;

	free(xsel.clipboard);
	xsel.clipboard = NULL;

	if (xsel.primary != NULL) {
		xsel.clipboard = xstrdup(xsel.primary);
		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
	}
}

void
clippaste(const Arg *dummy)
{
	Atom clipboard;

	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, xsel.xtarget, clipboard,
			xw.win, CurrentTime);
}

void
selpaste(const Arg *dummy)
{
	XConvertSelection(xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
numlock(const Arg *dummy)
{
	win.mode ^= MODE_NUMLOCK;
}

void
zoom(const Arg *arg)
{
	Arg larg;

	larg.f = usedfontsize + arg->f;
	zoomabs(&larg);
}

void
zoomabs(const Arg *arg)
{
	xunloadfonts();
	xloadfonts(usedfont, arg->f);
	cresize(0, 0);
	redraw();
	xhints();
}

void
zoomreset(const Arg *arg)
{
	Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		zoomabs(&larg);
	}
}

void
configreload(const Arg *arg)
{
	(void)arg;
	lua_config_reload();
}

void
ttysend(const Arg *arg)
{
	ttywrite(arg->s, strlen(arg->s), 1);
}

void
tabnew(const Arg *arg)
{
	int col, row;
	Tab *newtab;
	char *cwd = NULL;

	/* Get CWD from current terminal before switching */
	if (g_term)
		cwd = terminal_getcwd(g_term);

	col = (win.w - 2 * borderpx) / win.cw;
	row = (win.h - 2 * borderpx) / win.ch;
	col = MAX(1, col);
	row = MAX(1, row);

	newtab = tab_new();
	if (!newtab)
		return;

	newtab->root_pane = pane_new(col, row);
	if (!newtab->root_pane) {
		/* Remove the tab we just added */
		g_tabs.count--;
		free(newtab);
		return;
	}
	newtab->active_pane = newtab->root_pane;

	tab_switch(g_tabs.count - 1);

	/* Fork new shell in the same directory as the old terminal */
	selinit();
	ttynew(NULL, shell, NULL, NULL, cwd);

	redraw();
}

void
tabnext(const Arg *arg)
{
	tab_next();
	redraw();
}

void
tabprev(const Arg *arg)
{
	tab_prev();
	redraw();
}

void
tabclose(const Arg *arg)
{
	if (g_tabs.count <= 1) {
		/* Last tab - exit */
		exit(0);
	}
	tab_close(g_tabs.active);
	redraw();
}

void
winrename(const Arg *arg)
{
	g_tabs.renaming = RENAME_WINDOW;
	g_tabs.rename_len = 0;
	g_tabs.rename_buf[0] = '\0';

	if (g_tabs.window_title_manual && g_tabs.window_custom_title[0]) {
		strncpy(g_tabs.rename_buf, g_tabs.window_custom_title,
		        sizeof(g_tabs.rename_buf) - 1);
		g_tabs.rename_buf[sizeof(g_tabs.rename_buf) - 1] = '\0';
		g_tabs.rename_len = strlen(g_tabs.rename_buf);
	}

	redraw();
}

void
tabrename(const Arg *arg)
{
	Tab *tab = g_tabs.tabs[g_tabs.active];

	if (!tab)
		return;

	g_tabs.renaming = RENAME_TAB;
	g_tabs.rename_len = 0;
	g_tabs.rename_buf[0] = '\0';

	if (tab->title_manual && tab->custom_title[0]) {
		strncpy(g_tabs.rename_buf, tab->custom_title,
		        sizeof(g_tabs.rename_buf) - 1);
		g_tabs.rename_buf[sizeof(g_tabs.rename_buf) - 1] = '\0';
		g_tabs.rename_len = strlen(g_tabs.rename_buf);
	}

	redraw();
}

void
splitv(const Arg *arg)
{
	Pane *new_pane;
	Tab *tab = g_tabs.tabs[g_tabs.active];
	char *cwd = NULL;
	int tw, th;

	if (!tab || !tab->active_pane)
		return;

	/* Get CWD from current terminal before splitting */
	if (g_term)
		cwd = terminal_getcwd(g_term);

	new_pane = pane_split(tab->active_pane, PANE_VSPLIT,
	                            tab->active_pane->col, tab->active_pane->row);
	if (!new_pane)
		return;

	tw = win.w - 2 * borderpx;
	th = win.h - 2 * borderpx;
	if (g_tabs.count > 1)
		th -= win.ch;  /* Account for tab bar */
	pane_layout(tab->root_pane, borderpx, borderpx + (g_tabs.count > 1 ? win.ch : 0),
	            tw, th, win.cw, win.ch);

	/* Focus new pane and fork shell in same directory */
	tab->active_pane = new_pane;
	g_term = new_pane->terminal;
	xsyncmodeflags();
	selinit();
	ttynew(NULL, shell, NULL, NULL, cwd);

	redraw();
}

void
splith(const Arg *arg)
{
	Pane *new_pane;
	Tab *tab = g_tabs.tabs[g_tabs.active];
	char *cwd = NULL;
	int tw, th;

	if (!tab || !tab->active_pane)
		return;

	/* Get CWD from current terminal before splitting */
	if (g_term)
		cwd = terminal_getcwd(g_term);

	new_pane = pane_split(tab->active_pane, PANE_HSPLIT,
	                            tab->active_pane->col, tab->active_pane->row);
	if (!new_pane)
		return;

	tw = win.w - 2 * borderpx;
	th = win.h - 2 * borderpx;
	if (g_tabs.count > 1)
		th -= win.ch;  /* Account for tab bar */
	pane_layout(tab->root_pane, borderpx, borderpx + (g_tabs.count > 1 ? win.ch : 0),
	            tw, th, win.cw, win.ch);

	/* Focus new pane and fork shell in same directory */
	tab->active_pane = new_pane;
	g_term = new_pane->terminal;
	xsyncmodeflags();
	selinit();
	ttynew(NULL, shell, NULL, NULL, cwd);

	redraw();
}

void
focuspane(const Arg *arg)
{
	Tab *tab = g_tabs.tabs[g_tabs.active];
	Pane *neighbor;

	if (!tab || !tab->root_pane || !tab->active_pane)
		return;

	neighbor = pane_find_neighbor(tab->root_pane, tab->active_pane, arg->i);
	if (neighbor) {
		pane_focus(neighbor);
		redraw();
	}
}

void
closepane(const Arg *arg)
{
	Tab *tab = g_tabs.tabs[g_tabs.active];
	Pane *to_close, *parent, *sibling;
	int tw, th;
	Pane *leaf;

	if (!tab || !tab->root_pane || !tab->active_pane)
		return;

	if (tab->root_pane->type == PANE_LEAF) {
		tabclose(arg);
		return;
	}

	/* Find parent of active pane and sibling */
	to_close = tab->active_pane;
	parent = NULL;
	sibling = NULL;

	/* For now, just close the tab if there's only the root */
	/* TODO: proper pane removal and tree restructuring */

	/* Simple case: if active_pane is child1 or child2 of root */
	if (tab->root_pane->child1 == to_close) {
		parent = tab->root_pane;
		sibling = parent->child2;
	} else if (tab->root_pane->child2 == to_close) {
		parent = tab->root_pane;
		sibling = parent->child1;
	}

	if (parent && sibling) {
		pane_free(to_close);

		/* Replace parent with sibling */
		if (sibling->type == PANE_LEAF) {
			parent->type = PANE_LEAF;
			parent->terminal = sibling->terminal;
			parent->child1 = NULL;
			parent->child2 = NULL;
			sibling->terminal = NULL;  /* Don't free it */
			free(sibling);
		} else {
			parent->type = sibling->type;
			parent->child1 = sibling->child1;
			parent->child2 = sibling->child2;
			parent->split_ratio = sibling->split_ratio;
			parent->terminal = NULL;
			free(sibling);
		}

		tw = win.w - 2 * borderpx;
		th = win.h - 2 * borderpx;
		if (g_tabs.count > 1)
			th -= win.ch;
		pane_layout(tab->root_pane, borderpx, borderpx + (g_tabs.count > 1 ? win.ch : 0),
		            tw, th, win.cw, win.ch);

		if (parent->type == PANE_LEAF) {
			tab->active_pane = parent;
			g_term = parent->terminal;
		} else {
			leaf = parent;
			while (leaf->type != PANE_LEAF && leaf->child1)
				leaf = leaf->child1;
			tab->active_pane = leaf;
			g_term = leaf->terminal;
		}
		xsyncmodeflags();

		redraw();
	}
}

void
killpane(const Arg *arg)
{
	Tab *tab = g_tabs.tabs[g_tabs.active];
	Terminal *t;

	if (!tab || !tab->active_pane || !tab->active_pane->terminal)
		return;

	t = tab->active_pane->terminal;
	if (t->pid > 0) {
		fprintf(stderr, "killpane: sending SIGKILL to pid %d\n", t->pid);
		kill(t->pid, SIGKILL);
	}

	closepane(arg);
}

void
newwindow(const Arg *arg)
{
	pid_t pid;

	(void)arg;
	fprintf(stderr, "newwindow: spawning new st instance\n");

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "newwindow: fork failed: %s\n", strerror(errno));
		return;  /* Don't die(), just report error */
	}

	if (pid == 0) {
		/* Child process - create new session to fully detach */
		setsid();

		execlp("ft", "ft", NULL);

		fprintf(stderr, "newwindow: exec failed: %s\n", strerror(errno));
		_exit(127);
	}

	/* Parent continues - child runs independently */
}

/*
 * Check if string is a URL (not a file path)
 */
static int
is_url(const char *s)
{
	return (strncmp(s, "http://", 7) == 0 ||
	        strncmp(s, "https://", 8) == 0 ||
	        strncmp(s, "ftp://", 6) == 0 ||
	        strncmp(s, "file://", 7) == 0);
}

/*
 * Open a URL or file path.
 * - URLs always use xdg-open
 * - File paths use configured handlers (Lua configurable)
 */
void
openurl(const char *url)
{
	char expanded[4096];
	const char *to_open = url;
	FileHandler handler;
	const char *home;
	pid_t pid;
	int devnull;
	char cmd[4096];
	const char *p;
	char *d, *end;
	size_t len;

	if (!url || !url[0])
		return;

	if (url[0] == '~' && url[1] == '/') {
		home = getenv("HOME");
		if (home) {
			snprintf(expanded, sizeof(expanded), "%s%s", home, url + 1);
			to_open = expanded;
		}
	}

	/* URLs always use xdg-open */
	if (is_url(to_open)) {
		/* Suppress focus events to avoid sending escape sequences to PTY */
		url_open_suppress_focus = 1;
		clock_gettime(CLOCK_MONOTONIC, &url_open_time);

		pid = fork();
		if (pid == -1) {
			fprintf(stderr, "openurl: fork failed: %s\n", strerror(errno));
			url_open_suppress_focus = 0;
			return;
		}
		if (pid == 0) {
			setsid();
			/* Close ALL inherited file descriptors to prevent any
			 * interference with the parent terminal or PTY */
			for (int fd = 0; fd < 1024; fd++)
				close(fd);
			devnull = open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > 2)
					close(devnull);
			}
			execlp("xdg-open", "xdg-open", to_open, NULL);
			execlp("sensible-browser", "sensible-browser", to_open, NULL);
			_exit(127);
		}
		return;
	}

	handler = lua_config_get_file_handler(to_open);

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "openurl: fork failed: %s\n", strerror(errno));
		return;
	}

	if (pid == 0) {
		/* Child process */
		setsid();

		switch (handler.type) {
		case FILE_HANDLER_XDG:
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			execlp("xdg-open", "xdg-open", to_open, NULL);
			_exit(127);
			break;

		case FILE_HANDLER_APP:
			/* Run external app (detached) */
			close(STDIN_FILENO);
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			if (strstr(handler.command, "%s")) {
				/* Build command with file path substituted */
				p = handler.command;
				d = cmd;
				end = cmd + sizeof(cmd) - 1;
				while (*p && d < end) {
					if (p[0] == '%' && p[1] == 's') {
						len = strlen(to_open);
						if (d + len < end) {
							memcpy(d, to_open, len);
							d += len;
						}
						p += 2;
					} else {
						*d++ = *p++;
					}
				}
				*d = '\0';
				execlp("/bin/sh", "sh", "-c", cmd, NULL);
			} else {
				/* No %s, just pass file as argument */
				execlp("/bin/sh", "sh", "-c", handler.command, "sh", to_open, NULL);
			}
			_exit(127);
			break;

		case FILE_HANDLER_TERMINAL:
			/* Open in new terminal window with editor */
			{
				if (strstr(handler.command, "%s")) {
					p = handler.command;
					d = cmd;
					end = cmd + sizeof(cmd) - 1;
					while (*p && d < end) {
						if (p[0] == '%' && p[1] == 's') {
							len = strlen(to_open);
							if (d + len < end) {
								memcpy(d, to_open, len);
								d += len;
							}
							p += 2;
						} else {
							*d++ = *p++;
						}
					}
					*d = '\0';
				} else {
					/* No %s, append file path */
					snprintf(cmd, sizeof(cmd), "%s %s",
					         handler.command, to_open);
				}
				execlp("ft", "ft", "-e", "/bin/sh", "-c", cmd, NULL);
				/* Fallback to st if ft not found */
				execlp("st", "st", "-e", "/bin/sh", "-c", cmd, NULL);
				_exit(127);
			}
			break;
		}

		_exit(127);
	}

	/* Parent continues */
}

void
togglelog(const Arg *arg)
{
	int state;

	(void)arg;
	if (!g_term)
		return;

	state = terminal_toggle_log(g_term);
	xnotify(state ? "Logging ON" : "Logging OFF");
}

/* Map a window pixel x to a terminal column in the active pane. */
static int
x2col(int px)
{
	int x = px - borderpx;
	Tab *tab;
	Pane *ap;
	int col;

	if (g_tabs.count > 0 && g_tabs.active >= 0) {
		tab = g_tabs.tabs[g_tabs.active];
		if (tab && tab->active_pane) {
			ap = tab->active_pane;
			x = px - ap->x;
			LIMIT(x, 0, ap->w - 1);
			col = x / win.cw;
			if (col >= ap->col)
				col = ap->col - 1;
			return col;
		}
	}

	LIMIT(x, 0, win.tw - 1);
	return x / win.cw;
}

/* Map a window pixel y to a terminal row in the active pane. */
static int
y2row(int py)
{
	int y = py - borderpx;
	Tab *tab;
	Pane *ap;
	int row;

	if (g_tabs.count > 0 && g_tabs.active >= 0) {
		tab = g_tabs.tabs[g_tabs.active];
		if (tab && tab->active_pane) {
			ap = tab->active_pane;
			y = py - ap->y;
			LIMIT(y, 0, ap->h - 1);
			row = y / win.ch;
			if (row >= ap->row)
				row = ap->row - 1;
			return row;
		}
	}

	LIMIT(y, 0, win.th - 1);
	return y / win.ch;
}

int
evcol(XEvent *e)
{
	return x2col(e->xbutton.x);
}

int
evrow(XEvent *e)
{
	return y2row(e->xbutton.y);
}

/* Selection type (regular/rectangular) implied by the held modifiers. */
static int
selmasktype(uint state)
{
	int type;

	state &= ~(Button1Mask | forcemousemod);
	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state))
			return type;
	}

	return SEL_REGULAR;
}

/* Top and bottom edge of the active pane, in window pixel coords. */
static void
panevbounds(int *top, int *bot)
{
	Tab *tab;

	if (g_tabs.count > 0 && g_tabs.active >= 0) {
		tab = g_tabs.tabs[g_tabs.active];
		if (tab && tab->active_pane) {
			*top = tab->active_pane->y;
			*bot = tab->active_pane->y + tab->active_pane->h;
			return;
		}
	}

	*top = borderpx;
	*bot = borderpx + win.th;
}

static void
selautoscrollstop(void)
{
	autoscroll.dir = 0;
}

/*
 * Record where a Button1 drag currently is. If it is past the top or bottom
 * edge of the active pane, run() starts stepping the scrollback.
 */
static void
selautoscrolltrack(int px, int py, uint state)
{
	int top, bot;

	panevbounds(&top, &bot);

	autoscroll.x = px;
	autoscroll.y = py;
	autoscroll.state = state;

	if (py < top)
		autoscroll.dir = -1;
	else if (py >= bot)
		autoscroll.dir = 1;
	else
		autoscroll.dir = 0;
}

/*
 * One scroll-select step: scroll the active pane and drag the loose end of the
 * selection along with it. Returns 1 if the viewport moved, so the caller can
 * force a redraw.
 */
static int
selautoscrollstep(void)
{
	int moved;

	if (!autoscroll.dir)
		return 0;

	/*
	 * Bail out if the drag is over or the selection went away - we may
	 * never see the release, e.g. when the pointer is grabbed away.
	 */
	if (!(buttons & (1 << (Button1 - 1))) || !g_term ||
	    g_term->sel.mode == SEL_IDLE) {
		selautoscrollstop();
		return 0;
	}

	moved = autoscroll.dir < 0 ? selscrollup(autoscrolllines)
	                           : selscrolldown(autoscrolllines);

	/* Keep the loose end of the selection pinned to the pane edge. */
	selextend(x2col(autoscroll.x), y2row(autoscroll.y),
	          selmasktype(autoscroll.state), 0);

	return moved > 0;
}

void
mousesel(XEvent *e, int done)
{
	selextend(evcol(e), evrow(e), selmasktype(e->xbutton.state), done);
	if (done)
		setsel(getsel(), e->xbutton.time);
}

void
mousereport(XEvent *e)
{
	int len, btn, code;
	int x = evcol(e), y = evrow(e);
	int state = e->xbutton.state;
	char buf[40];
	static int ox, oy;

	if (e->type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;
		/* MODE_MOUSEMOTION: no reporting if no button is pressed */
		if (IS_SET(MODE_MOUSEMOTION) && buttons == 0)
			return;
		/* Set btn to lowest-numbered pressed button, or 12 if no
		 * buttons are pressed. */
		for (btn = 1; btn <= 11 && !(buttons & (1<<(btn-1))); btn++)
			;
		code = 32;
	} else {
		btn = e->xbutton.button;
		/* Only buttons 1 through 11 can be encoded */
		if (btn < 1 || btn > 11)
			return;
		if (e->type == ButtonRelease) {
			/* MODE_MOUSEX10: no button release reporting */
			if (IS_SET(MODE_MOUSEX10))
				return;
			/* Don't send release events for the scroll wheel */
			if (btn == 4 || btn == 5)
				return;
		}
		code = 0;
	}

	ox = x;
	oy = y;

	/* Encode btn into code. If no button is pressed for a motion event in
	 * MODE_MOUSEMANY, then encode it as a release. */
	if ((!IS_SET(MODE_MOUSESGR) && e->type == ButtonRelease) || btn == 12)
		code += 3;
	else if (btn >= 8)
		code += 128 + btn - 8;
	else if (btn >= 4)
		code += 64 + btn - 4;
	else
		code += btn - 1;

	if (!IS_SET(MODE_MOUSEX10)) {
		code += ((state & ShiftMask  ) ?  4 : 0)
		      + ((state & Mod1Mask   ) ?  8 : 0) /* meta key: alt */
		      + ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				code, x+1, y+1,
				e->type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+code, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len, 0);
}

uint
buttonmask(uint button)
{
	return button == Button1 ? Button1Mask
	     : button == Button2 ? Button2Mask
	     : button == Button3 ? Button3Mask
	     : button == Button4 ? Button4Mask
	     : button == Button5 ? Button5Mask
	     : 0;
}

int
mouseaction(XEvent *e, uint release)
{
	MouseShortcut *ms;
	uint state;

	/* ignore Button<N>mask for Button<N> - it's set on release */
	state = e->xbutton.state & ~buttonmask(e->xbutton.button);

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (ms->release == release &&
		    ms->button == e->xbutton.button &&
		    (match(ms->mod, state) ||  /* exact or forced */
		     match(ms->mod, state & ~forcemousemod))) {
			ms->func(&(ms->arg));
			return 1;
		}
	}

	return 0;
}

void
bpress(XEvent *e)
{
	int btn = e->xbutton.button;
	struct timespec now;
	int snap;
	int x = e->xbutton.x;
	int y = e->xbutton.y;
	int tabw, clicked_tab;
	Tab *tab;
	Pane *clicked_pane;
	Terminal *prev, *cur;
	int col, row;
	char urlbuf[4096];

	if (1 <= btn && btn <= 11)
		buttons |= 1 << (btn-1);

	selautoscrollstop();

	/* Handle mouse wheel for scrollback (Shift+wheel or when not in mouse mode) */
	if (btn == Button4 || btn == Button5) {
		if (!tisaltscr() && (e->xbutton.state & ShiftMask || !TERM_HAS_MOUSE())) {
			Arg a = { .i = 5 };  /* scroll 5 lines at a time */
			if (btn == Button4)
				kscrollup(&a);
			else
				kscrolldown(&a);
			return;
		}
	}

	/*
	 * Check for tab/pane switching BEFORE MODE_MOUSE check.
	 * This ensures clicks on different panes work even when
	 * the active pane has mouse mode enabled (e.g., nvim).
	 */
	/* Tab bar click - only for Button1 */
	if (btn == Button1 && g_tabs.count > 1 && y < win.ch) {
		tabw = win.w / g_tabs.count;
		if (tabw < 50) tabw = 50;
		if (tabw > 200) tabw = 200;
		clicked_tab = x / tabw;
		if (clicked_tab >= 0 && clicked_tab < g_tabs.count) {
			tab_switch(clicked_tab);
			redraw();
		}
		return;
	}

	/*
	 * Pane focus - for ALL buttons (not just Button1).
	 * This ensures middle-click paste goes to the correct pane.
	 */
	if (g_tabs.count > 0 && g_tabs.active >= 0) {
		tab = g_tabs.tabs[g_tabs.active];
		if (tab && tab->root_pane && tab->root_pane->type != PANE_LEAF) {
			clicked_pane = pane_at_coords(tab->root_pane, x, y);
			if (clicked_pane && clicked_pane != tab->active_pane) {
				prev = g_term;

				pane_focus(clicked_pane);

				/*
				 * Only one selection at a time: drop the old
				 * pane's highlight. xsel.primary keeps the text,
				 * so pasting it still works.
				 */
				if (prev && prev != g_term) {
					cur = g_term;
					g_term = prev;
					selclear();
					g_term = cur;
				}
				redraw();
				/*
				 * Swallow the click for apps that read the mouse
				 * themselves, so click-to-focus does not also move
				 * e.g. nvim's cursor. Otherwise fall through: a
				 * press-drag-release starting in an unfocused pane
				 * has to select, not just change focus.
				 */
				if (TERM_HAS_MOUSE() &&
				    !(e->xbutton.state & forcemousemod))
					return;
			}
		}
	}

	/*
	 * Ctrl+Click to open URLs and file paths.
	 * Check this before mouse mode to ensure it works even in vim/nvim.
	 */
	if (btn == Button1 && (e->xbutton.state & ControlMask)) {
		col = evcol(e);
		row = evrow(e);

		if (url_extract(col, row, urlbuf, sizeof(urlbuf)) > 0) {
			openurl(urlbuf);
			return;
		}
	}

	if (TERM_HAS_MOUSE() && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 0))
		return;

	if (btn == Button1) {
		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (TIMEDIFF(now, xsel.tclick2) <= tripleclicktimeout) {
			snap = SNAP_LINE;
		} else if (TIMEDIFF(now, xsel.tclick1) <= doubleclicktimeout) {
			snap = SNAP_WORD;
		} else {
			snap = 0;
		}
		xsel.tclick2 = xsel.tclick1;
		xsel.tclick1 = now;

		selstart(evcol(e), evrow(e), snap);
	}
}

void
propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	xpev = &e->xproperty;

	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		selnotify(e);
	}
}

void
selnotify(XEvent *e)
{
	ulong nitems, ofs, rem;
	int format;
	uchar *data, *last, *repl;
	Atom type, incratom, property = None;
	incratom = XInternAtom(xw.dpy, "INCR", 0);
	ofs = 0;

	if (e->type == SelectionNotify)
		property = e->xselection.property;
	else if (e->type == PropertyNotify)
		property = e->xproperty.atom;

	if (property == None)
		return;

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, ofs,
					BUFSIZ/4, False, AnyPropertyType,
					&type, &format, &nitems, &rem,
					&data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
			/*
			 * If there is some PropertyNotify with no data, then
			 * this is the signal of the selection owner that all
			 * data has been transferred. We won't need to receive
			 * PropertyNotify events anymore.
			 */
			MODBIT(xw.attrs.event_mask, 0, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);
		}

		if (type == incratom) {
			/*
			 * Activate the PropertyNotify events so we receive
			 * when the selection owner does send us the next
			 * chunk of data.
			 */
			MODBIT(xw.attrs.event_mask, 1, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);

			/*
			 * Deleting the property is the transfer start signal.
			 */
			XDeleteProperty(xw.dpy, xw.win, (int)property);
			continue;
		}

		/*
		 * As seen in getsel:
		 * Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while ((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
			ttywrite("\033[200~", 6, 0);
		ttywrite((char *)data, nitems * format / 8, 1);
		if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
			ttywrite("\033[201~", 6, 0);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while (rem > 0);

	/*
	 * Deleting the property again tells the selection owner to send the
	 * next data chunk in the property.
	 */
	XDeleteProperty(xw.dpy, xw.win, (int)property);
}

void
xclipcopy(void)
{
	clipcopy(NULL);
}

void
selclear_(XEvent *e)
{
	selclear();
}

void
selrequest(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string, clipboard;
	char *seltext;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	if (xsre->property == None)
		xsre->property = xsre->target;

	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
	if (xsre->target == xa_targets) {
		/* respond with the supported type */
		string = xsel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *) &string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {
		/*
		 * xith XA_STRING non ascii characters may be incorrect in the
		 * requestor. It is not our problem, use utf8.
		 */
		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		if (xsre->selection == XA_PRIMARY) {
			seltext = xsel.primary;
		} else if (xsre->selection == clipboard) {
			seltext = xsel.clipboard;
		} else {
			fprintf(stderr,
				"Unhandled clipboard selection 0x%lx\n",
				xsre->selection);
			return;
		}
		if (seltext != NULL) {
			XChangeProperty(xsre->display, xsre->requestor,
					xsre->property, xsre->target,
					8, PropModeReplace,
					(uchar *)seltext, strlen(seltext));
			xev.property = xsre->property;
		}
	}

	/* all done, send a notification to the listener */
	if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
setsel(char *str, Time t)
{
	if (!str)
		return;

	free(xsel.primary);
	xsel.primary = str;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
	if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
		selclear();
}

void
xsetsel(char *str)
{
	setsel(str, CurrentTime);
}

void
brelease(XEvent *e)
{
	int btn = e->xbutton.button;
	int x = e->xbutton.x;
	int y = e->xbutton.y;

	if (1 <= btn && btn <= 11)
		buttons &= ~(1 << (btn-1));

	if (btn == Button1)
		selautoscrollstop();

	/*
	 * Ctrl+Click opens URLs - don't send mouse release to PTY.
	 * This matches the early return in bpress() for Ctrl+Click.
	 */
	if (btn == Button1 && (e->xbutton.state & ControlMask))
		return;

	if (TERM_HAS_MOUSE() && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 1))
		return;
	if (btn == Button1)
		mousesel(e, 1);
}

void
bmotion(XEvent *e)
{
	if (TERM_HAS_MOUSE() && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	/*
	 * Scroll when selecting past the top/bottom edge of the active pane.
	 * run() drives the actual stepping on a timer, so scrolling keeps
	 * going while the pointer is held still outside the pane.
	 */
	if (e->xbutton.state & Button1Mask)
		selautoscrolltrack(e->xbutton.x, e->xbutton.y, e->xbutton.state);
	else
		selautoscrollstop();

	mousesel(e, 0);
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		win.w = width;
	if (height != 0)
		win.h = height;

	col = (win.w - 2 * borderpx) / win.cw;
	row = (win.h - 2 * borderpx) / win.ch;
	col = MAX(1, col);
	row = MAX(1, row);

	tresize(col, row);
	xresize(col, row);
	ttyresize(win.tw, win.th);
}

void
xresize(int col, int row)
{
	win.tw = col * win.cw;
	win.th = row * win.ch;

	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h,
			DefaultDepth(xw.dpy, xw.scr));
	XftDrawChange(xw.draw, xw.buf);
	if (bgimg.loaded)
		bgimg_scale();
	xclear(0, 0, win.w, win.h);

	/* resize to new width */
	xw.specbuf = xrealloc(xw.specbuf, col * sizeof(GlyphFontSpec));
}

ushort
sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;

}

int
xloadcolor(int i, const char *name, Color *ncolor)
{
	XRenderColor color = { .alpha = 0xffff };

	if (!name) {
		if (BETWEEN(i, 16, 255)) { /* 256 color */
			if (i < 6*6*6+16) { /* same colors as xterm */
				color.red   = sixd_to_16bit( ((i-16)/36)%6 );
				color.green = sixd_to_16bit( ((i-16)/6) %6 );
				color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
			} else { /* greyscale */
				color.red = 0x0808 + 0x0a0a * (i - (6*6*6+16));
				color.green = color.blue = color.red;
			}
			return XftColorAllocValue(xw.dpy, xw.vis,
			                          xw.cmap, &color, ncolor);
		} else
			name = colorname[i];
	}

	return XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, ncolor);
}

void
xloadcols(void)
{
	int i;
	static int loaded;
	Color *cp;

	if (loaded) {
		for (cp = dc.col; cp < &dc.col[dc.collen]; ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
	} else {
		dc.collen = MAX(LEN(colorname), 256);
		dc.col = xmalloc(dc.collen * sizeof(Color));
	}

	for (i = 0; i < dc.collen; i++)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (colorname[i])
				die("could not allocate color '%s'\n", colorname[i]);
			else
				die("could not allocate color %d\n", i);
		}
	loaded = 1;
}

int
xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	*r = dc.col[x].color.red >> 8;
	*g = dc.col[x].color.green >> 8;
	*b = dc.col[x].color.blue >> 8;

	return 0;
}

int
xsetcolorname(int x, const char *name)
{
	Color ncolor;

	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	if (!xloadcolor(x, name, &ncolor))
		return 1;

	XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
	dc.col[x] = ncolor;

	return 0;
}

/*
 * Background image functions
 */
static void
bgimg_free(void)
{
	if (bgimg.data) {
		free(bgimg.data);
		bgimg.data = NULL;
	}
	if (bgimg.pix) {
		XFreePixmap(xw.dpy, bgimg.pix);
		bgimg.pix = None;
	}
	bgimg.loaded = 0;
	bgimg.orig_w = 0;
	bgimg.orig_h = 0;
	bgimg.pix_w = 0;
	bgimg.pix_h = 0;
}

static int
bgimg_load(const char *path)
{
	FILE *fp;
	png_structp png;
	png_infop info;
	png_uint_32 w, h;
	int bit_depth, color_type;
	/*
	 * volatile: libpng longjmps back into the setjmp() below on a broken
	 * file, so these have to survive the jump to be cleaned up there.
	 */
	png_bytep * volatile row_pointers = NULL;
	volatile png_uint_32 nrows = 0;
	png_uint_32 x, y;

	bgimg_free();

	if (!path || !path[0])
		return 0;

	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "bgimg: cannot open %s\n", path);
		return 0;
	}

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		fclose(fp);
		return 0;
	}

	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		fclose(fp);
		return 0;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		while (nrows > 0)
			free(row_pointers[--nrows]);
		free(row_pointers);
		return 0;
	}

	png_init_io(png, fp);
	png_read_info(png, info);

	w = png_get_image_width(png, info);
	h = png_get_image_height(png, info);
	bit_depth = png_get_bit_depth(png, info);
	color_type = png_get_color_type(png, info);

	/* Convert to RGBA */
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (color_type == PNG_COLOR_TYPE_RGB ||
	    color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	if (bit_depth == 16)
		png_set_strip_16(png);

	png_read_update_info(png, info);

	row_pointers = malloc(sizeof(png_bytep) * h);
	if (!row_pointers) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return 0;
	}

	for (y = 0; y < h; y++) {
		row_pointers[y] = malloc(png_get_rowbytes(png, info));
		if (!row_pointers[y])
			png_longjmp(png, 1);
		nrows = y + 1;
	}

	png_read_image(png, row_pointers);

	/* Store as flat RGBA buffer */
	bgimg.data = malloc(w * h * 4);
	if (!bgimg.data)
		png_longjmp(png, 1);

	for (y = 0; y < h; y++)
		memcpy(bgimg.data + y * w * 4, row_pointers[y], w * 4);

	while (nrows > 0)
		free(row_pointers[--nrows]);
	free(row_pointers);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(fp);

	bgimg.orig_w = w;
	bgimg.orig_h = h;
	bgimg.loaded = 1;
	strncpy(bgimg.path, path, sizeof(bgimg.path) - 1);
	bgimg.path[sizeof(bgimg.path) - 1] = '\0';

	fprintf(stderr, "bgimg: loaded %s (%dx%d)\n", path, w, h);
	return 1;
}

static void
bgimg_scale(void)
{
	unsigned long pixel;
	XImage *img;
	int dst_w, dst_h;
	float scale_x, scale_y, scale;
	int scaled_w, scaled_h, crop_x, crop_y;
	unsigned char *buf;
	float sx, sy;
	int sx0, sy0, sx1, sy1;
	float fx, fy;
	unsigned char *p00, *p10, *p01, *p11, *dst, *src;

	if (!bgimg.loaded || !bgimg.data || win.w <= 0 || win.h <= 0)
		return;

	dst_w = win.w;
	dst_h = win.h;

	/* Fill/crop: scale to cover, then center-crop */
	scale_x = (float)dst_w / bgimg.orig_w;
	scale_y = (float)dst_h / bgimg.orig_h;
	scale = (scale_x > scale_y) ? scale_x : scale_y;

	scaled_w = (int)(bgimg.orig_w * scale + 0.5f);
	scaled_h = (int)(bgimg.orig_h * scale + 0.5f);

	crop_x = (scaled_w - dst_w) / 2;
	crop_y = (scaled_h - dst_h) / 2;

	buf = malloc(dst_w * dst_h * 4);
	if (!buf)
		return;

	/* Bilinear scale from original to destination with crop */
	for (int dy = 0; dy < dst_h; dy++) {
		for (int dx = 0; dx < dst_w; dx++) {
			/* Map destination pixel back to source */
			sx = (float)(dx + crop_x) / scale;
			sy = (float)(dy + crop_y) / scale;

			sx0 = (int)sx;
			sy0 = (int)sy;
			sx1 = sx0 + 1;
			sy1 = sy0 + 1;

			if (sx0 < 0) sx0 = 0;
			if (sy0 < 0) sy0 = 0;
			if (sx1 >= bgimg.orig_w) sx1 = bgimg.orig_w - 1;
			if (sy1 >= bgimg.orig_h) sy1 = bgimg.orig_h - 1;

			fx = sx - (int)sx;
			fy = sy - (int)sy;

			p00 = bgimg.data + (sy0 * bgimg.orig_w + sx0) * 4;
			p10 = bgimg.data + (sy0 * bgimg.orig_w + sx1) * 4;
			p01 = bgimg.data + (sy1 * bgimg.orig_w + sx0) * 4;
			p11 = bgimg.data + (sy1 * bgimg.orig_w + sx1) * 4;

			dst = buf + (dy * dst_w + dx) * 4;
			for (int c = 0; c < 4; c++) {
				float v = p00[c] * (1-fx) * (1-fy)
				        + p10[c] * fx * (1-fy)
				        + p01[c] * (1-fx) * fy
				        + p11[c] * fx * fy;
				dst[c] = (unsigned char)(v + 0.5f);
			}
		}
	}

	if (bgimg.pix)
		XFreePixmap(xw.dpy, bgimg.pix);

	bgimg.pix = XCreatePixmap(xw.dpy, xw.win, dst_w, dst_h,
	                          DefaultDepth(xw.dpy, xw.scr));

	/* Convert RGBA to the format X expects (BGRA on little-endian) */
	img = XCreateImage(xw.dpy, xw.vis, DefaultDepth(xw.dpy, xw.scr),
	                           ZPixmap, 0, NULL, dst_w, dst_h, 32, 0);
	if (!img) {
		free(buf);
		return;
	}

	img->data = malloc(img->bytes_per_line * dst_h);
	if (!img->data) {
		img->data = NULL; /* prevent XDestroyImage from freeing */
		XDestroyImage(img);
		free(buf);
		return;
	}

	for (int y = 0; y < dst_h; y++) {
		for (int x = 0; x < dst_w; x++) {
			src = buf + (y * dst_w + x) * 4;
			pixel = ((unsigned long)src[0] << 16) |
			                      ((unsigned long)src[1] << 8) |
			                      (unsigned long)src[2];
			XPutPixel(img, x, y, pixel);
		}
	}

	XPutImage(xw.dpy, bgimg.pix, dc.gc, img, 0, 0, 0, 0, dst_w, dst_h);
	XDestroyImage(img);
	free(buf);

	bgimg.pix_w = dst_w;
	bgimg.pix_h = dst_h;
}

static void
bgimg_paint(int x, int y, int w, int h)
{
	if (!bgimg.loaded || !bgimg.pix)
		return;

	if (x + w > bgimg.pix_w)
		w = bgimg.pix_w - x;
	if (y + h > bgimg.pix_h)
		h = bgimg.pix_h - y;
	if (w <= 0 || h <= 0)
		return;

	XCopyArea(xw.dpy, bgimg.pix, xw.buf, dc.gc, x, y, w, h, x, y);
}

/* Public API for Lua */
void
bgimg_set(const char *path, float opacity)
{
	if (!path || !path[0]) {
		bgimg_free();
		bgimg.path[0] = '\0';
		bgimg.opacity = 0.0f;
		return;
	}
	bgimg_load(path);
	bgimg.opacity = opacity;
	if (bgimg.loaded && win.w > 0 && win.h > 0)
		bgimg_scale();
}

const char *
bgimg_get_path(void)
{
	return bgimg.path;
}

float
bgimg_get_opacity(void)
{
	return bgimg.opacity;
}

void
bgimg_set_opacity(float opacity)
{
	if (opacity < 0.0f) opacity = 0.0f;
	if (opacity > 1.0f) opacity = 1.0f;
	bgimg.opacity = opacity;
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2)
{
	int cidx;

	if (bgimg.loaded && bgimg.opacity > 0.0f) {
		bgimg_paint(x1, y1, x2 - x1, y2 - y1);
		if (bgimg.opacity < 1.0f) {
			cidx = IS_SET(MODE_REVERSE) ? defaultfg : defaultbg;
			XRenderColor rc;
			rc.red   = dc.col[cidx].color.red;
			rc.green = dc.col[cidx].color.green;
			rc.blue  = dc.col[cidx].color.blue;
			rc.alpha = (unsigned short)(0xffff * (1.0f - bgimg.opacity));
			XRenderFillRectangle(xw.dpy, PictOpOver,
				XftDrawPicture(xw.draw), &rc,
				x1, y1, x2 - x1, y2 - y1);
		}
	} else {
		XftDrawRect(xw.draw,
				&dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg],
				x1, y1, x2-x1, y2-y1);
	}
}

void
xhints(void)
{
	XClassHint class = {opt_name ? opt_name : termname,
	                    opt_class ? opt_class : termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh;

	sizeh = XAllocSizeHints();

	sizeh->flags = PSize | PResizeInc | PBaseSize | PMinSize;
	sizeh->height = win.h;
	sizeh->width = win.w;
	sizeh->height_inc = win.ch;
	sizeh->width_inc = win.cw;
	sizeh->base_height = 2 * borderpx;
	sizeh->base_width = 2 * borderpx;
	sizeh->min_height = win.ch + 2 * borderpx;
	sizeh->min_width = win.cw + 2 * borderpx;
	if (xw.isfixed) {
		sizeh->flags |= PMaxSize;
		sizeh->min_width = sizeh->max_width = win.w;
		sizeh->min_height = sizeh->max_height = win.h;
	}
	if (xw.gm & (XValue|YValue)) {
		sizeh->flags |= USPosition | PWinGravity;
		sizeh->x = xw.l;
		sizeh->y = xw.t;
		sizeh->win_gravity = xgeommasktogravity(xw.gm);
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm,
			&class);
	XFree(sizeh);
}

int
xgeommasktogravity(int mask)
{
	switch (mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}

	return SouthEastGravity;
}

int
xloadfont(Font *f, FcPattern *pattern)
{
	FcPattern *configured, *match;
	FcResult result;
	XGlyphInfo extents;
	int wantattr, haveattr;

	/*
	 * Manually configure instead of calling XftMatchFont
	 * so that we can use the configured pattern for
	 * "missing glyph" lookups.
	 */
	configured = FcPatternDuplicate(pattern);
	if (!configured)
		return 1;

	FcConfigSubstitute(NULL, configured, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, configured);

	match = FcFontMatch(NULL, configured, &result);
	if (!match) {
		FcPatternDestroy(configured);
		return 1;
	}

	if (!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(configured);
		FcPatternDestroy(match);
		return 1;
	}

	if ((XftPatternGetInteger(pattern, "slant", 0, &wantattr) ==
	    XftResultMatch)) {
		/*
		 * Check if xft was unable to find a font with the appropriate
		 * slant but gave us one anyway. Try to mitigate.
		 */
		if ((XftPatternGetInteger(f->match->pattern, "slant", 0,
		    &haveattr) != XftResultMatch) || haveattr < wantattr) {
			f->badslant = 1;
			fputs("font slant does not match\n", stderr);
		}
	}

	if ((XftPatternGetInteger(pattern, "weight", 0, &wantattr) ==
	    XftResultMatch)) {
		if ((XftPatternGetInteger(f->match->pattern, "weight", 0,
		    &haveattr) != XftResultMatch) || haveattr != wantattr) {
			f->badweight = 1;
			fputs("font weight does not match\n", stderr);
		}
	}

	XftTextExtentsUtf8(xw.dpy, f->match,
		(const FcChar8 *) ascii_printable,
		strlen(ascii_printable), &extents);

	f->set = NULL;
	f->pattern = configured;

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = DIVCEIL(extents.xOff, strlen(ascii_printable));

	return 0;
}

void
xloadfonts(const char *fontstr, double fontsize)
{
	FcPattern *pattern;
	double fontval;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((const FcChar8 *)fontstr);

	if (!pattern)
		die("can't open font %s\n", fontstr);

	if (fontsize > 1) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = fontval;
		} else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = -1;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		defaultfontsize = usedfontsize;
	}

	if (xloadfont(&dc.font, pattern))
		die("can't open font %s\n", fontstr);

	if (usedfontsize < 0) {
		FcPatternGetDouble(dc.font.match->pattern,
		                   FC_PIXEL_SIZE, 0, &fontval);
		usedfontsize = fontval;
		if (fontsize == 0)
			defaultfontsize = fontval;
	}

	/* Setting character width and height. */
	win.cw = ceilf(dc.font.width * cwscale);
	win.ch = ceilf(dc.font.height * chscale);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (xloadfont(&dc.ifont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (xloadfont(&dc.ibfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (xloadfont(&dc.bfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

void
xunloadfont(Font *f)
{
	XftFontClose(xw.dpy, f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
}

void
xunloadfonts(void)
{
	/* Free the loaded fonts in the font cache.  */
	while (frclen > 0)
		XftFontClose(xw.dpy, frc[--frclen].font);

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

int
ximopen(Display *dpy)
{
	XIMCallback imdestroy = { .client_data = NULL, .callback = ximdestroy };
	XICCallback icdestroy = { .client_data = NULL, .callback = xicdestroy };

	xw.ime.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
	if (xw.ime.xim == NULL)
		return 0;

	if (XSetIMValues(xw.ime.xim, XNDestroyCallback, &imdestroy, NULL))
		fprintf(stderr, "XSetIMValues: "
		                "Could not set XNDestroyCallback.\n");

	xw.ime.spotlist = XVaCreateNestedList(0, XNSpotLocation, &xw.ime.spot,
	                                      NULL);

	if (xw.ime.xic == NULL) {
		xw.ime.xic = XCreateIC(xw.ime.xim, XNInputStyle,
		                       XIMPreeditNothing | XIMStatusNothing,
		                       XNClientWindow, xw.win,
		                       XNDestroyCallback, &icdestroy,
		                       NULL);
	}
	if (xw.ime.xic == NULL)
		fprintf(stderr, "XCreateIC: Could not create input context.\n");

	return 1;
}

void
ximinstantiate(Display *dpy, XPointer client, XPointer call)
{
	if (ximopen(dpy))
		XUnregisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
		                                 ximinstantiate, NULL);
}

void
ximdestroy(XIM xim, XPointer client, XPointer call)
{
	xw.ime.xim = NULL;
	XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                               ximinstantiate, NULL);
	XFree(xw.ime.spotlist);
}

int
xicdestroy(XIC xim, XPointer client, XPointer call)
{
	xw.ime.xic = NULL;
	return 1;
}

void
xinit(int cols, int rows)
{
	XGCValues gcvalues;
	Cursor cursor;
	Window parent, root;
	pid_t thispid = getpid();
	XColor xmousefg, xmousebg;
	int depth;

	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);
	root = XRootWindow(xw.dpy, xw.scr);

	xw.vis = XDefaultVisual(xw.dpy, xw.scr);
	depth = XDefaultDepth(xw.dpy, xw.scr);
	xw.cmap = XDefaultColormap(xw.dpy, xw.scr);

	/* font */
	if (!FcInit())
		die("could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

	/* colors */
	xloadcols();

	/* Apply any colors set from Lua config before X init */
	lua_config_apply_deferred_colors();

	/* adjust fixed window geometry */
	win.w = 2 * borderpx + cols * win.cw;
	win.h = 2 * borderpx + rows * win.ch;
	if (xw.gm & XNegative)
		xw.l += DisplayWidth(xw.dpy, xw.scr) - win.w - 2;
	if (xw.gm & YNegative)
		xw.t += DisplayHeight(xw.dpy, xw.scr) - win.h - 2;

	/* Events */
	xw.attrs.background_pixel = 0;
	xw.attrs.border_pixel = 0;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask | KeyReleaseMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	xw.attrs.colormap = xw.cmap;

	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = root;
	xw.win = XCreateWindow(xw.dpy, root, xw.l, xw.t,
			win.w, win.h, 0, depth, InputOutput,
			xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);
	if (parent != root)
		XReparentWindow(xw.dpy, xw.win, parent, xw.l, xw.t);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.dpy, xw.win, GCGraphicsExposures,
			&gcvalues);

	/* Create pixmap for double buffering */
	xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h, depth);
	XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
	XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, win.w, win.h);

	/* font spec buffer */
	xw.specbuf = xmalloc(cols * sizeof(GlyphFontSpec));

	/* Xft rendering context */
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

	/* Scale background image if loaded during config init */
	if (bgimg.loaded)
		bgimg_scale();

	/* input methods */
	if (!ximopen(xw.dpy)) {
		XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                                       ximinstantiate, NULL);
	}

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xw.dpy, mouseshape);
	XDefineCursor(xw.dpy, xw.win, cursor);

	if (XParseColor(xw.dpy, xw.cmap, colorname[mousefg], &xmousefg) == 0) {
		xmousefg.red   = 0xffff;
		xmousefg.green = 0xffff;
		xmousefg.blue  = 0xffff;
	}

	if (XParseColor(xw.dpy, xw.cmap, colorname[mousebg], &xmousebg) == 0) {
		xmousebg.red   = 0x0000;
		xmousebg.green = 0x0000;
		xmousebg.blue  = 0x0000;
	}

	XRecolorCursor(xw.dpy, cursor, &xmousefg, &xmousebg);

	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	xw.netwmiconname = XInternAtom(xw.dpy, "_NET_WM_ICON_NAME", False);
	xw.netwmicon = XInternAtom(xw.dpy, "_NET_WM_ICON", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
	XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&thispid, 1);

	xseticon();

	win.mode = MODE_NUMLOCK;
	resettitle();
	xhints();
	XMapWindow(xw.dpy, xw.win);
	XSync(xw.dpy, False);

	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick1);
	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick2);
	xsel.primary = NULL;
	xsel.clipboard = NULL;
	xsel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if (xsel.xtarget == None)
		xsel.xtarget = XA_STRING;
}

int
xmakeglyphfontspecs(XftGlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	float winx = pane_ox + x * win.cw, winy = pane_oy + y * win.ch, xp, yp;
	ushort mode, prevmode = USHRT_MAX;
	Font *font = &dc.font;
	int frcflags = FRC_NORMAL;
	float runewidth = win.cw;
	Rune rune;
	FT_UInt glyphidx;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	int i, f, numspecs = 0;

	for (i = 0, xp = winx, yp = winy + font->ascent; i < len; ++i) {
		/* Fetch rune and mode for current glyph. */
		rune = glyphs[i].u;
		mode = glyphs[i].mode;

		/* Skip dummy wide-character spacing. */
		if (mode == ATTR_WDUMMY)
			continue;

		/* Determine font for glyph if different from previous glyph. */
		if (prevmode != mode) {
			prevmode = mode;
			font = &dc.font;
			frcflags = FRC_NORMAL;
			runewidth = win.cw * ((mode & ATTR_WIDE) ? 2.0f : 1.0f);
			if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD)) {
				font = &dc.ibfont;
				frcflags = FRC_ITALICBOLD;
			} else if (mode & ATTR_ITALIC) {
				font = &dc.ifont;
				frcflags = FRC_ITALIC;
			} else if (mode & ATTR_BOLD) {
				font = &dc.bfont;
				frcflags = FRC_BOLD;
			}
			yp = winy + font->ascent;
		}

		/* Lookup character index with default font. */
		glyphidx = XftCharIndex(xw.dpy, font->match, rune);
		if (glyphidx) {
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = glyphidx;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			xp += runewidth;
			numspecs++;
			continue;
		}

		/* Fallback on font cache, search the font cache for match. */
		for (f = 0; f < frclen; f++) {
			glyphidx = XftCharIndex(xw.dpy, frc[f].font, rune);
			/* Everything correct. */
			if (glyphidx && frc[f].flags == frcflags)
				break;
			/* We got a default font for a not found glyph. */
			if (!glyphidx && frc[f].flags == frcflags
					&& frc[f].unicodep == rune) {
				break;
			}
		}

		/* Nothing was found. Use fontconfig to find matching font. */
		if (f >= frclen) {
			if (!font->set)
				font->set = FcFontSort(0, font->pattern,
				                       1, 0, &fcres);
			fcsets[0] = font->set;

			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 *
			 * Xft and fontconfig are design failures.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, rune);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

			FcConfigSubstitute(0, fcpattern,
					FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets, 1,
					fcpattern, &fcres);

			/* Allocate memory for the new cache entry. */
			if (frclen >= frccap) {
				frccap += 16;
				frc = xrealloc(frc, frccap * sizeof(Fontcache));
			}

			frc[frclen].font = XftFontOpenPattern(xw.dpy,
					fontpattern);
			if (!frc[frclen].font)
				die("XftFontOpenPattern failed seeking fallback font: %s\n",
					strerror(errno));
			frc[frclen].flags = frcflags;
			frc[frclen].unicodep = rune;

			glyphidx = XftCharIndex(xw.dpy, frc[frclen].font, rune);

			f = frclen;
			frclen++;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);
		}

		specs[numspecs].font = frc[f].font;
		specs[numspecs].glyph = glyphidx;
		specs[numspecs].x = (short)xp;
		specs[numspecs].y = (short)yp;
		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

void
xdrawglyphfontspecs(const XftGlyphFontSpec *specs, Glyph base, int len, int x, int y)
{
	int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);

	int winx = pane_ox + x * win.cw, winy = pane_oy + y * win.ch,
	    width = charlen * win.cw;
	Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;
	XRectangle r;

	/* Fallback on color display for attributes not supported by the font */
	if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
		if (dc.ibfont.badslant || dc.ibfont.badweight)
			base.fg = defaultattr;
	} else if ((base.mode & ATTR_ITALIC && dc.ifont.badslant) ||
	    (base.mode & ATTR_BOLD && dc.bfont.badweight)) {
		base.fg = defaultattr;
	}

	if (IS_TRUECOL(base.fg)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if (IS_TRUECOL(base.bg)) {
		colbg.alpha = 0xffff;
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}

	/* Change basic system colors [0-7] to bright system colors [8-15] */
	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
		fg = &dc.col[base.fg + 8];

	if (IS_SET(MODE_REVERSE)) {
		if (fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg,
					&revfg);
			fg = &revfg;
		}

		if (bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg,
					&revbg);
			bg = &revbg;
		}
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		colfg.alpha = fg->color.alpha;
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
		fg = &revfg;
	}

	if (base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if (base.mode & ATTR_BLINK && win.mode & MODE_BLINK)
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	/* Intelligent cleaning up of the borders. */
	if (x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + win.ch +
			((winy + win.ch >= borderpx + win.th)? win.h : 0));
	}
	if (winx + width >= borderpx + win.tw) {
		xclear(winx + width, (y == 0)? 0 : winy, win.w,
			((winy + win.ch >= borderpx + win.th)? win.h : (winy + win.ch)));
	}
	if (y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if (winy + win.ch >= borderpx + win.th)
		xclear(winx, winy + win.ch, winx + width, win.h);

	/* Clean up the region we want to draw to. */
	if (bgimg.loaded && bgimg.opacity > 0.0f) {
		XRenderColor rc;
		rc.red   = bg->color.red;
		rc.green = bg->color.green;
		rc.blue  = bg->color.blue;
		rc.alpha = (unsigned short)(0xffff * (1.0f - bgimg.opacity));
		XRenderFillRectangle(xw.dpy, PictOpOver,
			XftDrawPicture(xw.draw), &rc,
			winx, winy, width, win.ch);
	} else {
		XftDrawRect(xw.draw, bg, winx, winy, width, win.ch);
	}

	/* Set the clip region because Xft is sometimes dirty. */
	r.x = 0;
	r.y = 0;
	r.height = win.ch;
	r.width = width;
	XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

	/* Render the glyphs. */
	XftDrawGlyphFontSpec(xw.draw, fg, specs, len);

	/* Render underline and strikethrough. */
	if (base.mode & ATTR_UNDERLINE) {
		XftDrawRect(xw.draw, fg, winx, winy + dc.font.ascent * chscale + 1,
				width, 1);
	}

	if (base.mode & ATTR_STRUCK) {
		XftDrawRect(xw.draw, fg, winx, winy + 2 * dc.font.ascent * chscale / 3,
				width, 1);
	}

	/* Reset clip to none. */
	XftDrawSetClip(xw.draw, 0);
}

void
xdrawglyph(Glyph g, int x, int y)
{
	int numspecs;
	XftGlyphFontSpec spec;

	numspecs = xmakeglyphfontspecs(&spec, &g, 1, x, y);
	xdrawglyphfontspecs(&spec, g, numspecs, x, y);
}

void
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	Color drawcol;

	/* remove the old cursor */
	if (selected(ox, oy))
		og.mode ^= ATTR_REVERSE;
	xdrawglyph(og, ox, oy);

	if (IS_SET(MODE_HIDE))
		return;

	/* Hide cursor during blink-off phase for blinking cursor types */
	/* Cursor types 0,1,3,5 are blinking; 2,4,6 are steady */
	if (cursorblinktimeout && !cursorblink_on) {
		if (win.cursor == 0 || win.cursor == 1 ||
		    win.cursor == 3 || win.cursor == 5)
			return;
	}

	/*
	 * Select the right color for the right mode.
	 */
	g.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE;

	if (IS_SET(MODE_REVERSE)) {
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (selected(cx, cy)) {
			drawcol = dc.col[defaultcs];
			g.fg = defaultrcs;
		} else {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultcs;
		}
	} else {
		if (selected(cx, cy)) {
			g.fg = defaultfg;
			g.bg = defaultrcs;
		} else {
			g.fg = defaultbg;
			g.bg = defaultcs;
		}
		drawcol = dc.col[g.bg];
	}

	/* draw the new one */
	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		case 7: /* st extension */
			g.u = 0x2603; /* snowman (U+2603) */
			/* FALLTHROUGH */
		case 0: /* Blinking Block */
		case 1: /* Blinking Block (Default) */
		case 2: /* Steady Block */
			xdrawglyph(g, cx, cy);
			break;
		case 3: /* Blinking Underline */
		case 4: /* Steady Underline */
			XftDrawRect(xw.draw, &drawcol,
					pane_ox + cx * win.cw,
					pane_oy + (cy + 1) * win.ch - \
						cursorthickness,
					win.cw, cursorthickness);
			break;
		case 5: /* Blinking bar */
		case 6: /* Steady bar */
			XftDrawRect(xw.draw, &drawcol,
					pane_ox + cx * win.cw,
					pane_oy + cy * win.ch,
					cursorthickness, win.ch);
			break;
		}
	} else {
		XftDrawRect(xw.draw, &drawcol,
				pane_ox + cx * win.cw,
				pane_oy + cy * win.ch,
				win.cw - 1, 1);
		XftDrawRect(xw.draw, &drawcol,
				pane_ox + cx * win.cw,
				pane_oy + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				pane_ox + (cx + 1) * win.cw - 1,
				pane_oy + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				pane_ox + cx * win.cw,
				pane_oy + (cy + 1) * win.ch - 1,
				win.cw, 1);
	}
}

void
xsetenv(void)
{
	char buf[sizeof(long) * 8 + 1];

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);
}

void
xseticontitle(char *p)
{
	XTextProperty prop;

	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMIconName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmiconname);
	XFree(prop.value);
}

/* Try to load PNG icon and set as window icon. Returns 1 on success, 0 on failure */
static int
xseticonpng(const char *path)
{
	unsigned char r, g, b, a;
	FILE *fp;
	png_structp png;
	png_infop info;
	png_uint_32 w, h;
	int bit_depth, color_type;
	/*
	 * volatile: libpng longjmps back into the setjmp() below on a broken
	 * file, so these have to survive the jump to be cleaned up there.
	 */
	png_bytep * volatile row_pointers = NULL;
	unsigned long * volatile icon_data = NULL;
	volatile png_uint_32 nrows = 0;
	png_uint_32 x, y;
	int success = 0;

	fp = fopen(path, "rb");

	if (!fp)
		return 0;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		fclose(fp);
		return 0;
	}

	info = png_create_info_struct(png);
	if (!info) {
		png_destroy_read_struct(&png, NULL, NULL);
		fclose(fp);
		return 0;
	}

	if (setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		while (nrows > 0)
			free(row_pointers[--nrows]);
		free(row_pointers);
		free(icon_data);
		return 0;
	}

	png_init_io(png, fp);
	png_read_info(png, info);

	w = png_get_image_width(png, info);
	h = png_get_image_height(png, info);
	bit_depth = png_get_bit_depth(png, info);
	color_type = png_get_color_type(png, info);

	/* Convert to RGBA */
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (color_type == PNG_COLOR_TYPE_RGB ||
	    color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	if (bit_depth == 16)
		png_set_strip_16(png);

	png_read_update_info(png, info);

	row_pointers = malloc(sizeof(png_bytep) * h);
	if (!row_pointers) {
		png_destroy_read_struct(&png, &info, NULL);
		fclose(fp);
		return 0;
	}

	for (y = 0; y < h; y++) {
		row_pointers[y] = malloc(png_get_rowbytes(png, info));
		if (!row_pointers[y])
			png_longjmp(png, 1);
		nrows = y + 1;
	}

	png_read_image(png, row_pointers);

	/* Allocate icon data: width + height + pixels */
	icon_data = malloc((2 + w * h) * sizeof(unsigned long));
	if (!icon_data)
		png_longjmp(png, 1);

	icon_data[0] = w;
	icon_data[1] = h;

	/* Convert RGBA to ARGB (X11 _NET_WM_ICON format) */
	for (y = 0; y < h; y++) {
		png_bytep row = row_pointers[y];
		for (x = 0; x < w; x++) {
			png_bytep px = &row[x * 4];
			r = px[0];
			g = px[1];
			b = px[2];
			a = px[3];
			icon_data[2 + y * w + x] = ((unsigned long)a << 24) |
			                           ((unsigned long)r << 16) |
			                           ((unsigned long)g << 8) |
			                           (unsigned long)b;
		}
	}

	XChangeProperty(xw.dpy, xw.win, xw.netwmicon, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)icon_data, 2 + w * h);

	success = 1;

	free(icon_data);
	while (nrows > 0)
		free(row_pointers[--nrows]);
	free(row_pointers);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(fp);

	return success;
}

static void
xseticonemoji(void)
{
	unsigned char r, g, b;
	FT_Library ft;
	FT_Face face;
	FT_UInt glyph_index;
	int icon_size = 64;
	unsigned long *icon_data;
	int i, x, y;
	uint32_t emoji = g_icon_emoji;
	int best, best_diff, diff, w, h;
	unsigned char *p, a;

	if (FT_Init_FreeType(&ft))
		return;

	if (FT_New_Face(ft, "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", 0, &face)) {
		FT_Done_FreeType(ft);
		return;
	}

	/* Select a bitmap strike for color emoji */
	if (face->num_fixed_sizes > 0) {
		best = 0;
		best_diff = abs(face->available_sizes[0].height - icon_size);
		for (i = 1; i < face->num_fixed_sizes; i++) {
			diff = abs(face->available_sizes[i].height - icon_size);
			if (diff < best_diff) {
				best_diff = diff;
				best = i;
			}
		}
		FT_Select_Size(face, best);
		icon_size = face->available_sizes[best].height;
	}

	glyph_index = FT_Get_Char_Index(face, emoji);
	if (!glyph_index) {
		FT_Done_Face(face);
		FT_Done_FreeType(ft);
		return;
	}

	if (FT_Load_Glyph(face, glyph_index, FT_LOAD_COLOR)) {
		FT_Done_Face(face);
		FT_Done_FreeType(ft);
		return;
	}

	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
		FT_Done_Face(face);
		FT_Done_FreeType(ft);
		return;
	}

	FT_Bitmap *bmp = &face->glyph->bitmap;
	w = bmp->width;
	h = bmp->rows;

	if (w == 0 || h == 0) {
		FT_Done_Face(face);
		FT_Done_FreeType(ft);
		return;
	}

	/* Allocate icon data: width + height + pixels */
	icon_data = malloc((2 + w * h) * sizeof(unsigned long));
	if (!icon_data) {
		FT_Done_Face(face);
		FT_Done_FreeType(ft);
		return;
	}

	icon_data[0] = w;
	icon_data[1] = h;

	/* Convert bitmap to ARGB */
	if (bmp->pixel_mode == FT_PIXEL_MODE_BGRA) {
		/* Color emoji - BGRA format */
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				p = bmp->buffer + y * bmp->pitch + x * 4;
				b = p[0];
				g = p[1];
				r = p[2];
				a = p[3];
				icon_data[2 + y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		}
	} else {
		/* Grayscale - use white with alpha */
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				a = bmp->buffer[y * bmp->pitch + x];
				icon_data[2 + y * w + x] = (a << 24) | 0xFFFFFF;
			}
		}
	}

	XChangeProperty(xw.dpy, xw.win, xw.netwmicon, XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)icon_data, 2 + w * h);

	free(icon_data);
	FT_Done_Face(face);
	FT_Done_FreeType(ft);
}

/* Set window icon - tries PNG first, falls back to emoji */
static void
xseticon(void)
{
	if (g_icon_use_emoji) {
		xseticonemoji();
		return;
	}

	/* Try PNG icon locations in order of preference */
	static const char *icon_paths[] = {
		/* Installed location */
		"/usr/local/share/icons/hicolor/64x64/apps/fearless-terminal.png",
		"/usr/share/icons/hicolor/64x64/apps/fearless-terminal.png",
		/* Development/local locations */
		"./assets/icons/hicolor/64x64/apps/fearless-terminal.png",
		"assets/icons/hicolor/64x64/apps/fearless-terminal.png",
		NULL
	};

	for (int i = 0; icon_paths[i]; i++) {
		if (xseticonpng(icon_paths[i]))
			return;
	}

	xseticonemoji();
}

/* Change window icon to a different emoji (called from Lua) */
void
xchangeicon(uint32_t emoji)
{
	g_icon_emoji = emoji;
	g_icon_use_emoji = 1;  /* User explicitly requested emoji */

	xseticonemoji();
}

void
xsettitle(char *p)
{
	XTextProperty prop;

	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
}

void
updatetitle(void)
{
	static char title[1024];
	int pos = 0;
	Tab *tab;
	const char *name;

	if (g_tabs.window_title_manual && g_tabs.window_custom_title[0]) {
		xsettitle(g_tabs.window_custom_title);
		return;
	}

	if (g_tabs.count == 0) {
		xsettitle("st");
		return;
	}

	for (int i = 0; i < g_tabs.count && pos < (int)sizeof(title) - 50; i++) {
		tab = g_tabs.tabs[i];
		name = NULL;

		if (tab && tab->title_manual && tab->custom_title[0]) {
			name = tab->custom_title;
		} else if (tab && tab->active_pane && tab->active_pane->terminal) {
			name = terminal_get_foreground_process(tab->active_pane->terminal);
		}
		if (!name || !name[0])
			name = "terminal";

		if (i > 0) {
			pos += snprintf(title + pos, sizeof(title) - pos, " | ");
		}

		/* Mark active tab with * */
		if (i == g_tabs.active) {
			pos += snprintf(title + pos, sizeof(title) - pos, "*%s", name);
		} else {
			pos += snprintf(title + pos, sizeof(title) - pos, "%s", name);
		}
	}

	title[sizeof(title) - 1] = '\0';
	xsettitle(title);
}

int
xstartdraw(void)
{
	/*
	 * Always draw. The previous IS_SET(MODE_VISIBLE) gate could get
	 * stuck off when an UnmapNotify cleared the bit and no subsequent
	 * VisibilityNotify arrived to restore it (common over SSH/X11
	 * forwarding and across some WM workspace switches), leaving the
	 * terminal silently un-rendered while the PTY kept advancing.
	 */
	return 1;
}

void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, x, ox, numspecs;
	Glyph base, new;
	XftGlyphFontSpec *specs = xw.specbuf;
	numspecs = xmakeglyphfontspecs(specs, &line[x1], x2 - x1, x1, y1);

	i = ox = 0;
	for (x = x1; x < x2 && i < numspecs; x++) {
		new = line[x];
		if (new.mode == ATTR_WDUMMY)
			continue;
		if (selected(x, y1))
			new.mode ^= ATTR_REVERSE;
		if (i > 0 && ATTRCMP(base, new)) {
			xdrawglyphfontspecs(specs, base, i, ox, y1);
			specs += i;
			numspecs -= i;
			i = 0;
		}
		if (i == 0) {
			ox = x;
			base = new;
		}
		i++;
	}
	if (i > 0)
		xdrawglyphfontspecs(specs, base, i, ox, y1);
}

/* Draw UTF-8 string with font fallback (for emoji support in tab bar) */
static void
drawutf8string(XftColor *color, int x, int y, const char *str, int len)
{
	const char *p = str;
	const char *end = str + len;
	int xpos = x;
	Rune rune;
	FT_UInt glyphidx;
	int charlen;
	unsigned char c;
	XftFont *font;
	int f;
	XGlyphInfo extents;

	while (p < end) {
		charlen = 1;
		c = *p;

		if ((c & 0x80) == 0) {
			rune = c;
		} else if ((c & 0xE0) == 0xC0 && p + 1 < end) {
			rune = (c & 0x1F) << 6 | (p[1] & 0x3F);
			charlen = 2;
		} else if ((c & 0xF0) == 0xE0 && p + 2 < end) {
			rune = (c & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
			charlen = 3;
		} else if ((c & 0xF8) == 0xF0 && p + 3 < end) {
			rune = (c & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
			charlen = 4;
		} else {
			rune = c;
		}

		p += charlen;

		/* Try main font first */
		glyphidx = XftCharIndex(xw.dpy, dc.font.match, rune);
		font = dc.font.match;

		if (!glyphidx) {
			for (f = 0; f < frclen; f++) {
				glyphidx = XftCharIndex(xw.dpy, frc[f].font, rune);
				if (glyphidx && frc[f].flags == FRC_NORMAL) {
					font = frc[f].font;
					break;
				}
			}

			if (!glyphidx) {
				FcFontSet *fcsets[1];
				FcPattern *fcpattern, *fontpattern;
				FcCharSet *fccharset;
				FcResult fcres;

				if (!dc.font.set)
					dc.font.set = FcFontSort(0, dc.font.pattern, 1, 0, &fcres);
				fcsets[0] = dc.font.set;

				fcpattern = FcPatternDuplicate(dc.font.pattern);
				fccharset = FcCharSetCreate();

				FcCharSetAddChar(fccharset, rune);
				FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
				FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

				FcConfigSubstitute(0, fcpattern, FcMatchPattern);
				FcDefaultSubstitute(fcpattern);

				fontpattern = FcFontSetMatch(0, fcsets, 1, fcpattern, &fcres);

				if (fontpattern) {
					if (frclen >= frccap) {
						frccap += 16;
						frc = xrealloc(frc, frccap * sizeof(Fontcache));
					}

					/*
					 * On success the font takes ownership of
					 * the pattern; on failure we still own it.
					 */
					frc[frclen].font = XftFontOpenPattern(xw.dpy, fontpattern);
					if (frc[frclen].font) {
						frc[frclen].flags = FRC_NORMAL;
						frc[frclen].unicodep = rune;
						font = frc[frclen].font;
						glyphidx = XftCharIndex(xw.dpy, font, rune);
						frclen++;
					} else {
						FcPatternDestroy(fontpattern);
					}
				}

				FcPatternDestroy(fcpattern);
				FcCharSetDestroy(fccharset);
			}
		}

		if (glyphidx) {
			XftGlyphFontSpec spec;
			spec.font = font;
			spec.glyph = glyphidx;
			spec.x = xpos;
			spec.y = y;
			XftDrawGlyphFontSpec(xw.draw, color, &spec, 1);
		}

		/* Advance position - use main font metrics for consistency */
		XftTextExtentsUtf8(xw.dpy, dc.font.match, (FcChar8 *)&rune, charlen > 1 ? charlen : 1, &extents);
		xpos += extents.xOff ? extents.xOff : win.cw;
	}
}

void
drawtabbar(void)
{
	int i, x, tabw;
	XftColor *bg, *fg;
	char prompt[300];
	const char *label;
	Tab *tab;
	int is_active;
	char tabtitle[256];
	const char *title, *process, *lua_title;

	/* In rename mode show the prompt even when no tab bar is drawn,
	 * so the user has visual feedback while typing. */
	if (g_tabs.count <= 1 && g_tabs.renaming) {
		label = g_tabs.renaming == RENAME_TAB ? "tab" : "win";
		snprintf(prompt, sizeof(prompt), " %s> %s_ ", label, g_tabs.rename_buf);
		XftDrawRect(xw.draw, &dc.col[defaultfg], 0, 0, win.w, win.ch);
		drawutf8string(&dc.col[defaultbg], 4, dc.font.ascent,
		               prompt, strlen(prompt));
		return;
	}

	if (g_tabs.count <= 1)
		return;  /* No tab bar for single tab */

	tabw = win.w / g_tabs.count;
	if (tabw < 50) tabw = 50;
	if (tabw > 200) tabw = 200;

	x = 0;
	for (i = 0; i < g_tabs.count; i++) {
		tab = g_tabs.tabs[i];
		is_active = (i == g_tabs.active);

		if (is_active) {
			/* Active tab - brighter */
			bg = &dc.col[defaultfg];
			fg = &dc.col[defaultbg];
		} else {
			/* Inactive tab - dimmer */
			bg = &dc.col[8];  /* bright black / dark gray */
			fg = &dc.col[7];  /* white */
		}

		XftDrawRect(xw.draw, bg, x, 0, tabw - 2, win.ch);

		title = "";
		process = "terminal";

		/* Check for rename mode on active tab slot */
		if (is_active && g_tabs.renaming) {
			label = g_tabs.renaming == RENAME_TAB ? "tab" : "win";
			snprintf(tabtitle, sizeof(tabtitle), " %s> %s_ ", label, g_tabs.rename_buf);
			drawutf8string(fg, x + 4, dc.font.ascent, tabtitle, strlen(tabtitle));
			x += tabw;
			continue;
		}

		/* User-set tab name takes precedence over process name */
		if (tab && tab->title_manual && tab->custom_title[0]) {
			title = tab->custom_title;
		}

		if (tab && tab->active_pane && tab->active_pane->terminal) {
			process = terminal_get_foreground_process(tab->active_pane->terminal);
		}

		/* Try Lua formatter first, else use custom title or process name */
		lua_title = lua_config_format_tab(i, is_active, title, process);
		if (lua_title) {
			snprintf(tabtitle, sizeof(tabtitle), " %s ", lua_title);
		} else if (title[0]) {
			snprintf(tabtitle, sizeof(tabtitle), " %s ", title);
		} else if (process && process[0]) {
			snprintf(tabtitle, sizeof(tabtitle), " %s ", process);
		} else {
			snprintf(tabtitle, sizeof(tabtitle), " %d ", i + 1);
		}

		drawutf8string(fg, x + 4, dc.font.ascent, tabtitle, strlen(tabtitle));

		x += tabw;
	}
}

/* Show a notification overlay for ~1.5 seconds */
void
xnotify(const char *msg)
{
	snprintf(notification.text, sizeof(notification.text), "%s", msg);
	clock_gettime(CLOCK_MONOTONIC, &notification.time);
	notification.active = 1;
}

static void
drawnotification(void)
{
	struct timespec now;
	int len, textw, texth, x, y;

	if (!notification.active)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed = (now.tv_sec - notification.time.tv_sec) +
	                 (now.tv_nsec - notification.time.tv_nsec) / 1e9;
	if (elapsed > 1.5) {
		notification.active = 0;
		return;
	}

	len = strlen(notification.text);
	textw = len * win.cw + 20; /* padding */
	texth = win.ch + 16;
	x = (win.w - textw) / 2;
	y = (win.h - texth) / 2;

	XSetForeground(xw.dpy, dc.gc, dc.col[0].pixel);  /* black bg */
	XFillRectangle(xw.dpy, xw.buf, dc.gc, x, y, textw, texth);

	XSetForeground(xw.dpy, dc.gc, dc.col[7].pixel);  /* white border */
	XDrawRectangle(xw.dpy, xw.buf, dc.gc, x, y, textw - 1, texth - 1);

	drawutf8string(&dc.col[7], x + 10, y + 8 + dc.font.ascent,
	               notification.text, len);
}

void
xfinishdraw(void)
{
	drawtabbar();
	drawnotification();
	updatetitle();
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, win.w,
			win.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc,
			dc.col[IS_SET(MODE_REVERSE)?
				defaultfg : defaultbg].pixel);
}

static void
drawpane(Pane *p, int is_active)
{
	Terminal *old;
	Pane *active_pane;

	if (!p)
		return;

	if (p->type == PANE_LEAF && p->terminal) {
		pane_ox = p->x;
		pane_oy = p->y;

		old = g_term;
		g_term = p->terminal;

		tfulldirt();
		draw();

		/* Note: cursor is drawn inside draw() */

		g_term = old;
	} else {
		Tab *active_tab = (g_tabs.active >= 0 && g_tabs.active < g_tabs.count)
		                  ? g_tabs.tabs[g_tabs.active] : NULL;
		active_pane = (active_tab) ? active_tab->active_pane : NULL;
		drawpane(p->child1, is_active && (p->child1 == active_pane));
		drawpane(p->child2, is_active && (p->child2 == active_pane));
	}
}

static void
drawpaneborders(Pane *p)
{
	int bx, by;

	if (!p || p->type == PANE_LEAF)
		return;

	if (p->type == PANE_VSPLIT) {
		/* Vertical line between left/right */
		bx = p->child1->x + p->child1->w;
		XSetForeground(xw.dpy, dc.gc, dc.col[8].pixel);  /* dark gray */
		XFillRectangle(xw.dpy, xw.buf, dc.gc, bx - 1, p->y, 2, p->h);
	} else if (p->type == PANE_HSPLIT) {
		/* Horizontal line between top/bottom */
		by = p->child1->y + p->child1->h;
		XSetForeground(xw.dpy, dc.gc, dc.col[8].pixel);
		XFillRectangle(xw.dpy, xw.buf, dc.gc, p->x, by - 1, p->w, 2);
	}

	drawpaneborders(p->child1);
	drawpaneborders(p->child2);
}

static void
drawactivepaneborder(Pane *active)
{
	int bw, x, y, w, h;

	if (!active || active->type != PANE_LEAF || activepaneborder == 0)
		return;

	XSetForeground(xw.dpy, dc.gc, dc.col[activepanebordercolor].pixel);

	bw = activepaneborder; /* border width from config */
	x = active->x;
	y = active->y;
	w = active->w;
	h = active->h;

	/* Draw 4 edges of the border */
	XFillRectangle(xw.dpy, xw.buf, dc.gc, x, y, w, bw);           /* top */
	XFillRectangle(xw.dpy, xw.buf, dc.gc, x, y + h - bw, w, bw);  /* bottom */
	XFillRectangle(xw.dpy, xw.buf, dc.gc, x, y, bw, h);           /* left */
	XFillRectangle(xw.dpy, xw.buf, dc.gc, x + w - bw, y, bw, h);  /* right */
}

void
drawallpanes(void)
{
	Tab *tab = g_tabs.tabs[g_tabs.active];
	int tw, th, tbar_h;
	Pane *stack[MAX_PANE_DEPTH];
	int sp;
	Pane *p;
	int cx, cy;

	if (!tab || !tab->root_pane)
		return;

	if (!xstartdraw())
		return;

	tw = win.w - 2 * borderpx;
	th = win.h - 2 * borderpx;
	tbar_h = (g_tabs.count > 1) ? win.ch : 0;
	th -= tbar_h;
	pane_layout(tab->root_pane, borderpx, borderpx + tbar_h, tw, th, win.cw, win.ch);

	/* Clear background */
	if (bgimg.loaded && bgimg.opacity > 0.0f) {
		bgimg_paint(0, 0, win.w, win.h);
		if (bgimg.opacity < 1.0f) {
			XRenderColor rc;
			rc.red   = dc.col[defaultbg].color.red;
			rc.green = dc.col[defaultbg].color.green;
			rc.blue  = dc.col[defaultbg].color.blue;
			rc.alpha = (unsigned short)(0xffff * (1.0f - bgimg.opacity));
			XRenderFillRectangle(xw.dpy, PictOpOver,
				XftDrawPicture(xw.draw), &rc, 0, 0, win.w, win.h);
		}
	} else {
		XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
		XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, win.w, win.h);
	}

	sp = 0;
	stack[sp++] = tab->root_pane;
	while (sp > 0) {
		p = stack[--sp];
		if (p->type == PANE_LEAF && p->terminal) {
			if (!p->terminal->term.line || p->terminal->term.row < 1 || p->terminal->term.col < 1)
				continue;

			pane_ox = p->x;
			pane_oy = p->y;
			g_term = p->terminal;
			tfulldirt();
			drawregion(0, 0, p->terminal->term.col, p->terminal->term.row);
			if (p == tab->active_pane) {
				cx = p->terminal->term.c.x;
				cy = p->terminal->term.c.y;
				LIMIT(cx, 0, p->terminal->term.col - 1);
				LIMIT(cy, 0, p->terminal->term.row - 1);
				if (p->terminal->term.line[cy])
					xdrawcursor(cx, cy, p->terminal->term.line[cy][cx],
					            cx, cy, p->terminal->term.line[cy][cx]);
			}
		} else {
			if (p->child1 && sp < MAX_PANE_DEPTH) stack[sp++] = p->child1;
			if (p->child2 && sp < MAX_PANE_DEPTH) stack[sp++] = p->child2;
		}
	}

	drawpaneborders(tab->root_pane);

	if (tab->root_pane->type != PANE_LEAF && tab->active_pane) {
		drawactivepaneborder(tab->active_pane);
	}

	/* Restore active pane's terminal */
	if (tab->active_pane && tab->active_pane->terminal)
		g_term = tab->active_pane->terminal;

	/* Reset offset to default (for single pane) */
	pane_ox = borderpx;
	pane_oy = borderpx + (g_tabs.count > 1 ? win.ch : 0);

	xfinishdraw();
}

void
xximspot(int x, int y)
{
	if (xw.ime.xic == NULL)
		return;

	xw.ime.spot.x = borderpx + x * win.cw;
	xw.ime.spot.y = borderpx + (y + 1) * win.ch;

	XSetICValues(xw.ime.xic, XNPreeditAttributes, xw.ime.spotlist, NULL);
}

void
expose(XEvent *ev)
{
	redraw();
}

void
visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	MODBIT(win.mode, e->state != VisibilityFullyObscured, MODE_VISIBLE);
}

void
unmap(XEvent *ev)
{
	win.mode &= ~MODE_VISIBLE;
}

void
xsetpointermotion(int set)
{
	MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void
xsetmode(int set, unsigned int flags)
{
	int mode = win.mode;

	MODBIT(win.mode, set, flags);
	if ((win.mode & MODE_REVERSE) != (mode & MODE_REVERSE))
		redraw();

	/*
	 * Store per-terminal window mode flags in winmode field
	 * (not term.mode) to avoid bit collision with MODE_ECHO etc.
	 *
	 * These are modes that applications enable/disable per-terminal
	 * via escape sequences and must follow the active pane, otherwise
	 * one pane's app state leaks into another pane (e.g. bracketed
	 * paste markers being injected into a shell that doesn't expect
	 * them).
	 */
	if (g_term) {
		unsigned int perterm = MODE_MOUSE | MODE_APPCURSOR
		                     | MODE_BRCKTPASTE | MODE_APPKEYPAD
		                     | MODE_MOUSESGR | MODE_8BIT
		                     | MODE_KBDLOCK | MODE_FOCUS
		                     | MODE_HIDE;
		if (flags & perterm)
			MODBIT(g_term->winmode, set, flags & perterm);
	}
}

void
xsyncmodeflags(void)
{
	unsigned int sync = MODE_MOUSE | MODE_APPCURSOR
	                  | MODE_BRCKTPASTE | MODE_APPKEYPAD
	                  | MODE_MOUSESGR | MODE_8BIT
	                  | MODE_KBDLOCK | MODE_FOCUS
	                  | MODE_HIDE;
	if (g_term) {
		win.mode &= ~sync;
		win.mode |= (g_term->winmode & sync);
	}
}

int
xsetcursor(int cursor)
{
	if (!BETWEEN(cursor, 0, 7)) /* 7: st extension */
		return 1;
	win.cursor = cursor;
	return 0;
}

void
xseturgency(int add)
{
	XWMHints *h = XGetWMHints(xw.dpy, xw.win);

	MODBIT(h->flags, add, XUrgencyHint);
	XSetWMHints(xw.dpy, xw.win, h);
	XFree(h);
}

void
xbell(void)
{
	if (!(IS_SET(MODE_FOCUSED)))
		xseturgency(1);
	if (bellvolume)
		XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
}

void
focus(XEvent *ev)
{
	long elapsed_ms;
	XFocusChangeEvent *e = &ev->xfocus;
	int suppress_focus_report = 0;
	struct timespec now;

	if (e->mode == NotifyGrab)
		return;

	/*
	 * If we recently opened a URL via Ctrl+Click, suppress focus
	 * in/out escape sequences to avoid corrupting PTY input.
	 * Clear suppression on FocusIn (returning from browser) or
	 * after 30 seconds timeout.
	 */
	if (url_open_suppress_focus) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (now.tv_sec - url_open_time.tv_sec) * 1000
		           + (now.tv_nsec - url_open_time.tv_nsec) / 1000000;
		if (elapsed_ms < 30000) {
			suppress_focus_report = 1;
		}
		/* Clear on FocusIn (returning from browser) or timeout */
		if (ev->type == FocusIn || elapsed_ms >= 30000) {
			url_open_suppress_focus = 0;
		}
	}

	if (ev->type == FocusIn) {
		if (xw.ime.xic)
			XSetICFocus(xw.ime.xic);
		win.mode |= MODE_FOCUSED;
		xseturgency(0);
		if (cursorblinktimeout) {
			cursorblink_on = 1;
			clock_gettime(CLOCK_MONOTONIC, &cursorblink_last);
		}
		if (IS_SET(MODE_FOCUS) && !suppress_focus_report)
			ttywrite("\033[I", 3, 0);
	} else {
		if (xw.ime.xic)
			XUnsetICFocus(xw.ime.xic);
		win.mode &= ~MODE_FOCUSED;
		if (IS_SET(MODE_FOCUS) && !suppress_focus_report)
			ttywrite("\033[O", 3, 0);
	}
}

int
match(uint mask, uint state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);

}

char*
kmap(KeySym k, uint state)
{
	Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for (i = 0; i < LEN(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == LEN(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;

		if (!match(kp->mask, state))
			continue;

		if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if (IS_SET(MODE_NUMLOCK) && kp->appkey == 2)
			continue;

		if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

void
kpress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym = NoSymbol;
	char buf[64], *customkey;
	int len;
	Rune c;
	Status status;
	Shortcut *bp;
	Tab *tab;

	if (IS_SET(MODE_KBDLOCK))
		return;

	if (cursorblinktimeout) {
		cursorblink_on = 1;
		clock_gettime(CLOCK_MONOTONIC, &cursorblink_last);
	}

	if (xw.ime.xic) {
		len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
		if (status == XBufferOverflow)
			return;
	} else {
		len = XLookupString(e, buf, sizeof buf, &ksym, NULL);
	}

	if (g_tabs.renaming) {
		if (ksym == XK_Return || ksym == XK_KP_Enter) {
			/* Confirm rename */
			if (g_tabs.renaming == RENAME_WINDOW) {
				if (g_tabs.rename_len > 0) {
					strncpy(g_tabs.window_custom_title, g_tabs.rename_buf,
					        sizeof(g_tabs.window_custom_title) - 1);
					g_tabs.window_custom_title[sizeof(g_tabs.window_custom_title) - 1] = '\0';
					g_tabs.window_title_manual = 1;
				} else {
					g_tabs.window_custom_title[0] = '\0';
					g_tabs.window_title_manual = 0;
				}
			} else if (g_tabs.renaming == RENAME_TAB) {
				tab = g_tabs.tabs[g_tabs.active];
				if (tab) {
					if (g_tabs.rename_len > 0) {
						strncpy(tab->custom_title, g_tabs.rename_buf,
						        sizeof(tab->custom_title) - 1);
						tab->custom_title[sizeof(tab->custom_title) - 1] = '\0';
						tab->title_manual = 1;
					} else {
						tab->custom_title[0] = '\0';
						tab->title_manual = 0;
					}
				}
			}
			g_tabs.renaming = RENAME_NONE;
			updatetitle();
			redraw();
			return;
		} else if (ksym == XK_Escape) {
			/* Cancel rename */
			g_tabs.renaming = RENAME_NONE;
			redraw();
			return;
		} else if (ksym == XK_BackSpace) {
			/* Delete character */
			if (g_tabs.rename_len > 0) {
				g_tabs.rename_buf[--g_tabs.rename_len] = '\0';
				redraw();
			}
			return;
		} else if (len > 0 && buf[0] >= 32 && buf[0] < 127) {
			/* Add printable character */
			if (g_tabs.rename_len < (int)sizeof(g_tabs.rename_buf) - 1) {
				g_tabs.rename_buf[g_tabs.rename_len++] = buf[0];
				g_tabs.rename_buf[g_tabs.rename_len] = '\0';
				redraw();
			}
			return;
		}
		return;  /* Consume all keys in rename mode */
	}

	/* 0. Lua key handler (can override C shortcuts) */
	if (lua_config_handle_key(e->state, ksym))
		return;

	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if ((customkey = kmap(ksym, e->state))) {
		ttywrite(customkey, strlen(customkey), 1);
		return;
	}

	/* 3. composed string from input method */
	if (len == 0)
		return;
	if (len == 1 && e->state & Mod1Mask) {
		if (IS_SET(MODE_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	ttywrite(buf, len, 1);
}

void
cmessage(XEvent *e)
{
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			win.mode |= MODE_FOCUSED;
			xseturgency(0);
		} else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			win.mode &= ~MODE_FOCUSED;
		}
	} else if (e->xclient.data.l[0] == xw.wmdeletewin) {
		ttyhangup();
		exit(0);
	}
}

void
resize(XEvent *e)
{
	if (e->xconfigure.width == win.w && e->xconfigure.height == win.h)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
}

void
run(void)
{
	XEvent ev;
	int w = win.w, h = win.h;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy), xev, drawing;
	int maxfd, i, ttyactivity;
	struct timespec seltv, *tv, now, lastblink, trigger;
	double timeout;
	Tab *tab;
	Pane *stack[MAX_PANE_DEPTH];
	int sp;
	Pane *p;
	int fd;
	double elapsed;
	int cursor_timeout;
	Terminal *t;
	int needs_relayout, changed;
	Pane *pstack[MAX_PANE_DEPTH], *parent_stack[MAX_PANE_DEPTH];
	int psp;
	Pane *parent, *sibling, *leaf;
	int tw, th;

	/* Waiting for window mapping */
	do {
		XNextEvent(xw.dpy, &ev);
		/*
		 * This XFilterEvent call is required because of XOpenIM. It
		 * does filter out the key event and some client message for
		 * the input method too.
		 */
		if (XFilterEvent(&ev, None))
			continue;
		if (ev.type == ConfigureNotify) {
			w = ev.xconfigure.width;
			h = ev.xconfigure.height;
		}
	} while (ev.type != MapNotify);

	ttynew(opt_line, shell, opt_io, opt_cmd, NULL);
	cresize(w, h);

	clock_gettime(CLOCK_MONOTONIC, &cursorblink_last);  /* Initialize cursor blink timer */
	for (timeout = -1, drawing = 0, lastblink = (struct timespec){0};;) {
		FD_ZERO(&rfd);
		FD_SET(xfd, &rfd);
		maxfd = xfd;

		/* Add all terminal fds from all panes in all tabs */
		for (i = 0; i < g_tabs.count; i++) {
			tab = g_tabs.tabs[i];
			if (tab && tab->root_pane) {
				sp = 0;
				stack[sp++] = tab->root_pane;
				while (sp > 0) {
					p = stack[--sp];
					if (p->type == PANE_LEAF) {
						/* Read fd once to avoid race with SIGCHLD */
						fd = p->terminal ? p->terminal->cmdfd : -1;
						if (fd >= 0) {
							FD_SET(fd, &rfd);
							if (fd > maxfd)
								maxfd = fd;
						}
					} else {
						if (p->child1 && sp < MAX_PANE_DEPTH) stack[sp++] = p->child1;
						if (p->child2 && sp < MAX_PANE_DEPTH) stack[sp++] = p->child2;
					}
				}
			}
		}

		if (XPending(xw.dpy))
			timeout = 0;  /* existing events might not set xfd */

		/* Ensure cursor blink timeout is always respected */
		if (cursorblinktimeout) {
			elapsed = TIMEDIFF(now, cursorblink_last);
			cursor_timeout = cursorblinktimeout - (int)elapsed;
			if (cursor_timeout < 0)
				cursor_timeout = 0;
			if (timeout < 0 || cursor_timeout < timeout)
				timeout = cursor_timeout;
		}

		seltv.tv_sec = timeout / 1E3;
		seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
		tv = timeout >= 0 ? &seltv : NULL;

		if (pselect(maxfd + 1, &rfd, NULL, NULL, tv, NULL) < 0) {
			if (errno == EINTR)
				continue;  /* Just interrupted, retry */
			if (errno == EBADF) {
				/* An fd was closed (SIGCHLD) - skip to cleanup */
				clock_gettime(CLOCK_MONOTONIC, &now);
				ttyactivity = 1;  /* Force redraw after cleanup */
				goto cleanup_dead_panes;
			}
			die("select failed: %s\n", strerror(errno));
		}
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* Read from all ready terminals in all panes */
		ttyactivity = 0;
		for (i = 0; i < g_tabs.count; i++) {
			tab = g_tabs.tabs[i];
			if (tab && tab->root_pane) {
				sp = 0;
				stack[sp++] = tab->root_pane;
				while (sp > 0) {
					p = stack[--sp];
					if (p->type == PANE_LEAF) {
						/* Read fd once to avoid race with SIGCHLD */
						fd = p->terminal ? p->terminal->cmdfd : -1;
						if (fd >= 0 && FD_ISSET(fd, &rfd)) {
							terminal_read(p->terminal);
							ttyactivity = 1;
						}
					} else {
						if (p->child1 && sp < MAX_PANE_DEPTH) stack[sp++] = p->child1;
						if (p->child2 && sp < MAX_PANE_DEPTH) stack[sp++] = p->child2;
					}
				}
			}
		}

cleanup_dead_panes:
		/* Clean up dead panes and tabs */
		for (i = g_tabs.count - 1; i >= 0; i--) {
			tab = g_tabs.tabs[i];
			if (!tab || !tab->root_pane)
				continue;

			/* First, handle single pane tabs */
			if (tab->root_pane->type == PANE_LEAF) {
				t = tab->root_pane->terminal;
				if (!t || (t->cmdfd < 0 && t->pid <= 0)) {
					tab_close(i);
					ttyactivity = 1;
				}
				continue;
			}

			/* For split panes, find and close dead leaf panes recursively */
			needs_relayout = 0;
			do {
				changed = 0;
				psp = 0;
				pstack[psp] = tab->root_pane;
				parent_stack[psp] = NULL;
				psp++;

				while (psp > 0 && !changed) {
					psp--;
					p = pstack[psp];
					parent = parent_stack[psp];

					if (p->type == PANE_LEAF) {
						t = p->terminal;
						if (!t || (t->cmdfd < 0 && t->pid <= 0)) {
							if (!parent) {
								/* Root is dead leaf - should have been caught above */
								continue;
							}
							sibling = (parent->child1 == p) ? parent->child2 : parent->child1;
							pane_free(p);

							/* Replace parent with sibling */
							if (sibling->type == PANE_LEAF) {
								parent->type = PANE_LEAF;
								parent->terminal = sibling->terminal;
								parent->child1 = NULL;
								parent->child2 = NULL;
								sibling->terminal = NULL;
								free(sibling);
								/* Focus the parent (now holding sibling's terminal) */
								tab->active_pane = parent;
								if (parent->terminal)
									g_term = parent->terminal;
							} else {
								parent->type = sibling->type;
								parent->child1 = sibling->child1;
								parent->child2 = sibling->child2;
								parent->split_ratio = sibling->split_ratio;
								parent->terminal = NULL;
								free(sibling);
								/* Focus first leaf in the restructured subtree */
								leaf = parent;
								while (leaf && leaf->type != PANE_LEAF)
									leaf = leaf->child1;
								tab->active_pane = leaf;
								if (leaf && leaf->terminal)
									g_term = leaf->terminal;
							}
							changed = 1;
							needs_relayout = 1;
							ttyactivity = 1;
						}
					} else {
						if (p->child2 && psp < MAX_PANE_DEPTH) {
							pstack[psp] = p->child2;
							parent_stack[psp] = p;
							psp++;
						}
						if (p->child1 && psp < MAX_PANE_DEPTH) {
							pstack[psp] = p->child1;
							parent_stack[psp] = p;
							psp++;
						}
					}
				}
			} while (changed);  /* Keep looping until no more dead panes */

			if (needs_relayout)
				xsyncmodeflags();

			if (needs_relayout) {
				tw = win.w - 2 * borderpx;
				th = win.h - 2 * borderpx;
				if (g_tabs.count > 1)
					th -= win.ch;
				pane_layout(tab->root_pane, borderpx, borderpx + (g_tabs.count > 1 ? win.ch : 0),
				            tw, th, win.cw, win.ch);
			}
		}

		xev = 0;
		while (XPending(xw.dpy)) {
			xev = 1;
			XNextEvent(xw.dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			if (handler[ev.type])
				(handler[ev.type])(&ev);
		}

		if (g_tabs.count == 0)
			break;

		/*
		 * Keep scroll-selecting while Button1 is held outside the
		 * active pane, even when the pointer is not moving and so
		 * produces no motion events.
		 */
		if (autoscroll.dir &&
		    TIMEDIFF(now, autoscroll.last) >= autoscrolldelay) {
			autoscroll.last = now;
			if (selautoscrollstep())
				ttyactivity = 1;
		}

		/*
		 * Handle cursor blinking BEFORE activity check to ensure
		 * cursor keeps blinking even during continuous output.
		 */
		if (cursorblinktimeout) {
			elapsed = TIMEDIFF(now, cursorblink_last);
			if (elapsed >= cursorblinktimeout) {
				cursorblink_on = !cursorblink_on;
				cursorblink_last = now;
				/* Force cursor cell to be redrawn */
				if (g_term)
					g_term->term.dirty[g_term->term.c.y] = 1;
			}
		}

		/*
		 * To reduce flicker and tearing, when new content or event
		 * triggers drawing, we first wait a bit to ensure we got
		 * everything, and if nothing new arrives - we draw.
		 * We start with trying to wait minlatency ms. If more content
		 * arrives sooner, we retry with shorter and shorter periods,
		 * and eventually draw even without idle after maxlatency ms.
		 * Typically this results in low latency while interacting,
		 * maximum latency intervals during `cat huge.txt`, and perfect
		 * sync with periodic updates from animations/key-repeats/etc.
		 */
		if (ttyactivity || xev) {
			if (!drawing) {
				trigger = now;
				drawing = 1;
			}
			timeout = (maxlatency - TIMEDIFF(now, trigger)) \
			          / maxlatency * minlatency;
			if (timeout > 0)
				continue;  /* we have time, try to find idle */
		}

		/* idle detected or maxlatency exhausted -> draw */
		timeout = -1;
		if (blinktimeout && tattrset(ATTR_BLINK)) {
			timeout = blinktimeout - TIMEDIFF(now, lastblink);
			if (timeout <= 0) {
				if (-timeout > blinktimeout) /* start visible */
					win.mode |= MODE_BLINK;
				win.mode ^= MODE_BLINK;
				tsetdirtattr(ATTR_BLINK);
				lastblink = now;
				timeout = blinktimeout;
			}
		}

		/* Cursor blink timeout for next iteration */
		if (cursorblinktimeout) {
			elapsed = TIMEDIFF(now, cursorblink_last);
			cursor_timeout = cursorblinktimeout - (int)elapsed;
			if (cursor_timeout > 0 && (timeout < 0 || cursor_timeout < timeout))
				timeout = cursor_timeout;
		}

		/* Wake up in time for the next scroll-select step */
		if (autoscroll.dir) {
			double scroll_timeout = autoscrolldelay -
			                        TIMEDIFF(now, autoscroll.last);
			if (scroll_timeout < 0)
				scroll_timeout = 0;
			if (timeout < 0 || scroll_timeout < timeout)
				timeout = scroll_timeout;
		}

		drawallpanes();
		XFlush(xw.dpy);
		drawing = 0;
	}
}

void
usage(void)
{
	die("usage: %s [-aiv] [-c class] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid]"
	    " [[-e] command [args ...]]\n"
	    "       %s [-aiv] [-c class] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid] -l line"
	    " [stty_args ...]\n", argv0, argv0);
}

int
main(int argc, char *argv[])
{
	Tab *first_tab;

	xw.l = xw.t = 0;
	xw.isfixed = False;
	xsetcursor(cursorshape);

	ARGBEGIN {
	case 'a':
		allowaltscreen = 0;
		break;
	case 'c':
		opt_class = EARGF(usage());
		break;
	case 'e':
		if (argc > 0)
			--argc, ++argv;
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		xw.gm = XParseGeometry(EARGF(usage()),
				&xw.l, &xw.t, &cols, &rows);
		break;
	case 'i':
		xw.isfixed = 1;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 'l':
		opt_line = EARGF(usage());
		break;
	case 'n':
		opt_name = EARGF(usage());
		break;
	case 't':
	case 'T':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
		die("%s " VERSION "\n", argv0);
		break;
	default:
		usage();
	} ARGEND;

run:
	if (argc > 0) /* eat all remaining arguments */
		opt_cmd = argv;

	if (!opt_title)
		opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];

	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	cols = MAX(cols, 1);
	rows = MAX(rows, 1);

	first_tab = tab_new();
	if (!first_tab)
		die("couldn't create first tab\n");

	/* Create root pane (this also creates the terminal and calls tnew) */
	first_tab->root_pane = pane_new(cols, rows);
	if (!first_tab->root_pane)
		die("couldn't create root pane\n");

	first_tab->active_pane = first_tab->root_pane;

	g_term = first_tab->root_pane->terminal;
	g_tabs.active = 0;

	xinit(cols, rows);
	xsetenv();
	selinit();

	/* Initialize Lua configuration AFTER X11 is ready */
	lua_config_init();

	run();

	return 0;
}
