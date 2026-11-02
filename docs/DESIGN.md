# nvflux 1.0 — Design

Status: **implemented** (phases 0-4) · 2026-10-23

nvflux 1.0 is a ground-up rewrite that replaces the old nvflux (internally
v2.x), which only held the memory clock. Items marked **[NEEDS TESTERS]** can't
be tested on the reference machine. Until someone confirms them, `doctor` warns
about them.

---

## 1. Vision

**nvflux makes NVIDIA GPU clocks behave, safely, with one short command.**

The driver keeps raising and lowering the GPU's clocks. Usually that's fine. When
it isn't, people get audio crackle over HDMI/DP, screen flicker, stutter, loud
fans, or results that change from run to run. nvflux holds the clocks where they
need to be. It never pushes the hardware past what NVIDIA allows.

Principles, in priority order:

1. **Can't break the GPU.** Only NVIDIA's own clock-lock calls, only values the
   driver reports as supported, and the driver's heat and power protections always
   stay on.
2. **Works everywhere.** Any distro, desktop or window manager, display server,
   init system or suspend system. No daemon.
3. **Obvious to use.** You say what you want in plain words, not MHz. Every
   message says what happened and what to do next. Undo is always one word.
4. **Small and boring.** A single C program and a few tiny shell hooks.

### Non-goals

- Overclocking, undervolting, clock offsets, voltage, power limits, fan control.
  Other tools (LACT, nvidia-settings) do that; nvflux will not.
- AMD and Intel GPUs.
- A daemon, or automatic per-application switching. A future tray app may switch
  profiles, but it only ever calls the CLI.
- Data-center-only features (Hopper "deferred" memory locks, MIG, vGPU).

---

## 2. What the research found

### 2.1 What NVIDIA's API allows (NVML)

| Function | GPUs | Notes |
|---|---|---|
| `nvmlDeviceSetGpuLockedClocks(min, max)` | **Volta and newer** (incl. Turing GTX 16 / RTX 20) | Locks graphics clock to a range. Also accepts symbolic values, see below. |
| `nvmlDeviceResetGpuLockedClocks` | Volta+ | Back to driver control. |
| `nvmlDeviceSetMemoryLockedClocks(min, max)` | **Ampere and newer** (RTX 30+) | Locks memory clock to a range. Turing users report `NOT_SUPPORTED`. Doesn't work on Hopper. |
| `nvmlDeviceResetMemoryLockedClocks` | Ampere+ | Back to driver control. |
| `nvmlDeviceGetSupportedMemoryClocks` | Kepler+ | The memory clocks a lock may use. |
| `nvmlDeviceGetSupportedGraphicsClocks(mem)` | Kepler+ | The graphics clocks allowed **at a given memory clock**. |
| `nvmlDeviceSetPersistenceMode` | all, Linux | Keeps the driver initialised while idle. Reset to off on reboot. |
| `nvmlDeviceGetCurrentClocksEventReasons` | | Why clocks are being held back right now: heat, power cap, idle, … |
| `nvmlDeviceGetArchitecture`, `GetUUID`, `GetHandleByUUID` | | Detecting support; identifying GPUs reliably. |

- **Symbolic graphics locks.** `SetGpuLockedClocks` accepts
  `NVML_CLOCK_LIMIT_ID_TDP` (0xffffff01) and `NVML_CLOCK_LIMIT_ID_UNLIMITED`
  (0xffffff02) instead of MHz:
  - `unlimited, tdp`: never above the base (TDP) clock, may go lower.
  - `tdp, unlimited`: never below the base clock, may boost higher.
  - `tdp, tdp`: held at the base clock.

  These work on any supported card without knowing its MHz values, which makes
  them ideal for presets. Confirmed on GeForce (§2.5).
- **No read-back.** NVML has **no function to read the currently locked clocks**
  (an NVIDIA forum question about this has gone unanswered since 2024). So nvflux
  keeps its own record of what it set, and `status` shows both what was requested
  and what is observed.
- **Applications clocks are deprecated** and print "deprecated" on driver 615.
  Never use them.
- **Locks are lost** on reboot and driver reload (documented). Whether they
  survive suspend isn't documented. LACT re-applies its settings after resume
  for the same reason. nvflux sidesteps the question by unlocking before sleep
  (§4 rule 11).
- **Root required** for every setter (documented).

### 2.2 Safety facts

- Clock locks set a **target**. The power cap and thermal slowdown still apply on
  top of them: NVIDIA's own docs describe power throttling still happening with
  locked clocks. A lock can't push the card past its power limit or slowdown
  temperature.
- What *has* caused trouble for users:
  - very low graphics locks (one report of a lockup at 300 MHz)
  - memory clocks that also cap the core: on an RTX 3060, 405 MHz memory allows at
    most 420 MHz core, so even the cursor stutters
  - overclock and undervolt offsets

  nvflux refuses the first two by default and never does the third.
- A memory lock also changes power behaviour (a forum report says it pins the PCIe
  mode too). Measured on the reference machine: with memory locked at 7501 MHz,
  idle draw is about 32 W in P0. Measured costs per mode are in §2.5.

### 2.3 Why people lock clocks (use cases)

| Symptom | Cause | What helps |
|---|---|---|
| HDMI/DP audio crackles or drops | Memory clock (and PCIe link) switching | Hold memory steady (the old nvflux's approach, confirmed by several forum reports) |
| Monitor flickers or flashes black, often at high refresh or with several monitors | P-state switching | Hold memory steady, or keep a graphics floor |
| Desktop lag, a hitch when an app starts | Graphics clock takes about a second to ramp up | A graphics minimum (users report 615–1455 MHz depending on card) |
| Loud fans, heat, high power | Boosting higher than needed | Cap graphics at base clock |
| Games starved, low FPS after `balanced` | Memory held too low | Memory at highest |
| Benchmarks and ML runs vary | Boost depends on temperature | Lock graphics (NVIDIA's own TensorRT docs recommend this) |

Not every case is fixed by clocks: some audio cases are fixed by a driver update
or a different cable. `doctor` should say so instead of over-promising.

### 2.4 Linux integration facts

- **Reference machine.** Debian 13, systemd 257, driver 615.71.10 with the open
  kernel modules, RTX 3060, desktop. Persistence is on through nvidia-persistenced.
  Kernel suspend notifiers are in use, so there are no nvidia-suspend units.
- **The driver's own udev rule** runs `nvidia-modprobe` on `KERNEL=="nvidia"`
  add/bind to create the device nodes.
- **Limits of udev `RUN`.** The man page says programs started from `RUN` are
  killed when event handling ends; long jobs should use `SYSTEMD_WANTS`. eudev's
  timeouts aren't documented. A `RUN` job must therefore finish in well under a
  second and must never wait.
- **Suspend handling varies.**
  - With the open modules, kernel suspend notifiers replace nvidia's systemd units
    (driver 595+).
  - The proprietary module still uses systemd units.
  - NVIDIA documents no other init system: those files "serve as a reference".
- **Laptops (runtime D3).** A dGPU that isn't in use powers itself off (Turing+,
  default on Ampere+ notebooks).
  - Any NVML call, including just reading status, **wakes it up**; background
    `nvidia-smi` and monitoring tools are a known cause of battery drain.
  - Persistence mode with UVM persistence blocks runtime D3.
  - Whether a clock lock keeps the dGPU awake is unknown. **[NEEDS TESTERS]**
- **musl** (Alpine, Void-musl): NVIDIA's userspace is glibc-only, so nvflux works
  wherever the driver itself works (gcompat or a glibc layer).
- **NixOS / Guix** need a module or package that uses their setuid wrappers.
  `install.sh` isn't for them.

### 2.5 Experiments on the reference machine (RTX 3060, driver 615, 2026-10-23)

Phase 1 (locks applied live, no suspend). Idle is the GPU doing nothing; load is
an unthrottled `glxgears`:

| Locks | Idle | Load |
|---|---|---|
| none | P8, 210/405 MHz, **16.7 W** | P0, 1897/7501, 52 W |
| memory 810 (memory part of `balanced`/`powersave`) | P5, **16.5 W** (no extra cost) | 1897/810, 38 W |
| memory 7501 (`max`) | P0, **31.5 W** (+15 W) | 1897/7501, 50.6 W |
| graphics `tdp,tdp` | held at **1320** = this card's base clock, 33 W | 1320 |
| graphics `unlimited,tdp` (cap) | normal idle, 17.5 W | **capped at 1320**, 36 W |
| graphics `tdp,unlimited` (floor) | **didn't hold**: 210 MHz, then 1777 | 1905 |
| memory 810 + cap (`powersave`) | 16.2 W | 1320/810, **24.9 W** |
| graphics `1200,1200` (numeric) | **held at 1200**, P5, 17.9 W | 1200 |

Findings:
- Symbolic `tdp` values work on GeForce. The cap is reliable.
- The symbolic floor didn't hold at idle, while a numeric lock did. **`balanced`
  needs a numeric floor**, which still needs one more test.
- Mixing numeric and symbolic values, or min > max, gives `Invalid Argument`
  (safe).
- **The driver rounds an unsupported memory value up, not to the nearest**: 1000
  became 5001. nvflux must snap values itself and never pass the driver a value
  that isn't in the table.
- Holding memory at 810 costs nothing at idle. `max` costs about 15 W at idle, so `status`
  should say so.

Phase 2 (suspend with memory 810 + graphics 1200 locked): **the system resumed
to a black screen and needed a hard reset.**
- The kernel resumed normally from S3 and logged no NVIDIA/Xid errors.
  Networking and the desktop processes kept running, but nothing was displayed.
- The journal shows **no other suspend in 43 recorded boots**. Plain suspend
  (without locks) has never been tested on this machine, so it's unknown whether
  the locks caused this. KDE Wayland + NVIDIA black screens after resume are
  common on their own.
- Phase 2's floor measurements were lost: the log was on tmpfs. Future
  experiment logs go to persistent storage.

Test 1, graphics floor (log: `nvflux-lab/test1-floor.log`). Idle was sampled
every 1 s; the ramp was sampled every 0.1 s after load starts. The driver
updates readings about every 0.5 s.

| Setup | Idle | Clock while load starts | Reaches 1897 after |
|---|---|---|---|
| no locks | **memory flips 405↔810 every few seconds** (P8↔P5), graphics 250–490, ~17 W | ~430 | ~0.9 s |
| symbolic floor `tdp,unl` | same as no locks: **no effect at all** | ~465 | ~0.8 s |
| numeric floor 1320–2100 | steady 1320 / 810, **19.3 W (+2.5 W)** | 1320 | ~0.7 s |
| numeric floor 900–2100 | steady 900 / 810, **17.5 W (+0.6 W)** | 900 | ~0.6 s |
| memory 810 + floor 1320 (≈ `balanced`) | steady 1320 / 810, 19.5 W | 1320 | ~1.0 s (memory stays 810) |

Findings:
- **The symbolic floor doesn't work.** `balanced` must use a numeric floor.
- A floor doesn't make full boost arrive sooner; the clock jump still takes about
  0.6–1 s. What it changes is the **clock while load starts: 2–3× higher**
  (900–1320 instead of about 430). That's what removes the "first second of
  anything is laggy" feeling.
- A numeric graphics floor also **holds memory at ≥ 810 at idle**, because P8
  caps graphics at 420 MHz. That stops the idle 405↔810 memory flipping seen in
  the baseline. That flipping is exactly the switching that causes audio
  crackle, and the baseline shows it happening constantly on a plain desktop.
- NVML has no "base clock" getter (customer boost reads "not supported" and the
  applications-clock getters are deprecated). So the floor has to come from the
  supported table: the supported clock nearest **~43% of the max graphics clock**
  (900 MHz here, +0.6 W).

Test 2, screen off (log: `nvflux-lab/test2-screenoff.log`): `balanced` clocks
(memory 810, graphics 945–2100), then the monitor turned off for 15 s through
KDE (DPMS), then back on.
- **The locks survived the screen turning off and back on.** Memory stayed at
  810 and graphics never went below 945 the whole time.
- Power: about 17 W with the screen on (no lock: about 16.7 W), 11.3 W with the
  screen off.
- So no trigger is needed for screen blanking. Only sleep (handled by rule 11),
  reboot and driver reload drop locks.

Consequence for the design, whatever the cause: **nvflux drops its locks
before sleep and re-applies them after resume** (§4 rule 11, §6). The GPU then
always suspends and resumes in the driver's default state, exactly as it would
without nvflux. nvflux can't make resume worse, and whether locks survive sleep
no longer matters.

### 2.6 Prior art

| Tool | Approach | Takeaway |
|---|---|---|
| `nvidia-smi -lgc/-lmc` | Raw MHz, root, lost at reboot | Most users' first attempt; nvflux is the "make it stick and make it safe" layer |
| LACT | GUI + root daemon (`lactd`); NVIDIA locked clocks since 0.7.2, OC since 0.10 | Proves demand. Needs a daemon and a systemd-style service, and exposes raw MHz. nvflux is the lightweight, works-anywhere opposite |
| GreenWithEnvy, TuxClocker | GUI overclocking | Out of scope (overclocking) |
| nvidia-settings PowerMizer | X11-only, coarse | Doesn't work on Wayland |
| power-profiles-daemon | 3 plain-word profiles, balanced by default | The naming model people already understand |

---

## 3. User experience

### 3.1 The mental model

Users pick a **mode** in plain words. A mode decides both clocks. Modes are
saved and come back automatically after reboot and resume. `off` gives
everything back to the driver.

```
nvflux balanced    everyday use: no audio crackle, no flicker, no lag   ← most people
nvflux powersave   cooler, quieter fans, less power
nvflux max         full speed for games, CUDA and heavy work
nvflux off         the driver decides everything (undo)
```

Names (decided): `powersave / balanced / max / off`.
- People already know them: GNOME and KDE use power-saver / balanced /
  performance, and the old nvflux used powersave / balanced.
- `balanced` reads as "the one most people pick".
- `max` rather than `performance` or `game`: it's short and also fits CUDA and
  rendering work.

What each mode sets (all values chosen from the GPU's supported tables):

| Mode | Memory | Graphics |
|---|---|---|
| `balanced` | Lowest clock that doesn't cap the core (the old nvflux's `powersave` logic) | Numeric floor: the supported clock nearest 45% of max (≈ 945 on an RTX 3060), up to max |
| `powersave` | Same as balanced | Cap at base clock: `unlimited, tdp` |
| `max` | Highest clock | Driver decides (boosts freely; never pinned high) |
| `off` | Driver decides | Driver decides |

- `balanced` uses a numeric floor because the symbolic one had no effect in
  testing (§2.5). Measured cost: +0.6 W at idle.
- `powersave` halved power under load in testing (52 W → 25 W).
- Holding only the memory clock (the old nvflux's `powersave`) is no longer a
  mode. `balanced` does the same for 0.6 W more, and experts can still run
  `nvflux lock memory …`. See §3.7 for the rules on any card.
- `max` deliberately doesn't lock the graphics clock high. Boost already reaches
  the maximum under load, and a high floor would only add heat at idle.

**GPUs that can't lock memory** (Turing: GTX 16 / RTX 20) still get the graphics
part of each mode. nvflux says so plainly:

```
$ nvflux balanced
✓ balanced: graphics floor set on GPU 0 (GeForce RTX 2070)
  This GPU can't hold its memory clock (that needs an RTX 30 or newer),
  so audio crackle may not be fixed. Everything else applies.
```

### 3.2 Expert commands

```
nvflux lock memory 5001             memory at the supported clock nearest 5001
nvflux lock graphics 1500           graphics held at ~1500
nvflux lock graphics 900-1800       graphics kept between 900 and 1800
nvflux lock graphics 1200 --gpu 1   only on GPU 1
```

- Values snap to the nearest supported clock, and the output says so.
- Expert locks are saved as the `custom` mode and reloaded like any other.
- Locks that make the GPU sluggish are refused with an explanation and need
  `--force`:
  - memory clocks that cap the core
  - graphics **caps** below 45% of the card's max graphics clock (the same
    line as `balanced`'s floor; 945 MHz on an RTX 3060). Floors never need
    `--force`: they can't slow anything down.

### 3.3 Everyday commands

```
nvflux              same as 'nvflux status': what's going on + what you can do
nvflux status       add --json for scripts and the future tray
nvflux info         what each GPU supports (clock tables, which modes work)
nvflux doctor       checks everything and explains problems in plain words
nvflux help         the problem → mode guide, then all commands
nvflux version
nvflux reload       re-apply the saved mode (used by the boot/resume hooks)
```

`--temporary` works with every mode and with `lock`. It applies now without
changing the saved mode, so the saved mode comes back at the next reload, sleep
or reboot:
- `nvflux max --temporary` gives full speed for one gaming or CUDA session.
- The sleep hooks use `nvflux off --temporary` to drop the locks before sleep.
- The future tray uses it for quick switches.

`status` shows both when they differ: `max (temporary, saved: balanced)`.

### 3.4 What users see

Output is terse and plain ASCII (decided after trying the first version): no
✓/✗/° symbols, no colour, no extra hint lines. Explanations live in `help` and
the man page.

```
$ nvflux balanced
GPU 0: balanced: memory 810 MHz, graphics 945-2100 MHz

$ nvflux max --temporary
GPU 0: max: memory 7501 MHz
(temporary, saved: balanced)

$ nvflux
mode: balanced
GPU 0: NVIDIA GeForce RTX 3060
  memory      810 MHz  (locked 810)
  graphics    945 MHz  (locked 945-2100)
  power        17 W, 43 C

$ nvflux lock memory 405
nvflux: GPU 0: 405 MHz memory limits graphics to 420 MHz; add --force to allow

$ nvflux balance
nvflux: unknown command 'balance'; did you mean 'balanced'?

$ nvflux balanced          # not root, not setuid
nvflux: changing clocks needs root; use sudo or join the nvflux group
```

Rules:
- Results go to stdout, one line per GPU.
- Errors and warnings go to stderr as `nvflux: <what>; <fix>` and
  `nvflux: warning: <what>`, each on a single line.
- Plain ASCII only. Tests check every message for non-ASCII characters and
  control codes.
- Forgiving input: case-insensitive, with hidden aliases that print a one-line
  warning naming the real command: `performance` → `max`; `auto`, `reset`,
  `default` → `off`; `power-saver` → `powersave`; `restore` → `reload`; the old
  `lock <MHz>` → `lock memory <MHz>`.

### 3.5 `nvflux doctor`

One command for "it doesn't work". It checks, in order, and prints ✓/✗ with a fix
for each:

1. NVIDIA driver loaded; `libnvidia-ml` found and trusted; driver version.
2. Each GPU: architecture, and which modes it supports.
3. Permission: setuid bit present, user in the `nvflux` group (and whether a
   re-login is pending).
4. The saved mode, and whether the observed clocks match it.
5. Boot and resume hooks installed for **this** system's init and suspend method.
6. Persistence mode.
7. Clocks currently being held back, and why (heat, power cap).
8. Laptop: dGPU asleep or awake, on battery or AC, and a warning if a mode may
   keep the dGPU awake.
9. For audio complaints: suggests updating the driver or trying DisplayPort if
   `balanced` didn't help.

### 3.6 Machine interface (for the future tray)

- `nvflux status --json` and `nvflux info --json`, with a `"schema": 1` field.
  Fields are only ever added, never renamed or removed.
- Stable exit codes:

  | Code | Meaning |
  |---|---|
  | 0 | OK |
  | 1 | Failed (GPU or driver error) |
  | 2 | Usage error |
  | 3 | Permission (not in group, setuid missing) |
  | 4 | Not supported by this GPU/driver |
  | 5 | Partly applied (some GPUs failed) |

- `status` must **not wake a sleeping laptop dGPU**. If
  `/sys/bus/pci/devices/<gpu>/power/runtime_status` is `suspended`, report
  `"asleep"` without calling NVML. This lets a tray poll without draining the
  battery.

---

## 3.7 Working on any card

nvflux must behave correctly on every GPU that supports locks, not just the
reference RTX 3060. Rules:

**P1. No hardcoded MHz, ever.** Every target is computed at runtime from that
GPU's own tables:
- `GetSupportedMemoryClocks`
- `GetSupportedGraphicsClocks(mem)`, the graphics table for each memory clock
- `GetMaxClockInfo`

Each GPU in a multi-GPU system gets its own targets. Tables are sized
dynamically (retry on `INSUFFICIENT_SIZE`); the RTX 3060 alone reports 227
graphics clocks.

**P2. Each mode is a rule, not a number:**

| Mode | Memory rule | Graphics rule |
|---|---|---|
| `balanced` | Lowest memory clock whose graphics table reaches the GPU's overall graphics max (i.e. doesn't cap the core) | Floor = supported clock nearest **45% of the graphics max at that memory clock**, ceiling = that max |
| `powersave` | as `balanced` | Symbolic cap `unlimited,tdp`. Fallback if the driver rejects symbolic values (older drivers): cap at the supported clock nearest 63% of max |
| `max` | Highest memory clock | driver |
| `off` | reset | reset |

- `balanced` holds memory **and** sets a graphics floor (decided). Crackle and
  lag usually appear together on desktop setups, and a floor alone still allows
  memory switching during bursty desktop load (video, scrolling, app launches).
  Heavy work belongs on `max`, and every hint says so.
- Why percentages:
  - 45% is a floor well above every idle P-state cap yet cheap. Measured
    +0.6 W at 900 MHz.
  - 63% matches base/max on the reference card, and NVIDIA SKUs typically sit
    around 55–75%.
  - Both are only *starting points* that the expert `lock` command overrides.
  - The symbolic `tdp` cap is preferred precisely because it's the card's real
    base clock, with no guessing.

**P3. Detect what each GPU can do, and degrade gracefully, per GPU:**

| GPU | Graphics locks | Memory locks | Result |
|---|---|---|---|
| Pascal and older | no | no | `doctor` explains; nothing applied |
| Volta, Turing (GTX 16, RTX 20) | yes | no | modes apply their graphics part and say plainly that memory locking needs RTX 30+ |
| Ampere, Ada, Blackwell and newer | yes | yes | everything |
| Hopper / data-center | yes | different API (deferred) | memory part unsupported; out of scope |

Arch gates follow NVIDIA's documented minimums. A `NOT_SUPPORTED` at apply time
is always handled the same way as a failed gate: that part is skipped, with a
clear message, never an error for the whole run.

**P4. Old drivers.** Each NVML symbol is optional, except the few needed to read
clocks. A missing setter disables only the features that need it.

**P5. Don't fight the display engine.** Memory clock changes have to happen in a
display blanking window. With several or high-refresh monitors the driver may
need a higher memory clock just to feed the displays.
- After applying a memory lock, nvflux re-reads the clock a moment later. If
  the driver is running memory above the lock, nvflux reports that instead of
  claiming success.
- `doctor` shows the "display clock setting" limit reason when it's active.
- **[NEEDS TESTERS]** with multi-monitor high-refresh setups whether a low lock is
  honoured or overridden. If honoured and it causes flicker, `balanced` and
  `powersave` must step up to the next memory clock.

**P6. Laptops and power.**
- **Never turn on persistence mode** on a GPU with runtime power management
  (`power/control` = `auto`). On a laptop it keeps the dGPU awake and drains the
  battery.
- Turn it on only for GPUs without runtime PM that aren't already persistent
  (headless desktops and servers, where the driver otherwise tears down and
  drops the lock).
- Never wake a sleeping dGPU (§3.6).
- **[NEEDS TESTERS]** (laptops) whether a lock survives runtime D3. If not, laptops
  rely on reload-after-wake, which needs a trigger. Until tested, `doctor`
  warns that locks on laptop dGPUs may not persist.

**P7. Test on more than one card.**
- `policy.c` is pure and gets unit tests with fixture tables covering shapes, not
  just one card:
  - 3060-style (one capped low clock)
  - GDDR6X-style (a different top clock)
  - every low clock capped
  - a single clock
  - no caps reported
  - graphics table > 256 entries
  - a Turing-like GPU without memory support
  - symbolic values rejected
- `nvflux info --json` output from real users becomes new fixtures over time.

## 4. Safety rules (enforced in code)

1. Only these setters are ever called: `SetGpuLockedClocks`,
   `ResetGpuLockedClocks`, `SetMemoryLockedClocks`, `ResetMemoryLockedClocks`,
   `SetPersistenceMode`. No offsets, voltage, power limit, fan or application
   clocks. Tests check this list against the symbols we load.
2. **Every MHz value comes from the GPU's supported tables**, read fresh on each
   apply. Saved values are re-snapped after a driver update.
3. Graphics values are checked against the table **for the chosen memory clock**.
4. Memory clocks that cap the core are refused without `--force`.
5. Graphics caps below 45% of the card's max graphics clock are refused
   without `--force`. Floors are always allowed.
6. **All-or-nothing per GPU.** If the graphics lock fails after the memory lock
   succeeded, the memory lock is reset. A GPU never ends up half-configured.
7. `off` always resets both clocks, even if nvflux doesn't think they're set
   (another tool may have set them).
8. GPUs are identified by **UUID** in the saved state. A GPU that's gone or
   replaced is skipped with a notice, never mixed up.
9. **Boot escape hatch:** if the kernel command line contains `nvflux=off`,
   `reload` does nothing. This recovers from a bad setting without needing a
   working desktop.
10. `reload` never wakes a sleeping laptop dGPU just to lock it. How locks behave
    across runtime D3 is **[NEEDS TESTERS]**.
11. **Release before sleep.** The suspend hooks reset both locks before the
    system sleeps (`nvflux off --temporary`, which doesn't touch the saved mode). After
    resume they re-apply the saved mode, after a short settle delay. The GPU
    never goes through suspend/resume locked. This comes from a black screen on
    resume seen in testing (§2.5).

---

## 5. Privileges

```
-rwsr-x---  root:nvflux  /usr/local/bin/nvflux     (mode 4750)
```

- One binary (decided). Only `nvflux` group members can run it. The kernel
  enforces this, so no polkit, sudoers or daemon is needed. Everyone else uses
  `sudo nvflux …`. People outside the group can't run nvflux without sudo at
  all, `status` included; that's accepted.
- If nvflux runs without its setuid bit (a build that isn't installed, or a
  `nosuid` mount), commands that change clocks exit with code 3 and say: "run it
  with sudo, or install it and join the nvflux group".
- `install.sh` creates the group (`groupadd`, or `addgroup` on busybox) and adds
  the installing user (`$SUDO_USER`, `$DOAS_USER`, or asks).
- Hardening carried over from the old nvflux:
  - fds 0–2 are made safe
  - root is dropped right away for read-only commands
  - before changing clocks, the environment is cleared, PATH is fixed and umask
    is set
  - NVML is loaded only from trusted, root-owned locations
  - no external programs are run
  - the state file is written atomically without following symlinks
- New:
  - `status`, `info`, `doctor`, `help` and `version` run entirely without root,
    since reading clocks doesn't need it.
  - The state file parser is fuzzed.

---

## 6. Restoring after boot and resume (any init)

`nvflux reload` can run any number of times without harm, so several triggers
may fire and that's fine. The installer detects what exists and installs only
those hooks.

**Boot** (the driver is loaded and the device nodes exist):

| System | Hook |
|---|---|
| systemd | `nvflux-reload.service` (oneshot, after `nvidia-persistenced`, `systemd-modules-load`, `WantedBy=multi-user.target`) |
| OpenRC (Gentoo, Alpine, Artix) | `/etc/init.d/nvflux` (start only) + `rc-update add nvflux default` |
| runit (Void, Artix) | Read from runit's own stage 2 (`/etc/runit/2`): Void runs `/etc/rc.local`, so a marked block goes there; Artix runs `/etc/local.d/*.start` and has no `rc.local` (checked on the Artix image), so `/etc/local.d/nvflux.start`. Anything else: printed instructions |
| dinit (Artix, Chimera) | `/etc/dinit.d/nvflux` (`type = scripted`), enabled by a link in `/etc/dinit.d/boot.d` |
| s6 / s6-rc (Artix) | best effort: the installer prints the line to add |
| SysV (Devuan, Slackware) | LSB `/etc/init.d/nvflux` + `update-rc.d`; Slackware `/etc/rc.d/rc.local` |
| anything else | `install.sh` prints the one line to add to the boot script |

**Driver reload** (a driver update without reboot): **no udev rule in 1.0.**
- udev `RUN` jobs are killed when event handling ends.
- Whether NVML is ready when the driver binds is untested.
- A reboot after a driver update (the common case) is covered by the boot hook.

`doctor` notices when the driver version changed since the last reload and says
to run `nvflux reload`. A udev rule can be added later once it's tested.

**Suspend and resume.** Unlock before sleep, re-apply after (§4 rule 11):

| System | Before sleep | After resume |
|---|---|---|
| systemd | `nvflux-suspend.service` (`Before=sleep.target` and nvidia-suspend; `WantedBy=sleep.target`): `off --temporary` | `nvflux-resume.service` (`After=` the sleep targets and nvidia-resume; the same pattern as the old nvflux's resume unit): `reload --wait 15` |
| elogind | `/etc/elogind/system-sleep/nvflux` `pre` | same script, `post` |
| Void `zzz` | `/etc/zzz.d/suspend/nvflux` | `/etc/zzz.d/resume/nvflux` |
| pm-utils | `/etc/pm/sleep.d/50nvflux` `suspend`/`hibernate` | `resume`/`thaw` |

If no pre-sleep hook exists on a system, locks stay on during sleep. `doctor`
reports this.

**How the installer decides** (`install.sh`):
- The init system comes from process 1's name (`/proc/1/comm`): `systemd`,
  `runit`, `dinit`, `s6-svscan`, `openrc-init`. A plain `init` means OpenRC if
  `/run/openrc` exists, Slackware if `/etc/slackware-version` exists, otherwise
  SysV.
- Sleep hooks go in wherever elogind, `zzz` or pm-utils is present (on systemd,
  the units cover sleep).
- Boot hooks run `reload --wait 15`. That only waits when the kernel shows an
  NVIDIA GPU whose driver isn't ready yet, so a machine without an NVIDIA card
  doesn't boot slower.
- Everything installed is recorded in `/var/lib/nvflux/installed`: files,
  folders it had to create, enabled services, `rc.local` blocks, and whether it
  created the group. `uninstall.sh` reverses exactly that list, and `doctor`
  uses it to check the hooks. Reinstalling first undoes the earlier install,
  keeping the group and the saved mode.
- Tested in `tests/hooks/run.sh` against fake roots for each init and sleep
  system, using stand-in system commands. Every case installs, checks, then
  uninstalls and checks that nothing is left.

No XDG autostart. Nothing depends on the desktop, display server or window
manager.

---

## 7. Files

| Path | Purpose |
|---|---|
| `/usr/local/bin/nvflux` | The program (4750 root:nvflux) |
| `/var/lib/nvflux/state` | Saved mode (text, versioned, keyed by GPU UUID) |
| `/run/nvflux/current` | What's applied right now if it differs from the saved mode (`--temporary`). `/run` is cleared at boot, and `reload` removes it |
| `/var/lib/nvflux/installed` | What `install.sh` installed (files, folders, services, group); read by `doctor` and `uninstall.sh` |
| init/suspend hook files | Only those for the detected systems (see §6) |
| `/usr/local/share/man/man1/nvflux.1` | Man page |
| `/usr/local/share/{bash-completion/completions,zsh/site-functions,fish/vendor_completions.d}/` | Shell completions |

State file example:

```
# nvflux state, format 1. Written by nvflux; edit with care.
mode=custom
GPU-0726ca8b-8dbb-9075-bd95-645ebcdf678f.memory=5001
GPU-0726ca8b-8dbb-9075-bd95-645ebcdf678f.graphics=900-1800
```

For named modes, only `mode=balanced` is stored, so the right clocks are picked
fresh on each GPU and after driver updates.

Old nvflux state files are bare words with no header, so they can't be confused
with the new format. They're converted by **behaviour, not by name**, so
upgraders keep what they had:

| Old nvflux state | Becomes | Why |
|---|---|---|
| `powersave` | `balanced` | Old powersave only held memory low, which is what balanced does now. Mapping it to the new powersave would suddenly cap their graphics |
| `balanced` | `balanced` | |
| `performance` | `max` | |
| `auto` | `off` | |
| `lock <MHz> [--force]` | `custom` (memory lock) | |

---

## 8. Build, code layout, testing

- **C11**, `cc` + GNU `make`, no dependencies beyond libc and libdl.
- nvflux's own compiler and linker flags (PIE, hardening, warnings) are kept
  in separate variables, so packagers' `CFLAGS`, `CPPFLAGS` and `LDFLAGS` add to
  them rather than replace them. This was found on Fedora and openSUSE, where
  overriding `CFLAGS` dropped `-fPIE`.
  `libnvidia-ml` is loaded at runtime with `dlopen`, so no NVIDIA headers are
  needed to build. Builds on glibc and musl.
- Scripts are **POSIX sh** only (works with dash and busybox).

Code layout:

| File | Role |
|---|---|
| `nvml.c` | dlopen shim: the only file that knows about NVML |
| `gpu.c` | Reads a GPU's tables, clocks and support into plain structs |
| `policy.c` | **Pure functions**: mode + tables → exact clocks, with every safety rule. No I/O, so it can be fully unit-tested against recorded clock tables from real GPUs |
| `apply.c` | All-or-nothing apply and rollback |
| `state.c` | Load, save and migrate state |
| `priv.c` | Privilege handling |
| `cli.c`, `out.c` | Parsing, human and JSON output |
| `doctor.c` | The `doctor` checks |
| `status.c` | `status` and `info` |

Testing:

- **Unit tests** for `policy.c` using recorded tables (RTX 3060 recorded on the
  reference machine; collect more from users with `nvflux info --json`).
- **Integration tests:** the fake NVML from the old nvflux, extended with graphics locks,
  symbolic TDP values, failures partway through for rollback tests, and suspended
  GPUs. Run as fake root with `unshare -r`, so no real hardware is touched.
- **Sanitizers:** ASan/UBSan on every test run. GCC `-fanalyzer` and cppcheck in
  `make check`. Fuzz the state parser.
- **Distro matrix** (`tests/containers/run.sh`, docker): in fresh Debian, Fedora,
  Arch, openSUSE, Alpine (musl), Void and Gentoo containers it runs:
  - a real `install.sh`, including installing a compiler, then a real
    `uninstall.sh`
  - the full test suite as an unprivileged user, with sanitizers where the
    toolchain has them

  Init layouts are tested against fake roots (`tests/hooks/run.sh`), checked
  against real Artix runit/dinit/OpenRC/s6 and Void images.
- **Fuzzing:** `make fuzz` (clang/libFuzzer) on the state parser. 36 million
  inputs in 60 s found nothing. A cheaper random-mutation pass runs in every
  `make test`.
- **Manual checklist** on real hardware for what fakes can't cover: suspend,
  udev timing, laptop runtime D3.

---

## 9. Hardware experiments

Run on the reference RTX 3060. Each needs the maintainer's OK first, and logs go
to persistent storage, not /tmp.

1. ~~Baseline idle power and P-state~~ **done**, see §2.5.
2. ~~Symbolic locks on GeForce~~ **done**: they work; base clock = 1320 MHz.
3. ~~Combined locks~~ **done**: they combine as expected.
4. ~~Suspend with locks~~ **done**: black screen on resume, cause unknown. This
   led to rule 11. Optional: one plain suspend with **no** locks, to learn
   whether suspend works on this machine at all. Only if the maintainer wants
   to risk another hard reset.
4b. ~~Numeric floor for `balanced`~~ **done**: a numeric floor holds; the
    symbolic one doesn't. See §2.5.
5. ~~Screen off (DPMS)~~ **done**: locks survive. See §2.5.
6. Boot/driver-load timing: not needed. `reload --wait` retries for a few
   seconds at boot, and there's no udev rule in 1.0.
7. Without persistence mode: not testable with a display attached (the display
   keeps the driver initialised). Handled by rule P6.
8. **[NEEDS TESTERS]** Laptop: does a lock survive runtime D3?

---

## 10. Open questions

1. ~~Mode names~~ **decided**: `powersave / balanced / max / off`.
2. ~~Init systems~~ **decided**:
   - first-class (tested in CI): systemd, OpenRC, runit, dinit, SysV
   - best effort: s6
   - later: NixOS module, Guix package
3. ~~`--force` threshold~~ **decided**: caps below 45% of max need `--force`.
4. ~~udev rule~~ **decided**: not in 1.0 (§6).
5. **[NEEDS TESTERS]**: laptops (runtime D3), multi-monitor high-refresh,
   RTX 20 / GTX 16.

---

## Sources

- NVML Device Commands: https://docs.nvidia.com/deploy/nvml-api/api/group__nvmlDeviceCommands.html
- nvidia-smi documentation: https://docs.nvidia.com/deploy/nvidia-smi/index.html
- nvml.h (go-nvml copy, NVML API 13): https://github.com/NVIDIA/go-nvml/blob/main/gen/nvml/nvml.h
- NVIDIA README, power management: https://download.nvidia.com/XFree86/Linux-x86_64/595.58.03/README/powermanagement.html
- NVIDIA README, dynamic power management: https://download.nvidia.com/XFree86/Linux-x86_64/595.58.03/README/dynamicpowermanagement.html
- Reading locked clocks (unanswered): https://forums.developer.nvidia.com/t/is-nvidia-hiding-locked-gpu-clock-or-am-i-missing-something-how-to-see-locked-gpu-memory-clocks/280966
- Turing memory lock not supported: https://forums.developer.nvidia.com/t/lock-memory-clock-in-pascal-and-turing-cards/244093
- HDMI audio and memory lock: https://forums.developer.nvidia.com/t/audio-pass-through-via-hdmi-often-interrupted/270808
- HDMI audio, lock didn't help: https://discuss.cachyos.org/t/audio-stutters-on-nvidia-3090-via-hdmi-memory-clock-settings-dont-help/33056
- Flicker on P-state change: https://forums.developer.nvidia.com/t/flickering-at-the-top-of-the-screen/256447
- Desktop lag fixed by graphics floor: https://discuss.cachyos.org/t/nvidia-fixing-system-stutters-lag-in-the-first-couple-of-seconds-of-doing-anything/19652
- Graphics floor and idle power: https://forums.developer.nvidia.com/t/frequent-lags-and-loss-of-smoothness-in-kde-plasma-with-dual-monitors-on-nvidia-3070-ti-wayland-x11/311888?page=2
- Power throttling with locked clocks (TensorRT): https://docs.nvidia.com/deeplearning/tensorrt/latest/performance/hw-sw-environment.html
- dGPU woken by monitoring tools: https://bbs.archlinux.org/viewtopic.php?id=302241
- udev RUN restrictions: https://www.freedesktop.org/software/systemd/man/255/udev.conf.html
- LACT locked clocks: https://www.gamingonlinux.com/2025/03/linux-gpu-configuration-tool-lact-adds-amd-rdna4-support-and-nvidia-locked-clocks-support/
- power-profiles-daemon origin: https://www.hadess.net/2020/09/power-profiles-daemon-new-project.html
