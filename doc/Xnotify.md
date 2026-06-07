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