/*
ngx_lua_cpp.h

The MIT License (MIT)

Copyright (c) 2025-2026 PaintDream

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once

#include <string>
#include <tuple>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

#ifdef NGX_LUA_CPP_EXPORT
	#ifdef __GNUC__
		#define NGX_LUA_CPP_API __attribute__ ((visibility ("default")))
	#else
		#define NGX_LUA_CPP_API __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
	#endif
#else
	#ifdef __GNUC__
		#define NGX_LUA_CPP_API __attribute__ ((visibility ("default")))
	#else
		#define NGX_LUA_CPP_API __declspec(dllimport) // Note: actually gcc seems to also supports this syntax.
	#endif
#endif

struct lua_State;
namespace iris {
	int ngx_lua_cpp_resume(lua_State* L, int narg);
	int ngx_lua_cpp_yield(lua_State* L, int narg);
}

#define IRIS_LUA_RESUME ngx_lua_cpp_resume
#define IRIS_LUA_YIELD ngx_lua_cpp_yield

#include "iris/iris_lua.h"
#include "iris/iris_dispatcher.h"
#include "iris/iris_coroutine.h"

namespace iris {
	struct ngx_warp_t : iris_warp_t<iris_async_worker_t<>, false, ngx_warp_t> {
		using base_t = iris_warp_t<iris_async_worker_t<>, false, ngx_warp_t>;
		template <typename... args_t>
		ngx_warp_t(args_t&&... args) : base_t(std::forward<args_t>(args)...) {}
		static ngx_warp_t* get_current() noexcept {
			return static_cast<ngx_warp_t*>(base_t::get_current());
		}

		void enter_warp() {}
		void leave_warp() {}

		size_t enter_join_warp() {
			return 0;
		}

		size_t leave_join_warp() {
			return 0;
		}

		void suspend_warp() {}
		void resume_warp() {}
		void flush_warp();
	};

	struct ngx_lua_cpp_t {
	public:
		ngx_lua_cpp_t();
		~ngx_lua_cpp_t() noexcept;

		static void lua_registar(iris_lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>);
		iris_lua_t::optional_result_t<void> start(size_t thread_count);
		iris_lua_t::optional_result_t<void> stop();
		bool is_running() const noexcept;
		size_t get_hardware_concurrency() const noexcept;
		// example async demo: sleep
		iris_coroutine_t<size_t> sleep(size_t milliseconds);

		// demo 1: parallel mandelbrot renderer.
		// mode == 0 -> parallel (one row-task per row, pre-dispatched to the worker pool),
		// mode != 0 -> serial (all rows rendered on a single worker thread).
		// returns (bmp_bytes, render_elapsed_ms). bmp is a 24bpp uncompressed BMP.
		using mandelbrot_result_t = std::tuple<std::string, double>;
		iris_coroutine_t<mandelbrot_result_t> mandelbrot(size_t width, size_t height, size_t iterations, double cx, double cy, double zoom, size_t mode);

		// demo 2: concurrent http fetch (fan-out / fan-in).
		// each url is fetched by a blocking WinHTTP request running on the worker pool.
		// returns (entries, total_elapsed_ms); entry = (url, ok, status, bytes, elapsed_ms, info)
		// where info is the first 256 bytes of the body when ok, or the error message.
		using fetch_entry_t = std::tuple<std::string, bool, size_t, size_t, double, std::string>;
		using fetch_result_t = std::tuple<std::vector<fetch_entry_t>, double>;
		iris_coroutine_t<fetch_result_t> fetch(std::vector<std::string> urls, size_t timeout_ms);

		// demo 4: UDP round-trip (datagram echo).
		// sends `payload` as a UDP datagram to host:port and waits up to
		// timeout_ms for a reply datagram. The blocking socket work runs on the
		// worker pool, exactly like fetch/mandelbrot.
		// returns (ok, reply, elapsed_ms, info); info is "" on success,
		// otherwise the error message (resolution / sendto / recvfrom timeout).
		using udp_result_t = std::tuple<bool, std::string, double, std::string>;
		iris_coroutine_t<udp_result_t> udp_echo(std::string host, size_t port, std::string payload, size_t timeout_ms);

		// demo 5: UDP servers owned by the C++ instance.
		// nginx's stream module compiles "listen ... udp" out on Windows
		// (ngx_stream_core_module.c: #if !(NGX_WIN32)), so a real UDP listener
		// cannot be created in nginx.conf on this platform. These methods bind
		// datagram sockets in ngx_lua_cpp instead, on any platform:
		//   udp_listen(port)      -> "" on success, or an error message
		//   udp_recv(port, ms)    -> (ok, payload, from_host, from_port, err)
		//                            blocks up to ms on the worker pool for the
		//                            next datagram; ms == 0 polls the queue
		//   udp_send(host, port, payload) -> (ok, err) one datagram, no wait
		//   udp_reply(port, payload)      -> (ok, err) reply to the last sender
		//                            of `port`, sent FROM the listening socket
		//                            (so connected UDP clients accept it)
		// Each listened port owns one background thread that only fills a
		// bounded queue; the nginx worker is never blocked.
		using udp_packet_result_t = std::tuple<bool, std::string, std::string, size_t, std::string>;
		std::string udp_listen(size_t port);
		iris_coroutine_t<udp_packet_result_t> udp_recv(size_t port, size_t timeout_ms);
		iris_coroutine_t<std::tuple<bool, std::string>> udp_send(std::string host, size_t port, std::string payload);
		iris_coroutine_t<std::tuple<bool, std::string>> udp_reply(size_t port, std::string payload);

		// demo 6: crypto primitives (pure C++, no external deps).
		// sha256(data) -> hex string; hmac_sha256(key, data) -> hex string;
		// hmac_sha256_raw(key, data) -> raw bytes (for JWT-style base64url
		// signatures); base64url_encode(data) -> unpadded base64url string;
		// base64url_decode(data) -> (out, err);
		// random_hex(nbytes) -> hex string of n random bytes.
		std::string sha256(const std::string& data);
		std::string hmac_sha256(const std::string& key, const std::string& data);
		std::string hmac_sha256_raw(const std::string& key, const std::string& data);
		std::string base64url_encode(const std::string& data);
		std::tuple<std::string, std::string> base64url_decode(const std::string& data);
		std::string random_hex(size_t nbytes);

		// demo 7: cross-request in-memory bus + LRU cache (shared C++ state).
		// pub(topic, payload) appends a timestamped event; events(topic,
		// last_id) returns (events, next_id) where each event is
		// {id, ts_ms, payload} -- a cursor-based poll, safe for many
		// concurrent consumers (WebSocket/SSE feeds).
		// cache_set(key, value, ttl_ms) / cache_get(key) / cache_stats().
		using bus_event_t = std::tuple<double, double, std::string>; // (id, ts_ms, payload)
		void pub(const std::string& topic, const std::string& payload);
		std::tuple<std::vector<bus_event_t>, double> events(const std::string& topic, double last_id);
		bool cache_set(const std::string& key, const std::string& value, double ttl_ms);
		std::tuple<bool, std::string, double> cache_get(const std::string& key);
		std::map<std::string, double> cache_stats();

		// demo 8: file hashing + log analysis (blocking I/O on the worker pool).
		// file_sha256(path) -> (hex, size_bytes, elapsed_ms)
		iris_coroutine_t<std::tuple<std::string, double, double>> file_sha256(std::string path);
		// log_analyze(path, max_lines) -> (stats, top_urls, statuses, per_second)
		// stats:   {lines, total, status_2xx, status_3xx, status_4xx, status_5xx, qps, elapsed_ms}
		// top_urls:  array of {url, count} (sorted desc, up to 20)
		// statuses:  array of {code, count}
		// per_second: array of {bucket_index, count} (1-second buckets)
		using log_stats_t = std::tuple<
			std::map<std::string, double>,
			std::vector<std::tuple<std::string, double>>,
			std::vector<std::tuple<std::string, double>>,
			std::vector<std::tuple<double, double>>>;
		iris_coroutine_t<log_stats_t> log_analyze(std::string path, double max_lines);

		// demo 3: in-memory job queue shared by all requests.
		// job kinds: "count_primes" (payload: limit), "sleep_steps" (payload: steps, ms).
		// jobs run in background coroutines that hop pool <-> warp to report progress.
		std::string job_submit(const std::string& kind, const std::map<std::string, double>& payload);
		std::tuple<std::string, double, double, std::string> job_query(const std::string& id); // (status, progress, elapsed_ms, result)
		std::vector<std::tuple<std::string, std::string, std::string, double, double>> job_list(); // (id, kind, status, progress, elapsed_ms)

		std::shared_ptr<iris_async_worker_t<>> get_async_worker() noexcept { return async_worker; }
		
		// inspect internal
		void* __async_worker__(void* new_async_worker_ptr);

	protected:
		bool set_async_worker(std::shared_ptr<iris_async_worker_t<>> worker);
		void process_events();
		void stop_impl();
		void reset_main_warp();
		friend struct ngx_hooker_t;

	protected:
		struct ngx_job_t {
			std::string id;
			std::string kind;
			std::map<std::string, double> payload;
			std::string status;   // queued / running / done / failed
			std::string result;
			double progress = 0.0; // 0..1
			double elapsed_ms = 0.0;
			double created_ms = 0.0;
		};

		iris_coroutine_t<void> run_job(std::shared_ptr<ngx_job_t> job);

		std::mutex jobs_mutex;
		std::map<std::string, std::shared_ptr<ngx_job_t>> jobs;
		size_t job_counter = 0;

		struct udp_listener_t;
		std::mutex listeners_mutex;
		std::map<size_t, std::shared_ptr<udp_listener_t>> udp_listeners;

		struct event_bus_t;
		std::shared_ptr<event_bus_t> event_bus;
		struct lru_cache_t;
		std::shared_ptr<lru_cache_t> lru_cache;

		std::shared_ptr<iris_async_worker_t<>> async_worker;
		std::unique_ptr<ngx_warp_t> main_warp;
		std::unique_ptr<ngx_warp_t::preempt_guard_t> main_warp_guard;
		size_t main_thread_index = ~(size_t)0;
	};

	template <typename>
	struct is_non_void_iris_coroutine_instance : std::false_type {};

	template <typename return_t>
	struct is_non_void_iris_coroutine_instance<iris_coroutine_t<return_t>> : std::bool_constant<!std::is_void_v<return_t>> {};

	template <typename return_t>
	struct is_coroutine_non_void_return_t : std::false_type {};

	template <typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(*)(args_t...)>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(args_t...)>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename class_t, typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(class_t::*)(args_t...)>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	template <typename class_t, typename return_t, typename... args_t>
	struct is_coroutine_non_void_return_t<return_t(class_t::*)(args_t...) const>
		: is_non_void_iris_coroutine_instance<std::remove_cvref_t<return_t>> {};

	extern int ngx_iris_wrap_coroutine_with_returns_key;
	template <typename type_t>
	struct iris_lua_traits_t<type_t, std::enable_if_t<is_coroutine_non_void_return_t<type_t>::value>> {
		using type = iris_lua_traits_t<type_t>;
		static constexpr bool value = true;

		template <auto ptr, typename executor_t, typename... args_t>
		static iris_lua_t::reflection_t lua_tostack(lua_State* L, std::nullptr_t, executor_t&& executor, args_t&&... args) {
			executor(L, std::forward<args_t>(args)...);
			lua_pushlightuserdata(L, &ngx_iris_wrap_coroutine_with_returns_key);
			lua_rawget(L, LUA_REGISTRYINDEX);
			lua_insert(L, -2);
			lua_call(L, 1, 1);

			return &iris_lua_t::reflection<ptr, executor_t, args_t...>;
		}
	};
}

