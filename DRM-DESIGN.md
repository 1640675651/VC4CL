# Running VC4CL on the vc4 DRM driver

Status: implemented and tested on hardware. The kernel side is in
[`../vc4-compute/`](../vc4-compute/README.md); the VC4CL side is the DRM backend in `src/hal/DRM.cpp`. See
[Test results](#test-results).

## Goal

Today, VC4CL bypasses the kernel when running as root on the KMS graphics stack. It writes the V3D
registers through `/dev/mem` and takes memory from the firmware's `gpu_mem` heap through the mailbox.
[KMS.md](KMS.md) lists what goes wrong: power management, VPM reservation, and kernels that time out but
keep running. The patches described there make that work headless, but VC4CL still can't safely share
the GPU with OpenGL.

The goal here is to make the `vc4` kernel driver the only thing that touches V3D. Mesa (OpenGL) and
VC4CL (OpenCL) both submit jobs to it, and it decides when each one runs. In short:

- **OpenCL and OpenGL can run at the same time** without corrupting each other.
- **No `/dev/mem` and no `gpu_mem` carve-out.** Buffers are normal vc4 buffer objects (BOs) from CMA.
- **Timeouts are handled in one place.** The driver stops a hung kernel and keeps its memory alive
  until the QPUs have really stopped.

Not a goal: letting unprivileged users run OpenCL. See [Security](#security).

## How the GPU is shared: one job at a time

V3D has 12 QPUs (shader processors). They're shared by coordinate, vertex and fragment shaders
(OpenGL) and by "user programs" (OpenCL kernels). A QPU program can't be preempted: once started, it
runs until it ends. Three ways to share were considered:

1. **Take turns.** A compute job waits until no GL job is running, then runs alone. GL jobs submitted
   in the meantime wait behind it.
2. **Fully concurrent.** All QPUs are shared dynamically. OpenCL can then take all 12 QPUs and starve
   GL until the 100 ms GL hangcheck resets the GPU. VPM must be permanently partitioned.
3. **Concurrent with a fixed split** (QPU reservation registers). GL always makes progress, but the
   OpenCL work-group size limit drops from 12 to however many QPUs OpenCL gets.

**This design uses option 1.** It's the only one where neither side can break the other:

- VPM can be reserved for OpenCL only while a compute job runs.
- The GL hangcheck never runs during a compute job.
- A reset after a compute timeout only aborts the compute job.

The cost is that GL rendering stalls for as long as a compute job runs, since there's no
preemption. How long that is depends on how VC4CL maps kernel launches to jobs:

- **Usually, one `clEnqueueNDRangeKernel` is one job.** VC4C's "work-group loop" makes the QPUs
  iterate over all work-groups within a single launch. It's enabled whenever the kernel uses the
  work-group-ID UNIFORMs, which in practice is the common case: every kernel tested so far ran
  "all at once", including the demo's 128 work-groups in one 0.6 ms launch.
- Only kernels without the work-group loop are launched once per work-group, giving many small jobs.

So a long kernel is one long job, and the desktop freezes for its whole duration. A 60 Hz frame has
a 16.7 ms budget. VC4CL therefore needs to split large NDRanges into several jobs of a few
milliseconds each when GL may be running. The loop currently starts at work-group ID 0
(hard-coded in the generated code), so splitting needs a work-group ID offset in VC4C as well as
VC4CL. This is not designed yet. Option 3 could be added later as an opt-in for headless,
compute-heavy systems.

## How the vc4 driver runs jobs today

(`vc4_gem.c` and `vc4_irq.c` at Raspberry Pi kernel `c1432b4bae5b`, 6.6.31.)

- Every job is a `struct vc4_exec_info`. Submitting it (`vc4_queue_submit()`) assigns a sequence number
  (seqno) and a `dma_fence`, adds the fence to all of the job's BOs, and appends the job to
  `bin_job_list`.
- The hardware has two control-list threads: CT0 (binning) and CT1 (rendering). The head of
  `bin_job_list` is binning. When it's done (`FLDONE` interrupt), it moves to `render_job_list`. When
  rendering is done (`FRDONE`), `finished_seqno` is incremented, the fence is signalled, and the job
  moves to `job_done_list`. A worker then frees it (`vc4_complete_exec()`).
- Binning of job N+1 overlaps with rendering of job N. Jobs complete in submission order.
- `vc4_submit_next_bin_job()` starts the head of the bin list. There's already a rule for holding a job
  back: if its perfmon differs from the one of the job being rendered, it waits, and
  `vc4_irq_finish_render_job()` starts it once rendering is done. Compute jobs use the same mechanism.
- Every 100 ms, a hangcheck timer checks whether CT0 or CT1 made progress. If not, it calls
  `vc4_reset()`, which power-cycles V3D through runtime PM, then calls `vc4_irq_reset()`. That cancels
  the bin job and force-completes the render job.

## Kernel changes

### The compute job

A compute job is a `vc4_exec_info` with `is_compute` set. It has no control lists, only a list of QPU
programs (code address and UNIFORMs address per QPU) and a timeout. It goes through
`vc4_queue_submit()` like a GL job, so it gets a seqno, a fence, BO references and a runtime-PM
reference, and is freed by `vc4_complete_exec()`.

Its lifecycle:

1. **Queued** at the tail of `bin_job_list`, like any job.
2. **Started** when it reaches the head of the bin list *and* the render list is empty. At that point
   nothing else is running on V3D, because the head of the bin list is the only job that could be
   binning. Starting (`vc4_compute_start()`):
   1. flushes the GPU caches (`vc4_flush_caches()`), so the QPUs see the code and UNIFORMs just
      written by the CPU;
   2. sets `VPMBASE` to 16 (16 × 256 bytes = 4 KB, the most VC4C uses);
   3. clears the user-program counters in `SRQCS`;
   4. writes one `SRQUA`/`SRQPC` pair per program;
   5. starts the completion poll timer.
3. **Completed** when the completed-programs counter in `SRQCS` equals the number of programs.
   `vc4_compute_finish()`:
   1. sets `VPMBASE` back to 0, as `vc4_v3d_init_hw()` does;
   2. increments `finished_seqno`, signals the fence and moves the job to `job_done_list`, all exactly
      as for a finished render job;
   3. starts the next bin job.

   Incrementing `finished_seqno` is valid because a started compute job is the only job in flight,
   so all earlier seqnos are finished.

GL jobs submitted while a compute job is queued or running wait behind it in the bin list. When the
last render job before a waiting compute job finishes, `vc4_irq_finish_render_job()` must start the
next bin job even if no perfmon changed, so that condition gets `|| nextbin->is_compute`.

The compute job's fence is added to its BOs with `DMA_RESV_USAGE_WRITE`, not `READ`, since a kernel may
write any of its buffers. Later readers of those BOs, such as GL texturing from an OpenCL result, then
wait for it.

### Detecting completion

The completion poll is an hrtimer: every 100 µs for the first 10 ms of a job, then every 1 ms. It
compares the completed count in `SRQCS` with the job's program count, all under `job_lock`. The timer
only runs while a compute job runs.

An interrupt would be nicer. VC4C kernels end by raising a QPU host interrupt (`not irq, qpu_num`), but
those are reported through the undocumented debug registers (`DBQITE`/`DBQITC`), and it's unverified
whether they reach the ARM under KMS. That's an open question; polling works regardless.

### Timeouts and reset

- Each compute job has its own deadline: the `timeout_ms` from userspace, 30 s by default, at most 10
  minutes. The 100 ms GL hangcheck doesn't apply: it measures control-list progress, and a compute job
  has no control list. While the head of the bin list is a compute job, the hangcheck ignores it.
- When the deadline passes, the poll timer marks the job as timed out and schedules the existing
  `hangcheck.reset_work`. That saves the hang state and calls `vc4_reset()`.
- `vc4_reset()` then calls `vc4_irq_reset()`. Normally that cancels the bin job and force-completes
  the first render job. A compute job at the head of the bin list changes this:
  - **Not started yet** (the reset was for a GL hang): `vc4_cancel_bin_job()` leaves it queued. It
    starts once rendering drains.
  - **Started:** `vc4_irq_reset()` calls `vc4_compute_cancel()` *instead of* cancel plus
    force-complete. Nothing renders during a compute job, and the next GL job, which cancelling
    starts, might be a render-only job that the force-complete would wrongly mark done.
    `vc4_compute_cancel()` checks whether the reset really stopped the QPUs. All user-program counters in `SRQCS`
    must read 0, which only a real power cycle achieves. If so, complete the job with `-ETIMEDOUT`.
    That signals the fence with that error, frees the BOs, and starts the next job.
  - **Started, but the counters aren't 0:** the QPUs may still be running and writing memory. The
    driver marks compute as **wedged** until reboot:
    - the job's fence is signalled with `-EIO`;
    - the job itself is parked on a list that's never freed, so its BOs (the memory the QPUs might
      still write) are never reused;
    - all further compute submissions fail with `-EIO`.
- `vc4_reset()` can only power-cycle V3D if runtime PM can suspend it. On the 6.6 kernel it can't:
  `vc4_v3d_bind()` never drops the reference it takes (see KMS.md, issue 1). This patch therefore also
  backports that one-line fix from `rpi-6.12.y`, a `pm_runtime_put_autosuspend()` at the end of
  `vc4_v3d_bind()`. With it, `vc4_reset()` really power-cycles V3D through the firmware power domain,
  which KMS.md showed resets the QPUs. It also fixes GL hang recovery, and V3D now powers off when idle.

### New uAPI

These definitions live in `vc4_compute_drm.h`, shared by the kernel module and VC4CL. The ioctl
numbers start at 0x20, well away from upstream's 0x00–0x0e. `DRM_VC4_PARAM_SUPPORTS_QPU_COMPUTE`
(0x1000) for `DRM_IOCTL_VC4_GET_PARAM` lets VC4CL detect the patched driver.

| ioctl | Purpose |
|---|---|
| `DRM_IOCTL_VC4_SUBMIT_QPU` | Queue a compute job: BO handles, QPU programs (code and UNIFORMs bus addresses), timeout, optional `in_sync`/`out_sync` syncobjs. Returns the job's seqno. |
| `DRM_IOCTL_VC4_WAIT_QPU` | Wait for a compute job's seqno, then return its status: 0, `-ETIMEDOUT` (timed out, GPU reset) or `-EIO` (wedged). Restartable like `WAIT_SEQNO`. |
| `DRM_IOCTL_VC4_QPU_BO_ADDR` | Return the GPU bus address of a BO. VC4C kernels take absolute buffer addresses in their UNIFORMs. |

Checks done by `SUBMIT_QPU`:

- the caller has `CAP_SYS_RAWIO`, or is in the `compute_gid` group (see [Security](#security));
- compute isn't wedged;
- `flags` and padding are 0;
- the job has 1 to 16 programs. 16 is the depth of the hardware request queue.
- the job has at least one BO;
- every code and UNIFORMs address lies inside one of the job's BOs.

The last check catches bugs early; it isn't a security boundary.

The job status reported by `WAIT_QPU` comes from a per-device record of the last failed compute
seqno. That's enough for VC4CL, which waits for every launch before submitting the next. A caller
that submits several jobs and only waits for the last one may miss an earlier failure. The fence error
on `out_sync` covers that case.

### Security

GL shaders are validated by the kernel (`vc4_validate_shaders.c`) precisely because V3D has no MMU: a
QPU program can read and write any physical memory. OpenCL kernels can't be validated that way. So
every compute ioctl requires `CAP_SYS_RAWIO`, the same capability `/dev/mem` needs today. That's no
less safe than the current setup.

The module parameter `compute_gid` additionally allows one group, disabled (`-1`) by default. The
check is done before `capable()`, so allowed group members don't cause capability-denial logs. The GID
is from the initial user namespace, so a container can't map its own group onto it. Members of that
group effectively get root rights, so the administrator has to choose it deliberately:

- **`video`** (GID 44 on Raspberry Pi OS) is the natural choice for single-user systems. Its members can
  already run GPU or VPU code through the firmware (`/dev/vcio`, `/dev/vchiq`), which is how VC4CL ran
  without root on the firmware stack, so nothing is added.
- **`render`** must not be used. The render node is meant to be safe for every GL client, and the
  group includes service accounts (on this Pi: `vnc`).

### Files changed

| File | Change |
|---|---|
| `vc4_compute_drm.h` (new) | uAPI described above |
| `vc4_compute.c` (new) | the three ioctls, BO lookup, start, completion poll, finish |
| `vc4_drv.h` | `vc4_exec_info`: `is_compute`, programs, timeout, start/timeout state. `vc4_dev`: `compute` (poll timer, wedged flag, failed seqno, wedged job list). Prototypes. |
| `vc4_gem.c` | Hooks: start compute jobs in `vc4_submit_next_bin_job()`, ignore them in the hangcheck, write fences in `vc4_update_bo_seqnos()`, free the program list in `vc4_complete_exec()`. Init/destroy of the compute state. `vc4_flush_caches()`, `vc4_lock_bo_reservations()`, `vc4_queue_submit()`, `vc4_complete_exec()` and `vc4_queue_hangcheck()` made non-static. `vc4_queue_submit()` gets an optional out-parameter for the job's seqno: the existing `SUBMIT_CL` returns `vc4->emit_seqno` after queueing, which is wrong if another process submitted in between, and would hide a compute job's failure status. |
| `vc4_irq.c` | `vc4_cancel_bin_job()` leaves a waiting compute job queued; `vc4_irq_reset()` hands a running one to `vc4_compute_cancel()`. `vc4_irq_finish_render_job()` also starts a waiting compute job. |
| `vc4_drv.c` | ioctl table entries, `GET_PARAM` capability |
| `vc4_v3d.c` | runtime-PM fix in `vc4_v3d_bind()` |
| `Makefile` | `vc4_compute.o` |

## VC4CL changes

A new HAL backend, `src/hal/DRM.cpp`, selected when `/dev/dri/renderD*` is a vc4 device whose
`GET_PARAM` reports `SUPPORTS_QPU_COMPUTE`:

- **Memory:** `DRM_IOCTL_VC4_CREATE_BO`, `DRM_IOCTL_VC4_MMAP_BO`, and `QPU_BO_ADDR` for the device
  address. Buffers come from CMA, so `gpu_mem` is no longer involved and the device's global memory is
  the CMA size.
- **Execution:** `SUBMIT_QPU` with every BO the launch references: code, UNIFORMs, kernel argument
  buffers. Then `WAIT_QPU`. VC4CL's per-launch timeout becomes the job's `timeout_ms`, and a failed
  status becomes `CL_OUT_OF_RESOURCES`, as now.
- **Caches:** vc4 BOs are write-combined on the CPU side and the driver flushes the GPU caches at job
  start, so no explicit flushing is needed.
- **Registers:** none touched. The register-poking and Mailbox paths stay for the firmware stack.

The executor collects all buffers a launch uses (kernel buffer, temporary buffers, argument buffers)
and passes them to `SystemAccess::executeQPU()`. The DRM backend removes duplicate handles, since the
driver locks each BO once: a buffer passed as two arguments would otherwise make the submission fail.

As implemented:

- `DRM::create()` probes `/dev/dri/renderD128`–`191` for a vc4 device that reports
  `SUPPORTS_QPU_COMPUTE`, then checks permission with a trial `QPU_BO_ADDR`. If the driver supports
  compute but the process isn't allowed to use it, VC4CL prints a hint about root or `compute_gid`.
  Without it, VC4CL would silently fall back to the old backends.
- The DRM backend is used for memory and execution together, or not at all. It's skipped when
  `VC4CL_NO_DRM` or any of the existing `VC4CL_EXECUTE_*`/`VC4CL_MEMORY_*` overrides is set.
- In DRM mode, no V3D or Mailbox object is created. The V3D one would set `power/control` to `on` and
  block the driver's reset. This also means performance counters are unavailable in DRM mode.
- System queries: QPU count and VPM size come from `GET_PARAM(V3D_IDENT1)`; global memory from
  `CmaTotal`; the QPU clock through a read-only firmware query (`firmwarePropertyCall()`, which needs
  no `Mailbox` instance); the temperature and ARM clock from sysfs.
- `v3d_reset` refuses to run in DRM mode, since the driver resets V3D itself.

## Testing plan

1. Install the module (see the vc4-compute README; it also needs a regenerated initramfs) and check
   that it loads and that `GET_PARAM` reports the capability.
2. `tests/compute_test` in vc4-compute, steps 1–2: hand-assembled QPU programs that only end their
   thread (`nop.thrend`), on 1 and 12 QPUs, and 100 jobs in a row. Check `WAIT_QPU` status 0 and the
   per-job latency. Then invalid submissions must fail with `-EINVAL`.
3. `compute_test` steps 3–4: a program that branches to itself forever, with a 500 ms timeout.
   `WAIT_QPU` must return `-ETIMEDOUT` (not `-EIO`), and the next job must work.
4. Run GL (glmark2) during steps 2 and 3. GL must keep working, with no hangcheck resets other than the
   one for the compute timeout.
5. VC4CL DRM backend: the demo, then clpeak, with and without the desktop running.

## Test results

2026-10-02, Raspberry Pi 3B, kernel `6.6.31+rpt-rpi-v8` with the patched module, desktop (Xorg)
running, `tests/compute_test` from vc4-compute:

| Test | Result |
|---|---|
| Normal user | `QPU_BO_ADDR` fails with `EPERM`, as intended |
| `compute_gid=44` (`video`), normal user in `video`, no `sudo` | tests 1–2 pass. The same user with all supplementary groups dropped (`setpriv --clear-groups`) gets `EPERM`, and passes again with only GID 44 added back. Setting `compute_gid` back to `-1` restores `EPERM`. |
| 1 / 12 QPUs, program that only ends its thread | status 0. The first job takes 0.29 ms (V3D powers on); 12 QPUs take 0.14 ms. |
| 100 jobs of 12 QPUs | 0.13 ms per job, submit plus wait |
| Code address outside the BOs, misaligned code, 17 programs | all `-EINVAL` |
| Program that never ends, 500 ms timeout | `-ETIMEDOUT` after 500 ms. The kernel log has one `Resetting GPU.` per timeout, with no reset loop and no wedge. The `SRQCS` counters are 0 afterwards, so V3D was really power-cycled. |
| Next job after the timeout | status 0, 0.15 ms |
| The above twice, while glmark2 (build, texture, shading) runs | glmark2 completes normally (exit 0, score 45). Exactly 2 GPU resets, one per compute timeout. Compute jobs take 0.15–0.16 ms. |

Between tests, V3D runtime-suspends as expected, which confirms the backported `vc4_v3d_bind()` fix.
The buffer addresses (e.g. `0xee185000`) are CMA memory seen through the GPU's `0xC0000000`
(uncached) alias. `gpu_mem` isn't used.

### VC4CL through the DRM backend

Same setup, desktop running, VC4CL run **as a normal user** with `compute_gid=44`:

| Test | Result |
|---|---|
| `clinfo` | uses `/dev/dri/renderD128`; 300 MHz; global memory 256 MiB (CMA), was 76 MiB (`gpu_mem`); max allocation 128 MiB |
| vector-add demo | correct; one job with 8 programs and 3 buffers, 391 µs (599 µs with register poking) |
| never-ending kernel, then a second kernel in the same process | first: `CL_OUT_OF_RESOURCES` after VC4CL's 30 s timeout, with one GPU reset; second: correct results in 1 ms |
| clpeak | **runs to the end** (exit 0), with no GPU resets and no kernel warnings. Register poking hung at the `float4` compute test and corrupted kernel memory. Bandwidth `float`…`float16`: 0.23–1.44 GB/s; it was skipped before, because the allocation failed. Single-precision compute: 0.61 / 1.19 / 2.26 / 3.89 / 6.12 GFLOPS, about twice register poking's 0.30 / 0.60 (cause not investigated). Integer: 0.18–1.42 GOPS, 24-bit: 0.60–5.85. Transfers: writes 1.0–1.2 GB/s, reads 0.12–0.13 GB/s. Kernel launch latency: 23 µs. |

Reading from vc4 BOs on the CPU is slow (0.13 GB/s), because the driver maps them write-combined,
i.e. uncached for reads. Programs reading large results back would profit from a cached mapping with
explicit cache maintenance; that's a possible follow-up.

## Open questions

- Whether QPU host interrupts reach Linux. If they do, the poll timer can be replaced.
- Desktop latency: how long can a compute job be before GL stutter becomes noticeable? Should the
  driver enforce a maximum, or leave it to VC4CL?
- Upstreamability: a compute ioctl for a GPU without an MMU is unlikely to be accepted upstream, so
  this stays an out-of-tree module unless the security question can be answered.
