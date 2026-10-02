# Running VC4CL on the KMS graphics stack

Current Raspberry Pi OS (Bullseye and later) drives the GPU with the Linux `vc4` DRM driver
(`dtoverlay=vc4-kms-v3d`) instead of the closed VideoCore firmware driver. VC4CL was written for the
firmware stack. Running as root, it uses
Mailbox memory from the firmware's `gpu_mem` heap and runs kernels by writing the V3D registers directly
through `/dev/mem` ("register poking"). Under KMS, those register writes collide with the `vc4` driver,
which considers itself the sole owner of V3D.

This file describes what goes wrong, what the patches in `src/hal/V3D.cpp` and `src/hal/hal.cpp` change,
and what still doesn't work.

## Symptoms

Tested on a Raspberry Pi 3B, Raspberry Pi OS Bookworm arm64, kernel `6.6.31+rpt-rpi-v8`, running as root
with the defaults (Mailbox memory, register poking):

- Small kernels work, for example a vector add.
- `clpeak` hangs at random. Sometimes it gets through the `float` and `float2` compute tests, and
  sometimes it hangs on the first one. It happens with and without the desktop running.
- After a hang, kernel memory is corrupted. The kernel log shows
  `WARNING ... mm/vmalloc.c:475 vmap_small_pages_range_noflush` (a page-table entry that should be empty
  is already filled), then `Internal error: Oops`, then
  `BUG: Bad page map in process ... pte:e000ff800000c3`. `fork()` then fails with `ENOMEM` while hundreds
  of MB are still free, and anything reading `/proc/<pid>` of the stuck process (`ps`, `pgrep -a`) blocks
  in uninterruptible sleep. Only a reboot recovers the system.

These were ruled out:

- **The kernels.** clpeak's `compute_sp_v1`…`v16` compile cleanly with VC4C and run correctly in VC4CL's
  software emulator (`VC4CL_EMULATOR=1`).
- **Power supply.** `glmark2` loads V3D heavily through the `vc4` driver and runs stably.

## Issues found

### 1. The vc4 driver powers V3D off when it is idle

The `vc4` driver uses runtime PM for V3D, with a 40 ms autosuspend delay (`vc4_v3d.c`). When no DRM client
is using the GPU, for example with no desktop running, V3D is clock-gated
(`vc4_v3d_runtime_suspend()` → `clk_disable_unprepare()`). VC4CL knows nothing about this and writes
registers of a block that may be off. A powered-off V3D returns `0xdeadbeef` for every register read.

Whether V3D happens to be powered at launch time depends on what else is running. That explains why a
hang can hit the very first kernel.

Workaround without the patch: `echo on | sudo tee /sys/bus/platform/devices/3fc00000.v3d/power/control`.
This setting resets to `auto` on every boot.

### 2. The vc4 driver reserves no VPM memory for user programs

Each time V3D is powered on, the driver runs (`vc4_v3d.c`, `vc4_v3d_init_hw()`):

```c
/* Take all the memory that would have been reserved for user
 * QPU programs, since we don't have an interface for running
 * them, anyway.
 */
V3D_WRITE(V3D_VPMBASE, 0);
```

`v3d_info` confirms it on a running system: `VPM Memory size: 12 KB`, `VPM User size: 0 KB`. VC4C kernels
pass every global store through VPM rows starting at 0. With no reservation, the hardware VPM allocator
can hand the same rows to OpenGL vertex and coordinate shading. This matters whenever a GL client (the
desktop, glamor) runs at the same time as an OpenCL kernel.

The compile-time VPM size isn't affected: VC4CL takes it from the total VPM size in `V3D_IDENT1`, not
from `VPMBASE`.

### 3. A timed-out kernel keeps running and its memory is freed

When a launch times out (`executor.cpp`, 30 s minimum), VC4CL returns `CL_OUT_OF_RESOURCES`. Nothing
stops the QPUs. The kernel's code and UNIFORM buffers are then freed, the application frees its buffers,
and the next launch reuses that memory. QPUs that are still running end up executing whatever the memory
now holds and writing to addresses taken from it. VideoCore IV has no MMU, so those writes can land
anywhere in RAM, including kernel page tables. That matches the `Bad page map` above.

So whatever causes a hang (issue 1, issue 2, or something else), this turns it into memory corruption.

### 4. V3D registers were accessed through a non-volatile pointer

`V3D::v3dBasePointer` was a plain `uint32_t*`. The compiler may merge, reorder or drop register writes,
and may hoist the read in the completion polling loop out of the loop. There was also no barrier between
writing the code and UNIFORMs and starting the QPUs. Nothing proves this caused the hangs, but it was
wrong either way.

### Not caused by KMS, but related

- **No `libvcsm.so` on arm64.** Raspberry Pi's arm64 `libraspberrypi0` doesn't ship `libvcsm.so`, so
  `VC4CL_MEMORY_CMA` / `VC4CL_MEMORY_VCSM` fail with `Could not find supported libvcsm.so`. The kernel
  side (`/dev/vcsm-cma`, module `vc_sm_cma`) exists. When CMake can't find the library, the build defines
  `NO_VCSM` and those modes are ignored.
- **Mailbox memory comes from `gpu_mem`.** As root, buffers come from the firmware heap, so the device
  reports `gpu_mem` as its global memory size (76 MB here). Large clpeak allocations fail with
  `CL_OUT_OF_RESOURCES` (-5).

## What the patches change

All changes take effect only when the `vc4_v3d` platform driver is bound, detected via
`/sys/bus/platform/drivers/vc4_v3d/*.v3d`. On the firmware stack, behavior is unchanged except for
issue 4.

| Issue | Change |
|---|---|
| 1 | `V3D` sets `power/control` to `on` on construction and waits for `runtime_status` to be `active`. The original value is restored on destruction. Before each launch, it re-checks power and that `V3D_IDENT0` reads `"V3D"`, and refuses to launch otherwise. |
| 2 | Before each launch, sets `VPMBASE` to 16 (16 × 256 B = 4 KB, the most VC4C uses) if it is lower. |
| 3 | On a timeout, the GPU is marked hung. VC4CL then tries to reset V3D the same way the `vc4` driver does in `vc4_reset()`: it sets `power/control` to `auto`, waits for `runtime_status` to become `suspended` (V3D is in the firmware `V3D` power domain, so suspending powers it off), then powers V3D back on. If that works, VC4CL continues normally. If not, VC4CL stays in the hung state: later launches fail immediately and `SystemAccess::deallocateBuffer()` leaks buffers instead of freeing them. **See "Timeout recovery" below: with the desktop running, the reset does not work.** |
| 4 | `v3dBasePointer` is `volatile`, and a `dsb sy` is issued before the QPUs are started. |

## Results

Headless (`lightdm` stopped), with the patches installed, `clpeak` runs to the end. Afterwards, the
kernel log has no `WARNING`, `Oops`, `Bad page map` or `vmalloc error` entries. Without the patches, the
same setup hung and corrupted kernel memory.

## Timeout recovery (tested with the desktop running)

Test: a kernel that spins forever and only reads memory. Results:

- After 30 s, VC4CL detected the timeout and refused further launches. It also refused to free the
  buffers. That part works.
- V3D did not runtime-suspend within 2 s of setting `power/control` to `auto`, so VC4CL didn't reset
  it. `runtime_status` stayed `active` even after the process exited. Something in the desktop stack
  holds a runtime-PM reference. `v3d_info` kept showing `Program queue: 12/0/0` (12 requests,
  0 completed), so the QPUs kept spinning.
- The next GL job (glmark2) couldn't get any QPUs. The `vc4` hangcheck then called `vc4_reset()`
  about once a second (`[drm] Resetting GPU.`). That didn't help either: `VPMBASE` kept the value VC4CL
  had written, which shows V3D was never actually power-cycled (a resume would have zeroed it).
- The firmware mailbox tag `SET_ENABLE_QPU` (0x30012) was acknowledged but had no effect.
- Only a reboot clears the QPUs.

Conclusion: with a GL client running, a QPU user program that never terminates can't be stopped from
userspace. The `vc4` driver's own reset can't stop it either, so GL stays wedged until reboot. Treat any
kernel timeout as needing a reboot unless the system is headless and the reset log says
`V3D was reset by power-cycling it`.

## Remaining limitations

- **Root only.** It still uses `/dev/mem` and writes to sysfs.
- **Not safe while a GL client uses the GPU.** VC4CL's launches aren't serialized with the `vc4`
  driver's jobs. The cache flushes and `SRQCS` resets in `V3D::executeQPU()` can hit GL jobs that are
  running, and VPM can still be contested. Run headless, for example with `sudo systemctl stop lightdm`.
- **Concurrent VC4CL processes.** If two processes both change `power/control`, the first one to exit
  restores its saved value, and the other may lose power mid-kernel. It then times out and recovers.
- **A power cycle can't undo writes that already happened.** If a runaway kernel wrote somewhere
  before the reset, that damage remains.

## The proper fix

Do the work the patches fake inside the `vc4` kernel driver: a privileged ioctl (gated on
`CAP_SYS_RAWIO`, since there is no MMU) that runs a list of QPU user programs. The driver would then:

- take a runtime-PM reference;
- reserve VPM;
- queue the job with bin/render jobs so it never overlaps them;
- write the `SRQ*` registers;
- signal completion through the normal seqno and fence path;
- reset the GPU through `vc4_reset()` on a hang.

VC4CL would get a `DRM` backend in `src/hal/` that allocates through `DRM_IOCTL_VC4_CREATE_BO` and submits
through the new ioctl. That works with the desktop running, needs neither `/dev/mem` nor `gpu_mem`, and
memory comes from CMA.

## Testing the patches

```bash
sudo systemctl stop lightdm          # headless: nothing else using V3D
sudo VC4CL_DEBUG=system ./a.out      # should log "V3D is managed by the vc4 DRM driver"
sudo clpeak --compute-sp             # check `sudo dmesg` afterwards for WARNING/Oops/Bad page map
```
