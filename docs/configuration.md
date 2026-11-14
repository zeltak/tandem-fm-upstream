# Settings and keys

Open Settings with **Ctrl+,** to change appearance, behaviour, what opens at
start, external tools, key bindings and plugins. Most changes apply immediately; a language change
takes effect after restarting Tandem.

Settings and key bindings are also plain TOML files you can edit yourself.
The default locations on Linux are:

- `~/.config/tandem-fm/tandem-fm.toml`: the settings. The settings dialog
  changes single lines and keeps the rest, comments included.
- `~/.config/tandem-fm/keys.toml`: the keys of every command, listed with
  their defaults (commented out), and the modal, yazi-like keys. Saving the
  file applies it right away; a mistake shows a message with the line.

```toml
[keys]
copy = "F5"
move-to-trash = ["F8", "Del"]
delete-permanently = []          # no key

[modal]
top = "g g"                      # a sequence
sort-by-size = ", s"
down = ["j", "Down"]
```

The paths above follow your XDG configuration directories.

Window size, columns and open tabs are Tandem's own business and live in
`~/.local/state/tandem-fm/state.toml`. Settings from older versions
(`tandem-fm.conf`) move over on the first start; the old file stays as
`tandem-fm.conf.bak`.

**View → Equal panel widths (50/50)** restores an equal split between the
left and right file panels. A preview on the right keeps its current width.
The command is also in the command palette and on the Keys page, where you
can assign a shortcut. It has no default key binding.

## Startup and workspaces

A workspace is the tabs of both panels under a name. **Alt+W** opens one in
place of the tabs open now; type a name that isn't there yet to save the open
tabs under it. **Save workspace…** (in the Tabs menu and the command palette)
updates one. Each workspace is also a command, so it can get a key on the
Keys page or in `keys.toml` (`workspace-<number>`).

**Settings → Startup** chooses what opens when Tandem starts: the tabs from
last time (the default), a workspace, or the home folder in both panels.
The window's size and place are always kept, and folders given on the
command line open instead. The same page renames, updates and deletes
workspaces. They are kept in `tandem-fm.toml`:

```toml
[startup]
open = "workspace"               # or "home"; missing: the tabs from last time
workspace = 1

[workspaces.1]
id = 1
name = "Photos"
active = "left"

[workspaces.1.left]
tabs = ["/home/me/Pictures", "/home/me/Pictures/2026"]
tabSlots = [1, 2]
tabGrids = [true, true]
currentTab = 0
```

## Use another configuration folder

`tandem-fm --config-dir ~/Dropbox/tandem` (or `TANDEM_CONFIG_DIR=…`) keeps the
settings, keys, state, bookmarks and folder history there instead of the usual
places, on every platform: a synced folder shares them between computers.
Changes from another computer apply at the next start (keys right away).

The files sit together under `DIR/tandem-fm/` when you use `--config-dir DIR`.
This also puts custom themes and plugins beside the settings.

[Back to the README](../README.md)
