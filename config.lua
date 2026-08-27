-- st terminal configuration
-- Tab icons similar to wezterm

-- Window icon (shown in taskbar/dock)
-- By default, the FT logo PNG from assets/icons/ is used.
-- Uncomment below to override with an emoji instead:
-- st.set_icon("🖥️")  -- Options: 🖥️, 💻, 🐱, 🦊, 🐧, ⌨️, etc.

-- Icon mappings (process name -> icon)
local icons = {
    -- Development
    ["vim"] = "💤",
    ["nvim"] = "💤",
    ["neovim"] = "",
    ["emacs"] = "📜",
    ["code"] = "💻",
    ["vscode"] = "💻",
    ["nano"] = "📄",

    -- Programming languages
    ["python"] = "🐍",
    ["python3"] = "🐍",
    ["node"] = "💚",
    ["npm"] = "📦",
    ["rust"] = "🦀",
    ["cargo"] = "📦",
    ["java"] = "☕",
    ["ruby"] = "💎",
    ["php"] = "🐘",
    ["go"] = "🐹",
    ["lua"] = "🌙",

    -- DevOps & Cloud
    ["docker"] = "🐳",
    ["kubernetes"] = "☸️",
    ["k8s"] = "☸️",

    -- System & Terminal
    ["bash"] = "🖥️",
    ["zsh"] = "🐚",
    ["fish"] = "🐠",
    ["terminal"] = "🖥️",
    ["ssh"] = "🔐",
    ["htop"] = "📊",
    ["top"] = "📈",
    ["btop"] = "📊",

    -- Git
    ["git"] = "🌿",
    ["lazygit"] = "🦥",

    -- File managers
    ["ranger"] = "🏹",
    ["mc"] = "📁",
    ["nnn"] = "📂",

    -- Tools
    ["make"] = "🔧",
    ["tmux"] = "🖼️",
    ["man"] = "📚",
    ["less"] = "📖",
    ["grep"] = "🔎",
    ["find"] = "🔦",
}

-- Default icon
local default_icon = "🖥️"

-- Format tab title function
function st.format_tab_title(index, is_active, title, process)
    local icon = default_icon
    local display_title = title

    if not title or title == "" or title == "terminal" then
        display_title = process or "terminal"
    end

    if process then
        local proc_lower = string.lower(process)
        if icons[proc_lower] then
            icon = icons[proc_lower]
        end
    end

    if icon == default_icon and display_title then
        local title_lower = string.lower(display_title)
        for keyword, keyword_icon in pairs(icons) do
            if title_lower:find(keyword, 1, true) then
                icon = keyword_icon
                break
            end
        end
    end

    return string.format("%s %s", icon, display_title)
end
