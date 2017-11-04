# libwasm
libwasm is a cross platform Web Assembly JIT engine with no external dependencies and good performance.  It was created to allow non-web programs to easily support web assembly.  In addition to providing an interpreter libwasm also specifies a stable ABI and libc allowing cross platform execution outside of the web.

# WasmRT
WasmRT - The web assembly runtime builds upon libwasm and is intended to allow applications to ship in webassembly without requiring recompilation on different hosts.  WasmRT provides a consistent and cross platform ABI intended to work well with command line applications.

# Why
Many programs embed language runtimes for extensibility such as Lua or Javascript.  While great for most purposes traditional embedded programmability forces a specific language on the user. Wasm allows extensibility to be added in a language neutral way.

