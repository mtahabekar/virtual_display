# API selection and verification limits

Inspected September 11, 2026. The installed KWin is 5.27.11, not Plasma 6. After switching from GNOME to Plasma Wayland, live registry inspection confirmed screencast protocol version 3. KWin's D-Bus `/Plugins` reports `kwin5_plugin_screencast` loaded. `/KWin` introspection matches the expected diagnostic interface. Source inspection was matched to the installed version, and the executable probes the real registry on every run.

## Native KWin screencast extension — selected provisionally

The `stream_virtual_output` request creates an output and a corresponding PipeWire stream. Holding the stream keeps the output alive; finishing it removes the output. KWin sends the node ID asynchronously. This fits independently owned outputs without a monitor-picker portal or a separate VNC server. Source: [KWin 5.27 screencast manager](https://raw.githubusercontent.com/KDE/kwin/Plasma/5.27/src/plugins/screencast/screencastmanager.cpp).

The exact v1.10.0 protocol XML is vendored, matching Ubuntu Noble's available `plasma-wayland-protocols` package version. The application requires protocol version 2 or later and binds no higher than version 3, which the vendored definition understands. Source: [KDE protocol v1.10.0](https://github.com/KDE/plasma-wayland-protocols/blob/v1.10.0/src/protocols/zkde-screencast-unstable-v1.xml).

This is **native but private**, not a stable public API. Current upstream explicitly warns regular clients against relying on it and has added newer events, including PipeWire object serials. The backend is therefore isolated and version-bounded; upgrading Plasma requires revalidation. It does not send an invented D-Bus `createOutput` call. Source: [current upstream protocol](https://raw.githubusercontent.com/KDE/plasma-wayland-protocols/master/src/protocols/zkde-screencast-unstable-v1.xml).

KWin 5.27's DRM backend prefixes the requested name with `Virtual-` and creates a 60000 mHz mode. Thus the fixed logical `QUEST-1` maps to connector `Virtual-QUEST-1`, and likewise for 2/3. Bare connector names would require another mechanism or a KWin change; neither is justified for this checkpoint. Source: [KWin v5.27.11 DRM virtual output](https://github.com/KDE/kwin/blob/v5.27.11/src/backends/drm/drm_virtual_output.cpp).

KWin filters the screencast global by the requesting executable's desktop entry and executable identity. Registration uses `X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1` and an absolute matching `Exec` path. This is why the utility must be probed under its registered executable path, and why generic `wayland-info` is insufficient to rule the interface out. Sources: [KWin display filtering](https://raw.githubusercontent.com/KDE/kwin/v5.27.11/src/wayland_server.cpp), [service-field lookup](https://raw.githubusercontent.com/KDE/kwin/v5.27.11/src/utils/serviceutils.h).

## Other options

| Option | Assessment for this project |
|---|---|
| `krfb-virtualmonitor` | KDE's existing virtual-monitor/VNC application is a useful reference and fallback experiment. Its RFB server and associated behavior are unnecessary for a direct PipeWire → encoder host. No VNC server was installed or started. |
| KWin D-Bus | The 5.27 `org.kde.KWin` interface has diagnostics and desktop controls, but no general virtual-output creation method. Probe the live service after login; use the Wayland request for creation. |
| KScreen/output-management | Use for enumerating modes, enabled state, and arrangement. Do not confuse virtual desktops/workspaces with monitor outputs. |
| xdg-desktop-portal | A broader public capture route, but monitor selection/permission lifecycle needs evaluation for unattended use. Not needed for the owned-stream experiment. |
| VKMS | A software KMS driver and fallback only if native KWin cannot meet the requirements. It adds a kernel display device and does not by itself prove integration with KWin's NVIDIA renderer or per-output capture. No module was loaded. |

References: [KRFB virtual-monitor implementation](https://github.com/KDE/krfb/blob/master/krfb/main-virtualmonitor.cpp), [KWin 5.27 D-Bus interface](https://raw.githubusercontent.com/KDE/kwin/Plasma/5.27/src/org.kde.KWin.xml), [kernel VKMS documentation](https://docs.kernel.org/gpu/vkms.html).

## What has and has not been validated

Validated: build with installed libraries; safe refusal on GNOME; isolated protocol tests; live Plasma interface access; three KWin DRM virtual outputs; KScreen enumeration with 2560×1440 @ 60 Hz modes; distinct live PipeWire nodes; duplicate-instance rejection; clean shutdown; recreation; arrangement restoration after a host restart; user confirmation that all three appear in KDE Display Configuration. See [passing report](plasma-milestone1-report.json).

Subsequent live tests verified ordinary window placement, captured content, NVENC initialization, three simultaneous encoders, and localhost receive/decode/reconnect. On September 12, 2026, the separate Quest client also hardware-decoded all three streams over an ADB reverse USB tunnel and recovered from stream restart, app pause/resume, and tunnel restoration. The native creation path is selected. See README.md, final-benchmark.json, and quest-e2e-report.md for measured performance: the 3×60 FPS target remains unmet. User services are installed and enabled; actual logout/login/reboot validation remains pending.

Two environment issues were resolved during the live test: the Snap IDE's XDG paths differed from Plasma's, and the stock Qt5 KScreen command crashed in NVIDIA EGL. Native user registration plus a Mesa software EGL override scoped only to the KScreen diagnostic fixed these issues without changing packages or compositor rendering. The custom KDE interface-list property also required removing a trailing semicolon; the corrected registration is accepted by KWin.
