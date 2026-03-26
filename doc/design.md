# ngx_lua_cpp Design Document

## 1. Overview

**ngx_lua_cpp** is a C++ extension framework for OpenResty/Nginx that lets developers write high-performance C++ logic — including C++20 coroutines — and expose it to Lua scripts running inside OpenResty's `ngx_lua` module. It is built on the [iris](https://github.com/paintdream/iris) header-only concurrency library and is distributed under the MIT License.

### Goals

- Enable C++ extensions for OpenResty without requiring any nginx or OpenResty header files at build time.
- Provide seamless Lua ↔ C++ type conversion and function binding via iris.
- Support C++20 coroutines that integrate transparently with OpenResty's Lua coroutine scheduler.
- Operate as a self-contained shared library (`.so` / `.dll`) loaded at runtime through LuaJIT's `require`.

## 2. Repository Structure

```
ngx_lua_cpp/
├── CMakeLists.txt              # Build configuration (CMake, C++20)
├── LICENSE                     # MIT License
├── README.md                   # User-facing documentation
├── cmake/
│   └── FindLuaJIT.cmake        # CMake module to locate LuaJIT
├── demo/
│   └── init_ngx_lua_cpp.lua    # Lua bootstrap script for nginx workers
├── doc/
│   └── design.md               # This document
├── src/
│   ├── ngx_lua_cpp.h           # Public header — exported types and macros
│   ├── ngx_lua_cpp.cpp         # Core implementation
│   └── iris/                   # iris library (header-only, vendored)
│       ├── iris_common.h       # Common utilities, allocators, helpers
│       ├── iris_common.inl     # Inline implementations for iris_common.h
│       ├── iris_lua.h          # Lua binding framework (iris_lua_t)
│       ├── iris_dispatcher.h   # Thread pool and warp/strand abstractions
│       └── iris_coroutine.h    # C++20 coroutine integration
└── web/
    ├── nginx.conf              # Example nginx configuration with debug APIs
    └── index.html              # Browser-based debug console UI
```

## 3. Architecture

The diagram below shows how the main components interact at runtime inside an OpenResty worker process:

```
┌─────────────────────────────────────────────────────────────────┐
│  OpenResty Worker Process                                       │
│                                                                 │
│  ┌──────────────┐   require("ngx_lua_cpp")   ┌──────────────┐  │
│  │  ngx_lua /   │ ──────────────────────────► │ libngx_lua_  │  │
│  │  LuaJIT VM   │ ◄──────────────────────────  │ cpp.so       │  │
│  │              │   Lua ↔ C++ calls           │              │  │
│  └──────┬───────┘                             └──────┬───────┘  │
│         │                                            │          │
│         │  ngx.sleep / yield / resume                │          │
│         ▼                                            ▼          │
│  ┌──────────────┐   hook process_events()    ┌──────────────┐   │
│  │  nginx event  │ ◄─────────────────────────│ ngx_hooker_t │   │
│  │  loop         │ ─────────────────────────►│ (singleton)  │   │
│  └──────────────┘   notify()                 └──────┬───────┘   │
│                                                      │          │
│                                              ┌───────▼──────┐   │
│                                              │ ngx_lua_     │   │
│                                              │ cpp_t        │   │
│                                              │ ┌──────────┐ │   │
│                                              │ │async_    │ │   │
│                                              │ │worker    │ │   │
│                                              │ │(thread   │ │   │
│                                              │ │pool)     │ │   │
│                                              │ └──────────┘ │   │
│                                              │ ┌──────────┐ │   │
│                                              │ │main_warp │ │   │
│                                              │ │(strand)  │ │   │
│                                              │ └──────────┘ │   │
│                                              └──────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### Runtime Flow

1. **Loading** — The nginx `init_worker_by_lua_block` directive executes `require("ngx_lua_cpp")`, which calls `luaopen_ngx_lua_cpp`. This registers the `ngx_lua_cpp_t` type with iris_lua and constructs the global `ngx_hooker_t` singleton.
2. **Initialization** — Lua code calls `lib.new()` to create an `ngx_lua_cpp_t` instance and `inst:start(N)` to spin up N worker threads.
3. **Request handling** — Inside `content_by_lua_block`, Lua calls C++ methods (e.g., `inst:sleep(40)`). If the method is a coroutine, the framework yields the current ngx_lua coroutine, dispatches work to the thread pool, and resumes the coroutine with the result once complete.
4. **Event loop integration** — `ngx_hooker_t` hooks `ngx_event_actions.process_events` so that every event-loop iteration drains completed async tasks back to the main thread via `main_warp->poll()`.

## 4. Key Components

### 4.1 `ngx_lua_cpp_t` (src/ngx_lua_cpp.h, src/ngx_lua_cpp.cpp)

The primary class exposed to Lua. Each instance owns:

| Member | Type | Purpose |
|---|---|---|
| `async_worker` | `shared_ptr<iris_async_worker_t<>>` | Thread pool for background work |
| `main_warp` | `unique_ptr<ngx_warp_t>` | Strand that serializes work back to the nginx main thread |
| `main_warp_guard` | `unique_ptr<preempt_guard_t>` | Keeps `main_warp` in the preempted (accepting tasks) state |
| `main_thread_index` | `size_t` | Index of the pseudo-thread representing the nginx main thread in the async worker |

**Lua API** (registered in `lua_registar`):

| Lua method | C++ method | Description |
|---|---|---|
| `lib.new()` | `place_new_object` | Creates a new `ngx_lua_cpp_t` instance |
| `inst:start(n)` | `start(size_t)` | Starts the thread pool with `n` worker threads |
| `inst:stop()` | `stop()` | Stops the thread pool and drains pending tasks |
| `inst:is_running()` | `is_running()` | Returns `true` if the thread pool is active |
| `inst:get_hardware_concurrency()` | `get_hardware_concurrency()` | Returns `std::thread::hardware_concurrency()` |
| `inst:sleep(ms)` | `sleep(size_t)` | Example coroutine: sleeps on a worker thread, returns the value |
| `inst:__async_worker__(ptr)` | `__async_worker__(void*)` | Introspection: get/set the internal async worker (for advanced use) |

### 4.2 `ngx_hooker_t` (src/ngx_lua_cpp.cpp)

A process-wide singleton (`get_instance()`) responsible for integrating with the nginx event loop. It is constructed on first access (typically when `luaopen_ngx_lua_cpp` is called).

**Initialization steps:**

1. Opens the host process handle (`dlopen(NULL)` / `GetModuleHandle(NULL)`).
2. Resolves nginx and ngx_lua symbols by name using `dlsym` / `GetProcAddress`:
   - `ngx_event_actions` — the global event actions table
   - `ngx_posted_delayed_events` — the delayed events queue
   - `ngx_http_lua_module` / `ngx_stream_lua_module` — module descriptors
   - `ngx_http_lua_get_co_ctx` / `ngx_stream_lua_get_co_ctx` — coroutine context accessors
3. Replaces `ngx_event_actions.process_events` with its own `proxy_ngx_process_events`, saving the original pointer.

**Event loop hook** (`process_events`):

On each nginx event loop iteration:
1. Removes any placeholder sleep events that were inserted by `ngx_lua_cpp_yield` (to trick ngx_lua into yielding).
2. Calls `process_events()` on every registered `ngx_lua_cpp_t` instance, which polls the main warp for completed tasks.
3. Calls the original `process_events` to perform normal nginx event processing.

**Yield/Resume mechanism:**

- **`ngx_lua_cpp_yield`** — Called when a C++ coroutine needs the Lua side to yield. It invokes `ngx.sleep(0)` internally to put the Lua coroutine into a sleeping state, making it compatible with ngx_lua's scheduler.
- **`ngx_lua_cpp_resume`** — Called when a C++ coroutine completes. It inserts the coroutine context into `ngx_posted_delayed_events` so nginx will wake and resume the Lua coroutine on the next event loop iteration. Return values are stored in the Lua registry keyed by the `lua_State*` pointer.

**Auto-discovery of `co_ctx` event queue offset:**

The offset of the `sleep.event_queue` field within `ngx_http_lua_co_ctx_t` (and the stream equivalent) is not known at compile time. The hooker discovers it at runtime by:
1. Calling `ngx.sleep(0)` once on the real yield function.
2. Observing which entry appears at the tail of `ngx_posted_delayed_events`.
3. Computing the byte offset from the `co_ctx` pointer to that queue entry.

This approach avoids any dependency on nginx internal header files.

### 4.3 `ngx_warp_t` (src/ngx_lua_cpp.h)

A specialization of `iris_warp_t` configured for the nginx environment:

```cpp
struct ngx_warp_t : iris_warp_t<iris_async_worker_t<>, false, ngx_warp_t> { ... };
```

- The `strand = false` template parameter means it operates in warp (non-strand) mode — routines are dispatched but not strictly serialized.
- `flush_warp()` calls `ngx_hooker_t::notify()`, which triggers `ngx_event_actions.notify()` to wake the nginx event loop so it will process the newly queued tasks promptly.
- The enter/leave/suspend/resume warp hooks are no-ops because the nginx main thread context does not need additional locking.

### 4.4 Coroutine Return Value Wrapping (src/ngx_lua_cpp.h)

For C++ coroutines that return a value (i.e., `iris_coroutine_t<T>` where `T` is non-void), the framework wraps the Lua-side coroutine function so that:

1. When the C++ coroutine yields, the Lua coroutine also yields (via the ngx.sleep trick).
2. When the C++ coroutine completes with a return value, the value is stored in the Lua registry.
3. When the Lua coroutine is resumed, a wrapper function (`get_coroutine_returns`) retrieves the stored values from the registry and returns them as Lua return values.

This is implemented through the `iris_lua_traits_t` specialization for coroutine function pointers and the `ngx_iris_wrap_coroutine_with_returns_key` registry entry.

### 4.5 iris Library (src/iris/)

The [iris](https://github.com/paintdream/iris) library is a header-only C++ concurrency framework providing:

| Header | Purpose |
|---|---|
| `iris_common.h` / `.inl` | Utilities: block allocators, binary search helpers, atomic guards, debug macros |
| `iris_lua.h` | Full-featured Lua binding: automatic type marshalling, metatables, ref counting, coroutine integration, error handling |
| `iris_dispatcher.h` | `iris_async_worker_t` (thread pool) and `iris_warp_t` (strand/warp task serialization) |
| `iris_coroutine.h` | `iris_coroutine_t<T>` — C++20 coroutine wrapper with `co_await` support for warp switching (`iris_switch`) |

## 5. Nginx Integration Details

### 5.1 No Header Dependency

ngx_lua_cpp deliberately avoids including any nginx or OpenResty headers. Instead, `ngx_lua_cpp.cpp` contains minimal forward declarations of the nginx structures it needs:

- `ngx_http_request_t` (first three fields only: `signature`, `connection`, `ctx`)
- `ngx_stream_session_t` (enough fields to reach `ctx`)
- `ngx_stream_lua_request_t` (first two fields: `connection`, `session`)
- `ngx_module_t` (first field: `ctx_index`)
- `ngx_event_actions_t` (full function pointer table)
- `ngx_queue_t` (doubly-linked list node)

If a custom nginx build changes the layout of these structures, the forward declarations can be updated without pulling in the full nginx source tree.

### 5.2 Symbol Resolution

All nginx/ngx_lua symbols are resolved at runtime via `dlsym` (Linux) or `GetProcAddress` (Windows). This means:

- The shared library has no link-time dependency on nginx.
- It works with any nginx binary that exports the required symbols.
- The library can detect HTTP vs. stream context at runtime by checking `ngx.config.subsystem`.

### 5.3 HTTP and Stream Support

The framework supports both `ngx_http_lua` and `ngx_stream_lua` subsystems. At registration time (`ngx_hooker_t::registar`), it queries `ngx.config.subsystem` and stores a boolean flag in the Lua registry. This flag is checked on every yield/resume to determine which set of context accessors and yield functions to use.

## 6. Threading Model

```
                    ┌──────────────────────────┐
                    │   nginx main thread       │
                    │                           │
                    │  ┌─────────────────────┐  │
                    │  │ main_warp (strand)   │  │
                    │  │ polled each event    │  │
                    │  │ loop iteration       │  │
                    │  └─────────────────────┘  │
                    └────────────┬───────────────┘
                                 │
                    ┌────────────┼───────────────┐
                    │            │               │
              ┌─────▼────┐ ┌────▼─────┐  ┌──────▼───┐
              │ worker 0  │ │ worker 1  │  │ worker N │
              │ (thread)  │ │ (thread)  │  │ (thread) │
              └───────────┘ └──────────┘  └──────────┘
```

- **Main thread** — The nginx event loop thread. `main_warp` queues are polled here during `process_events`. All Lua interactions happen on this thread.
- **Worker threads** — Managed by `iris_async_worker_t`. C++ coroutines use `co_await iris_switch<ngx_warp_t>(nullptr)` to move execution off the main thread, perform blocking work, then `co_await iris_switch(current)` to return results to the main warp.
- **Priority tasks** — Tasks marked as priority are redirected to `main_warp` via the `set_priority_task_handler` callback, ensuring they execute on the main thread.

## 7. Build System

The project uses **CMake** (minimum version 3.12) with C++20 enabled:

```
cmake -B build .
cmake --build build
```

**Key build details:**

- **Output**: Shared library (`libngx_lua_cpp.so` on Linux, `ngx_lua_cpp.dll` on Windows).
- **Lua dependency**: LuaJIT by default; can switch to Lua 5.1–5.4 via the `USE_LUA_VERSION` cache variable.
- **LuaJIT discovery**: Uses `cmake/FindLuaJIT.cmake`. Set the `LUA_DIR` environment variable if LuaJIT is not found automatically (typically points to OpenResty's installation directory).
- **Platform support**: Linux (GCC 11+, Clang 14+), Windows (Visual Studio 2019+). Non-MSVC builds add `-fPIC`.
- **Link dependencies** (non-MSVC): `m`, `dl`, `stdc++`, `pthread`, and the Lua library.

## 8. Demo and Debug Console

### 8.1 Lua Bootstrap (`demo/init_ngx_lua_cpp.lua`)

A minimal Lua module loaded once per worker via `init_worker_by_lua_block`. It creates a singleton `ngx_lua_cpp_t` instance and starts 4 worker threads:

```lua
local lib = require("ngx_lua_cpp")
local inst = lib.new()
inst:start(4)
return inst
```

Subsequent `require("init_ngx_lua_cpp")` calls in request handlers return the cached instance.

### 8.2 Web Debug Console (`web/`)

> **Security Warning:** The debug console exposes an endpoint that executes arbitrary Lua code on the server. It must **never** be deployed in production. Restrict access via firewall rules, bind to `localhost` only, and enable authentication if used in any shared environment.

A self-contained debug interface consisting of:

- **`nginx.conf`** — Configures an HTTP server on port 8080 with API endpoints:
  - `POST /api/exec` — Execute arbitrary Lua code in a sandboxed environment
  - `GET /api/logs` — Retrieve nginx error/access log tails
  - `POST /api/logs/clear` — Truncate log files
  - `GET /api/info` — Server status and ngx_lua_cpp state
- **`index.html`** — A single-page application providing a Lua terminal and log viewer with syntax highlighting and keyboard shortcuts.

## 9. Extending ngx_lua_cpp

To add a new C++ function accessible from Lua:

1. **Declare** the method in `ngx_lua_cpp_t` (in `ngx_lua_cpp.h`).
2. **Implement** it in `ngx_lua_cpp.cpp`. For async operations, return `iris_coroutine_t<T>` and use `co_await iris_switch` to move between warps.
3. **Register** it in `ngx_lua_cpp_t::lua_registar`:
   ```cpp
   lua.set_current<&ngx_lua_cpp_t::my_function>("my_function");
   ```
4. **Call** from Lua:
   ```lua
   local result = inst:my_function(args)
   ```

The iris_lua binding system handles automatic type marshalling between Lua and C++, including support for integers, floats, strings, booleans, tables, and userdata.
