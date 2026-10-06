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
| Subtests passed | 449 | 449 |
| Subtests failed | 2 | 5 |
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

## Results per suite

| Suite | SIMT pass | SIMT other | Classic pass | Classic other | Skipped |
|---|---|---|---|---|---|
| allocations | 6 | | 6 | | |
| api | 95 | | 92 | 3 fail | 69 |
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

### `api`: `work_group_suggested_local_size_1D`, `_2D`, `_3D` (classic mode)

In classic mode the maximum work-group size is 12 (one work-item per QPU). The test's "odd sizes"
case needs an odd work-group size that is not prime (counting 1 as prime) below the device maximum,
and there is none below 17. In SIMT mode the maximum is larger and the tests pass. This is a
limitation of the test, not a wrong result.

### `thread_dimensions`: time limit

These subtests didn't finish within 30 minutes. None of them reported a wrong result before being
stopped (their logs only show the test reducing its 128 MB buffer to what the CMA pool could
provide):

- SIMT: `full_2d_explicit_local`, `full_2d_implicit_local`, `full_3d_explicit_local`,
  `full_3d_implicit_local`, `quick_2d_explicit_local`, `quick_3d_explicit_local`
- Classic: `full_2d_explicit_local`, `full_3d_explicit_local`, `full_3d_implicit_local`

The suite launches very many kernels over a large range of work sizes. SIMT mode is slower here
because a SIMT chunk covers one row of a work-group in x, so work-groups narrower than 16 in x leave
lanes idle. Whether these subtests pass when run without the limit is untested.

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
