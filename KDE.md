# Noctalia on KDE Plasma (fork notes)

This fork's `kde` branch is upstream Noctalia plus fixes that make it behave as a native KDE Plasma 6 shell.
Each fix is one commit on top of an upstream release tag, so it can be reviewed, upstreamed or rebased on its own.

| Commit | What it fixes |
| --- | --- |
| filter the taskbar by monitor | KDE ignored the output filter, so every bar listed every window |
| lock through KDE's screen locker | KWin has no ext-session-lock, so lock did nothing |
| logout, reboot, shutdown through ksmserver | clean session save instead of killing the session |
| screen-off through kscreen-doctor --dpms | no wlr-output-power on KWin |

Install: `makepkg -si` in `packaging/arch/` (replaces the `noctalia` package), or build with Meson as in BUILDING.md.
The personal glue (key bridge, services, settings) lives in the separate private repo `noctalia-kde`.
