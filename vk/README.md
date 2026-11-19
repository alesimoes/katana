# kvk: katana's Vulkan compute layer

Single file `kvk.h`. In exactly one translation unit do `#define KVK_IMPL` before `#include "kvk.h"`.
It needs only the Khronos headers (`ref/vk/include`, `VK_NO_PROTOTYPES`); `vulkan-1.dll` / `libvulkan.so.1` is loaded at run time.
Failures never call `exit()`: functions return 0 / a handle with the `b`/`p`/`cb` field zeroed and `kvk_error()` gives the text. Single thread only.

## API
- `int kvk_init(KvkInfo*)`: 1 ok, 0 failure. Picks the discrete GPU; `KATANA_VK_DEVICE=<index>` forces one (invalid index = failure). Requires Vulkan 1.2.
  Enables whatever exists: cooperative_matrix, shaderFloat16, storageBuffer16BitAccess, shaderInt8/storageBuffer8BitAccess, shaderInt16, shader_integer_dot_product.
  `KvkInfo`: name, vram_bytes (largest DEVICE_LOCAL heap), subgroup_size, limits, flags `coopmat fp16 storage16 int8 int16 int_dot`, list `coop[]` of shapes.
- `kvk_shutdown()`, `kvk_active()`, `kvk_error()`, `kvk_stats()`.
- Buffers: `kvk_buf_create(bytes, KVK_USAGE_RW)`, `kvk_buf_destroy`, `kvk_bytes_allocated()` (for the VRAM budget),
  `kvk_buf_write` (completes before returning), `kvk_buf_write_q` (copies the data immediately into the upload area and enqueues the copy in the stream; runs on flush/wait),
  `kvk_buf_read` = `kvk_read_sync` (flush + wait + read). Any offsets/sizes. The upload area (coherent) and download area (cached) are persistent, grow on demand up to 64 MB, and larger writes go in chunks.
- `kvk_pipe_create(spirv, bytes, n_buffers, push_bytes, spec_ids, spec_vals, n_spec)`: all bindings are storage buffers in set 0, binding 0..n-1; push a multiple of 4; 32-bit spec constants.
- Flow: `kvk_begin` (optional), `kvk_dispatch(pipe, bufs, n, push, push_bytes, gx, gy, gz)`, `kvk_flush` (submits, does not wait), `kvk_wait`.
  Compute/transfer -> compute/transfer barrier before each dispatch and copy (`kvk_set_auto_barrier(0)` + `kvk_barrier()` to optimize later).
- Recorded graph: `kvk_rec_begin()`, `kvk_dispatch...`, `KvkRec r = kvk_rec_end()`, `kvk_rec_submit(r)` as many times as you like, `kvk_rec_free(&r)`, `kvk_rec_cancel()`.

## Recorded graph rules
Buffers (handles) and push constants are frozen at recording time. What changes per token (position, current token, cache length) must be
in a small buffer written with `kvk_buf_write_q` before `kvk_rec_submit` (the submit flushes the write first; the order is guaranteed by a barrier).
`write_q`, `read`, `flush`, `wait` are not allowed during recording. The command buffer uses SIMULTANEOUS_USE, so resubmitting without waiting is valid.
Destroying a buffer/pipeline used by a graph invalidates the graph (the caller's responsibility).

## Example
```c
#define KVK_IMPL
#include "kvk.h"
KvkInfo I; if (!kvk_init(&I)) { /* falls back to OpenCL/CPU; kvk_error() */ }
KvkPipe p = kvk_pipe_create(spv, sizeof spv, 2, 12, NULL, NULL, 0);
KvkBuf x = kvk_buf_create(n*4, KVK_USAGE_RW), y = kvk_buf_create(n*4, KVK_USAGE_RW);
kvk_buf_write(x, 0, hx, n*4);
KvkBuf io[2] = { x, y }; struct { float a, b; uint32_t n; } pc = { 3, 1, n };
kvk_dispatch(p, io, 2, &pc, 12, (n+63)/64, 1, 1);
kvk_buf_read(y, 0, hy, n*4);           /* flush + wait + read */
```

## Decisions and limits
- Descriptors: one set per (pipeline, buffer tuple), in a hash table; pools of 256 sets, a new pool when it fills up (FREE_DESCRIPTOR_SET to release when destroying a buffer/pipeline). Binding is always of the whole buffer (no offset).
- Each buffer has its own memory (no sub-allocation): respects `maxMemoryAllocationCount` (4096 on some cards); the engine should use a few large buffers.
- Ring of 8 command buffers + fences; `kvk_wait` waits for all and resets the upload area.
- Upload memory preferring coherent host-visible outside device-local; download preferring cached (invalidates the range if not coherent).
- There are no timeline semaphores, separate transfer queue or asynchronous queue; everything is on a single compute queue.
- Not tested: Linux (only `#ifdef _WIN32` guards), GPUs without Vulkan 1.2, real VRAM pressure (OOM), device loss.

## Build and test
    glslang.exe -V --target-env vulkan1.2 --vn addc_spv -o addc.h addc.comp   (likewise axpb, addp)
    b.bat                      (builds kvk_test.exe)
    kvk_test.exe               (discrete GPU)   |   set KVK_TEST_DEV=0 & kvk_test.exe   (picks an index; 0=Intel, 1=NVIDIA on this machine)

## Batch prompt shaders (`vk/shaders`)
`sh vk/shaders/gen.sh` generates all the SPIR-V headers. `mm.comp` (cooperative matrix product, copy of `tools/vkq4k`; only the variants
used: 5 types x 64x64 / 64x32w4 / 64x16w4 and the glu ones with fp16 output), `cvt`, `rms_wg16`, `gather`, `qkv_post`, `att_split` (three kernels,
`KATANA_VK_ATT=3`) and `att_flash` (single-launch attention, default). Variables: `KATANA_VK_BATCH=0` (prompt token by token), `KATANA_VK_PASS`
(largest batch per pass, default 256), `KATANA_VK_CAP`, `KATANA_VK_MINB` (smallest batch, default 2), `KATANA_VK_SKIP` (mask of skipped phases, for measurement only).
Hidden command `katana vkcheck model text.txt [ngen] [ntokens]` compares batch and token by token.
