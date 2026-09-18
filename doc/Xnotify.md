# Xnotify — Permission Notification Extension for XLibre

**Xnotify** is a small X extension added to this [personal fork of XLibre](https://github.com/kiyoshispreclerg/xserver/tree/xnotify) that allows:

- Real-time notification when a client attempts to perform privileged actions.
- Default operation using static configuration files in `<ServerConfigDir>/xnotify.conf*`.
- Dynamic permission querying and setting through an external permission manager.

### Currently Protected Actions

- `SELECTION`     — Clipboard access
- `COMPOSITE`     — Access windows
- `SCREEN`        — Capture and draw to screen
- `CURSOR`        — Capture cursor
- `INPUT`         — Raw keyboard access
- `INPUT_GRAB`    — Input grab
- and more.

### Operating Modes

1. **Static Mode**  
   Loads rules only from `<ServerConfigDir>/xnotify.conf` and drop-in files in `xnotify.conf.d/`.  
   Simple, lightweight, and already provides basic protection.

2. **Dynamic Mode (with external guard)**  
   The X server can communicate via Unix domain sockets (`$XDG_RUNTIME_DIR/xnotify.sock` and `xperms.sock`) with an external daemon.  
   This enables asking the user, dynamically saving permissions, pausing suspicious processes with SIGSTOP, etc.

### Configuration

You or your distribution can define permissions by allowing programs or entire pathes (using wildcard) to access one action or all at once.

Examples:

`ALLOW ALL /opt/xlibre/bin/*` # All actions allowed for every executable in this path

`ALLOW /usr/bin/*` # No action specified means ALL, so implicitly allowing all actions

`DENY RECORD` # Action denied for every executable

`DENY SELECION /usr/bin/firefox` # Action denied for this executable

`ALLOW SCREEN /usr/local/bin/obs` # Action allowed for this executable

`ALLOW COMPOSITE /usr/local/bin/obs`

`ALLOW MANAGE /usr/bin/python3|*myscript.py` # Action allowed for this executable only if its arguments match the string after `|`.

It is possible to allow a program to access an action only if there is a specific argument, something 
important to programs that are interpreted scripts. To do this, you write the desired argument just after 
the path, in the same line, separated by a pipe (`|`). Both the program path and the argument accept wildcards, 
but keep in mind that rules with wildcards are heavy in CPU cycles.

As the configuration is loaded sequentially, a file with this list of rules allows XLibre to work free, 
then allows programs in /usr/bin everything, but RECORD is denied globally, then gives OBS (in another path) the screen capture permission, 
and finally allows Python to run only the specified script.

If there is no files or no valid rules in the files, the X server will allow everything, as always has been the default.

### Runtime Commands (Guard → Server)

The guard talks to the server over the `xperms.sock` command socket with small
JSON datagrams (see `xnotify.c` for the exact fields per command: `ALLOW`,
`DENY`, `ALLOW_ALL`, `DENY_ALL`, `ALLOW_ACTION`, `DENY_ACTION`,
`QUERY_ACTION`, `ENABLE`, `DISABLE`, `STATUS`, `XNOTIFY` heartbeat). One more
command is available to reload the static configuration on demand:

`{"command":"RELOAD"}`

This tells the server to re-read `xnotify.conf` / `xnotify.conf.d/*.conf` from
disk right now, exactly as it does on startup — as if the extension were
internally restarting. It only wipes and rebuilds the **internal rule table**
(everything a rule-file `ALLOW`/`DENY` would have added, including anything
the guard itself had set dynamically) and calls the same `XnotifyLoadConfig()`
used at startup and when the guard disconnects.

It does **not**:
- drop the guard connection or touch heartbeat/liveness state,
- clear already-connected clients' cached permission masks (a client that
  already had an action granted keeps working; only a fresh permission check
  — one that misses the per-client cache — sees the reloaded rules),
- touch the pid/exe resolution cache or the pending-notification throttle.

Use it after editing the config files on disk so the new static rules take
effect without disturbing clients that are already running. The server
replies with a normal `STATUS` message once the reload completes.

A guard that wants to write a rule directly into the static config — for
example one running with no permanent rule store of its own — can ask the
server where that actually is instead of guessing, since `SYSCONFDIR` is a
compile-time constant of the server, not of the guard:

`{"command":"GET_CONFIG_PATH"}`

The server replies over the notify socket with:

`{"command":"CONFIG_PATH","dir":"<SYSCONFDIR>/xnotify.conf.d","file":"<SYSCONFDIR>/xnotify.conf"}`

`dir` is where a new drop-in `*.conf` file should be written (typically
requiring elevated privileges, e.g. via polkit); follow up with `RELOAD`
once the file is in place so the new rule takes effect.

### Difference from Xnamespace

**Xnotify** is a **simple** notification and permission system.  
The full **Xnamespace** will be a more complete permission namespace system, with powerful isolation, but also more complex.

Xnotify is **not** in opposition to Xnamespace — it is an alternative.

### Current Status

- Part of [this **XLibre** fork](https://github.com/kiyoshispreclerg/xserver/tree/xnotify).
- Still under development (personal hobby project).
- Code subject to change, as we rewrite the commit history everytime we pull upstream updates.
- Functional, but not considered stable for production use.

To use it with the external guardian, you can try `xnsguard` (available at https://github.com/kiyoshispreclerg/xnsguard).

And, yes, I, Kiyoshi, used AI to make a big part of this system, but I read, test and even use all this code in my main machine, of course 😊