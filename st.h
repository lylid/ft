/* See LICENSE for license details. */

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* macros */
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)		(((n) + ((d) - 1)) / (d))
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || \
				(a).bg != (b).bg)
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))

enum glyph_attribute {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

enum selection_mode {
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

#define Glyph Glyph_
typedef struct {
	Rune u;           /* character code */
	ushort mode;      /* attribute flags */
	uint32_t fg;      /* foreground  */
	uint32_t bg;      /* background  */
} Glyph;

typedef Glyph *Line;

typedef union {
	int i;
	uint ui;
	float f;
	const void *v;
	const char *s;
} Arg;

/* Buffer sizes for escape sequences */
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ

/* Cursor state */
typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

/* Selection state */
typedef struct {
	int mode;
	int type;
	int snap;
	struct {
		int x, y;
	} nb, ne, ob, oe;
	int alt;
} Selection;

/* Scrollback history size */
#define HISTSIZE 10000

/* Screen state */
typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	Line *line;   /* screen */
	Line *alt;    /* alternate screen */
	Line *hist;   /* scrollback history buffer */
	int histi;    /* current position in circular history buffer */
	int histn;    /* number of lines in history */
	int scr;      /* scroll position (0 = bottom, positive = scrolled up) */
	int *dirty;   /* dirtyness of lines */
	TCursor c;    /* cursor */
	int ocx;      /* old cursor col */
	int ocy;      /* old cursor row */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int *tabs;
	Rune lastc;   /* last printed char outside of sequence, 0 if control */
} Term;

/* CSI Escape sequence state */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	size_t len;            /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode[2];
} CSIEscape;

/* STR Escape sequence state */
typedef struct {
	char type;             /* ESC type ... */
	char *buf;             /* allocated raw string */
	size_t siz;            /* allocation size */
	size_t len;            /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;              /* nb of args */
} STREscape;

/* Combined terminal instance - all state for one terminal */
typedef struct Terminal {
	Term term;
	Selection sel;
	CSIEscape csiesc;
	STREscape stresc;
	int cmdfd;         /* PTY master fd */
	pid_t pid;         /* child process */
	char title[256];   /* window/tab title */
	/* Per-terminal read buffer (was static in ttyread) */
	char readbuf[BUFSIZ];
	int readbuflen;
	/* Per-terminal window mode flags (mouse, appcursor) - avoids bit collision with term.mode */
	unsigned int winmode;
	/* Logging */
	FILE *logfile;     /* log file handle, NULL if disabled */
	char logpath[512]; /* path to log file */
} Terminal;

/* Terminal instance management */
Terminal *terminal_new(int cols, int rows);
void terminal_free(Terminal *t);
size_t terminal_read(Terminal *t);
void terminal_write(Terminal *t, const char *s, size_t n, int may_echo);
void terminal_resize(Terminal *t, int cols, int rows);
void terminal_hangup(Terminal *t);
const char *terminal_get_foreground_process(Terminal *t);
char *terminal_getcwd(Terminal *t);    /* Get working directory of terminal's shell */
int terminal_toggle_log(Terminal *t);  /* Toggle logging, returns new state */

/* Current active terminal (for compatibility during refactor) */
extern Terminal *g_term;

/* Logging configuration */
typedef struct {
	int enabled;       /* 0 = off, 1 = on */
	char path[512];    /* log directory path */
} LogConfig;

extern LogConfig g_log_config;

/* Pane management for splits */
typedef enum {
	PANE_LEAF,    /* Terminal pane (no children) */
	PANE_HSPLIT,  /* Horizontal split (top/bottom) */
	PANE_VSPLIT   /* Vertical split (left/right) */
} PaneType;

typedef struct Pane {
	PaneType type;
	/* For PANE_LEAF: the terminal */
	Terminal *terminal;
	/* For split panes: children */
	struct Pane *child1;  /* top or left */
	struct Pane *child2;  /* bottom or right */
	float split_ratio;    /* 0.0-1.0, default 0.5 */
	/* Geometry (set during layout) */
	int x, y, w, h;       /* position and size in pixels */
	int col, row;         /* size in characters */
} Pane;

/* Tab management */
#define MAX_TABS 32

typedef struct Tab {
	int id;
	char title[256];
	char custom_title[256];  /* user-set tab label (Ctrl+Shift+E), empty = auto */
	int title_manual;        /* 1 = use custom_title in tab bar */
	Pane *root_pane;         /* root of pane tree */
	Pane *active_pane;       /* currently focused pane */
} Tab;

/* g_tabs.renaming values */
#define RENAME_NONE   0
#define RENAME_WINDOW 1  /* Ctrl+Shift+L: rename X11 window */
#define RENAME_TAB    2  /* Ctrl+Shift+E: rename active tab */

typedef struct TabState {
	Tab *tabs[MAX_TABS];
	int count;
	int active;           /* index of active tab */
	int next_id;          /* for unique tab IDs */
	int renaming;         /* RENAME_NONE / RENAME_WINDOW / RENAME_TAB */
	char rename_buf[256]; /* input buffer for rename */
	int rename_len;       /* current length of input */
	char window_custom_title[256]; /* user-set X11 window title (Ctrl+Shift+L) */
	int window_title_manual;       /* 1 = window_custom_title overrides updatetitle */
} TabState;

extern TabState g_tabs;

/* Tab functions */
Tab *tab_new(void);
void tab_free(Tab *t);
void tab_switch(int idx);
void tab_next(void);
void tab_prev(void);
void tab_close(int idx);
int tab_count(void);

/* Pane functions */
Pane *pane_new(int cols, int rows);
void pane_free(Pane *p);
Pane *pane_split(Pane *p, PaneType split_type, int cols, int rows);
void pane_close(Pane *p);
void pane_layout(Pane *p, int x, int y, int w, int h, int cw, int ch);
Pane *pane_find_neighbor(Pane *root, Pane *current, int direction);
Pane *pane_at_coords(Pane *p, int px, int py);
void pane_focus(Pane *p);
void pane_foreach_terminal(Pane *p, void (*fn)(Terminal *, void *), void *arg);

/* Direction constants for pane navigation */
#define DIR_LEFT  0
#define DIR_DOWN  1
#define DIR_UP    2
#define DIR_RIGHT 3

void die(const char *, ...);
void redraw(void);
void draw(void);
void drawallpanes(void);

void printscreen(const Arg *);
void printsel(const Arg *);
void sendbreak(const Arg *);
void toggleprinter(const Arg *);

int tattrset(int);
void tfulldirt(void);
void tnew(int, int);
void tresize(int, int);
void tsetdirtattr(int);
void drawregion(int, int, int, int);
void ttyhangup(void);
int ttynew(const char *, char *, const char *, char **, const char *);
size_t ttyread(void);
void ttyresize(int, int);
void ttywrite(const char *, size_t, int);

void resettitle(void);

/* Scrollback functions */
void kscrolldown(const Arg *);
void kscrollup(const Arg *);
int selscrollup(int n);   /* Scroll up during selection, returns lines scrolled */
int selscrolldown(int n); /* Scroll down during selection, returns lines scrolled */
int tisaltscr(void);

void selclear(void);
void selinit(void);
void selstart(int, int, int);
void selextend(int, int, int, int);
int selected(int, int);
char *getsel(void);

size_t utf8encode(Rune, char *);

/* URL/Path detection for Ctrl+Click */
int url_detect_at(int x, int y, int *start_x, int *end_x);
int url_extract(int x, int y, char *buf, size_t bufsz);

void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *xstrdup(const char *);

/* config.h globals */
extern char *utmp;
extern char *scroll;
extern char *stty_args;
extern char *vtiden;
extern wchar_t *worddelimiters;
extern int allowaltscreen;
extern int allowwindowops;
extern char *termname;
extern unsigned int tabspaces;
extern unsigned int defaultfg;
extern unsigned int defaultbg;
extern unsigned int defaultcs;
