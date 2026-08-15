# ngx_lua_cpp
Developing OpenResty ngx_lua extensions with C++ (including coroutines). Based on [iris](https://github.com/paintdream/iris) library.

## License

ngx_lua_cpp is distributed under MIT License.

## Build

ngx_lua_cpp requires a C++ 20 compatible compiler (Visual Studio 2019+, GCC 11+, Clang 14+).

Just use CMake to generate project with CMakeLists.txt.

If you got problem in finding LuaJIT, please configure its installation path (usually it could be found at OpenResty's installation directory) to environment variable LUA_DIR.

Notice that ngx_lua_cpp is an isolated component for LuaJIT. You do not need any header files from OpenResty. All related OpenResty/Nginx declarations could be find at ngx_lua_cpp.cpp. Modify these declarations if it is incompatible with your custom OpenResty build. (Usually you needn't do this.)

## Install

Configure **nginx.conf**, add these lines to your stream/http block:

```lua
	init_worker_by_lua_block {
		package.path = package.path .. ";[[ngx_lua_cpp source directory]]/demo/?.lua"
		package.cpath = package.cpath .. ";[[ngx_lua_cpp binary directory]]/?.dll;[[ngx_lua_cpp binary directory]]/lib?.so;"
		require("init_ngx_lua_cpp")
	}
```

**init_gnx_lua_cpp.lua** is a lua module shared by all requests. It initializes a **ngx_lua_cpp** instance and return it as its module instance. 

```lua
-- init_ngx_lua_cpp.lua

local lib = require("ngx_lua_cpp")
if lib then
	local inst = lib.new()
	inst:start(4) -- thread count
	return inst
else
	ngx.log(ngx.ERR, "ngx_lua_cpp not found!")
end

```

For http, configure at **http/server** block:

```lua
	location ~ /lua.html {
		default_type text/html;
		content_by_lua_block {
			local inst = require("init_ngx_lua_cpp")
			local value = inst:sleep(40)
			ngx.say("ngx_lua_cpp http demo! Running " .. tostring(inst:is_running()) .. " | " .. tostring(value))
		}
	}
```

For stream, configure at **stream** block)

```lua
	server {
		listen 1234;

		content_by_lua_block {
			local inst = require("init_ngx_lua_cpp")
			local value = inst:sleep(40)
			ngx.say("ngx_lua_cpp stream demo! Running " .. tostring(inst:is_running()) .. " | " .. tostring(value))
		}
	}
```

## Usage

In ngx_lua_cpp.cpp, you could see an example function: **a coroutine-based asynchornized "sleep"**.

It is registered in ngx_lua_cpp_t::lua_registar():

```C++
void ngx_lua_cpp_t::lua_registar(lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>) {
	// ...
	lua.set_current<&ngx_lua_cpp_t::sleep>("sleep");
}
```

Here is the implementation:

```C++
coroutine_t<size_t> ngx_lua_cpp_t::sleep(size_t millseconds) {
	warp_t* current = co_await iris_switch<warp_t>(nullptr);
	std::this_thread::sleep_for(std::chrono::milliseconds(millseconds));
	co_await iris_switch(current);
	co_return std::move(millseconds);
}
```

You can use coroutines and the warp (strand) system from the iris library, which are fully compatible with OpenResty/Nginx's task scheduler.

## Demo Center

The repository ships a **demo center** (`web/`) that showcases what this project is good at:
server-side logic written as plain C++20 coroutines, with blocking work automatically
offloaded to a worker pool while the nginx worker stays responsive.

### Run it

```bat
:: 1. build the library (x86 to match a 32-bit OpenResty, e.g. WinNMP's nginx.exe)
cmake -S . -B build -A Win32
cmake --build build --config Debug

:: 2. one-command helper: syncs config/UI into web/run, starts nginx, probes the server
powershell -ExecutionPolicy Bypass -File web/run-server.ps1          :: start
powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Restart :: stop, sync, start
powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Stop    :: stop

:: 3. open the demo center
start http://localhost:8080
```

Or manually: `nginx.exe -p C:/Code/ngx_lua_cpp/web/run -c conf/nginx.conf`.
`web/nginx.conf` resolves the Lua sources and the built library relative to the nginx
prefix (`web/run`), so no path editing is needed. `web/run-server.ps1` copies it (plus
`web/index.html` and `mime.types`) into `web/run/` on every start, keeping the run
prefix in sync with the canonical files. The web UI has five tabs: a Lua console, the
Mandelbrot renderer, the concurrent fetcher, the job queue and a log viewer.

### Demo 1: Mandelbrot parallel renderer

`inst:mandelbrot(width, height, iterations, cx, cy, zoom, mode)` returns
`(bmp_bytes, elapsed_ms)` where `bmp_bytes` is a 24bpp BMP.

* `mode = 0` (parallel): one row-task per row is **pre-dispatched** to the worker pool
  (`iris_awaitable_parallel` + `dispatch()`), then the coroutine **fan-ins** by awaiting
  them in order. On a 4-thread pool this typically yields **3.5x-4x speedup** over serial.
* `mode = 1` (serial): all rows rendered on a single worker thread (the comparison baseline).

The render itself is plain synchronous C++ code — no callbacks, no partitioning
bookkeeping: the coroutine just looks like a normal blocking function.

### Demo 2: concurrent HTTP fetch (fan-out / fan-in)

`inst:fetch(urls_table, timeout_ms)` returns `(entries, total_elapsed_ms)`; each entry is
`(url, ok, status, bytes, elapsed_ms, info)`. Every URL is fetched by a **blocking
WinHTTP request running on the worker pool**, all in parallel. Fetching N URLs takes
about as long as the slowest one. `info` holds the first 256 bytes of the body (or the
error message).

### Demo 3: in-memory job queue

`inst:job_submit(kind, payload)` / `inst:job_query(id)` / `inst:job_list()` share one C++
state map across all requests. Jobs run as background coroutines that **hop between the
worker pool and the nginx warp**: each chunk is computed on the pool, then
`co_await iris_switch(main_warp)` publishes progress on the nginx thread, then
`co_await iris_switch(nullptr)` returns to the pool. Kinds:

* `count_primes` — segmented sieve up to `payload.limit` (default 10,000,000).
* `sleep_steps` — sleeps `payload.ms` in `payload.steps` steps (a deterministic
  progress ticker).

### Why these demos

| Capability | Where it shows |
|---|---|
| C++20 coroutines as "normal blocking code" | `mandelbrot`/`fetch` bodies read top-to-bottom, no callbacks |
| Blocking work offloaded, worker never blocked | `/api/info` answers in ms while a heavy render runs |
| Parallel fan-out + fan-in | row tasks / URL tasks pre-dispatched, joined by `co_await` |
| Coroutine return values flow back into Lua | `co_return` of tuples/vectors/maps become Lua tables |
| Cross-request shared C++ state | the job queue lives in the C++ instance, shared by all workers/requests |
| warp-based scheduling | job progress hops pool &rarr; warp &rarr; pool via `iris_switch` |
