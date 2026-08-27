/* lua_config.c - Lua configuration support for st */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* for strcasecmp */
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>

#include "lua_config.h"
#include "st.h"

/* External functions from x.c */
extern void xunloadfonts(void);
extern void xloadfonts(const char *, double);
extern void cresize(int, int);
extern void redraw(void);
extern void xchangeicon(uint32_t emoji);
extern int xsetcolorname(int x, const char *name);
extern void bgimg_set(const char *path, float opacity);
extern const char *bgimg_get_path(void);
extern float bgimg_get_opacity(void);
extern void bgimg_set_opacity(float opacity);

/* Track whether X11 is initialized (colors can be set) */
static int x_initialized = 0;

/* Deferred color storage for colors set before X11 init */
#define MAX_DEFERRED_COLORS 260
static struct {
	int used;
	char color[32];
} deferred_colors[MAX_DEFERRED_COLORS];

/* Called from x.c after xinit/xloadcols */
void lua_config_apply_deferred_colors(void)
{
	x_initialized = 1;
	for (int i = 0; i < MAX_DEFERRED_COLORS; i++) {
		if (deferred_colors[i].used) {
			xsetcolorname(i, deferred_colors[i].color);
			deferred_colors[i].used = 0;
		}
	}
}

/* External variables */
extern int win_w, win_h;  /* Window dimensions - we'll get these another way */

static lua_State *L = NULL;
static char config_path[512] = {0};

/*
 * File handler configuration
 */
#define MAX_FILE_HANDLERS 64

typedef struct {
	char extension[32];    /* File extension (e.g., "pdf", "txt") */
	FileHandler handler;
} FileHandlerEntry;

static FileHandlerEntry file_handlers[MAX_FILE_HANDLERS];
static int file_handler_count = 0;
static FileHandler default_file_handler = {
	.type = FILE_HANDLER_TERMINAL,
	.command = "nvim"
};

/* st.set_font(font_string) - Set the terminal font */
static int l_set_font(lua_State *L)
{
	const char *fontstr = luaL_checkstring(L, 1);

	/* Unload current fonts */
	xunloadfonts();

	/* Load new font (size 0 means use size from font string) */
	xloadfonts(fontstr, 0);

	/* Trigger resize to recalculate dimensions */
	cresize(0, 0);

	/* Redraw */
	redraw();

	return 0;
}

/* st.set_font_size(size) - Set font size */
static int l_set_font_size(lua_State *L)
{
	double size = luaL_checknumber(L, 1);

	if (size < 1) {
		return luaL_error(L, "font size must be >= 1");
	}

	/* Get current font string - we need to expose usedfont */
	/* For now, this is a simplified version */
	extern char *usedfont;

	if (!usedfont) {
		return luaL_error(L, "no font loaded");
	}

	xunloadfonts();
	xloadfonts(usedfont, size);
	cresize(0, 0);
	redraw();

	return 0;
}

/* st.reload_config() - Reload configuration file */
static int l_reload_config(lua_State *L)
{
	(void)L;
	lua_config_reload();
	return 0;
}

/* st.get_config_path() - Get the config file path */
static int l_get_config_path(lua_State *L)
{
	lua_pushstring(L, config_path);
	return 1;
}

/* st.get_foreground_process() - Get foreground process of active terminal */
static int l_get_foreground_process(lua_State *L)
{
	if (g_term) {
		const char *proc = terminal_get_foreground_process(g_term);
		lua_pushstring(L, proc);
	} else {
		lua_pushstring(L, "terminal");
	}
	return 1;
}

/* st.get_tab_count() - Get number of tabs */
static int l_get_tab_count(lua_State *L)
{
	lua_pushinteger(L, g_tabs.count);
	return 1;
}

/* st.get_active_tab() - Get active tab index (1-based for Lua) */
static int l_get_active_tab(lua_State *L)
{
	lua_pushinteger(L, g_tabs.active + 1);
	return 1;
}

/* st.get_tab_info(index) - Get tab info as table */
static int l_get_tab_info(lua_State *L)
{
	int idx = luaL_checkinteger(L, 1) - 1;  /* Convert to 0-based */

	if (idx < 0 || idx >= g_tabs.count || !g_tabs.tabs[idx]) {
		lua_pushnil(L);
		return 1;
	}

	Tab *tab = g_tabs.tabs[idx];
	Pane *active_pane = tab->active_pane;
	Terminal *term = active_pane ? active_pane->terminal : NULL;

	lua_newtable(L);

	/* Tab index (1-based) */
	lua_pushinteger(L, idx + 1);
	lua_setfield(L, -2, "index");

	/* Tab title */
	lua_pushstring(L, tab->title[0] ? tab->title : "terminal");
	lua_setfield(L, -2, "title");

	/* Is active */
	lua_pushboolean(L, idx == g_tabs.active);
	lua_setfield(L, -2, "is_active");

	/* Foreground process */
	if (term) {
		const char *proc = terminal_get_foreground_process(term);
		lua_pushstring(L, proc);
	} else {
		lua_pushstring(L, "terminal");
	}
	lua_setfield(L, -2, "process");

	/* Terminal title (from escape sequences) */
	if (term && term->title[0]) {
		lua_pushstring(L, term->title);
	} else {
		lua_pushstring(L, "");
	}
	lua_setfield(L, -2, "term_title");

	return 1;
}

/* st.set_tab_title(index, title) - Set custom tab title */
static int l_set_tab_title(lua_State *L)
{
	int idx = luaL_checkinteger(L, 1) - 1;  /* Convert to 0-based */
	const char *title = luaL_checkstring(L, 2);

	if (idx < 0 || idx >= g_tabs.count || !g_tabs.tabs[idx]) {
		return luaL_error(L, "invalid tab index");
	}

	Tab *tab = g_tabs.tabs[idx];
	strncpy(tab->title, title, sizeof(tab->title) - 1);
	tab->title[sizeof(tab->title) - 1] = '\0';

	redraw();
	return 0;
}

/* st.set_icon(emoji_string) - Set window icon to an emoji */
static int l_set_icon(lua_State *L)
{
	const char *str = luaL_checkstring(L, 1);
	uint32_t codepoint = 0;

	/* Decode UTF-8 to get codepoint */
	unsigned char c = str[0];
	if ((c & 0x80) == 0) {
		/* ASCII */
		codepoint = c;
	} else if ((c & 0xE0) == 0xC0) {
		/* 2-byte UTF-8 */
		codepoint = (c & 0x1F) << 6;
		codepoint |= (str[1] & 0x3F);
	} else if ((c & 0xF0) == 0xE0) {
		/* 3-byte UTF-8 */
		codepoint = (c & 0x0F) << 12;
		codepoint |= (str[1] & 0x3F) << 6;
		codepoint |= (str[2] & 0x3F);
	} else if ((c & 0xF8) == 0xF0) {
		/* 4-byte UTF-8 (most emoji) */
		codepoint = (c & 0x07) << 18;
		codepoint |= (str[1] & 0x3F) << 12;
		codepoint |= (str[2] & 0x3F) << 6;
		codepoint |= (str[3] & 0x3F);
	}

	if (codepoint == 0) {
		return luaL_error(L, "invalid emoji string");
	}

	xchangeicon(codepoint);
	return 0;
}

/* st.set_logging(enabled, [path]) - Enable/disable logging */
static int l_set_logging(lua_State *L)
{
	int enabled = lua_toboolean(L, 1);
	const char *path = luaL_optstring(L, 2, NULL);

	g_log_config.enabled = enabled;
	if (path && path[0]) {
		strncpy(g_log_config.path, path, sizeof(g_log_config.path) - 1);
		g_log_config.path[sizeof(g_log_config.path) - 1] = '\0';
	} else {
		g_log_config.path[0] = '\0';  /* Use default */
	}

	fprintf(stderr, "lua_config: logging %s%s%s\n",
	        enabled ? "enabled" : "disabled",
	        (enabled && path) ? " -> " : "",
	        (enabled && path) ? path : "");

	return 0;
}

/* st.get_logging() - Get logging config as table */
static int l_get_logging(lua_State *L)
{
	lua_newtable(L);

	lua_pushboolean(L, g_log_config.enabled);
	lua_setfield(L, -2, "enabled");

	if (g_log_config.path[0]) {
		lua_pushstring(L, g_log_config.path);
	} else {
		/* Return default path */
		const char *home = getenv("HOME");
		if (!home) {
			struct passwd *pw = getpwuid(getuid());
			home = pw ? pw->pw_dir : "/tmp";
		}
		char defpath[512];
		snprintf(defpath, sizeof(defpath), "%s/.ft/logs", home);
		lua_pushstring(L, defpath);
	}
	lua_setfield(L, -2, "path");

	return 1;
}

/* Cursor blink timeout - defined in x.c */
extern unsigned int cursorblinktimeout;

/* Active pane border - defined in x.c */
extern unsigned int activepaneborder;
extern unsigned int activepanebordercolor;

/* st.set_pane_border(pixels) - Set active pane border width (0 = no border) */
static int l_set_pane_border(lua_State *L)
{
	int px = luaL_checkinteger(L, 1);
	if (px < 0) px = 0;
	if (px > 10) px = 10;  /* reasonable max */
	activepaneborder = (unsigned int)px;
	return 0;
}

/* st.get_pane_border() - Get active pane border width in pixels */
static int l_get_pane_border(lua_State *L)
{
	lua_pushinteger(L, activepaneborder);
	return 1;
}

/* st.set_pane_border_color(index) - Set active pane border color (0-15 standard, 256+ custom) */
static int l_set_pane_border_color(lua_State *L)
{
	int idx = luaL_checkinteger(L, 1);
	if (idx < 0) idx = 0;
	if (idx > 259) idx = 259;  /* max color index */
	activepanebordercolor = (unsigned int)idx;
	return 0;
}

/* st.get_pane_border_color() - Get active pane border color index */
static int l_get_pane_border_color(lua_State *L)
{
	lua_pushinteger(L, activepanebordercolor);
	return 1;
}

/* st.set_cursor_blink(ms) - Set cursor blink timeout in ms (0 = no blink) */
static int l_set_cursor_blink(lua_State *L)
{
	int ms = luaL_checkinteger(L, 1);
	if (ms < 0) ms = 0;
	cursorblinktimeout = (unsigned int)ms;
	fprintf(stderr, "lua_config: cursor blink %s\n",
	        ms ? "enabled" : "disabled");
	return 0;
}

/* st.get_cursor_blink() - Get cursor blink timeout in ms */
static int l_get_cursor_blink(lua_State *L)
{
	lua_pushinteger(L, cursorblinktimeout);
	return 1;
}

/* st.zoom(delta) - Zoom font in/out */
static int l_zoom(lua_State *L)
{
	extern void zoom(const Arg *);
	float delta = luaL_checknumber(L, 1);
	Arg arg = {.f = delta};
	zoom(&arg);
	return 0;
}

/* st.zoomreset() - Reset zoom to default */
static int l_zoomreset(lua_State *L)
{
	(void)L;
	extern void zoomreset(const Arg *);
	Arg arg = {.f = 0};
	zoomreset(&arg);
	return 0;
}

/* Helper to set color (deferred if X not ready) */
static int set_color_internal(int idx, const char *name)
{
	if (idx < 0 || idx >= MAX_DEFERRED_COLORS)
		return 1;

	if (x_initialized) {
		return xsetcolorname(idx, name);
	} else {
		/* Store for later */
		strncpy(deferred_colors[idx].color, name,
		        sizeof(deferred_colors[idx].color) - 1);
		deferred_colors[idx].color[sizeof(deferred_colors[idx].color) - 1] = '\0';
		deferred_colors[idx].used = 1;
		return 0;
	}
}

/* st.set_color(index, colorname) - Set a color by index
 * Indices 0-7: normal colors, 8-15: bright colors
 * 256: cursor bg, 257: reverse cursor, 258: foreground, 259: background
 */
static int l_set_color(lua_State *L)
{
	int idx = luaL_checkinteger(L, 1);
	const char *name = luaL_checkstring(L, 2);

	if (set_color_internal(idx, name) != 0) {
		return luaL_error(L, "failed to set color %d to '%s'", idx, name);
	}
	if (x_initialized)
		redraw();
	return 0;
}

/* st.set_colors(table) - Set multiple colors at once
 * Table format: { [0] = "#000000", [1] = "#ff0000", ... }
 * Or named: { foreground = "#c0caf5", background = "#000000", cursor = "#c0caf5" }
 */
static int l_set_colors(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	/* Iterate over table */
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		int idx = -1;

		if (lua_isinteger(L, -2)) {
			idx = lua_tointeger(L, -2);
		} else if (lua_isstring(L, -2)) {
			/* Handle named colors */
			const char *key = lua_tostring(L, -2);
			if (strcmp(key, "foreground") == 0 || strcmp(key, "fg") == 0)
				idx = 258;
			else if (strcmp(key, "background") == 0 || strcmp(key, "bg") == 0)
				idx = 259;
			else if (strcmp(key, "cursor") == 0 || strcmp(key, "cursor_bg") == 0)
				idx = 256;
			else if (strcmp(key, "cursor_fg") == 0 || strcmp(key, "reverse_cursor") == 0)
				idx = 257;
			/* Standard color names */
			else if (strcmp(key, "black") == 0) idx = 0;
			else if (strcmp(key, "red") == 0) idx = 1;
			else if (strcmp(key, "green") == 0) idx = 2;
			else if (strcmp(key, "yellow") == 0) idx = 3;
			else if (strcmp(key, "blue") == 0) idx = 4;
			else if (strcmp(key, "magenta") == 0) idx = 5;
			else if (strcmp(key, "cyan") == 0) idx = 6;
			else if (strcmp(key, "white") == 0) idx = 7;
			/* Bright color names */
			else if (strcmp(key, "bright_black") == 0) idx = 8;
			else if (strcmp(key, "bright_red") == 0) idx = 9;
			else if (strcmp(key, "bright_green") == 0) idx = 10;
			else if (strcmp(key, "bright_yellow") == 0) idx = 11;
			else if (strcmp(key, "bright_blue") == 0) idx = 12;
			else if (strcmp(key, "bright_magenta") == 0) idx = 13;
			else if (strcmp(key, "bright_cyan") == 0) idx = 14;
			else if (strcmp(key, "bright_white") == 0) idx = 15;
		}

		if (idx >= 0 && lua_isstring(L, -1)) {
			const char *color = lua_tostring(L, -1);
			set_color_internal(idx, color);
		}
		lua_pop(L, 1);  /* pop value, keep key for next iteration */
	}

	if (x_initialized)
		redraw();
	return 0;
}

/*
 * st.set_file_handler(extension, handler_type, [command])
 *
 * extension: file extension without dot (e.g., "pdf", "txt", "*" for default)
 * handler_type: "xdg", "terminal", or "app"
 * command: for "terminal" or "app", the command to run (use %s for file path)
 *
 * Examples:
 *   st.set_file_handler("pdf", "app", "atril %s")
 *   st.set_file_handler("*", "terminal", "nvim %s")
 *   st.set_file_handler("html", "xdg")
 */
static int l_set_file_handler(lua_State *L)
{
	const char *ext = luaL_checkstring(L, 1);
	const char *type_str = luaL_checkstring(L, 2);
	const char *cmd = luaL_optstring(L, 3, NULL);

	FileHandlerType type;
	if (strcmp(type_str, "xdg") == 0) {
		type = FILE_HANDLER_XDG;
	} else if (strcmp(type_str, "terminal") == 0) {
		type = FILE_HANDLER_TERMINAL;
		if (!cmd) cmd = "nvim";
	} else if (strcmp(type_str, "app") == 0) {
		type = FILE_HANDLER_APP;
		if (!cmd) {
			return luaL_error(L, "app handler requires a command");
		}
	} else {
		return luaL_error(L, "invalid handler type: %s (use 'xdg', 'terminal', or 'app')", type_str);
	}

	/* Handle default handler */
	if (strcmp(ext, "*") == 0) {
		default_file_handler.type = type;
		if (cmd) {
			strncpy(default_file_handler.command, cmd,
			        sizeof(default_file_handler.command) - 1);
			default_file_handler.command[sizeof(default_file_handler.command) - 1] = '\0';
		}
		fprintf(stderr, "lua_config: set default file handler: %s %s\n",
		        type_str, cmd ? cmd : "");
		return 0;
	}

	/* Check if we already have a handler for this extension */
	for (int i = 0; i < file_handler_count; i++) {
		if (strcasecmp(file_handlers[i].extension, ext) == 0) {
			file_handlers[i].handler.type = type;
			if (cmd) {
				strncpy(file_handlers[i].handler.command, cmd,
				        sizeof(file_handlers[i].handler.command) - 1);
				file_handlers[i].handler.command[sizeof(file_handlers[i].handler.command) - 1] = '\0';
			}
			fprintf(stderr, "lua_config: updated file handler for .%s: %s %s\n",
			        ext, type_str, cmd ? cmd : "");
			return 0;
		}
	}

	/* Add new handler */
	if (file_handler_count >= MAX_FILE_HANDLERS) {
		return luaL_error(L, "too many file handlers (max %d)", MAX_FILE_HANDLERS);
	}

	FileHandlerEntry *entry = &file_handlers[file_handler_count++];
	strncpy(entry->extension, ext, sizeof(entry->extension) - 1);
	entry->extension[sizeof(entry->extension) - 1] = '\0';
	entry->handler.type = type;
	if (cmd) {
		strncpy(entry->handler.command, cmd, sizeof(entry->handler.command) - 1);
		entry->handler.command[sizeof(entry->handler.command) - 1] = '\0';
	}

	fprintf(stderr, "lua_config: added file handler for .%s: %s %s\n",
	        ext, type_str, cmd ? cmd : "");
	return 0;
}

/*
 * st.get_file_handlers() - Get all configured file handlers as a table
 */
static int l_get_file_handlers(lua_State *L)
{
	lua_newtable(L);

	/* Add default handler */
	lua_newtable(L);
	switch (default_file_handler.type) {
	case FILE_HANDLER_XDG:
		lua_pushstring(L, "xdg");
		break;
	case FILE_HANDLER_TERMINAL:
		lua_pushstring(L, "terminal");
		break;
	case FILE_HANDLER_APP:
		lua_pushstring(L, "app");
		break;
	}
	lua_setfield(L, -2, "type");
	lua_pushstring(L, default_file_handler.command);
	lua_setfield(L, -2, "command");
	lua_setfield(L, -2, "*");

	/* Add specific handlers */
	for (int i = 0; i < file_handler_count; i++) {
		lua_newtable(L);
		switch (file_handlers[i].handler.type) {
		case FILE_HANDLER_XDG:
			lua_pushstring(L, "xdg");
			break;
		case FILE_HANDLER_TERMINAL:
			lua_pushstring(L, "terminal");
			break;
		case FILE_HANDLER_APP:
			lua_pushstring(L, "app");
			break;
		}
		lua_setfield(L, -2, "type");
		lua_pushstring(L, file_handlers[i].handler.command);
		lua_setfield(L, -2, "command");
		lua_setfield(L, -2, file_handlers[i].extension);
	}

	return 1;
}

/* st.set_background_image(path) - Set background image (PNG) */
static int l_set_background_image(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	float opacity = bgimg_get_opacity();
	if (opacity <= 0.0f)
		opacity = 0.3f; /* sensible default */
	bgimg_set(path, opacity);
	if (x_initialized)
		redraw();
	return 0;
}

/* st.set_background_opacity(opacity) - Set background opacity (0.0-1.0) */
static int l_set_background_opacity(lua_State *L)
{
	float opacity = (float)luaL_checknumber(L, 1);
	if (opacity < 0.0f) opacity = 0.0f;
	if (opacity > 1.0f) opacity = 1.0f;
	bgimg_set_opacity(opacity);
	if (x_initialized)
		redraw();
	return 0;
}

/* st.get_background_image() - Get background image path */
static int l_get_background_image(lua_State *L)
{
	lua_pushstring(L, bgimg_get_path());
	return 1;
}

/* st.get_background_opacity() - Get background opacity */
static int l_get_background_opacity(lua_State *L)
{
	lua_pushnumber(L, bgimg_get_opacity());
	return 1;
}

/* Register the 'st' module */
static const luaL_Reg st_lib[] = {
	{"set_font", l_set_font},
	{"set_font_size", l_set_font_size},
	{"reload_config", l_reload_config},
	{"get_config_path", l_get_config_path},
	{"get_foreground_process", l_get_foreground_process},
	{"get_tab_count", l_get_tab_count},
	{"get_active_tab", l_get_active_tab},
	{"get_tab_info", l_get_tab_info},
	{"set_tab_title", l_set_tab_title},
	{"set_icon", l_set_icon},
	{"set_logging", l_set_logging},
	{"get_logging", l_get_logging},
	{"set_cursor_blink", l_set_cursor_blink},
	{"get_cursor_blink", l_get_cursor_blink},
	{"set_pane_border", l_set_pane_border},
	{"get_pane_border", l_get_pane_border},
	{"set_pane_border_color", l_set_pane_border_color},
	{"get_pane_border_color", l_get_pane_border_color},
	{"zoom", l_zoom},
	{"zoomreset", l_zoomreset},
	{"set_color", l_set_color},
	{"set_colors", l_set_colors},
	{"set_file_handler", l_set_file_handler},
	{"get_file_handlers", l_get_file_handlers},
	{"set_background_image", l_set_background_image},
	{"set_background_opacity", l_set_background_opacity},
	{"get_background_image", l_get_background_image},
	{"get_background_opacity", l_get_background_opacity},
	{NULL, NULL}
};

static int luaopen_st(lua_State *L)
{
	luaL_newlib(L, st_lib);
	return 1;
}

/* Build config path */
static void build_config_path(void)
{
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}

	if (home) {
		snprintf(config_path, sizeof(config_path),
		         "%s/.config/ft/config.lua", home);
	}
}

/* Check if file exists */
static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* Initialize Lua and load config */
int lua_config_init(void)
{
	build_config_path();

	L = luaL_newstate();
	if (!L) {
		fprintf(stderr, "lua_config: failed to create Lua state\n");
		return -1;
	}

	/* Open standard libraries */
	luaL_openlibs(L);

	/* Register 'st' module */
	luaL_requiref(L, "st", luaopen_st, 1);
	lua_pop(L, 1);

	/* Load config file if it exists */
	if (file_exists(config_path)) {
		if (luaL_dofile(L, config_path) != LUA_OK) {
			fprintf(stderr, "lua_config: error loading %s: %s\n",
			        config_path, lua_tostring(L, -1));
			lua_pop(L, 1);
			return -1;
		}
		fprintf(stderr, "lua_config: loaded %s\n", config_path);
	} else {
		fprintf(stderr, "lua_config: no config at %s\n", config_path);
	}

	return 0;
}

/* Reload configuration from file */
int lua_config_reload(void)
{
	if (!L) {
		return -1;
	}

	if (!file_exists(config_path)) {
		fprintf(stderr, "lua_config: config file not found: %s\n", config_path);
		return -1;
	}

	if (luaL_dofile(L, config_path) != LUA_OK) {
		fprintf(stderr, "lua_config: error reloading %s: %s\n",
		        config_path, lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}

	fprintf(stderr, "lua_config: reloaded %s\n", config_path);
	return 0;
}

/* Cleanup Lua state */
void lua_config_cleanup(void)
{
	if (L) {
		lua_close(L);
		L = NULL;
	}
}

/* Get the Lua state */
lua_State *lua_config_get_state(void)
{
	return L;
}

/* Get config file path */
const char *lua_config_get_path(void)
{
	return config_path;
}

/*
 * Format tab title using Lua callback.
 * Calls st.format_tab_title(index, is_active, title, process) if defined.
 * Returns formatted string (static buffer) or NULL if no Lua formatter.
 */
const char *lua_config_format_tab(int index, int is_active, const char *title,
                                   const char *process)
{
	static char result[512];

	if (!L)
		return NULL;

	/* Get the format_tab_title function from st module */
	lua_getglobal(L, "st");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return NULL;
	}

	lua_getfield(L, -1, "format_tab_title");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return NULL;
	}

	/* Push arguments */
	lua_pushinteger(L, index + 1);  /* 1-based for Lua */
	lua_pushboolean(L, is_active);
	lua_pushstring(L, title ? title : "terminal");
	lua_pushstring(L, process ? process : "terminal");

	/* Call function: 4 args, 1 result */
	if (lua_pcall(L, 4, 1, 0) != LUA_OK) {
		fprintf(stderr, "lua_config: format_tab_title error: %s\n",
		        lua_tostring(L, -1));
		lua_pop(L, 2);  /* error + st table */
		return NULL;
	}

	/* Get result */
	if (lua_isstring(L, -1)) {
		const char *str = lua_tostring(L, -1);
		strncpy(result, str, sizeof(result) - 1);
		result[sizeof(result) - 1] = '\0';
		lua_pop(L, 2);  /* result + st table */
		return result;
	}

	lua_pop(L, 2);
	return NULL;
}

/*
 * Handle key event via Lua.
 * Calls st.on_key(mod, keysym) if defined.
 * Returns 1 if Lua consumed the key, 0 otherwise.
 *
 * mod is a table with boolean fields: ctrl, shift, alt, super
 * keysym is the X11 keysym integer
 */
int lua_config_handle_key(unsigned int mask, unsigned int keysym)
{
	if (!L)
		return 0;

	/* Get the on_key function from st module */
	lua_getglobal(L, "st");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}

	lua_getfield(L, -1, "on_key");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return 0;
	}

	/* Create modifier table */
	lua_newtable(L);
	lua_pushboolean(L, (mask & (1 << 2)) != 0);  /* ControlMask = 1 << 2 */
	lua_setfield(L, -2, "ctrl");
	lua_pushboolean(L, (mask & (1 << 0)) != 0);  /* ShiftMask = 1 << 0 */
	lua_setfield(L, -2, "shift");
	lua_pushboolean(L, (mask & (1 << 3)) != 0);  /* Mod1Mask (Alt) = 1 << 3 */
	lua_setfield(L, -2, "alt");
	lua_pushboolean(L, (mask & (1 << 6)) != 0);  /* Mod4Mask (Super) = 1 << 6 */
	lua_setfield(L, -2, "super");

	/* Push keysym */
	lua_pushinteger(L, keysym);

	/* Call function: 2 args (mod table, keysym), 1 result */
	if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
		fprintf(stderr, "lua_config: on_key error: %s\n",
		        lua_tostring(L, -1));
		lua_pop(L, 2);  /* error + st table */
		return 0;
	}

	/* Get result - true means Lua consumed the key */
	int consumed = lua_toboolean(L, -1);
	lua_pop(L, 2);  /* result + st table */
	return consumed;
}

/*
 * Get file handler for a given path.
 * Looks up handler by file extension, falls back to default handler.
 */
FileHandler lua_config_get_file_handler(const char *path)
{
	if (!path || !path[0])
		return default_file_handler;

	/* Find the last dot in the path */
	const char *dot = strrchr(path, '.');
	if (!dot || dot == path)
		return default_file_handler;

	/* Skip the dot */
	const char *ext = dot + 1;

	/* Look for a specific handler for this extension */
	for (int i = 0; i < file_handler_count; i++) {
		if (strcasecmp(file_handlers[i].extension, ext) == 0) {
			return file_handlers[i].handler;
		}
	}

	/* Return default handler */
	return default_file_handler;
}
