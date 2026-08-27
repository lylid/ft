/* lua_config.h - Lua configuration support for st */
#ifndef LUA_CONFIG_H
#define LUA_CONFIG_H

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Initialize Lua and load config */
int lua_config_init(void);

/* Reload configuration from file */
int lua_config_reload(void);

/* Apply deferred colors after X11 init */
void lua_config_apply_deferred_colors(void);

/* Cleanup Lua state */
void lua_config_cleanup(void);

/* Get the Lua state (for advanced use) */
lua_State *lua_config_get_state(void);

/* Config file path */
const char *lua_config_get_path(void);

/* Format tab title using Lua callback (returns NULL if no Lua formatter) */
const char *lua_config_format_tab(int index, int is_active, const char *title,
                                   const char *process);

/* Handle key event via Lua (returns 1 if Lua consumed the key, 0 otherwise) */
int lua_config_handle_key(unsigned int mask, unsigned int keysym);

/*
 * File handler types for Ctrl+Click
 */
typedef enum {
	FILE_HANDLER_XDG,      /* Use xdg-open (default for URLs) */
	FILE_HANDLER_TERMINAL, /* Open in new terminal with editor */
	FILE_HANDLER_APP       /* Open with specific application */
} FileHandlerType;

typedef struct {
	FileHandlerType type;
	char command[256];     /* For TERMINAL/APP: command to run (%s = file) */
} FileHandler;

/*
 * Get file handler for a given path.
 * Returns handler based on file extension or default handler.
 */
FileHandler lua_config_get_file_handler(const char *path);

#endif /* LUA_CONFIG_H */
