# ft - Fearless Terminal

A fork of [suckless st](https://st.suckless.org/) with tabs, split panes, Lua configuration, and session logging.

## Why ft?

ft uses Xft for font rendering - no OpenGL overhead, no GPU wake-ups for text. Simple and efficient.

**Why not tmux + st?**

- Single process, no IPC overhead
- Integrated tab bar and pane borders
- One config file (Lua), not two
- Mouse support that just works

## Philosophy

ft follows the suckless philosophy: minimal, efficient, hackable. But "minimal" doesn't mean "featureless" - it means no bloat. Tabs, splits and Lua config are about 4,700 lines of C on top of st, 9,300 in total, not abstraction layers.

- **X11 + Xft** - Battle-tested, efficient, no Wayland portals
- **Lua config** - Runtime changes without recompilation
- **No dependencies hell** - Just Xlib, Xft, Xrender, fontconfig, Lua

## Features

| Feature | Original ST | ft |
|---------|-------------|-----------|
| Multiple tabs | ✗ | ✓ |
| Split panes (H/V) | ✗ | ✓ |
| Active pane highlight | ✗ | ✓ |
| Vim-style pane navigation | ✗ | ✓ |
| Kill frozen pane (SIGKILL) | ✗ | ✓ |
| Tab renaming | ✗ | ✓ |
| Spawn new window | ✗ | ✓ |
| Clickable URLs/paths (Ctrl+Click) | ✗ | ✓ |
| Session logging | ✗ | ✓ |
| Lua runtime config | ✗ | ✓ |
| Custom window icon (PNG/emoji) | ✗ | ✓ |
| OSC52 clipboard | ✗ | ✓ |
| On-screen notifications | ✗ | ✓ |
| Background image (PNG) | ✗ | ✓ |
| Scrollback | ~2000 lines | 10000 lines |

## Building

```bash
make clean && make
sudo make install  # installs to /usr/local/bin
```

### Dependencies

- libX11, libXft, libXrender
- fontconfig, freetype2
- lua5.4 (or lua5.3)
- libpng

Debian/Ubuntu:
```bash
sudo apt install libx11-dev libxft-dev libxrender-dev libfontconfig-dev lua5.4-dev libpng-dev
```

### Static Build

Build a portable binary with most libraries statically linked (~3.8MB):

```bash
make ft-static
```

The static build only requires glibc at runtime - no need for X11/Lua/fontconfig packages on the target system. Useful for deploying to minimal systems or containers.

**Additional build dependencies for static linking:**

```bash
sudo apt install libexpat1-dev libpng-dev zlib1g-dev libbz2-dev libbrotli-dev
```

## Keyboard Shortcuts

### Tabs

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+T` | New tab |
| `Ctrl+Shift+W` | Close tab |
| `Ctrl+Shift+Left` | Previous tab |
| `Ctrl+Shift+Right` | Next tab |
| `Ctrl+Shift+E` | Edit (rename) active tab |
| `Ctrl+Shift+L` | Label (rename) application window - visible in WM Alt+Tab |
| `Ctrl+Shift+N` | New window (spawn new ft instance) |

### Split Panes

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+R` | Split right (vertical) |
| `Ctrl+Shift+D` | Split down (horizontal) |
| `Ctrl+Shift+Q` | Close current pane |
| `Ctrl+Shift+K` | Kill pane (SIGKILL frozen process) |

**Note:** When splits exist, the active pane is highlighted with a colored border (configurable via Lua).

### Clipboard & Selection

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+C` | Copy selection to clipboard |
| `Ctrl+Shift+V` | Paste from clipboard |
| `Ctrl+Shift+Y` | Paste from primary selection |
| `Shift+Insert` | Paste from primary selection |
| `Middle-click` | Paste primary selection |

### Zoom

| Shortcut | Action |
|----------|--------|
| `Ctrl+=` | Zoom in |
| `Ctrl+-` | Zoom out |
| `Ctrl+0` | Reset zoom |
| `Ctrl+NumPad+` | Zoom in (numpad) |
| `Ctrl+NumPad-` | Zoom out (numpad) |
| `Ctrl+Shift+PageUp` | Zoom in (alternative) |
| `Ctrl+Shift+PageDown` | Zoom out (alternative) |
| `Ctrl+Shift+Home` | Reset zoom (alternative) |

### Scrollback

| Shortcut | Action |
|----------|--------|
| `Shift+PageUp` | Scroll up |
| `Shift+PageDown` | Scroll down |

### Logging & Config

| Shortcut | Action |
|----------|--------|
| `Ctrl+Shift+Z` | Toggle session logging for current pane |
| `Ctrl+Shift+F5` | Reload Lua config |

## Mouse Support

- **Ctrl+Click** - Open URL or file path under cursor
- **Click on tab bar** - Switch to clicked tab
- **Click on pane** - Focus clicked pane (when splits exist)
- **Click and drag** - Select text
- **Double-click** - Select word
- **Triple-click** - Select line
- **Middle-click** - Paste primary selection

### Clickable Links

Ctrl+Click on URLs or file paths to open them:

- **URLs**: `http://`, `https://`, `ftp://`, `file://`
- **Absolute paths**: `/path/to/file`
- **Home paths**: `~/path/to/file`
- **Relative paths**: `./file` or `../file`

URLs open in your default browser via `xdg-open`. File paths open with the system default application.

## Session Logging

Log terminal output to files for later review.

### Toggle Logging (Per-Pane)

Press `Ctrl+Shift+Z` to toggle logging for the current pane. A notification will appear showing "Logging ON" or "Logging OFF".

### Enable Logging for All New Terminals

Add to `~/.config/ft/config.lua`:

```lua
-- Enable logging with default path ($HOME/.ft/logs)
st.set_logging(true)

-- Or with custom path:
st.set_logging(true, "/path/to/my/logs")
```

### Log Files

- **Location:** `$HOME/.ft/logs/` (default) or custom path
- **Filename format:** `YYYYMMDDhhmmss_<ID>.log`
- **Content:** Raw terminal output (including escape sequences)

### Why Built-in Logging?

The standard advice is "just use `script` or `tee`":

```bash
script -q ~/log.txt
bash | tee ~/log.txt
```

**What breaks:**

| Problem | Why |
|---------|-----|
| OSC52 clipboard | Escape sequences get eaten by pipe |
| `$PWD` in splits | New shell doesn't inherit working directory |
| Colors/formatting | terminfo issues through pipes |
| Interactive input | Wrong buffering |
| Ctrl+C, signals | Go to wrong process |
| `ssh -t` | PTY allocation mess |

**ft logs *after* the PTY:**

```
PTY → Terminal → screen
         ↓
      logfile
```

Terminal sees exactly what it always sees. Log is just a side output. OSC52 works. Splits work. Everything works.

## Window Icon

By default, ft displays the **FT logo** (green F, blue T) as the window icon in your taskbar/dock. The PNG icon is loaded from:

1. `/usr/local/share/icons/hicolor/64x64/apps/fearless-terminal.png` (installed)
2. `/usr/share/icons/hicolor/64x64/apps/fearless-terminal.png`
3. `./assets/icons/hicolor/64x64/apps/fearless-terminal.png` (development)

### Override with Emoji

To use an emoji instead of the PNG logo, add to your `~/.config/ft/config.lua`:

```lua
st.set_icon("🚀")  -- Or any emoji: 🖥️, 💻, 🐱, 🦊, 🐧, etc.
```

If no PNG is found and no emoji is configured, it falls back to the 🖥️ emoji.

## Background Image

Display a PNG image behind the terminal text with configurable transparency.

The image uses **fill (crop)** scaling: it maintains its aspect ratio, scales to cover the entire window, and crops any overflow. The terminal's background color is drawn semi-transparently over the image, controlled by an opacity value.

### Setup

Add to `~/.config/ft/config.lua`:

```lua
st.set_background_image("/path/to/wallpaper.png")
st.set_background_opacity(0.3)  -- 0.0 = image hidden, 1.0 = bg fully transparent
```

### How Opacity Works

The opacity value controls how transparent the terminal background color is, revealing the image underneath:

| Opacity | Effect |
|---------|--------|
| `0.0` | Image completely hidden (normal terminal) |
| `0.3` | Image faintly visible behind text |
| `0.7` | Image clearly visible, text still readable |
| `1.0` | Background fully transparent, image at full brightness |

To remove the background image:

```lua
st.set_background_image("")
```

Reload config with `Ctrl+Shift+F5` to apply changes at runtime.

## Lua Configuration

Create `~/.config/ft/config.lua` for runtime configuration.

Two files in this repository are meant as examples: `config.lua` is the one I
use every day (tab icons per process, colours, font), and `config.lua.example`
is a documented starting point. Copy either to `~/.config/ft/config.lua`.

### Example Config

```lua
-- Set font
st.set_font("JetBrainsMono Nerd Font:pixelsize=14:antialias=true")

-- Or just change size
st.set_font_size(16)

-- Enable logging for all terminals
st.set_logging(true)

-- Override default window icon (FT logo) with emoji
-- st.set_icon("🚀")

-- Set cursor blink speed (default 800ms, 0 = no blink)
st.set_cursor_blink(500)

-- Active pane border (only visible with splits)
st.set_pane_border(1)        -- width in pixels (0 = disabled)
st.set_pane_border_color(7)  -- color: 7=white, 8=gray, 14=cyan (default)

-- Background image with transparency
st.set_background_image("/path/to/wallpaper.png")
st.set_background_opacity(0.3)  -- 0.0 = opaque bg, 1.0 = fully transparent bg

-- Custom tab title formatter (optional)
function st.format_tab_title(index, is_active, title, process)
    if process == "nvim" then
        return " " .. title .. " "
    end
    return " " .. process .. " "
end
```

Press `Ctrl+Shift+F5` to reload config without restarting.

### Lua API Reference

| Function | Description |
|----------|-------------|
| `st.set_font(str)` | Set font (e.g., `"Mono:pixelsize=14"`) |
| `st.set_font_size(n)` | Set font size only |
| `st.set_icon(emoji)` | Override PNG icon with emoji (default: FT logo) |
| `st.set_logging(enabled, [path])` | Enable/disable logging |
| `st.get_logging()` | Get logging config `{enabled, path}` |
| `st.set_cursor_blink(ms)` | Set cursor blink interval (0 = no blink) |
| `st.get_cursor_blink()` | Get cursor blink interval in ms |
| `st.set_pane_border(px)` | Set active pane border width (0 = disabled) |
| `st.get_pane_border()` | Get active pane border width in pixels |
| `st.set_pane_border_color(idx)` | Set active pane border color (0-15) |
| `st.get_pane_border_color()` | Get active pane border color index |
| `st.set_tab_title(index, title)` | Set tab title (1-indexed) |
| `st.get_tab_count()` | Get number of tabs |
| `st.get_active_tab()` | Get active tab index (1-indexed) |
| `st.get_tab_info(index)` | Get tab info table |
| `st.get_foreground_process()` | Get current pane's foreground process |
| `st.get_config_path()` | Get config file path |
| `st.reload_config()` | Reload config file |
| `st.zoom(delta)` | Zoom font (positive = in, negative = out) |
| `st.zoomreset()` | Reset zoom to default |
| `st.set_background_image(path)` | Set background image (PNG), `""` to clear |
| `st.set_background_opacity(n)` | Set background opacity (0.0-1.0) |
| `st.get_background_image()` | Get background image path |
| `st.get_background_opacity()` | Get background opacity |
| `st.set_file_handler(ext, type, [cmd])` | Set handler for file extension |
| `st.get_file_handlers()` | Get all configured file handlers |

### File Handlers (Ctrl+Click)

Configure how files are opened when Ctrl+Clicking on paths:

```lua
-- Open PDFs with atril (external app)
st.set_file_handler("pdf", "app", "atril %s")

-- Open images with feh
st.set_file_handler("png", "app", "feh %s")
st.set_file_handler("jpg", "app", "feh %s")

-- Open HTML files with xdg-open (system default)
st.set_file_handler("html", "xdg")

-- Set default handler for all other files (opens in new terminal with nvim)
st.set_file_handler("*", "terminal", "nvim %s")
```

**Handler types:**

| Type | Description |
|------|-------------|
| `"terminal"` | Open in new ft terminal window (default: nvim) |
| `"app"` | Open with external application (detached) |
| `"xdg"` | Use system default via xdg-open |

The `%s` in the command is replaced with the file path. If omitted, the file path is appended as an argument.

**Default behavior** (without configuration):
- URLs → xdg-open (browser)
- Files → nvim in new terminal

### Custom Keybindings

You can override or add keybindings by defining `st.on_key` in your config:

```lua
-- Custom keybinding handler
-- Return true to consume the key, false to pass to default handlers
function st.on_key(mod, keysym)
    -- mod is a table: {ctrl=bool, shift=bool, alt=bool, super=bool}
    -- keysym is the X11 keysym (integer)

    -- Example: Ctrl++ for zoom in (keysym 43 is '+')
    if mod.ctrl and keysym == 43 then
        st.zoom(1)
        return true
    end

    -- Example: Ctrl+- for zoom out (keysym 45 is '-')
    if mod.ctrl and keysym == 45 then
        st.zoom(-1)
        return true
    end

    -- Example: Ctrl+0 for reset zoom (keysym 48 is '0')
    if mod.ctrl and keysym == 48 then
        st.zoomreset()
        return true
    end

    return false  -- Pass to default handlers
end
```

Common X11 keysyms: `+` = 43, `-` = 45, `=` = 61, `0` = 48, `a-z` = 97-122.

### Tab Info Table

`st.get_tab_info(index)` returns:

```lua
{
    index = 1,           -- Tab index (1-based)
    title = "terminal",  -- Tab title
    is_active = true,    -- Whether tab is active
    process = "zsh",     -- Foreground process name
    term_title = "...",  -- Terminal title (from escape sequences)
}
```

## OSC52 Clipboard Support

Applications like neovim can copy to system clipboard via OSC52 escape sequences.

### Neovim Setup

```lua
-- In your neovim config
vim.g.clipboard = {
  name = 'OSC 52',
  copy = {
    ['+'] = require('vim.ui.clipboard.osc52').copy('+'),
    ['*'] = require('vim.ui.clipboard.osc52').copy('*'),
  },
  paste = {
    ['+'] = require('vim.ui.clipboard.osc52').paste('+'),
    ['*'] = require('vim.ui.clipboard.osc52').paste('*'),
  },
}
```

## Compile-Time Configuration

Edit `config.h` and recompile for:

- Default font
- Color scheme
- Keybindings
- Scrollback size (default: 10000 lines)
- Border size
- Default terminal size

## Architecture

```
Window
└── TabState
    └── Tab
        ├── title
        └── root_pane (binary tree)
            ├── PANE_LEAF → Terminal (PTY)
            └── PANE_HSPLIT/VSPLIT
                ├── child1
                └── child2
```

Each pane has its own PTY. Tabs are independent.

## File Structure

```
ft/
├── st.c                        # Terminal emulation + tab/pane management
├── st.h                        # Data structures (Terminal, Pane, Tab)
├── x.c                         # X11 window, input, drawing, event loop
├── win.h                       # Window mode flags
├── config.def.h                # Compile-time defaults
├── config.h                    # The compile-time config I actually run
├── lua_config.c                # Lua API implementation
├── lua_config.h                # Lua API declarations
├── config.lua                  # The runtime config I actually run
├── config.lua.example          # Documented starting point for your own
├── classic_terminal_colors.lua # Colour scheme
└── TokyoNight_terminal_colors.lua
```

## Development

ft is a fork of [suckless st](https://st.suckless.org/). Roughly half the C in
this repository is st's, unchanged - the tabs, split panes, Lua config and
session logging on top of it are mine.

In many parts I used Claude LLM for help.

## Credits

- [st](https://st.suckless.org/) - Original terminal by suckless.org
- Tabs, panes, Lua, logging - ft extensions

## Note

Just a small experiment.

## License

MIT/X License - See LICENSE file
