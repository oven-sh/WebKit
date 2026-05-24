# macos-cross

Support files for cross-compiling JavaScriptCore for macOS on a Linux
builder (`Dockerfile.macos` / `macos-cross-release.sh`).

The only piece of the JSCOnly build that cannot run unmodified on a Linux
host is `mig`, the Mach Interface Generator. `Source/WTF/wtf/
PlatformJSCOnly.cmake`'s `APPLE` branch runs it on `MachExceptions.defs` to
generate the `mach_exc` RPC stubs that WTF's Mach-exception-based signal
handling (`wtf/threads/Signals.cpp`, used for Wasm out-of-bounds trapping
and VM watchdog traps) compiles into `libWTF.a`.

- `mig` — replacement for Apple's `mig.sh` driver: preprocesses the `.defs`
  with the cross clang against the macOS SDK and pipes the result to
  `migcom`. Same argument routing as Apple's script.
- `mach/` — minimal Mach type definitions so Apple's `migcom` (from
  [apple-oss-distributions/bootstrap_cmds](https://github.com/apple-oss-distributions/bootstrap_cmds),
  a plain flex/bison program) compiles as a **Linux host tool**. Values are
  copied from the macOS SDK's `<mach/message.h>`. These headers are only
  used to *compile migcom itself* — the code migcom emits is plain text and
  every size in its output is a target-compiled `sizeof()` expression, so
  the host-side definitions cannot leak into the target ABI. The msgh_ids
  in the generated dispatcher (2405–2410, the `mach_exc` subsystem) come
  from the SDK's `mach_exc.defs`, not from these stubs.

`Dockerfile.macos` builds `migcom` from a pinned `bootstrap_cmds` tag using
these headers and puts `mig` on `PATH` before configuring WebKit.
