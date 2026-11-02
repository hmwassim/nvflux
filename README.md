# nvflux

Lock NVIDIA GPU memory and graphics clocks, safely.

```sh
git clone https://github.com/hmwassim/nvflux.git
cd nvflux
sudo ./install.sh
nvflux balanced
```

Log out and back in once after installing, so you can use nvflux without sudo.

## What it fixes

| Problem | Mode |
|---|---|
| HDMI/DP audio crackles, screen flickers, desktop lags | `nvflux balanced` |
| Loud fans, hot GPU, power use | `nvflux powersave` |
| Games or CUDA too slow | `nvflux max` |
| Undo | `nvflux off` |

The mode is saved and applied again at boot and after sleep.

## Commands

```
nvflux                        mode and clocks
nvflux info                   supported clocks, and what each mode sets
nvflux doctor                 check the setup
nvflux reload                 apply the saved mode again
nvflux lock memory <MHz>      lock the memory clock
nvflux lock graphics <MHz>    lock the graphics clock (or <min>-<max>)

--temporary   apply without saving    --gpu <N>   one GPU only
--force       allow a slow lock       --json      machine-readable output
```

See `man nvflux` for details.

```console
$ nvflux balanced
GPU 0: balanced: memory 810 MHz, graphics 945-2100 MHz
$ nvflux
mode: balanced
GPU 0: NVIDIA GeForce RTX 3060
  memory      810 MHz  (locked 810)
  graphics    945 MHz  (locked 945-2100)
  power        17 W, 43 C
```

## Safety

- It only uses NVIDIA's own clock-lock functions (`nvidia-smi -lgc/-lmc` use
  the same ones), with clock values the driver reports as supported.
- The driver's power limit and thermal protection always stay on. No
  overclocking, voltage, power limit or fan changes.
- Locks that would slow the GPU down are refused unless you add `--force`.
- Locks are released before sleep and applied again after.
- `nvflux=off` on the kernel command line boots without the locks.

## Requirements

- The proprietary NVIDIA driver.
- Graphics locks: GTX 16 / RTX 20 or newer. Memory locks: RTX 30 or newer.
- Any distro, desktop and window manager. Boot hooks for systemd, OpenRC,
  runit, dinit and SysV; sleep hooks for systemd, elogind, zzz and pm-utils.
  `doctor` says what to do on anything else.

## Permissions

nvflux is setuid root, mode 4750, group `nvflux`. Members of the group change
clocks without sudo; everyone else uses sudo. The installer adds the user who
ran it. `sudo ./install.sh --everyone` lets every user change clocks.

## Uninstall

```sh
sudo ./uninstall.sh
```

It unlocks the clocks and removes everything the installer added.

## Upgrading from the old nvflux

Run `sudo ./install.sh`. It removes the old autostart entries and keeps your
saved mode: the old `powersave` becomes `balanced` (which does what it did),
`performance` becomes `max`, `auto` becomes `off`. `nvflux restore` and
`nvflux lock <MHz>` still work.

## Help wanted

These can't be tested on the developer's machine. If you have one, run
`nvflux doctor` and `nvflux info --json` and open an issue with the output:

- Laptops: does a lock survive the GPU powering itself off?
- Several or high-refresh monitors: does the driver keep memory above the lock?
- RTX 20 / GTX 16: do the graphics locks work?

## Building by hand

```sh
make            # build/nvflux
make test       # unit, integration and installer tests (no root, no GPU)
make check      # static analysis and shell checks
```

## License

MIT
