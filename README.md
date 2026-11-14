# <img src="data/tandem-fm.svg" alt="Tandem logo" width="64" height="64" align="absmiddle"> Tandem

<img src="data/tandem-owl.jpg" alt="Tandem's mascot: an owl with one blue and one orange eye, like the two panels." width="150" align="right">

**A two-panel file manager for Linux, built for the keyboard.**

Keep your source in one panel and your destination in the other. Browse with
tabs, preview files in place, and copy, move or rename them with familiar
Total Commander keys. Tandem is built with Qt 6 Widgets and C++20, with
background file operations and built-in SFTP and SMB.

[Install](#install) · [Keyboard basics](#keyboard-basics) ·
[Build from source](#build-from-source) · [Contribute](CONTRIBUTING.md)

![Tandem with wallpapers in the left panel, the Tandem source folder in the right, and a preview of the selected image on the right.](docs/screenshots/overview.png)

*Two file panels, with a preview of the selected file beside them.*

## What you can do

- **Get around quickly.** Tabs in each panel, clickable paths, quick search,
  filtering as you type, bookmarks on letter keys, and fuzzy search through
  your visited folders. Pin a tab to keep it in its folder and across
  workspaces. Save the tabs of both panels as a workspace and get
  them back with Alt+W. A command palette puts the rest within reach.
- **Keep working during transfers.** Copy, move and delete in the background,
  with progress, pause, cancel and queues per drive. Undo supported local
  operations with Ctrl+Z.
- **Work with servers and archives.** Connect over SFTP or SMB, with other
  protocols available through GVfs. Browse archives like folders, extract
  files, and update ZIP, 7z and tar archives. Save servers with their user,
  port and key file under **Servers…**, or import them from `~/.ssh/config`;
  they are kept in `servers.toml` and passwords in the desktop keyring when
  available.
- **Look before you open.** An integrated viewer and content previews handle
  text, hex, images, PDF, Markdown, fonts, video and audio. Browse pictures
  with a thumbnail grid and see Git status alongside your files.
- **Handle the bigger jobs.** Rename batches with a live preview, compare and
  synchronize folders, find duplicates, inspect disk usage, calculate and
  verify checksums, and edit permissions, ACLs and filesystem flags.
- **Make it yours.** Rebind command keys, enable optional vim/yazi-style
  modal keys, choose light and dark themes, or add your own theme. External
  programs become file actions through [plugins](docs/plugins.md).

Linux is the main platform. An experimental macOS build is available; see
[the macOS notes](packaging/macos/README.md) for setup and platform differences.
The interface is available in English and German.

## Install

### Arch Linux

Prebuilt packages for **x86_64** are available from the
[mooned repository](https://repo.mooned.de/). Follow its signing-key setup,
then add this to `/etc/pacman.conf`:

```ini
[mooned]
Server = https://repo.mooned.de/$arch
```

Update your system and install Tandem:

```sh
sudo pacman -Syu tandem-fm-git
```

The `-git` package follows the project's main branch. To build the Arch
package yourself, clone this repository and run:

```sh
cd packaging/arch
makepkg -si
```

### Other Linux distributions

[Build from source](#build-from-source), then run `./build/tandem-fm` or
install it with CMake.

You can choose the starting folders on the command line:

```sh
tandem-fm ~/Projects ~/Downloads
```

## Keyboard basics

| Key | Action |
| --- | --- |
| Tab | Switch panels |
| Enter / Backspace | Open the current item / go to the parent folder |
| Insert / Space | Mark or unmark an item |
| Shift+↑/↓/PgUp/PgDn/Home/End | Mark (or unmark) the files the cursor passes over |
| F3 / F4 | View / edit; the viewer follows the cursor, ↑ / ↓ (j / k) and N / P step through the folder, T keeps to the same type, Space plays videos and music |
| F5 / F6 | Copy / move to the other panel |
| F7 / F8 | New folder / move to trash |
| F2, Shift+F6 | Rename |
| Ctrl+M | Rename multiple files |
| Ctrl+Q | Show or hide previews |
| Ctrl+F | Filter the current folder |
| Alt+F7 | Search a local folder tree by name or file contents; results open in a new tab |
| Ctrl+Shift+F | Filter as you type through all subfolders, in a new tab; results work like any folder |
| Ctrl+B / Ctrl+Shift+B | Put the selection on shelf 1–4 / bring a shelf here, which empties it: 1–4 copies, Shift+1–4 moves, Space looks inside (or click one at the bottom) |
| Ctrl+J | Jump to a visited folder |
| Alt+W | Open a workspace, or save the open tabs as one |
| Ctrl+Shift+P | Open the command palette |

**F1** opens the searchable shortcut reference. Change command keys in
**Settings → Keys**; **Ctrl+,** opens Settings. Modal keys are optional and
can also be customized in `keys.toml`.

## Rename with a preview

Build names from counters, dates, EXIF dates and parts of the old name.
Find and replace, regular expressions and case changes work together,
and the preview highlights what will change before you apply it.

![Batch rename preview turning numbered camera filenames into autumn-walk_001.jpg and similar names.](docs/screenshots/batch-rename.png)

Prefer your editor? Send the names to a text editor, change them there,
then review the resulting plan in Tandem.

## Compare and synchronize

Compare two folder trees and review the actions before copying anything.
Copy changes in both directions or mirror one side onto the other. Change
individual actions, leave files alone, or open a file comparison from the
list. Synchronization works with local folders and supported servers.

![Folder comparison showing which example files will be copied left or right, and why.](docs/screenshots/folder-sync.png)

## Themes, settings and plugins

Tandem includes its own light and dark themes alongside Nord, Dracula,
Gruvbox, Catppuccin, Solarized, Tokyo Night and others. It can follow your
desktop's light/dark preference. Custom themes are JSON files; optional
file-type colours come from the theme too.

Settings → Bookmarks lists all favourites, including those without a key. Pick a
folder to edit its bookmark keys, one character per step (`w s`). The full modal
chain includes the bookmark prefix (normally `g w s`). Clearing a key keeps the
favourite; removing a favourite also removes its key. Assigning a key already in
use moves it to the selected favourite. Changes use the existing `folders.tsv`
and are saved across restarts.

Settings and key bindings live in readable TOML files. The settings dialog
preserves your comments when changing values, and saved key bindings reload
without restarting. Use `--config-dir` to keep settings, keys, open tabs,
bookmarks and folder history together in a folder of your choice:

```sh
tandem-fm --config-dir ~/Sync/tandem
```

- [Themes](docs/themes.md): choose a theme, make your own, or colour files by type.
- [Settings and keys](docs/configuration.md): file locations, TOML examples and portable configuration.
- [Plugins](docs/plugins.md): turn scripts and installed tools into commands on your selection.

## Build from source

You need a C++20 compiler, CMake 3.24 or newer, Ninja, **Qt 6.5 or newer**
(Core, Gui, Widgets, Pdf and Test), **libarchive 3.3 or newer**, and **toml++**.

```sh
git clone https://github.com/carnager/tandem-fm.git
cd tandem-fm
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tandem-fm
```

Optional integrations are enabled when their development libraries are
found at build time:

| Dependency | Enables |
| --- | --- |
| libssh ≥ 0.11 | Native SFTP |
| libsmbclient | Native SMB |
| GIO, with GVfs at runtime | Other network locations and devices |
| libsecret | Server passwords in the desktop keyring |
| libacl | POSIX ACL editing on Linux |
| KSyntaxHighlighting for Qt 6 | Syntax highlighting in the viewer |
| Qt Multimedia (with its FFmpeg backend at runtime) | Playing videos and music in the viewer and preview |
| Qt Linguist tools | Compiled translations and translation updates |

At runtime, `fd` speeds up filename search, `rg` handles content search,
and `ffmpeg` provides video/audio previews and video thumbnails. Name search
has a built-in fallback; content search can use `grep` when `rg` is missing.
Install a diff tool such as `meld` for graphical file comparisons, or use
the built-in display of `diff -u` output.

To install the build:

```sh
sudo cmake --install build
```

To run the tests:

```sh
ctest --test-dir build --output-on-failure
```

## Contributing

Bug reports, translations and focused improvements are welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the workflow and
[AGENTS.md](AGENTS.md) for the code and UI guidelines. For a bug report,
include the steps to reproduce it, your platform, and whether it involves
local files, a server or an archive.

## License

[GPL-3.0-or-later](LICENSE).
