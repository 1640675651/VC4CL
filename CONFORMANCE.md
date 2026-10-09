# OpenCL conformance test results

Results of the Khronos OpenCL conformance tests (OpenCL-CTS) for VC4CL with the DRM back-end on a
Raspberry Pi 3, in both execution modes of VC4C:

- **SIMT**: the default. Work-items run in the 16 SIMD lanes of a QPU, see VC4C's `doc/SIMT.md`.
- **Classic**: one work-item per QPU, selected with `VC4CL_NO_SIMT=1`.

The device implements the OpenCL 1.2 **embedded profile**. Images, `double`, `half` and 64-bit
integers are not supported, so the tests for them are skipped. These are results of running the CTS,
not a conformance submission to Khronos.

## Summary

| | SIMT | Classic |
|---|---|---|
| Suites run | 18 | 18 |
| Subtests passed | 449 | 452 |
| Subtests failed | 2 | 2 |
| Subtests timed out (30-minute limit of the runner) | 6 | 3 |
| Subtests skipped (unsupported features) | 150 | 150 |

All failures are known and explained below. None of them is caused by a known bug in VC4CL, VC4C or
VC4CLStdLib.

## Test setup

| | |
|---|---|
| Hardware | Raspberry Pi 3 (VideoCore IV, 12 QPUs), 1 GB RAM |
| OS | Raspberry Pi OS, kernel 6.6.31+rpt-rpi-v8, vc4 DRM driver with the compute patches (`vc4-compute`) |
| Memory | CMA pool, 256 MB |
| OpenCL-CTS | `main`, commit 9feccbb (2026-09-29) |
| VC4CL | branch `drm` |
| VC4C | branch `simt` |
| VC4CLStdLib | branch `conformance` |
| Front-end | clang 14.0.6 |

The suites were run with a script (`run_cts.sh`) that runs every subtest as a separate process. A
subtest counts as passed if the process exits with 0 and prints `PASSED`, and as skipped if it
prints that the test isn't supported. Each subtest has a 30-minute limit.

Most suites were last run on 2026-10-05, after the latest compiler fixes. `allocations`, `atomics`,
`buffers`, `computeinfo`, `events`, `mem_host_flags`, `multiples`, `profiling` and
`thread_dimensions` were last run on 2026-10-04, before VC4C's printf support, its register
allocation fixes and its rounding of float literals. `thread_dimensions`' two failing subtests were
rerun with the fix for global sizes above 2^24.

On 2026-10-06 classic mode changed to the same work-group limits as SIMT mode (192 instead of 12).
`api` and `basic` were rerun in classic mode with the new limits. The other classic-mode results are
from before; with the larger limits, classic mode now also runs the largest `thread_dimensions`
configurations (see below), so its `thread_dimensions` timeouts are expected to match SIMT mode's.

On 2026-10-09 `api` and `basic` were rerun in both modes after the work-item loops were completed
(kernels with barriers or `__local` memory accept 192 work-items in both modes, SIMT kernels with
barriers run several work-groups at the same time; VC4C's `doc/SIMT.md`, roadmap item 3), with the
same results as above (`cts-results/stage5`).

## Results per suite

| Suite | SIMT pass | SIMT other | Classic pass | Classic other | Skipped |
|---|---|---|---|---|---|
| allocations | 6 | | 6 | | |
| api | 95 | | 95 | | 69 |
| atomics | 13 | | 13 | | |
| basic | 64 | | 64 | | 48 |
| buffers | 91 | | 91 | | 12 |
| commonfns | 17 | 2 fail | 17 | 2 fail | |
| computeinfo | 2 | | 2 | | 3 |
| contractions | 16 | | 16 | | |
| events | 30 | | 30 | | |
| geometrics | 8 | | 8 | | |
| mem_host_flags | 9 | | 9 | | |
| multiples | 7 | | 7 | | |
| printf | 15 | | 15 | | 7 |
| profiling | 22 | | 22 | | 11 |
| relationals | 17 | | 17 | | |
| select | 22 | | 22 | | |
| thread_dimensions | 6 | 6 timeout | 9 | 3 timeout | |
| vectors | 9 | | 9 | | |

Running all of these takes about 6 hours per mode, most of it in `thread_dimensions`, `basic`,
`vectors` and `relationals`.

## Remaining failures

### `commonfns`: `mix` and `mixf` (both modes)

The hardware only rounds toward zero. `mix(x, y, a)` is computed as `x + (y - x) * a`, and with
large operands the two roundings toward zero add up to more than the absolute error the CTS allows
(e.g. `mix(0x1.8fe16p+25, 0x1.7c4aap+26, 0x1.6e2d4cp-1)` returns `0x1.48eceep+26`, the reference is
`0x1.48ecef3cf1ap+26`). The embedded profile allows rounding toward zero, but this test's bound
doesn't account for it. Passing it would need software rounding to nearest for this function.

### Fixed: `api`: `work_group_suggested_local_size_1D`, `_2D`, `_3D` (classic mode)

These failed while classic mode reported a maximum work-group size of 12 (one work-item per QPU): the
test's "odd sizes" case needs an odd work-group size that is not prime (counting 1 as prime) below
the device maximum, and there is none below 17. Since 2026-10-06 classic mode reports the same limits
as SIMT mode (192 work-items; kernels with barriers or `__local` memory still accept only 12), and
the three subtests pass.

### `thread_dimensions`: time limit and slowness

These subtests didn't finish within 30 minutes. None of them reported a wrong result before being
stopped (their logs only show the test reducing its 128 MB buffer to what the CMA pool could
provide):

- SIMT: `full_2d_explicit_local`, `full_2d_implicit_local`, `full_3d_explicit_local`,
  `full_3d_implicit_local`, `quick_2d_explicit_local`, `quick_3d_explicit_local`
- Classic: `full_2d_explicit_local`, `full_3d_explicit_local`, `full_3d_implicit_local`

The suite runs global sizes of up to 1024 × 1024 × 1024 work-items. It skips the largest ones if the
local size is small (below 16 work-items for more than 8192² work-items in total, below 64 for more
than 16384²), "as it will take a long time". In classic mode the maximum work-group size is 12, so the
test picks local sizes of 8 or 11 and skips them. In SIMT mode it picks 93 or 128 and runs them,
about 2^30 work-items each, at about 0.5 million work-items per second in both modes.

So SIMT mode gets more work, it isn't slower. Measured for `quick_3d_explicit_local` with timestamps:
the 52 configurations both modes run took 447 s in classic and 432 s in SIMT mode. Classic mode then
skipped the configurations of 1024³ and 1023³ work-items and passed after 458 s. SIMT mode ran the
first of them and had not finished it after 459 s. The other subtests run larger global sizes in
both modes, which is why they also take longer than 30 minutes in classic mode.

**Rerun with a 6-hour limit** (2026-10-06 02:09 to 12:00, stopped before all subtests ran):

| Subtest | SIMT | Classic |
|---|---|---|
| `full_2d_implicit_local` | pass, 40 min | (passed in 30 min before) |
| `quick_2d_explicit_local` | pass, 2 h 11 min | (passed in 15 min before) |
| `quick_3d_explicit_local` | timeout after 6 h | (passed in 5–8 min before) |
| `full_2d_explicit_local` | stopped after 1 h, 669 configurations passed | not run |
| `full_3d_implicit_local` | not run | not run |
| `full_3d_explicit_local` | not run | not run |

**Why a single configuration takes so long.** Every work-item of the test kernel adds 1 to its own
output word with `atom_add` (and sets error bits with `atom_or` if its ID is out of range). The result
buffer can't hold one word per work-item for the large sizes, so the test covers the index range in
windows: for every window it fills the buffer with zeros (`clEnqueueFillBuffer`), launches the
**whole** NDRange (work-items outside the window skip the atomic), maps the buffer and checks every
word on the CPU. For 1024 × 1024 × 1024 work-items and the 80 MB the CMA pool provided, these are 52
windows, so 52 launches of 2^30 work-items, about 56 billion work-item executions for one
configuration. `quick_3d_explicit_local` has two such configurations (1024³ and 1023³), which is why
it needs roughly a day in SIMT mode. On the side of VC4CL and VC4C, these costs add up:

- **The kernel can't use SIMT mode** (it uses atomics), in either mode, so every chunk of the
  NDRange is a single work-item. VC4CL writes a block of UNIFORMs (IDs, sizes, arguments, about 14
  words) for every chunk on the host, and a launch carries at most 64K UNIFORM words, so a launch of
  2^30 work-items is about 230,000 GPU jobs. Each job is a separate ioctl, and the driver polls for
  its completion every 100 µs (every 1 ms after the first 10 ms). This keeps the CPU busy for the
  whole run.
- **Global atomics are serialized**: every atomic takes the GPU-wide hardware mutex and does a DMA
  read and a DMA write, so all 12 QPUs together reach about 0.5 million atomics per second.
- **The fill runs on the CPU, one `memcpy` per 4-byte pattern**, i.e. 20 million calls for an 80 MB
  buffer, into write-combined memory, once per window.
- **The check** reads the whole buffer, mapped uncached, on the CPU once per window. This cost is
  the test's own.

None of this is wrong behavior, but it makes kernels with many work-items and little work per
work-item much slower than necessary whenever they can't use SIMT mode. Possible improvements:

1. Let the QPU loop over the work-items of its chunk and compute their IDs itself, instead of one
   UNIFORM block per work-item written by the host. This would reduce the host work and the number of
   GPU jobs by orders of magnitude for every non-SIMT kernel with independent work-items, and fits
   the work-item loops planned for larger work-groups (VC4C `doc/SIMT.md`, roadmap item 3).
2. Fill buffers with word-sized stores instead of a `memcpy` per pattern, or on the GPU.
3. Cheaper global atomics, e.g. batching the atomics of a QPU under one mutex lock where the
   addresses allow it (harder).

## Skipped subtests

- **Images** (no image support): image tests in `basic`, `api` and `profiling`.
- **64-bit integers, `double` and `half`**: `intmath_long*` (`basic`), `buffer_read_half`
  and `buffer_write_half` (`buffers`), and the `long`, `double` and `half` tests of `printf`.
- **OpenCL 2.0 and later**: the `size_t` and `ptrdiff_t` tests of `printf` (OpenCL 3.1), SVM, pipes, sub-groups, device-side enqueue, program-scope variables,
  linear IDs, work-group functions, immutable memory, kernel cloning and other newer API queries
  (`basic`, `api`, `buffers`, `computeinfo`).

## Suites not run

- `compiler`: its build needs `spirv-as` (SPIRV-Tools), which isn't installed.
- `integer_ops`, `conversions` and `math_brute_force`: not run yet. They take many hours to days on
  this GPU; `math_brute_force` has a reduced mode (`-w`).
- `device_partition` and `multiple_device_context`: not run yet.
- Not applicable to this device: `images` (no image support), `half`, the OpenCL 2.0+ suites
  (`c11_atomics`, `device_execution`, `generic_address_space`, `pipes`, `SVM`, `subgroups`,
  `workgroups`, `non_uniform_work_group`, `device_timer`, `spirv_new`, `spir`), and the interop
  suites (`gl`, `gles`, `d3d10`, `d3d11`, `vulkan`).

## Bugs found and fixed through the CTS

The first runs had many failures; all except the ones above were fixed:

- **VC4CL:**
  - rectangular buffer copies;
  - device limits: image limits without image support, local memory size;
  - argument and property validation;
  - event wait lists across contexts (a user event of another context blocked the queue forever);
  - user event status and callbacks;
  - the error code for failed buffer allocations;
  - no fallback to the firmware back-end when DRM compute is not permitted (it hung until timeout);
  - `printf`, which was not implemented.
- **VC4C, compiler bugs affecting both modes:**
  - TMU loads with unused lanes at address 0, which corrupted memory and crashed the system;
  - three loop vectorizer bugs;
  - work-item functions for out-of-range dimensions;
  - `reqd_work_group_size` above 12 rejected at compile time;
  - global IDs and sizes beyond 2^24;
  - merging of element copies sharing flags;
  - two register allocation fixups breaking conditionally written values (intermittent wrong results,
    depending on the nondeterministic allocation order);
  - float literals rounded to nearest instead of toward zero.
- **VC4C, SIMT mode:** a work-group uniform value stored at a per-work-item address, and
  multi-dimensional work-groups.
- **VC4CLStdLib:** `clamp` with scalar bounds.

VC4C's `doc/SIMT.md` ("Roadmap", "Known bugs") describes the compiler bugs in detail.

## Reproducing

Build OpenCL-CTS from its `main` branch against the OpenCL and SPIR-V headers, one suite at a time
and with a single job (the 1 GB Pi runs out of memory with parallel builds):

```sh
cd OpenCL-CTS/build
make -j1 test_basic
```

Point the ICD loader at the VC4CL build and run each subtest, with `VC4CL_NO_SIMT=1` for classic
mode:

```sh
export OCL_ICD_VENDORS=/path/to/icd    # directory with a .icd file naming VC4CL/build/src/libVC4CL.so
export LD_LIBRARY_PATH=/path/to/VC4C/build/src
cd test_conformance/basic
./test_basic --list
VC4CL_NO_SIMT=1 ./test_basic vstore_local
```

The user running the tests needs access to the GPU's compute jobs. The `compute_gid` parameter of
the vc4 module has to be set to the user's group (e.g. `echo 44 | sudo tee
/sys/module/vc4/parameters/compute_gid` for the `video` group); it resets on reboot.

Some suites need a longer time limit: `vector_creation` (`basic`) takes about 19 minutes.
