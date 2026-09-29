# PSP debugging helpers

Scripts used to develop the port against a real PSP over [PSPLink](https://github.com/pspdev/psplinkusb) and against PPSSPP. None of them is needed to build or play; see the top-level README for that.

All of them keep their state (pspsh fifo and log, `hw-*.log` transcripts) in `$PSP_WORK`, default `~/.unreal-psp`, and serve the console from `$PSP_HOST`, default `$PSP_WORK/psplink_host`. Put these in `$PSP_HOST`:

| file | what |
|---|---|
| `Unreal.prx` | the PSPLink build (`./build-psp.sh --psplink`), run from the host over USB; game data stays on the card |
| `EBOOT.PBP` | the card build, pushed with `push-and-run.sh` |
| `Unreal-lazy.ini` | the `System/Unreal.ini` to push along with it |
| `Unreal.int` | optional, pushed if present |

## Hardware

Start PSPLink on the PSP, connect USB. Then:

```
tools/psp/run-hw.sh <label> <seconds> [game arguments...]
```

runs `host0:/Unreal.prx` with the given arguments for that many seconds, ends the session with `reset` (never `exit`: after an `exit` the USB link is gone until the cable is reseated), and saves the transcript as `$PSP_WORK/hw-<label>.log` with a frame-rate summary. Useful arguments are listed at the end of `PSP-TODO.md`; the ones you will want first:

```
tools/psp/run-hw.sh dm 120 'DmRadikus.unr?Game=UnrealI.DeathMatchGame'
tools/psp/run-hw.sh replay 90 '-LOAD=0 -WALKDELAY=15 -AUTOWALK=12'   # load save slot 0, wait, walk forward
```

`push-and-run.sh <label> <seconds> [args]` first copies `EBOOT.PBP`, the ini and the `.int` to the card (md5-verified by copying them back), then runs like `run-hw.sh`. `pull-save.sh <slot>` copies `Save/Save<slot>.usa` (and hub files) from the card into the PPSSPP save folder so a save made on the console can be replayed in the emulator.

`pspsh_drive.py` is the pty wrapper the scripts use to feed `pspsh` from a fifo; pspsh block-buffers when its stdout is a file.

If the shell stops answering (`ls` prints no listing), the PSP is sitting on a crash dialog: power-cycle it and start PSPLink again.

## Emulator

`ppsspp-map.sh <map-url> <seconds>` runs one map in PPSSPP (macOS app path inside; edit for other systems) with the memory report on and tabulates heap use over time from `System/Unreal.log`. PPSSPP is several times faster than the PSP and capped at 20 fps here, so it is for logic and memory, never for frame rate. Its ini needs `MusicME=0` (no Media Engine in the emulator).
