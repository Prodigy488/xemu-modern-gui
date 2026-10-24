# libfatx

FATX filesystem library from https://github.com/mborgerson/fatx (commit
fe41b2c), by Matt Borgerson, GPL v2 (see LICENSE.txt). `ext.c` is BSD
licensed (see its header).

Local change: optional `io_read` / `io_write` callbacks in `struct fatx_fs`
(fatx.h, fatx_dev.c, fatx.c) so the library can work on xemu's hard disk
image through QEMU's block layer instead of a host file. `ext.c`'s memrchr is renamed `ext_memrchr` so it
can't clash with the C library's.
- `fatx_ts_to_time_t()` clears the `struct tm` before `mktime()`; its
  `tm_isdst` was uninitialized, so times could come out an hour apart from
  one call to the next.
