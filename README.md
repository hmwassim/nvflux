# nvflux

Stops HDMI/DisplayPort audio dropouts on NVIDIA GPUs by locking the memory
clock.

```sh
git clone https://github.com/hmwassim/nvflux.git
cd nvflux
sudo ./install.sh
nvflux powersave
```

That's it. The profile is re-applied at every login and after resuming from
suspend, and you never need `sudo` again.

## Why

The NVIDIA driver keeps raising and lowering the memory clock as GPU load
changes. Every switch briefly interrupts HDMI/DP audio, which you hear as
crackles or short dropouts. Locking the memory clock to one value stops the
switching.

## Usage

| Command              | What it does                                             |
|----------------------|----------------------------------------------------------|
| `nvflux powersave`   | Lock memory to its lowest clock that doesn't limit the GPU core. Best for desktop/video. |
| `nvflux balanced`    | Lock memory to a middle clock that doesn't limit the core. |
| `nvflux performance` | Lock memory to its highest clock. Use for gaming.        |
| `nvflux lock <MHz>`  | Lock memory to the supported clock nearest to `<MHz>`. Refuses clocks that limit the core unless you add `--force`. |
| `nvflux auto`        | Unlock and let the driver manage the clock again.        |
| `nvflux status`      | Show the saved profile and current clocks (the default). |
| `nvflux restore`     | Re-apply the saved profile (runs automatically at login).|

```console
$ nvflux powersave
GPU 0 (NVIDIA GeForce RTX 3060): memory locked to 810 MHz
$ nvflux
profile: powersave
GPU 0 (NVIDIA GeForce RTX 3060): memory 810 MHz, core 405 MHz
```

Why not the very lowest clock? It belongs to the GPU's idle state, which also
caps the core: an RTX 3060 at 405 MHz memory allows at most 420 MHz core
(instead of 2100), which makes even the mouse cursor stutter. `powersave` and
`balanced` check each memory clock's core limit and only choose among clocks
that keep the full range. On an RTX 3060 that's 7501/7301/5001/810 MHz, so
`powersave` takes 810 and `balanced` 5001. `lock` refuses a capped clock
unless you add `--force`.

`powersave` can still starve games of memory bandwidth, so switch to
`performance` before playing. With several GPUs, the profile is applied to each of them.

## Requirements

- An NVIDIA GPU that supports memory clock locking. NVIDIA documents it for
  Ampere (RTX 30) and newer. Some older GPUs support it too; if yours doesn't,
  nvflux says so.
- The proprietary NVIDIA driver.
- Any distro, desktop, display server and init system. The installer fetches
  a C compiler and `make` if they're missing.

It's best to have `nvidia-persistenced` running (most driver packages enable
it). nvflux turns on persistence mode itself if it isn't on, so the driver
doesn't drop the lock while the GPU is idle.

## How it works

nvflux talks to the driver through NVML (`libnvidia-ml.so.1`), the library
`nvidia-smi` itself is built on. It doesn't run or parse `nvidia-smi`.

| Path                                 | Purpose                           |
|--------------------------------------|-----------------------------------|
| `/usr/local/bin/nvflux`              | The program (setuid root)         |
| `/var/lib/nvflux/state`              | The saved profile, e.g. `powersave` |
| `/etc/xdg/autostart/nvflux.desktop`  | Runs `nvflux restore` at login    |
| `/usr/local/lib/systemd/system/nvflux-resume.service` | Runs `nvflux restore` after resume (systemd) |
| `/etc/elogind/system-sleep/nvflux`   | The same, on elogind systems      |
| `/etc/zzz.d/resume/nvflux`           | The same, for Void's `zzz`        |

Memory clock locks are lost on reboot and driver reload, and can be lost
across suspend. That's why nvflux re-applies the saved profile at login and
after resume. The installer sets up whichever resume mechanism your system
has: systemd, elogind, or `zzz`.

**Window managers without XDG autostart** (i3, sway, dwm, Hyprland, …): add
`nvflux restore` to your startup config instead.

**No supported suspend system?** Run `nvflux restore` after resuming, or hook
it into whatever handles suspend on your system.

### Security

nvflux is setuid root so that anyone can switch profiles without `sudo`. To keep
that safe:

- Only the four profile commands and `restore` keep root. Everything else drops
  it before doing anything.
- Root code starts from an empty environment. The NVIDIA library is loaded
  only from system directories, which the dynamic linker enforces for setuid
  programs, or from a fixed path that must be root-owned and not writable by
  others.
- No external programs are run.
- The state file is written atomically and never follows symlinks.

## Building by hand

```sh
make                 # builds into build/
make test            # runs the test suite against a fake NVIDIA library
sudo make install    # honours PREFIX, BINDIR, STATEDIR, AUTOSTARTDIR, DESTDIR
```

The tests need neither root nor a GPU. They use a user namespace
(`unshare -r`) and a stand-in `libnvidia-ml.so.1`.

## Uninstall

```sh
sudo ./uninstall.sh
```

This unlocks the memory clock and removes everything nvflux installed.

## Troubleshooting

- **"changing clocks needs root"**: the setuid bit is missing. Run
  `sudo ./install.sh` again.
- **"not supported by this GPU or driver"**: your GPU can't lock its memory
  clock (pre-Turing, or a data-center GPU that only allows locking across a
  driver reload).
- **Still hearing dropouts**: run `nvflux status` a few times. The memory clock
  should not move. If it does, something else (another tool, or the driver
  unloading) is resetting it.

## Upgrading from nvflux 1.x

Run `sudo ./install.sh`. It replaces the old binary and autostart entry. The
`ultra` profile is gone; if it was saved, it's cleared and you pick a new
profile. `powersave` and `balanced` now skip memory clocks that cap the core,
so `powersave` no longer slows the whole GPU down. `nvflux clocks` is now part
of `nvflux status`.

## License

MIT
