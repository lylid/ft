-- Classic Linux Terminal Colors (VGA/CGA palette)
-- Load this file to apply the classic console color scheme
--
-- Usage: Add to your config.lua:
--   dofile("/path/to/classic_terminal_colors.lua")
-- Or copy the st.set_colors() call to your config.lua

st.set_colors({
    -- Normal colors (0-7)
    [0]  = "#000000",  -- black
    [1]  = "#AA0000",  -- red
    [2]  = "#00AA00",  -- green
    [3]  = "#AA5500",  -- yellow (brown)
    [4]  = "#0000AA",  -- blue
    [5]  = "#AA00AA",  -- magenta
    [6]  = "#00AAAA",  -- cyan
    [7]  = "#AAAAAA",  -- white (light gray)

    -- Bright colors (8-15)
    [8]  = "#555555",  -- bright black (dark gray)
    [9]  = "#FF5555",  -- bright red
    [10] = "#55FF55",  -- bright green
    [11] = "#FFFF55",  -- bright yellow
    [12] = "#5555FF",  -- bright blue
    [13] = "#FF55FF",  -- bright magenta
    [14] = "#55FFFF",  -- bright cyan
    [15] = "#FFFFFF",  -- bright white

    -- Special colors
    foreground = "#AAAAAA",  -- default text color (light gray)
    background = "#000000",  -- default background (black)
    cursor     = "#AAAAAA",  -- cursor color
})
