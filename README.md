# ngx_lua_cpp
Developing OpenResty ngx_lua extensions with C++ (including coroutines). Based on [iris](https://github.com/paintdream/iris) library.

## License

ngx_lua_cpp is distributed under MIT License.

## Build

ngx_lua_cpp requires a C++ 20 compatible compiler (Visual Studio 2019+, GCC 11+, Clang 14+).

Just use CMake to generate project with CMakeLists.txt.

If you got problem in finding LuaJIT, please configure its installation path (usually it could be found at OpenResty's installation directory) to environment variable LUA_DIR.

On Windows with an official OpenResty build, the package ships `lua51.dll` but no import
library — run `web/make-lua51-lib.ps1` once to generate `lua51.lib` next to it, then pass
`-DLUA_LIBRARY=<openresty>\lua51.lib` to CMake (see "Run it" below).

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
:: 0. one-time: get a modern OpenResty (the WinNMP-bundled nginx 1.7.7 core
::    predates UDP support; the official 64-bit build is used by the helpers):
::    download openresty-1.27.1.2-win64.zip from openresty.org and extract to
::    C:\Tools\OpenResty\openresty-1.27.1.2-win64, then generate the LuaJIT
::    import library the official package does not ship:
powershell -ExecutionPolicy Bypass -File web/make-lua51-lib.ps1

:: 1. build the library (x64 to match the official 64-bit OpenResty)
cmake -S . -B build64 -A x64 -DLUA_LIBRARY=C:\Tools\OpenResty\openresty-1.27.1.2-win64\lua51.lib
cmake --build build64 --config Debug

:: 2. one-command helper: syncs config/UI into web/run, starts nginx, probes the server
powershell -ExecutionPolicy Bypass -File web/run-server.ps1          :: start
powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Restart :: stop, sync, start
powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -TestUdp :: start + UDP smoke test
powershell -ExecutionPolicy Bypass -File web/run-server.ps1 -Stop    :: stop

:: 3. open the demo center
start http://localhost:8080

:: 4. UDP smoke test (C++-owned listeners on 9000 = echo, 9001 = hello)
powershell -ExecutionPolicy Bypass -File web/udp-probe.ps1 -All
```

Or manually: `nginx.exe -p C:/Code/ngx_lua_cpp/web/run -c conf/nginx.conf`.
`web/nginx.conf` resolves the Lua sources and the built library relative to the nginx
prefix (`web/run`), so no path editing is needed. `web/run-server.ps1` copies it (plus
`web/index.html` and `mime.types`) into `web/run/` on every start, keeping the run
prefix in sync with the canonical files. The web UI has thirteen tabs: a Lua console,
the Mandelbrot renderer, the concurrent fetcher, the job queue, the UDP datagram probe,
realtime push (WebSocket/SSE/syslog), the API gateway (JWT/webhook), the cache, the
upload hasher, log analysis, DNS and a log viewer.

> **Run notes.** The demo runs as a **single nginx process**
> (`master_process off`) — ngx_lua's `init_worker_by_lua` executes in the
> master process too, which would duplicate the UDP listeners and worker pool
> otherwise. The config prefers the **Release** library: the Debug build's
> `IRIS_ASSERT` (plain `assert()`) can pop blocking message boxes on the
> desktop when an internal invariant trips, which stalls the event loop.

> **UDP support (Demo 4/5)** works on any platform: nginx compiles
> `listen ... udp` out on Windows (`#if !(NGX_WIN32)` in
> `ngx_stream_core_module.c`), so the demo's datagram listeners (9000/9001)
> are owned by the ngx_lua_cpp C++ instance (`udp_listen`/`udp_recv`/
> `udp_send`), not by nginx — see `demo/udp_echo_server.lua` and
> `web/nginx-udp-sample.conf` (Linux-only native stream UDP variant).

### Run it on Linux

```bash
# 1. build (set LUA_DIR to your OpenResty LuaJIT if it isn't auto-detected)
LUA_DIR=/usr/local/openresty/luajit cmake -S . -B build
cmake --build build -j

# 2. one-command helper (uses `openresty`, falls back to `nginx`; override with NGINX_BIN=...)
./web/run-server.sh            # start
./web/run-server.sh restart    # stop, sync, start
./web/run-server.sh stop       # stop
./web/run-server.sh build      # cmake configure + build

# 3. open the demo center
xdg-open http://localhost:8080
```

The same `web/nginx.conf` works on both platforms: the Lua init block appends the
Windows (`build/Debug/?.dll`) and Linux (`build/lib?.so`) library paths to
`package.cpath`, so `require` picks whichever exists. The shell helper is careful
about process management: it matches the nginx master by its command line
(`-p <prefix>`) so it never touches an unrelated system nginx, survives stale pid
files (graceful stop -> force-kill fallback), and cleans the pid file afterwards.

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

### Demo 4/5: UDP datagram echo + UDP servers

Two UDP listeners (9000, 9001) prove datagram I/O end to end. Because nginx
compiles `listen ... udp` **out on Windows** (`#if !(NGX_WIN32)` in
`ngx_stream_core_module.c` — no Windows nginx/OpenResty can do native
stream UDP), the listeners are owned by the ngx_lua_cpp C++ instance:

* `udp_listen(port)` binds a datagram socket and spawns one background
  receive thread that only fills a bounded queue — the nginx worker is never
  blocked. Works on Windows *and* Linux, with any OpenResty.
* `udp_recv(port, timeout_ms)` returns `(ok, payload, from_host, from_port, err)`.
  With `timeout_ms == 0` it polls the queue non-blockingly (how
  `demo/udp_echo_server.lua` drives the demo inside chained `ngx.timer`
  handlers); with a positive timeout it blocks on the worker pool until a
  datagram arrives.
* `udp_send(host, port, payload)` sends one datagram without waiting
  (client-style, from a fresh socket).
* `udp_reply(port, payload)` sends the reply **from the listening socket** to
  the last sender of `port` — the reply source address is then the port the
  client sent to, which connected UDP clients require (this is how the demo
  echo service answers).
* `udp_echo(host, port, payload, timeout_ms)` is the client-side round-trip:
  send + wait-for-reply on the same socket, offloaded to the worker pool.
  Returns `(ok, reply, elapsed_ms, info)`.

`demo/udp_echo_server.lua` starts the demo services from `init_worker`:
port 9000 replies `pong [<payload>] lua_cpp=true from=<addr>:<port>`, port
9001 replies a fixed `hello-udp`.

Try it from the demo center's **UDP tab** (browser → `/api/udp` → C++ pool →
UDP listener → back), from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File web/udp-probe.ps1 -All
```

or from the Lua console:

```lua
local inst = require("init_ngx_lua_cpp")
local r = inst:udp_echo("127.0.0.1", 9000, "hello-udp", 2000)
print(r[1], r[2], r[3], r[4])   -- true, "pong [...]", elapsed_ms, ""
```

On Linux, nginx's stream module *does* support `listen ... udp`
(ngx_stream_lua): see `web/nginx-udp-sample.conf` for a native nginx-side
variant.

### Demo 6/7/8: crypto, event bus + LRU cache, file/log pipelines

All of these live in the C++ instance and are callable from Lua:

| API | What it does | Where it shows |
|---|---|---|
| `sha256` / `hmac_sha256` / `hmac_sha256_raw` / `base64url_encode` / `base64url_decode` / `random_hex` | pure C++ crypto primitives (no OpenSSL/BCrypt deps) | JWT & webhook signatures, upload hashes |
| `pub(topic, payload)` / `events(topic, last_id)` | cross-request event bus with cursor-based polling (bounded per topic) | WebSocket/SSE feeds, syslog wall, job progress |
| `cache_set` / `cache_get` / `cache_stats` | LRU cache with TTL + eviction, shared by all requests | cache tab, DNS response cache |
| `file_sha256(path)` | blocking file read + hash offloaded to the worker pool | upload pipeline (hashes nginx's temp file) |
| `log_analyze(path, max_lines)` | fast C++ access.log parser (QPS, status codes, TOP URLs, per-second buckets) | log analysis tab |

The event bus is what ties everything together: the UDP echo loop, the syslog
collector and the job runner all `pub()` progress; the realtime tab consumes
it over WebSocket (`/ws`, subscribe with `topic:<name>`) or SSE
(`/api/stream?topic=jobs`).

### Demo B4/B5: API gateway (JWT + rate limit) and HMAC webhooks

* `POST /api/auth/login` issues an HS256 JWT (signature computed by the C++
  `hmac_sha256_raw`); `location /api/secure/` is gated by `access_by_lua`:
  token verification + `resty.limit.req` rate limiting (10 req/s, burst 20).
* `POST /api/webhook/ingest` verifies the `X-Ngx-Signature` header
  (hex HMAC-SHA256 of the raw body, also C++-computed) and hands the payload
  to the job queue; the UI computes the signature with browser WebCrypto.

### Demo A1/A2: DNS-over-UDP forwarder and syslog collector

* `demo/dns_server.lua` listens on UDP 53 (socket owned by the C++ layer),
  answers a static override table (`myapp.test` → 127.0.0.1) with a
  hand-built A record and forwards everything else to an upstream resolver
  via `udp_echo`, caching responses 30 s in the C++ LRU cache.
* `demo/syslog_collector.lua` listens on UDP 514, parses RFC3164 lines and
  publishes them to the bus (syslog wall in the realtime tab) plus keeps a
  ring buffer for the HTTP API.

### Demo D9/D11: cache + upload hash

The cache tab exercises the LRU cache including a computed-cache demo
(miss → compute on the pool → store → hit). The upload tab POSTs a raw file
body; nginx buffers it (memory, or a temp file for large bodies) and the C++
`file_sha256` hashes it on the pool — the browser compares with WebCrypto.

### Demo E12: bridging the WinNMP legacy stack (php-cgi + MySQL)

The new OpenResty can front the old WinNMP PHP/MySQL stack through FastCGI.
Start the legacy services once (WinNMP paths):

```bat
:: php-cgi 5.4 with the project dir whitelisted (WinNMP's php.ini sets an
:: open_basedir pointing at c:/work/wt-nmp which would reject our files)
cd C:\Tools\WinNMP\bin\php-5.4.35
php-cgi.exe -b 127.0.0.1:9000 -c php.ini -d open_basedir=C:\Code\ngx_lua_cpp\web\run\html

:: mysql 5.6 (the bundled mysql.ini points at c:/work, override the datadir)
cd C:\Tools\WinNMP\bin\mysql-5.6.22
mysqld.exe --datadir=%CD%\data --port=3306 --skip-log-bin --console
```

Then open http://localhost:8080/php/test.php — a PHP 5.4 page that runs a
live `SELECT VERSION()` against the WinNMP MySQL server through the new
gateway. `web/nginx.conf` ships the FastCGI location (no external
fastcgi_params file needed).

### Why these demos

| Capability | Where it shows |
|---|---|
| C++20 coroutines as "normal blocking code" | `mandelbrot`/`fetch`/`udp_echo` bodies read top-to-bottom, no callbacks |
| Blocking work offloaded, worker never blocked | `/api/info` answers in ms while a heavy render runs |
| Parallel fan-out + fan-in | row tasks / URL tasks pre-dispatched, joined by `co_await` |
| Coroutine return values flow back into Lua | `co_return` of tuples/vectors/maps become Lua tables |
| Cross-request shared C++ state | the job queue lives in the C++ instance, shared by all workers/requests |
| warp-based scheduling | job progress hops pool &rarr; warp &rarr; pool via `iris_switch` |
| UDP end-to-end on OpenResty | C++-owned UDP listeners + `udp_echo` worker-pool datagrams |
| Realtime push | WebSocket + SSE consumers over the C++ event bus (syslog/UDP/jobs) |
| API gateway | HS256 JWT gate + rate limiting; HMAC-signed webhooks |
| Protocol servers in Lua | DNS-over-UDP forwarder, syslog collector on C++ sockets |
| Legacy stack bridge | FastCGI to WinNMP php-cgi + MySQL through the new nginx |
