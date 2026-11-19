# Vendored protocols

loom generates bindings for every protocol `wayland-protocols` installs on
the build machine. That covers the standard set — but it is not the whole
ecosystem, and a library that says "complete" has to mean it.

Three other families of protocol are in real use and are not in
`wayland-protocols`:

| directory   | source                                                          | what it is                                                          |
|-------------|-----------------------------------------------------------------|---------------------------------------------------------------------|
| `wlr/`      | <https://gitlab.freedesktop.org/wlroots/wlr-protocols>           | wlroots' extensions. `layer-shell` is here, and it is what every bar, launcher, notification daemon, lock screen and wallpaper tool binds. |
| `plasma/`   | <https://invent.kde.org/libraries/plasma-wayland-protocols>      | KDE's. Needed to talk to Plasma clients, and for `plasma-window-management`. |
| `hyprland/` | <https://github.com/hyprwm/hyprland-protocols>                   | Hyprland's. Global shortcuts, toplevel export, lock notification.   |

## Why they are checked in rather than depended on

They are separate packages (`wlr-protocols`, `plasma-wayland-protocols`,
`hyprland-protocols`), and none of them is guaranteed to be installed. If
loom enumerated them from the system the way it does `wayland-protocols`,
then whether a compositor built on loom supports `layer-shell` would depend
on which packages happened to be present when it was built — so the same
source would produce a different feature set on two machines, and the
failure would appear at runtime as "this bar does not start".

Checking the XML in makes the build self-contained and the answer the same
everywhere. It is also what the clients themselves do: neowall, waybar and
the rest all vendor the protocol files they need.

These are interface definitions, not code. Each file carries its own
copyright header (MIT or similar); nothing here is relicensed.

## Updating

    tools/vendor_protocols.sh

Re-fetches all three upstreams and reports what changed. The commits these
were taken from:

    wlr-protocols          bf4fc79abc359eea5a0edec0ac6d4a2b2955f82a  2026-03-09
    hyprland-protocols     cc9a8fd253bdc00f48a967ecf4828211ef08751f  2026-09-15
    plasma-wayland-protocols  c5ac4db818f4a575a6ccfe0065b73ecfaba6e93e  2026-09-24

## Duplicates

Some of these declare interfaces that `wayland-protocols` has since adopted
under another name — `wlr-data-control` versus `ext-data-control`, for
instance. Both are generated: they are different interfaces with different
names, a client asks for one by name, and dropping either would break the
clients that ask for it. The selector in `CMakeLists.txt` only de-duplicates
files that declare *the same* interfaces.
