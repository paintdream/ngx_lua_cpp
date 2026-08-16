/*
ngx_lua_cpp.cpp

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

#include "ngx_lua_cpp.h"
#include "iris/iris_common.inl"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <random>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace iris {
	int ngx_iris_wrap_coroutine_with_returns_key;
	// minimal forward declaration, modify if nginx header changes
	using ngx_int_t = int;
	using ngx_uint_t = unsigned int;
	using ngx_msec_t = uintptr_t;
	struct ngx_event_t;
	struct ngx_connection_t;
	struct ngx_cycle_t;
	struct ngx_http_request_t {
		uint32_t signature;
		ngx_connection_t* connection;
		void** ctx;
		// ...
	};

	struct ngx_log_t;
	typedef uint8_t* (*ngx_log_handler_pt) (ngx_log_t* log, uint8_t* buf, size_t len);
	struct ngx_stream_session_t {
		uint32_t signature;
		ngx_connection_t* connection;
		off_t received;
		time_t start_sec;
		ngx_msec_t start_msec;
		ngx_log_handler_pt log_handler;
		void** ctx;
	};

	struct ngx_stream_lua_request_t {
		ngx_connection_t* connection;
		ngx_stream_session_t* session;
		// ...
	};

	typedef void (*ngx_event_handler_pt)(ngx_event_t* ev);
	typedef ngx_int_t(*ngx_http_handler_pt)(ngx_http_request_t* r);
	typedef void (*ngx_stream_lua_cleanup_pt)(void* data);

	struct ngx_module_t {
		uint32_t ctx_index;
		// ...
	};

	struct ngx_http_lua_co_ctx_t;
	struct ngx_stream_lua_co_ctx_t;
	struct ngx_http_lua_ctx_t;
	struct ngx_stream_lua_ctx_t;

	struct ngx_event_actions_t {
		ngx_int_t (*add)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*del)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*enable)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*disable)(ngx_event_t* ev, ngx_int_t event, ngx_uint_t flags);
		ngx_int_t (*add_conn)(ngx_connection_t* c);
		ngx_int_t (*del_conn)(ngx_connection_t* c, ngx_uint_t flags);
		ngx_int_t (*notify)(ngx_event_handler_pt handler);
		ngx_int_t (*process_events)(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags);
		ngx_int_t (*init)(ngx_cycle_t* cycle, ngx_msec_t timer);
		void (*done)(ngx_cycle_t* cycle);
	};

	struct ngx_queue_t {
		ngx_queue_t* prev;
		ngx_queue_t* next;
	};

	static void ngx_queue_insert_tail(ngx_queue_t* h, ngx_queue_t* x) {
		x->prev = h->prev;
		x->prev->next = x;
		x->next = h;
		h->prev = x;
	}

	static void ngx_queue_remove(ngx_queue_t* x) {
		x->next->prev = x->prev;
		x->prev->next = x->next;
	}

	// let's go
	struct ngx_hooker_t {
		static ngx_hooker_t& get_instance() {
			static ngx_hooker_t instance;
			return instance;
		}

		static void* get_host_module_handle() {
#ifdef _WIN32
			return ::GetModuleHandleA(nullptr);
#else
			return ::dlopen(nullptr, RTLD_NOLOAD);
#endif
		}

		template <typename T>
		static void get_proc_address(T*& target, void* host, const char* name) {
			if (target == nullptr) {
#ifdef _WIN32
				target = reinterpret_cast<T*>(::GetProcAddress((HMODULE)host, name));
#else
				target = reinterpret_cast<T*>(::dlsym(host, name));
#endif
			}
		}

		ngx_hooker_t() {
			void* host = get_host_module_handle();
			get_proc_address(actions, host, "ngx_event_actions");
			get_proc_address(ngx_posted_delayed_events, host, "ngx_posted_delayed_events");
			get_proc_address(ngx_http_lua_module, host, "ngx_http_lua_module");
			get_proc_address(ngx_stream_lua_module, host, "ngx_stream_lua_module");
			get_proc_address(ngx_http_lua_get_co_ctx, host, "ngx_http_lua_get_co_ctx");
			get_proc_address(ngx_stream_lua_get_co_ctx, host, "ngx_stream_lua_get_co_ctx");

			prev_ngx_process_events = actions->process_events;
			actions->process_events = &ngx_hooker_t::proxy_ngx_process_events;
		}

		static int get_coroutine_returns(lua_State* L) {
			if (!lua_isnone(L, 1)) {
				// not actually yielded, returns all parameter as return values
				return lua_gettop(L);
			}

			lua_pushlightuserdata(L, L);
			lua_rawget(L, LUA_REGISTRYINDEX);
			if (lua_isnil(L, -1)) {
				return luaL_error(L, "No coroutine return value collected!");
			}

			lua_pushlightuserdata(L, L);
			lua_pushnil(L);
			lua_rawset(L, LUA_REGISTRYINDEX);

			int table_index = lua_absindex(L, -1);
			lua_rawgeti(L, table_index, 1);
			int nrets = lua_tointeger(L, -1);
			lua_pop(L, 1);

			for (int i = 1; i <= nrets; i++) {
				lua_rawgeti(L, table_index, i + 1);
			}

			return nrets;
		}

		void registar(iris_lua_t lua) {
			const std::string_view wrapper = 
				"local get_coroutine_returns = ..."
				"return function (func)\n"
				"	return function (...)\n"
				"		return get_coroutine_returns(func(...))\n"
				"	end\n"
				"end\n";
			lua.set_registry(static_cast<const void*>(&ngx_iris_wrap_coroutine_with_returns_key), lua.call<iris_lua_t::ref_t>(lua.load(wrapper, "=(ngx_lua_cpp)"), &get_coroutine_returns));

			lua_State* L = lua.get_state();
			lua_getglobal(L, "ngx");
			lua_getfield(L, -1, "config");
			if (lua_istable(L, -1)) {
				lua_getfield(L, -1, "subsystem");
				const char* subsystem = lua_tostring(L, -1);

				lua_pushlightuserdata(L, this);
				lua_pushboolean(L, strcmp(subsystem, "stream") == 0);
				lua_rawset(L, LUA_REGISTRYINDEX);
				lua_pop(L, 3);
			} else {
				lua_pop(L, 2);
			}
		}

		void insert(ngx_lua_cpp_t* bridge) {
			iris::iris_binary_insert(cpp_list, bridge);
		}

		void remove(ngx_lua_cpp_t* bridge) {
			auto it = iris::iris_binary_find(cpp_list.begin(), cpp_list.end(), bridge);
			if (it != cpp_list.end()) {
				cpp_list.erase(it);
			}
		}

		void notify() {
			if (notified.exchange(1, std::memory_order_relaxed) == 0) {
				if (actions->notify != nullptr) {
					actions->notify(&ngx_hooker_t::ngx_event_handler);
				}
			}
		}

		ngx_http_lua_co_ctx_t* get_http_co_ctx(lua_State* L) {
			auto* http_request = reinterpret_cast<ngx_http_request_t*>(lua_getexdata(L));
			if (http_request != nullptr) {
				ngx_http_lua_ctx_t* ctx = static_cast<ngx_http_lua_ctx_t*>(http_request->ctx[ngx_http_lua_module->ctx_index]);
				if (ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua context");
				}

				ngx_http_lua_co_ctx_t* co_ctx = ngx_http_lua_get_co_ctx(L, ctx);
				if (co_ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua coroutine context");
				}

				return co_ctx;
			} else {
				return nullptr;
			}
		}

		ngx_stream_lua_co_ctx_t* get_stream_co_ctx(lua_State* L) {
			auto* stream_request = reinterpret_cast<ngx_stream_lua_request_t*>(lua_getexdata(L));
			if (stream_request != nullptr) {
				ngx_stream_lua_ctx_t* ctx = static_cast<ngx_stream_lua_ctx_t*>(stream_request->session->ctx[ngx_stream_lua_module->ctx_index]);
				if (ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua context");
				}

				ngx_stream_lua_co_ctx_t* co_ctx = ngx_stream_lua_get_co_ctx(L, ctx);
				if (co_ctx == nullptr) {
					luaL_error(L, "Unexpected ngx_lua_cpp lua coroutine context");
				}

				return co_ctx;
			} else {
				return nullptr;
			}
		}

		bool is_http_context(lua_State* L) {
			lua_pushlightuserdata(L, this);
			lua_rawget(L, LUA_REGISTRYINDEX);
			bool is_http = !lua_toboolean(L, -1);
			lua_pop(L, 1);

			return is_http;
		}

		int ngx_lua_cpp_yield(lua_State* L, int narg) {
			int (*func)(lua_State*) = nullptr;

			if (is_http_context(L)) {
				ngx_http_lua_co_ctx_t* http_lua_co_ctx = get_http_co_ctx(L);
				if (http_lua_co_ctx != nullptr) {
					func = require_ngx_sleep(L, http_lua_co_ctx, ngx_http_lua_yield, offset_http_co_ctx_event_queue);
					pending_lua_http_co_ctxs.push_back(http_lua_co_ctx);
				} else {
					return luaL_error(L, "Unexpected ngx_lua_cpp http context");
				}
			} else {
				ngx_stream_lua_co_ctx_t* stream_lua_co_ctx = get_stream_co_ctx(L); // assuming no stream request
				if (stream_lua_co_ctx != nullptr) {
					func = require_ngx_sleep(L, stream_lua_co_ctx, ngx_stream_lua_yield, offset_stream_co_ctx_event_queue);
					pending_stream_http_co_ctxs.push_back(stream_lua_co_ctx);
				} else {
					return luaL_error(L, "Unexpected ngx_lua_cpp stream context");
				}
			}

			lua_settop(L, 0);
			lua_pushnumber(L, 0.0);
			return func(L);
		}

		int ngx_lua_cpp_resume(lua_State* L, int nrets) {
			ngx_queue_t* p = nullptr;
			if (is_http_context(L)) {
				ngx_http_lua_co_ctx_t* http_lua_co_ctx = get_http_co_ctx(L);
				if (http_lua_co_ctx != nullptr) {
					p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(http_lua_co_ctx) + offset_http_co_ctx_event_queue);
				} else {
					return LUA_ERRERR;
				}
			} else {
				ngx_stream_lua_co_ctx_t* stream_lua_co_ctx = get_stream_co_ctx(L);
				if (stream_lua_co_ctx != nullptr) {
					p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(stream_lua_co_ctx) + offset_stream_co_ctx_event_queue);
				} else {
					return LUA_ERRERR;
				}
			}

			ngx_queue_insert_tail(ngx_posted_delayed_events, p);

			if (nrets != 0) {
				lua_pushlightuserdata(L, L);
				lua_createtable(L, nrets + 1, 0);
				lua_pushinteger(L, nrets);
				lua_rawseti(L, -2, 1);

				for (int i = 1; i <= nrets; i++) {
					lua_pushvalue(L, i);
					lua_rawseti(L, -2, i + 1);
				}

				lua_rawset(L, LUA_REGISTRYINDEX);
				lua_pop(L, nrets);
			}

			return LUA_OK;
		}

	private:
		struct GCfuncC {
			void* nextgc;
			uint8_t marked;
			uint8_t gct;
			uint8_t ffid;
			uint8_t nupvalues;
			void* env;
			void* gclist;
			void* pc;
			int (*f)(lua_State*);
		};

		auto require_ngx_sleep(lua_State* L, void* co_ctx, int (*&f)(lua_State*), int& offset) -> int (*)(lua_State*) {
			if (f != nullptr) {
				return f;
			}

			ngx_queue_t* prev = ngx_posted_delayed_events->prev;
			lua_getglobal(L, "ngx");
			lua_getfield(L, -1, "sleep");
			f = reinterpret_cast<const GCfuncC*>(lua_topointer(L, -1))->f;

			// get offset for ngx_http_lua_co_ctx_t::sleep::event_queue
			lua_pushcclosure(L, f, 0);
			lua_pushnumber(L, 0.0);
			int result = lua_pcall(L, 1, 0, 0);
			if (result != LUA_OK) {
				lua_pop(L, 1);
			}

			ngx_queue_t* current = ngx_posted_delayed_events->prev;
			if (current != prev) {
				offset = reinterpret_cast<uintptr_t>(current) - reinterpret_cast<uintptr_t>(co_ctx);
				return f;
			} else {
				return nullptr;
			}
		}

		static ngx_int_t proxy_ngx_process_events(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags) {
			return ngx_hooker_t::get_instance().process_events(cycle, timer, flags);
		}

		static void ngx_event_handler(ngx_event_t* ev) {
			return ngx_hooker_t::get_instance().event_handler(ev);
		}

		ngx_int_t process_events(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags) {
			// dig out sleep placeholder events...
			if (offset_http_co_ctx_event_queue != 0) {				for (auto* co_ctx : pending_lua_http_co_ctxs) {
					ngx_queue_t* p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(co_ctx) + offset_http_co_ctx_event_queue);
					ngx_queue_remove(p);
				}
			}

			pending_lua_http_co_ctxs.clear();

			if (offset_stream_co_ctx_event_queue != 0) {
				for (auto* co_ctx : pending_stream_http_co_ctxs) {
					ngx_queue_t* p = reinterpret_cast<ngx_queue_t*>(reinterpret_cast<uintptr_t>(co_ctx) + offset_stream_co_ctx_event_queue);
					ngx_queue_remove(p);
				}
			}

			pending_stream_http_co_ctxs.clear();

			// safety valve: under sustained warp traffic (e.g. datagram
			// listeners polled by timers) `notified` can stay set while we
			// drain, which would otherwise starve the nginx event loop; any
			// remaining work is simply picked up on the next event pass.
			int spins = 0;
			do {
				spins++;
				if (spins > 1000) {
					break;
				}
				for (ngx_lua_cpp_t* p : cpp_list) {
					p->process_events();
				}
			} while (notified.exchange(0, std::memory_order_relaxed) == 1);

			if (actions->notify == nullptr) {
				// if target platform does not support notify(), then modify timer interval (win32).
				timer = std::min(timer, ngx_msec_t(16u));
			}

			return prev_ngx_process_events(cycle, timer, flags);
		}

		static void event_handler(ngx_event_t* ev) {}

		ngx_int_t(*prev_ngx_process_events)(ngx_cycle_t* cycle, ngx_msec_t timer, ngx_uint_t flags) = nullptr;
		std::vector<ngx_lua_cpp_t*> cpp_list;
		std::atomic<size_t> notified = 0;
		ngx_event_actions_t* actions = nullptr;
		ngx_module_t* ngx_http_lua_module = nullptr;
		ngx_module_t* ngx_stream_lua_module = nullptr;
		ngx_http_lua_co_ctx_t* (*ngx_http_lua_get_co_ctx)(lua_State* L, ngx_http_lua_ctx_t* ctx) = nullptr;
		ngx_stream_lua_co_ctx_t* (*ngx_stream_lua_get_co_ctx)(lua_State* L, ngx_stream_lua_ctx_t* ctx) = nullptr;
		int (*ngx_http_lua_yield)(lua_State*) = nullptr;
		int (*ngx_stream_lua_yield)(lua_State*) = nullptr;
		std::vector<ngx_http_lua_co_ctx_t*> pending_lua_http_co_ctxs;
		std::vector<ngx_stream_lua_co_ctx_t*> pending_stream_http_co_ctxs;
		int offset_http_co_ctx_event_queue = 0;
		int offset_stream_co_ctx_event_queue = 0;
		ngx_queue_t* ngx_posted_delayed_events = nullptr;
	};

	ngx_lua_cpp_t::ngx_lua_cpp_t() : async_worker(std::make_shared<iris_async_worker_t<>>()),
		event_bus(std::make_shared<event_bus_t>()),
		lru_cache(std::make_shared<lru_cache_t>()) {
		ngx_hooker_t::get_instance().insert(this);
		reset_main_warp();

		async_worker->set_priority_task_handler([this](iris_async_worker_t<>::task_base_t* task, size_t& priority) {
			main_warp->queue_routine([this, task]() {
				async_worker->execute_task(task);
			});

			return true;
		}, 0);
	}

	void ngx_lua_cpp_t::reset_main_warp() {
		if (main_warp_guard) {
			main_warp_guard.reset();
		}

		main_warp = std::make_unique<ngx_warp_t>(*async_worker);
		main_warp_guard = std::make_unique<ngx_warp_t::preempt_guard_t>(*main_warp, 0);
	}

	ngx_lua_cpp_t::~ngx_lua_cpp_t() noexcept {
		if (is_running()) {
			stop_impl();
		}

		ngx_hooker_t::get_instance().remove(this);
	}

	iris_lua_t::optional_result_t<void> ngx_lua_cpp_t::start(size_t thread_count) {
		if (async_worker->get_current_thread_index() != ~(size_t)0) {
			return iris_lua_t::result_error_t("ngx_lua_cpp_t::start(thread_count) -> incorrect current thread, please call me in main thread.");
		}

		if (is_running()) {
			return iris_lua_t::result_error_t("ngx_lua_cpp_t::start(thread_count) -> already started.");
		}

		if (thread_count > std::thread::hardware_concurrency() * 4) {
			thread_count = std::thread::hardware_concurrency() * 4;
		}

		async_worker->resize(thread_count);
		main_thread_index = async_worker->append(std::thread()); // for main thread polling
		async_worker->start();

		if (!ngx_warp_t::is_strand) {
			reset_main_warp();
		}

		return {};
	}

	iris_lua_t::optional_result_t<void> ngx_lua_cpp_t::stop() {
		if (!is_running()) {
			return iris_lua_t::result_error_t("ngx_lua_cpp_t::stop() -> not started.");
		}

		stop_impl();
		return {};
	}

	bool ngx_lua_cpp_t::is_running() const noexcept {
		return !async_worker->is_terminated();
	}

	iris_coroutine_t<size_t> ngx_lua_cpp_t::sleep(size_t millseconds) {
		ngx_warp_t* current = co_await iris_switch<ngx_warp_t>(nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(millseconds));
		co_await iris_switch(current);
		co_return std::move(millseconds);
	}

	// ---------------------------------------------------------------------------
	// demo 1: parallel mandelbrot renderer
	// ---------------------------------------------------------------------------

	namespace {
		struct palette_stop_t {
			double t;
			uint8_t r, g, b;
		};

		const palette_stop_t mandelbrot_palette[] = {
			{ 0.00, 0, 7, 100 },
			{ 0.16, 32, 107, 203 },
			{ 0.42, 237, 255, 255 },
			{ 0.64, 255, 170, 0 },
			{ 0.86, 0, 2, 0 },
		};

		void palette_color(double t, uint8_t& r, uint8_t& g, uint8_t& b) {
			const palette_stop_t* last = mandelbrot_palette + (sizeof(mandelbrot_palette) / sizeof(mandelbrot_palette[0])) - 1;
			if (t <= 0.0) { r = mandelbrot_palette[0].r; g = mandelbrot_palette[0].g; b = mandelbrot_palette[0].b; return; }
			if (t >= 1.0) { r = last->r; g = last->g; b = last->b; return; }
			for (const palette_stop_t* p = mandelbrot_palette + 1; p <= last; p++) {
				if (t <= p->t) {
					const palette_stop_t& a = *(p - 1);
					double k = (t - a.t) / (p->t - a.t);
					r = (uint8_t)(a.r + (p->r - a.r) * k);
					g = (uint8_t)(a.g + (p->g - a.g) * k);
					b = (uint8_t)(a.b + (p->b - a.b) * k);
					return;
				}
			}
			r = last->r; g = last->g; b = last->b;
		}

		// render one row (top-down) into out (width * 3 bytes, BGR order as BMP expects)
		void render_mandelbrot_row(uint8_t* out, size_t y, size_t width, size_t height, size_t iterations, double re_min, double im_max, double pixel_size) {
			const double ci = im_max - y * pixel_size;
			for (size_t x = 0; x < width; x++) {
				const double cr = re_min + x * pixel_size;
				double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
				size_t n = 0;
				while (n < iterations) {
					if (zr2 + zi2 > 4.0) break;
					zi = 2.0 * zr * zi + ci;
					zr = zr2 - zi2 + cr;
					zr2 = zr * zr;
					zi2 = zi * zi;
					n++;
				}

				if (n == iterations) {
					out[x * 3] = 0; out[x * 3 + 1] = 0; out[x * 3 + 2] = 0;
				} else {
					// smooth escape-time: mu = n + 1 - log2(log(|z|))
					double mu = (double)n + 1.0 - std::log2(std::log(zr2 + zi2) * 0.5);
					double t = std::clamp(mu / (double)iterations, 0.0, 1.0);
					uint8_t r, g, b;
					palette_color(t, r, g, b);
					out[x * 3] = b; out[x * 3 + 1] = g; out[x * 3 + 2] = r;
				}
			}
		}

		std::string encode_bmp(size_t width, size_t height, const uint8_t* rgb_topdown) {
			const size_t row_size = (width * 3 + 3) & ~(size_t)3;
			const size_t data_size = row_size * height;
			std::string bmp(54 + data_size, '\0');
			uint8_t* p = reinterpret_cast<uint8_t*>(bmp.data());
			p[0] = 'B'; p[1] = 'M';
			uint32_t v32 = (uint32_t)bmp.size(); std::memcpy(p + 2, &v32, 4);
			v32 = 0; std::memcpy(p + 6, &v32, 4);
			v32 = 54; std::memcpy(p + 10, &v32, 4);
			v32 = 40; std::memcpy(p + 14, &v32, 4);
			int32_t v32s = (int32_t)width; std::memcpy(p + 18, &v32s, 4);
			v32s = (int32_t)height; std::memcpy(p + 22, &v32s, 4);
			uint16_t v16 = 1; std::memcpy(p + 26, &v16, 2);
			v16 = 24; std::memcpy(p + 28, &v16, 2);
			v32 = 0; std::memcpy(p + 30, &v32, 4);
			v32 = (uint32_t)data_size; std::memcpy(p + 34, &v32, 4);
			// pixels are stored bottom-up
			for (size_t y = 0; y < height; y++) {
				std::memcpy(p + 54 + y * row_size, rgb_topdown + (height - 1 - y) * width * 3, width * 3);
			}
			return bmp;
		}
	}

	iris_coroutine_t<ngx_lua_cpp_t::mandelbrot_result_t> ngx_lua_cpp_t::mandelbrot(size_t width, size_t height, size_t iterations, double cx, double cy, double zoom, size_t mode) {
		width = std::clamp(width, size_t(16), size_t(1600));
		height = std::clamp(height, size_t(16), size_t(1200));
		iterations = std::clamp(iterations, size_t(4), size_t(100000));

		// hop to the worker pool; every Lua-called coroutine starts on the nginx thread.
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		const double span = 3.0 / std::max(zoom, 1e-6);
		const double re_min = cx - span * 0.5;
		const double im_max = cy + span * 0.5;
		const double pixel_size = span / (double)width;

		std::vector<uint8_t> pixels(width * height * 3);
		const auto t0 = std::chrono::steady_clock::now();

		if (mode == 0 && height > 1) {
			// parallel: pre-dispatch one row-task per row to the worker pool,
			// then fan-in by awaiting them in order.
			using row_task_t = iris_awaitable_t<ngx_warp_t, std::function<void()>>;
			std::vector<std::unique_ptr<row_task_t>> tasks;
			tasks.reserve(height);
			for (size_t y = 0; y < height; y++) {
				uint8_t* row = pixels.data() + y * width * 3;
				tasks.push_back(std::make_unique<row_task_t>(main, std::function<void()>([row, y, width, height, iterations, re_min, im_max, pixel_size]() {
					render_mandelbrot_row(row, y, width, height, iterations, re_min, im_max, pixel_size);
				}), 1));
				tasks.back()->dispatch();
			}
			for (auto& task : tasks) {
				co_await *task;
			}
		} else {
			// serial: render everything on this (pool) thread
			for (size_t y = 0; y < height; y++) {
				render_mandelbrot_row(pixels.data() + y * width * 3, y, width, height, iterations, re_min, im_max, pixel_size);
			}
		}

		const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::string bmp = encode_bmp(width, height, pixels.data());

		// hop back to the nginx warp so the completion handler can safely push
		// the return values onto the Lua stack.
		co_await iris_switch(main);
		co_return std::make_tuple(std::move(bmp), elapsed_ms);
	}

	// ---------------------------------------------------------------------------
	// demo 2: concurrent http fetch (fan-out / fan-in)
	// ---------------------------------------------------------------------------

	namespace {
		struct http_url_t {
			std::string host;
			std::string path = "/";
			size_t port = 80;
			bool ok = false;
			std::string error;
		};

		http_url_t parse_http_url(const std::string& url) {
			http_url_t result;
			if (url.rfind("http://", 0) != 0) {
				result.error = "only http:// URLs are supported";
				return result;
			}

			std::string rest = url.substr(7);
			size_t slash = rest.find('/');
			std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
			std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);

			size_t colon = authority.rfind(':');
			if (colon != std::string::npos) {
				result.port = (size_t)std::atol(authority.substr(colon + 1).c_str());
				result.host = authority.substr(0, colon);
			} else {
				result.host = authority;
			}

			if (result.host.empty() || result.port == 0) {
				result.error = "invalid url: " + url;
				return result;
			}
			if (path.empty()) path = "/";
			result.path = std::move(path);
			result.ok = true;
			return result;
		}

#ifdef _WIN32
		ngx_lua_cpp_t::fetch_entry_t http_get(const http_url_t& url, size_t timeout_ms, size_t max_bytes) {
			ngx_lua_cpp_t::fetch_entry_t entry;
			std::get<0>(entry) = "http://" + url.host + (url.port == 80 ? "" : ":" + std::to_string(url.port)) + url.path;
			const auto t0 = std::chrono::steady_clock::now();

			HINTERNET hSession = ::WinHttpOpen(L"ngx_lua_cpp-demo/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (hSession == nullptr) {
				std::get<5>(entry) = "WinHttpOpen failed";
				return entry;
			}

			::WinHttpSetTimeouts(hSession, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms, (int)timeout_ms);

			const std::wstring whost(url.host.begin(), url.host.end());
			const std::wstring wpath(url.path.begin(), url.path.end());

			HINTERNET hConnect = ::WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)url.port, 0);
			if (hConnect == nullptr) {
				::WinHttpCloseHandle(hSession);
				std::get<5>(entry) = "WinHttpConnect failed";
				return entry;
			}

			HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
			if (hRequest == nullptr) {
				::WinHttpCloseHandle(hConnect);
				::WinHttpCloseHandle(hSession);
				std::get<5>(entry) = "WinHttpOpenRequest failed";
				return entry;
			}

			bool ok = ::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
			if (ok) {
				ok = ::WinHttpReceiveResponse(hRequest, nullptr) != FALSE;
			}

			if (!ok) {
				std::get<5>(entry) = "WinHttpSendRequest/ReceiveResponse failed";
			} else {
				DWORD status = 0;
				DWORD status_len = sizeof(status);
				if (::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX)) {
					std::get<2>(entry) = (size_t)status;
				}

				std::string body;
				DWORD available = 0;
				while (::WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
					DWORD want = (DWORD)std::min<size_t>(available, max_bytes - body.size());
					if (want == 0) break;
					std::string chunk(want, '\0');
					DWORD read = 0;
					if (!::WinHttpReadData(hRequest, chunk.data(), want, &read) || read == 0) break;
					body.append(chunk.data(), read);
					if (body.size() >= max_bytes) break;
				}

				std::get<3>(entry) = body.size();
				std::get<1>(entry) = true;
				std::get<5>(entry) = body.substr(0, 256);
			}

			::WinHttpCloseHandle(hRequest);
			::WinHttpCloseHandle(hConnect);
			::WinHttpCloseHandle(hSession);

			std::get<4>(entry) = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			return entry;
		}
#else
		ngx_lua_cpp_t::fetch_entry_t http_get(const http_url_t& url, size_t timeout_ms, size_t max_bytes) {
			ngx_lua_cpp_t::fetch_entry_t entry;
			std::get<0>(entry) = "http://" + url.host + (url.port == 80 ? "" : ":" + std::to_string(url.port)) + url.path;
			std::get<5>(entry) = "not supported on this platform";
			return entry;
		}
#endif
	}

	iris_coroutine_t<ngx_lua_cpp_t::fetch_result_t> ngx_lua_cpp_t::fetch(std::vector<std::string> urls, size_t timeout_ms) {
		ngx_lua_cpp_t::fetch_result_t result;
		std::vector<fetch_entry_t>& entries = std::get<0>(result);
		entries.resize(urls.size());

		if (urls.empty()) {
			co_return std::move(result);
		}

		timeout_ms = std::clamp(timeout_ms, size_t(100), size_t(30000));

		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);
		const auto t0 = std::chrono::steady_clock::now();

		// fan-out: one blocking WinHTTP request per url, each on the worker pool.
		using fetch_task_t = iris_awaitable_t<ngx_warp_t, std::function<fetch_entry_t()>>;
		std::vector<std::unique_ptr<fetch_task_t>> tasks;
		std::vector<size_t> task_indices;
		tasks.reserve(urls.size());
		for (size_t i = 0; i < urls.size(); i++) {
			http_url_t url = parse_http_url(urls[i]);
			if (!url.ok) {
				// parse error: fill the entry directly, no pool task needed
				std::get<0>(entries[i]) = urls[i];
				std::get<5>(entries[i]) = url.error;
				continue;
			}
			tasks.push_back(std::make_unique<fetch_task_t>(main, std::function<fetch_entry_t()>([url, timeout_ms]() {
				return http_get(url, timeout_ms, 1024 * 1024);
			}), 1));
			tasks.back()->dispatch();
			task_indices.push_back(i);
		}

		// fan-in
		for (size_t k = 0; k < task_indices.size(); k++) {
			entries[task_indices[k]] = co_await *tasks[k];
		}

		std::get<1>(result) = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		co_await iris_switch(main);
		co_return std::move(result);
	}

	// ---------------------------------------------------------------------------
	// demo 4: UDP round-trip (datagram echo)
	// ---------------------------------------------------------------------------

	namespace {
#ifdef _WIN32
		using udp_socket_t = SOCKET;
		constexpr udp_socket_t ngx_udp_invalid_socket = INVALID_SOCKET;
#else
		using udp_socket_t = int;
		constexpr udp_socket_t ngx_udp_invalid_socket = -1;
#endif

		void udp_close_socket(udp_socket_t sock) {
#ifdef _WIN32
			::closesocket(sock);
#else
			::close(sock);
#endif
		}

		void udp_set_receive_timeout(udp_socket_t sock, DWORD timeout_ms) {
#ifdef _WIN32
			::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
			struct timeval tv = { (time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000) };
			::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
		}

		// resolve + send one datagram; returns (ok, err)
		std::tuple<bool, std::string> udp_sendto_impl(const std::string& host, size_t port, const std::string& payload) {
			if (host.empty() || port == 0 || port > 65535) {
				return { false, "invalid host or port" };
			}

#ifdef _WIN32
			WSADATA wsa;
			if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
				return { false, "WSAStartup failed" };
			}
#endif

			struct addrinfo hints = {};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_protocol = IPPROTO_UDP;

			char port_buf[16];
			std::snprintf(port_buf, sizeof(port_buf), "%zu", port);

			struct addrinfo* addr_list = nullptr;
			if (::getaddrinfo(host.c_str(), port_buf, &hints, &addr_list) != 0 || addr_list == nullptr) {
#ifdef _WIN32
				::WSACleanup();
#endif
				return { false, "getaddrinfo failed: " + host };
			}

			udp_socket_t sock = ::socket(addr_list->ai_family, addr_list->ai_socktype, addr_list->ai_protocol);
			if (sock == ngx_udp_invalid_socket) {
				::freeaddrinfo(addr_list);
#ifdef _WIN32
				::WSACleanup();
#endif
				return { false, "socket() failed" };
			}

			int sent = ::sendto(sock, payload.data(), (int)payload.size(), 0, addr_list->ai_addr, (int)addr_list->ai_addrlen);
			udp_close_socket(sock);
			::freeaddrinfo(addr_list);
#ifdef _WIN32
			::WSACleanup();
#endif

			if (sent < 0) {
				return { false, "sendto failed" };
			}
			return { true, "" };
		}

		// open a file for reading; on Windows this uses CreateFileA with
		// explicit share flags so files nginx itself keeps open (access log)
		// are readable -- the MSVC CRT fopen can fail with EACCES on them.
		FILE* ngx_file_open_read(const std::string& path) {
#ifdef _WIN32
			HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return nullptr;
			}
			int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
			if (fd < 0) {
				::CloseHandle(h);
				return nullptr;
			}
			return _fdopen(fd, "rb");
#else
			return std::fopen(path.c_str(), "rb");
#endif
		}

		// blocking datagram exchange; runs on the worker pool.
		// one socket carries both send and receive so the reply arrives on
		// the same ephemeral port we sent from.
		ngx_lua_cpp_t::udp_result_t udp_roundtrip_impl(const std::string& host, size_t port, const std::string& payload, size_t timeout_ms) {
			using namespace std::chrono;
			ngx_lua_cpp_t::udp_result_t result;
			const auto t0 = steady_clock::now();

			if (host.empty() || port == 0 || port > 65535) {
				std::get<3>(result) = "invalid host or port";
				return result;
			}

#ifdef _WIN32
			WSADATA wsa;
			if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
				std::get<3>(result) = "WSAStartup failed";
				return result;
			}
#endif

			// resolve into a datagram socket address (IPv4 or IPv6)
			struct addrinfo hints = {};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_protocol = IPPROTO_UDP;

			char port_buf[16];
			std::snprintf(port_buf, sizeof(port_buf), "%zu", port);

			struct addrinfo* addr_list = nullptr;
			if (::getaddrinfo(host.c_str(), port_buf, &hints, &addr_list) != 0 || addr_list == nullptr) {
#ifdef _WIN32
				::WSACleanup();
#endif
				std::get<3>(result) = "getaddrinfo failed: " + host;
				return result;
			}

			udp_socket_t sock = ::socket(addr_list->ai_family, addr_list->ai_socktype, addr_list->ai_protocol);
			if (sock == ngx_udp_invalid_socket) {
				::freeaddrinfo(addr_list);
#ifdef _WIN32
				::WSACleanup();
#endif
				std::get<3>(result) = "socket() failed";
				return result;
			}

			// receive timeout so a dead listener cannot hang the worker thread
			udp_set_receive_timeout(sock, (DWORD)std::clamp(timeout_ms, size_t(50), size_t(30000)));

			int sent = ::sendto(sock, payload.data(), (int)payload.size(), 0, addr_list->ai_addr, (int)addr_list->ai_addrlen);
			if (sent < 0) {
				udp_close_socket(sock);
				::freeaddrinfo(addr_list);
#ifdef _WIN32
				::WSACleanup();
#endif
				std::get<3>(result) = "sendto failed";
				return result;
			}

			char buf[65536];
			int recvd = ::recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
			if (recvd < 0) {
				std::get<3>(result) = "recvfrom failed or timed out";
			} else {
				std::get<0>(result) = true;
				std::get<1>(result) = std::string(buf, (size_t)recvd);
			}

			udp_close_socket(sock);
			::freeaddrinfo(addr_list);
#ifdef _WIN32
			::WSACleanup();
#endif

			std::get<2>(result) = duration<double, std::milli>(steady_clock::now() - t0).count();
			return result;
		}
	}

	// one background thread per listened port; it only receives datagrams
	// into a bounded queue. Blocking recvfrom is woken every 500 ms by the
	// socket receive timeout so the thread can observe stop().
	struct ngx_lua_cpp_t::udp_listener_t {
		size_t port = 0;
		udp_socket_t sock = ngx_udp_invalid_socket;
		std::atomic<bool> stop{ false };
		std::thread thread;

		std::mutex mutex;
		std::condition_variable cv;
		std::deque<std::tuple<std::string, std::string, size_t>> queue; // (payload, from_host, from_port)
		bool has_sender = false;
		struct sockaddr_storage sender = {};
		socklen_t sender_len = 0;

		~udp_listener_t() {
			stop_listen();
		}

		void stop_listen() {
			if (stop.exchange(true)) {
				return;
			}
			if (sock != ngx_udp_invalid_socket) {
				udp_close_socket(sock);
				sock = ngx_udp_invalid_socket;
			}
			cv.notify_all();
			if (thread.joinable()) {
				thread.join();
			}
		}

		void run() {
#ifdef _WIN32
			WSADATA wsa;
			::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

			char buf[65536];
			while (!stop.load(std::memory_order_relaxed)) {
				struct sockaddr_storage from = {};
				socklen_t from_len = sizeof(from);
				int n = ::recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &from_len);
				if (n < 0) {
					continue; // 500 ms timeout or transient error; re-check stop
				}

				char host_buf[NI_MAXHOST];
				char port_buf[NI_MAXSERV];
				if (::getnameinfo((struct sockaddr*)&from, from_len, host_buf, sizeof(host_buf), port_buf, sizeof(port_buf), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
					continue;
				}

				{
					std::lock_guard<std::mutex> guard(mutex);
					queue.emplace_back(std::string(buf, (size_t)n), host_buf, (size_t)std::atoi(port_buf));
					if (queue.size() > 4096) {
						queue.pop_front(); // bounded
					}
					// remember the sender so udp_reply() can answer from this socket
					sender = from;
					sender_len = from_len;
					has_sender = true;
				}
				cv.notify_one();
			}

#ifdef _WIN32
			::WSACleanup();
#endif
		}
	};

	void ngx_lua_cpp_t::stop_impl() {
		// stop UDP listeners first so queued recv waiters wake up cleanly
		{
			std::lock_guard<std::mutex> guard(listeners_mutex);
			for (auto& [port, listener] : udp_listeners) {
				listener->stop_listen();
			}
			udp_listeners.clear();
		}

		async_worker->terminate();
		async_worker->join();

		// manually polling events
		while (main_warp->poll()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		main_thread_index = ~(size_t)0;
		reset_main_warp();
	}

	std::string ngx_lua_cpp_t::udp_listen(size_t port) {
		if (port == 0 || port > 65535) {
			return "invalid port";
		}

		std::lock_guard<std::mutex> guard(listeners_mutex);
		if (udp_listeners.count(port) != 0) {
			return "already listening on port " + std::to_string(port);
		}

		auto listener = std::make_shared<udp_listener_t>();
		listener->port = port;

#ifdef _WIN32
		WSADATA wsa;
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			return "WSAStartup failed";
		}
#endif

		listener->sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (listener->sock == ngx_udp_invalid_socket) {
#ifdef _WIN32
			::WSACleanup();
#endif
			return "socket() failed";
		}

		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons((uint16_t)port);
		if (::bind(listener->sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
			udp_close_socket(listener->sock);
#ifdef _WIN32
			::WSACleanup();
#endif
			return "bind() failed on port " + std::to_string(port) + " (already in use?)";
		}

		// periodic wake-up so the receive thread can observe stop()
		udp_set_receive_timeout(listener->sock, 500);

		listener->thread = std::thread([listener]() { listener->run(); });
		udp_listeners[port] = listener;
		return "";
	}

	iris_coroutine_t<ngx_lua_cpp_t::udp_packet_result_t> ngx_lua_cpp_t::udp_recv(size_t port, size_t timeout_ms) {
		ngx_lua_cpp_t::udp_packet_result_t result; // (ok=false, "", "", 0, "")

		// fast path: timeout_ms == 0 polls the queue on the caller thread.
		// No worker-pool hop, no warp churn -- this is what the demo poll
		// loops (udp_echo_server/syslog/dns) run hundreds of times a second.
		if (timeout_ms == 0) {
			std::shared_ptr<udp_listener_t> listener;
			{
				std::lock_guard<std::mutex> guard(listeners_mutex);
				auto it = udp_listeners.find(port);
				if (it == udp_listeners.end()) {
					std::get<4>(result) = "not listening on port " + std::to_string(port);
					co_return std::move(result);
				}
				listener = it->second;
			}
			{
				std::lock_guard<std::mutex> lock(listener->mutex);
				if (!listener->queue.empty()) {
					auto& front = listener->queue.front();
					std::get<0>(result) = true;
					std::get<1>(result) = std::move(std::get<0>(front));
					std::get<2>(result) = std::move(std::get<1>(front));
					std::get<3>(result) = std::get<2>(front);
					listener->queue.pop_front();
				} else {
					std::get<4>(result) = "timeout";
				}
			}
			co_return std::move(result);
		}

		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		std::shared_ptr<udp_listener_t> listener;
		{
			std::lock_guard<std::mutex> guard(listeners_mutex);
			auto it = udp_listeners.find(port);
			if (it == udp_listeners.end()) {
				std::get<4>(result) = "not listening on port " + std::to_string(port);
				co_await iris_switch(main);
				co_return std::move(result);
			}
			listener = it->second;
		}

		using udp_task_t = iris_awaitable_t<ngx_warp_t, std::function<udp_packet_result_t()>>;
		udp_task_t task(main, std::function<udp_packet_result_t()>([listener, timeout_ms]() {
			ngx_lua_cpp_t::udp_packet_result_t r;
			std::unique_lock<std::mutex> lock(listener->mutex);

			if (listener->queue.empty()) {
				// wait for a datagram, the timeout, or listener shutdown;
				// timeout_ms == 0 polls without blocking
				listener->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [listener]() {
					return !listener->queue.empty() || listener->stop.load(std::memory_order_relaxed);
				});
			}

			if (!listener->queue.empty()) {
				auto& front = listener->queue.front();
				std::get<0>(r) = true;
				std::get<1>(r) = std::move(std::get<0>(front));
				std::get<2>(r) = std::move(std::get<1>(front));
				std::get<3>(r) = std::get<2>(front);
				listener->queue.pop_front();
			} else {
				std::get<4>(r) = "timeout";
			}
			return r;
		}), 1);
		task.dispatch();
		result = co_await task;

		co_await iris_switch(main);
		co_return std::move(result);
	}

	iris_coroutine_t<std::tuple<bool, std::string>> ngx_lua_cpp_t::udp_send(std::string host, size_t port, std::string payload) {
		std::tuple<bool, std::string> result = { false, "" };
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		using udp_task_t = iris_awaitable_t<ngx_warp_t, std::function<std::tuple<bool, std::string>()>>;
		udp_task_t task(main, std::function<std::tuple<bool, std::string>()>([host, port, payload]() {
			return udp_sendto_impl(host, port, payload);
		}), 1);
		task.dispatch();
		result = co_await task;

		co_await iris_switch(main);
		co_return std::move(result);
	}

	iris_coroutine_t<std::tuple<bool, std::string>> ngx_lua_cpp_t::udp_reply(size_t port, std::string payload) {
		std::tuple<bool, std::string> result = { false, "" };
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		std::shared_ptr<udp_listener_t> listener;
		{
			std::lock_guard<std::mutex> guard(listeners_mutex);
			auto it = udp_listeners.find(port);
			if (it == udp_listeners.end()) {
				std::get<1>(result) = "not listening on port " + std::to_string(port);
				co_await iris_switch(main);
				co_return std::move(result);
			}
			listener = it->second;
		}

		using udp_task_t = iris_awaitable_t<ngx_warp_t, std::function<std::tuple<bool, std::string>()>>;
		udp_task_t task(main, std::function<std::tuple<bool, std::string>()>([listener, payload]() {
			std::tuple<bool, std::string> r = { false, "" };
			sockaddr_storage sender;
			socklen_t sender_len;
			{
				std::lock_guard<std::mutex> guard(listener->mutex);
				if (!listener->has_sender) {
					std::get<1>(r) = "no sender recorded for this port yet";
					return r;
				}
				sender = listener->sender;
				sender_len = listener->sender_len;
			}
			// send FROM the listening socket so the reply source address is
			// the port the client sent to (connected clients require this)
			int sent = ::sendto(listener->sock, payload.data(), (int)payload.size(), 0, (struct sockaddr*)&sender, sender_len);
			if (sent < 0) {
				std::get<1>(r) = "sendto failed";
				return r;
			}
			std::get<0>(r) = true;
			return r;
		}), 1);
		task.dispatch();
		result = co_await task;

		co_await iris_switch(main);
		co_return std::move(result);
	}

	iris_coroutine_t<ngx_lua_cpp_t::udp_result_t> ngx_lua_cpp_t::udp_echo(std::string host, size_t port, std::string payload, size_t timeout_ms) {
		ngx_lua_cpp_t::udp_result_t result;

		// hop to the worker pool; the blocking socket exchange never touches
		// the nginx thread (same offload pattern as fetch/mandelbrot).
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		using udp_task_t = iris_awaitable_t<ngx_warp_t, std::function<udp_result_t()>>;
		udp_task_t task(main, std::function<udp_result_t()>([host, port, payload, timeout_ms]() {
			return udp_roundtrip_impl(host, port, payload, timeout_ms);
		}), 1);
		task.dispatch();
		result = co_await task;

		co_await iris_switch(main);
		co_return std::move(result);
	}

	// ---------------------------------------------------------------------------
	// demo 6: crypto primitives (pure C++, no external deps)
	// ---------------------------------------------------------------------------

	namespace {
		// ---- SHA-256 (FIPS 180-4) ----
		struct sha256_ctx_t {
			uint32_t state[8];
			uint64_t bit_len = 0;
			uint8_t buffer[64] = {};
			size_t buffer_len = 0;
		};

		constexpr uint32_t sha256_k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};

		uint32_t sha256_rotr(uint32_t x, unsigned n) {
			return (x >> n) | (x << (32 - n));
		}

		void sha256_transform(sha256_ctx_t& ctx, const uint8_t* block) {
			uint32_t w[64];
			for (unsigned i = 0; i < 16; i++) {
				w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
					((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
			}
			for (unsigned i = 16; i < 64; i++) {
				uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
				uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}

			uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
			uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

			for (unsigned i = 0; i < 64; i++) {
				uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
				uint32_t ch = (e & f) ^ (~e & g);
				uint32_t temp1 = h + S1 + ch + sha256_k[i] + w[i];
				uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
				uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
				uint32_t temp2 = S0 + maj;
				h = g; g = f; f = e; e = d + temp1;
				d = c; c = b; b = a; a = temp1 + temp2;
			}

			ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
			ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
		}

		void sha256_init(sha256_ctx_t& ctx) {
			ctx.state[0] = 0x6a09e667; ctx.state[1] = 0xbb67ae85;
			ctx.state[2] = 0x3c6ef372; ctx.state[3] = 0xa54ff53a;
			ctx.state[4] = 0x510e527f; ctx.state[5] = 0x9b05688c;
			ctx.state[6] = 0x1f83d9ab; ctx.state[7] = 0x5be0cd19;
			ctx.bit_len = 0;
			ctx.buffer_len = 0;
		}

		void sha256_update(sha256_ctx_t& ctx, const uint8_t* data, size_t len) {
			ctx.bit_len += (uint64_t)len * 8;
			while (len > 0) {
				size_t take = std::min(len, 64 - ctx.buffer_len);
				std::memcpy(ctx.buffer + ctx.buffer_len, data, take);
				ctx.buffer_len += take;
				data += take;
				len -= take;
				if (ctx.buffer_len == 64) {
					sha256_transform(ctx, ctx.buffer);
					ctx.buffer_len = 0;
				}
			}
		}

		std::string sha256_final_hex(sha256_ctx_t& ctx) {
			// append 0x80, zeros, then the 8-byte big-endian bit length,
			// WITHOUT touching ctx.bit_len (it must stay the message length)
			const uint64_t bits = ctx.bit_len;
			uint8_t pad[128];
			size_t pad_len = 0;
			pad[pad_len++] = 0x80;
			size_t zeros = (ctx.buffer_len < 56) ? (56 - ctx.buffer_len - 1) : (120 - ctx.buffer_len - 1);
			std::memset(pad + pad_len, 0, zeros);
			pad_len += zeros;
			for (int i = 7; i >= 0; i--) {
				pad[pad_len++] = (uint8_t)(bits >> (i * 8));
			}

			size_t off = 0;
			while (off < pad_len) {
				size_t take = std::min(pad_len - off, 64 - ctx.buffer_len);
				std::memcpy(ctx.buffer + ctx.buffer_len, pad + off, take);
				ctx.buffer_len += take;
				off += take;
				if (ctx.buffer_len == 64) {
					sha256_transform(ctx, ctx.buffer);
					ctx.buffer_len = 0;
				}
			}

			static const char* hexdigits = "0123456789abcdef";
			std::string out;
			out.reserve(64);
			for (uint32_t v : ctx.state) {
				for (int shift = 28; shift >= 0; shift -= 4) {
					out.push_back(hexdigits[(v >> shift) & 0xf]);
				}
			}
			return out;
		}

		std::string sha256_hex_impl(const std::string& data) {
			sha256_ctx_t ctx;
			sha256_init(ctx);
			sha256_update(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
			return sha256_final_hex(ctx);
		}

		std::string hmac_sha256_raw_impl(const std::string& key, const std::string& data) {
			std::string k = key;
			if (k.size() > 64) {
				std::string digest = sha256_hex_impl(k);
				std::string raw;
				raw.reserve(32);
				for (size_t i = 0; i + 1 < digest.size(); i += 2) {
					auto nib = [](char c) -> uint8_t {
						if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
						return (uint8_t)((c | 0x20) - 'a' + 10);
					};
					raw.push_back((char)((nib(digest[i]) << 4) | nib(digest[i + 1])));
				}
				k = std::move(raw);
			}
			uint8_t ipad[64], opad[64];
			std::memset(ipad, 0x36, sizeof(ipad));
			std::memset(opad, 0x5c, sizeof(opad));
			for (size_t i = 0; i < k.size(); i++) {
				ipad[i] ^= (uint8_t)k[i];
				opad[i] ^= (uint8_t)k[i];
			}

			sha256_ctx_t inner;
			sha256_init(inner);
			sha256_update(inner, ipad, sizeof(ipad));
			sha256_update(inner, reinterpret_cast<const uint8_t*>(data.data()), data.size());
			std::string inner_hex = sha256_final_hex(inner);

			std::string inner_raw;
			inner_raw.reserve(32);
			for (size_t i = 0; i + 1 < inner_hex.size(); i += 2) {
				auto nib = [](char c) -> uint8_t {
					if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
					return (uint8_t)((c | 0x20) - 'a' + 10);
				};
				inner_raw.push_back((char)((nib(inner_hex[i]) << 4) | nib(inner_hex[i + 1])));
			}

			sha256_ctx_t outer;
			sha256_init(outer);
			sha256_update(outer, opad, sizeof(opad));
			sha256_update(outer, reinterpret_cast<const uint8_t*>(inner_raw.data()), inner_raw.size());
			std::string outer_hex = sha256_final_hex(outer);

			std::string out;
			out.reserve(32);
			for (size_t i = 0; i + 1 < outer_hex.size(); i += 2) {
				auto nib = [](char c) -> uint8_t {
					if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
					return (uint8_t)((c | 0x20) - 'a' + 10);
				};
				out.push_back((char)((nib(outer_hex[i]) << 4) | nib(outer_hex[i + 1])));
			}
			return out;
		}

		std::string hmac_sha256_hex_impl(const std::string& key, const std::string& data) {
			const std::string raw = hmac_sha256_raw_impl(key, data);
			static const char* hexdigits = "0123456789abcdef";
			std::string out;
			out.reserve(64);
			for (unsigned char c : raw) {
				out.push_back(hexdigits[c >> 4]);
				out.push_back(hexdigits[c & 0xf]);
			}
			return out;
		}

		constexpr char b64url_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

		std::string base64url_encode_impl(const std::string& in) {
			std::string out;
			out.reserve((in.size() + 2) / 3 * 4);
			for (size_t i = 0; i < in.size(); i += 3) {
				uint32_t v = (uint8_t)in[i] << 16;
				if (i + 1 < in.size()) v |= (uint8_t)in[i + 1] << 8;
				if (i + 2 < in.size()) v |= (uint8_t)in[i + 2];
				out.push_back(b64url_table[(v >> 18) & 0x3f]);
				out.push_back(b64url_table[(v >> 12) & 0x3f]);
				if (i + 1 < in.size()) out.push_back(b64url_table[(v >> 6) & 0x3f]);
				if (i + 2 < in.size()) out.push_back(b64url_table[v & 0x3f]);
			}
			return out;
		}

		std::string base64url_decode_impl(const std::string& in, bool& ok) {
			ok = false;
			std::string out;
			out.reserve(in.size() * 3 / 4);
			uint32_t buf = 0;
			int bits = 0;
			for (char c : in) {
				if (c == '=') break;
				uint8_t v;
				if (c >= 'A' && c <= 'Z') v = (uint8_t)(c - 'A');
				else if (c >= 'a' && c <= 'z') v = (uint8_t)(c - 'a' + 26);
				else if (c >= '0' && c <= '9') v = (uint8_t)(c - '0' + 52);
				else if (c == '-') v = 62;
				else if (c == '_') v = 63;
				else return out; // invalid char
				buf = (buf << 6) | v;
				bits += 6;
				if (bits >= 8) {
					bits -= 8;
					out.push_back((char)((buf >> bits) & 0xff));
				}
			}
			ok = true;
			return out;
		}

		std::string random_hex_impl(size_t nbytes) {
			static std::mt19937_64 rng{ std::random_device{}() };
			static const char* hexdigits = "0123456789abcdef";
			std::string out;
			out.reserve(nbytes * 2);
			for (size_t i = 0; i < nbytes; i++) {
				uint64_t r = rng();
				out.push_back(hexdigits[r & 0xf]);
				out.push_back(hexdigits[(r >> 4) & 0xf]);
			}
			return out;
		}

		// ---- log line timestamp -> unix seconds (civil days algorithm) ----
		const char* log_months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

		double log_parse_time(const char* s, size_t len) {
			// expect [dd/Mon/yyyy:HH:MM:SS
			if (len < 20 || s[0] != '[') return 0;
			int day = 0, year = 0, hh = 0, mi = 0, ss = 0;
			int month = -1;
			for (int i = 0; i < 2; i++) day = day * 10 + (s[1 + i] - '0');
			for (int m = 0; m < 12; m++) {
				if (std::strncmp(s + 4, log_months[m], 3) == 0) { month = m + 1; break; }
			}
			for (int i = 0; i < 4; i++) year = year * 10 + (s[8 + i] - '0');
			hh = (s[13] - '0') * 10 + (s[14] - '0');
			mi = (s[16] - '0') * 10 + (s[17] - '0');
			ss = (s[19] - '0') * 10 + (s[20] - '0');
			if (month < 0) return 0;

			// days_from_civil (Howard Hinnant)
			int y = year - (month <= 2 ? 1 : 0);
			int era = (y >= 0 ? y : y - 399) / 400;
			unsigned yoe = (unsigned)(y - era * 400);
			unsigned mp = (unsigned)(month + (month > 2 ? -3 : 9));
			unsigned doy = (153 * mp + 2) / 5 + (unsigned)day - 1;
			unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
			long long days = (long long)era * 146097 + (long long)doe - 719468;
			return (double)days * 86400.0 + (double)(hh * 3600 + mi * 60 + ss);
		}
	}

	std::string ngx_lua_cpp_t::sha256(const std::string& data) {
		return sha256_hex_impl(data);
	}

	std::string ngx_lua_cpp_t::hmac_sha256(const std::string& key, const std::string& data) {
		return hmac_sha256_hex_impl(key, data);
	}

	std::string ngx_lua_cpp_t::hmac_sha256_raw(const std::string& key, const std::string& data) {
		return hmac_sha256_raw_impl(key, data);
	}

	std::string ngx_lua_cpp_t::base64url_encode(const std::string& data) {
		return base64url_encode_impl(data);
	}

	std::tuple<std::string, std::string> ngx_lua_cpp_t::base64url_decode(const std::string& data) {
		bool ok = false;
		std::string out = base64url_decode_impl(data, ok);
		if (!ok) {
			return { std::string(), "invalid base64url input" };
		}
		return { std::move(out), std::string() };
	}

	std::string ngx_lua_cpp_t::random_hex(size_t nbytes) {
		return random_hex_impl(std::clamp(nbytes, size_t(1), size_t(1024)));
	}

	// ---------------------------------------------------------------------------
	// demo 7: event bus + LRU cache (cross-request shared C++ state)
	// ---------------------------------------------------------------------------

	struct ngx_lua_cpp_t::event_bus_t {
		std::mutex mutex;
		double next_id = 1;
		std::map<std::string, std::deque<bus_event_t>> topics;

		void pub(const std::string& topic, const std::string& payload) {
			std::lock_guard<std::mutex> guard(mutex);
			auto& q = topics[topic];
			double id = next_id++;
			double ts = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
			q.emplace_back(id, ts, payload);
			if (q.size() > 512) {
				q.pop_front();
			}
		}

		std::pair<std::vector<bus_event_t>, double> poll(const std::string& topic, double last_id) {
			std::lock_guard<std::mutex> guard(mutex);
			std::vector<bus_event_t> out;
			double next = last_id;
			auto it = topics.find(topic);
			if (it != topics.end()) {
				for (const auto& e : it->second) {
					if (std::get<0>(e) > last_id) {
						out.push_back(e);
						next = std::get<0>(e);
					}
				}
			}
			return { std::move(out), next };
		}
	};

	struct ngx_lua_cpp_t::lru_cache_t {
		static double now_ms() {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::mutex mutex;
		size_t max_entries = 1024;
		size_t hits = 0;
		size_t misses = 0;
		size_t evictions = 0;
		std::list<std::string> lru; // front = most recently used
		std::map<std::string, std::pair<std::string, double>> entries; // key -> (value, expire_ts_ms; 0 = never)
		std::map<std::string, std::list<std::string>::iterator> iters;

		bool set(const std::string& key, const std::string& value, double ttl_ms) {
			std::lock_guard<std::mutex> guard(mutex);
			double now = now_ms();
			double expire = ttl_ms > 0 ? now + ttl_ms : 0.0;
			auto it = entries.find(key);
			if (it != entries.end()) {
				it->second = { value, expire };
				lru.splice(lru.begin(), lru, iters[key]);
				return true;
			}
			if (entries.size() >= max_entries) {
				std::string victim = lru.back();
				lru.pop_back();
				entries.erase(victim);
				iters.erase(victim);
				evictions++;
			}
			entries[key] = { value, expire };
			lru.push_front(key);
			iters[key] = lru.begin();
			return true;
		}

		std::tuple<bool, std::string, double> get(const std::string& key) {
			std::lock_guard<std::mutex> guard(mutex);
			auto it = entries.find(key);
			if (it == entries.end()) {
				misses++;
				return { false, std::string(), 0.0 };
			}
			double now = now_ms();
			if (it->second.second > 0 && now >= it->second.second) {
				entries.erase(it);
				lru.erase(iters[key]);
				iters.erase(key);
				misses++;
				return { false, std::string(), 0.0 };
			}
			hits++;
			lru.splice(lru.begin(), lru, iters[key]);
			double remain = it->second.second > 0 ? it->second.second - now : 0.0;
			return { true, it->second.first, remain };
		}

		std::map<std::string, double> stats() {
			std::lock_guard<std::mutex> guard(mutex);
			return {
				{ "entries", (double)entries.size() },
				{ "max_entries", (double)max_entries },
				{ "hits", (double)hits },
				{ "misses", (double)misses },
				{ "evictions", (double)evictions },
			};
		}
	};

	void ngx_lua_cpp_t::pub(const std::string& topic, const std::string& payload) {
		event_bus->pub(topic, payload);
	}

	std::tuple<std::vector<ngx_lua_cpp_t::bus_event_t>, double> ngx_lua_cpp_t::events(const std::string& topic, double last_id) {
		auto r = event_bus->poll(topic, last_id);
		return { std::move(r.first), r.second };
	}

	bool ngx_lua_cpp_t::cache_set(const std::string& key, const std::string& value, double ttl_ms) {
		return lru_cache->set(key, value, std::clamp(ttl_ms, 0.0, 86400000.0));
	}

	std::tuple<bool, std::string, double> ngx_lua_cpp_t::cache_get(const std::string& key) {
		return lru_cache->get(key);
	}

	std::map<std::string, double> ngx_lua_cpp_t::cache_stats() {
		return lru_cache->stats();
	}

	// ---------------------------------------------------------------------------
	// demo 8: file hashing + log analysis (blocking I/O on the worker pool)
	// ---------------------------------------------------------------------------

	iris_coroutine_t<std::tuple<std::string, double, double>> ngx_lua_cpp_t::file_sha256(std::string path) {
		std::tuple<std::string, double, double> result; // (hex, size_bytes, elapsed_ms)
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		const auto t0 = std::chrono::steady_clock::now();
		sha256_ctx_t ctx;
		sha256_init(ctx);
		double size_bytes = 0;
		FILE* f = ngx_file_open_read(path);
		if (f == nullptr) {
			std::get<0>(result) = "file not found";
		} else {
			char buf[65536];
			size_t n;
			while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
				sha256_update(ctx, reinterpret_cast<const uint8_t*>(buf), n);
				size_bytes += (double)n;
			}
			std::fclose(f);
			std::get<0>(result) = sha256_final_hex(ctx);
			std::get<1>(result) = size_bytes;
		}

		std::get<2>(result) = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

		co_await iris_switch(main);
		co_return std::move(result);
	}

	iris_coroutine_t<ngx_lua_cpp_t::log_stats_t> ngx_lua_cpp_t::log_analyze(std::string path, double max_lines) {
		ngx_lua_cpp_t::log_stats_t result;
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);

		auto& stats = std::get<0>(result);
		auto& top_urls = std::get<1>(result);
		auto& statuses = std::get<2>(result);
		auto& per_second = std::get<3>(result);

		const auto t0 = std::chrono::steady_clock::now();
		std::map<std::string, double> url_counts;
		std::map<double, double> second_counts;
		std::map<std::string, double> status_counts;
		double lines = 0, total = 0, first_ts = 0, last_ts = 0;

		FILE* f = ngx_file_open_read(path);
		if (f == nullptr) {
			stats["error"] = 1.0;
			stats["errno"] = (double)errno;
			stats["elapsed_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			co_await iris_switch(main);
			co_return std::move(result);
		}

		const double max_lines_d = max_lines > 0 ? max_lines : 100000;
		std::string line;
		line.reserve(1024);
		char c;
		bool skip_to_newline = false;
		while ((c = (char)std::fgetc(f)) != EOF) {
			if (skip_to_newline) {
				if (c == '\n') { skip_to_newline = false; }
				continue;
			}
			if (c == '\n') {
				lines++;
				// parse: "ip - - [ts] \"METHOD /path HTTP/x.y\" STATUS BYTES ..."
				size_t ts_start = line.find('[');
				size_t quote1 = line.find('"');
				size_t quote2 = quote1 == std::string::npos ? std::string::npos : line.find('"', quote1 + 1);
				size_t http_pos = line.find("HTTP/");
				if (ts_start != std::string::npos && quote1 != std::string::npos && quote2 != std::string::npos && http_pos != std::string::npos && http_pos < quote2) {
					double ts = log_parse_time(line.data() + ts_start, line.size() - ts_start);
					// request line between the quotes
					std::string req = line.substr(quote1 + 1, quote2 - quote1 - 1);
					// status code after the closing quote
					size_t p = quote2 + 1;
					while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
					size_t status_start = p;
					while (p < line.size() && line[p] >= '0' && line[p] <= '9') p++;
					std::string status = line.substr(status_start, p - status_start);

					if (ts > 0 && !status.empty()) {
						total++;
						if (first_ts == 0) first_ts = ts;
						last_ts = ts;
						second_counts[ts] += 1;
						status_counts[status] += 1;
						// request URL: "GET /path HTTP/1.1"
						size_t sp1 = req.find(' ');
						size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
						if (sp1 != std::string::npos && sp2 != std::string::npos) {
							url_counts[req.substr(sp1 + 1, sp2 - sp1 - 1)] += 1;
						}
					}
				}
				if (lines >= max_lines_d) {
					skip_to_newline = true; // stop parsing but drain the file
					break;
				}
				line.clear();
				continue;
			}
			line.push_back(c);
		}
		std::fclose(f);

		// top urls (up to 20, sorted desc)
		std::vector<std::pair<std::string, double>> url_vec(url_counts.begin(), url_counts.end());
		std::sort(url_vec.begin(), url_vec.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		for (size_t i = 0; i < url_vec.size() && i < 20; i++) {
			top_urls.emplace_back(std::move(url_vec[i].first), url_vec[i].second);
		}

		// statuses sorted numerically
		std::vector<std::pair<std::string, double>> status_vec(status_counts.begin(), status_counts.end());
		std::sort(status_vec.begin(), status_vec.end(), [](const auto& a, const auto& b) {
			double na = std::atof(a.first.c_str());
			double nb = std::atof(b.first.c_str());
			return na < nb;
		});
		for (auto& s : status_vec) {
			statuses.emplace_back(std::move(s.first), s.second);
		}

		// per-second buckets: last up to 60, relative index 0 = oldest
		std::vector<std::pair<double, double>> sec_vec(second_counts.begin(), second_counts.end());
		std::sort(sec_vec.begin(), sec_vec.end());
		size_t n = sec_vec.size();
		for (size_t i = n > 60 ? n - 60 : 0; i < n; i++) {
			per_second.emplace_back((double)(i - (n > 60 ? n - 60 : 0)), sec_vec[i].second);
		}

		double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		stats["lines"] = lines;
		stats["total"] = total;
		stats["status_2xx"] = status_counts["200"] + status_counts["201"] + status_counts["202"] + status_counts["204"];
		stats["status_3xx"] = status_counts["301"] + status_counts["302"] + status_counts["304"] + status_counts["307"];
		stats["status_4xx"] = status_counts["400"] + status_counts["401"] + status_counts["403"] + status_counts["404"] + status_counts["429"];
		stats["status_5xx"] = status_counts["500"] + status_counts["502"] + status_counts["503"] + status_counts["504"];
		stats["qps"] = (last_ts > first_ts && total > 0) ? total / (last_ts - first_ts) : 0.0;
		stats["span_seconds"] = last_ts > first_ts ? last_ts - first_ts : 0.0;
		stats["elapsed_ms"] = elapsed;

		co_await iris_switch(main);
		co_return std::move(result);
	}

	// ---------------------------------------------------------------------------
	// demo 3: in-memory job queue
	// ---------------------------------------------------------------------------

	std::string ngx_lua_cpp_t::job_submit(const std::string& kind, const std::map<std::string, double>& payload) {
		if (!is_running()) {
			return "";
		}

		auto job = std::make_shared<ngx_job_t>();
		{
			std::lock_guard<std::mutex> guard(jobs_mutex);
			job_counter++;
			char id_buf[32];
			std::snprintf(id_buf, sizeof(id_buf), "J%05zu", job_counter);
			job->id = id_buf;
			job->kind = kind;
			job->payload = payload;
			job->status = "queued";
			job->created_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
			jobs[job->id] = job;

			// keep the list bounded; ids are monotonic so the oldest jobs are first
			while (jobs.size() > 200) {
				jobs.erase(jobs.begin());
			}
		}

		// fire and forget: the runner hops to the worker pool and never touches Lua again
		iris_coroutine_t<void> runner = run_job(job);
		runner.run();
		return job->id;
	}

	std::tuple<std::string, double, double, std::string> ngx_lua_cpp_t::job_query(const std::string& id) {
		std::lock_guard<std::mutex> guard(jobs_mutex);
		auto it = jobs.find(id);
		if (it == jobs.end()) {
			return std::make_tuple(std::string("not_found"), 0.0, 0.0, std::string());
		}
		const std::shared_ptr<ngx_job_t>& job = it->second;
		return std::make_tuple(job->status, job->progress, job->elapsed_ms, job->result);
	}

	std::vector<std::tuple<std::string, std::string, std::string, double, double>> ngx_lua_cpp_t::job_list() {
		std::vector<std::tuple<std::string, std::string, std::string, double, double>> result;
		std::lock_guard<std::mutex> guard(jobs_mutex);
		result.reserve(jobs.size());
		for (auto& [id, job] : jobs) {
			result.emplace_back(id, job->kind, job->status, job->progress, job->elapsed_ms);
		}
		return result;
	}

	iris_coroutine_t<void> ngx_lua_cpp_t::run_job(std::shared_ptr<ngx_job_t> job) {
		ngx_warp_t* main = co_await iris_switch<ngx_warp_t>(nullptr);
		const auto t0 = std::chrono::steady_clock::now();

		{
			std::lock_guard<std::mutex> guard(jobs_mutex);
			job->status = "running";
		}
		event_bus->pub("jobs", job->id + "|" + job->kind + "|running|0.0|0.0");

		std::string result_text;
		bool failed = false;

		if (job->kind == "count_primes") {
			size_t limit = (size_t)(job->payload.count("limit") ? job->payload.at("limit") : 10000000.0);
			limit = std::clamp<size_t>(limit, 1000, 200000000);

			// base primes up to sqrt(limit)
			const size_t root = (size_t)std::sqrt((double)limit) + 1;
			std::vector<uint8_t> base(root + 1, 1);
			base[0] = base[1] = 0;
			for (size_t i = 2; i * i <= root; i++) {
				if (base[i]) {
					for (size_t k = i * i; k <= root; k += i) base[k] = 0;
				}
			}
			std::vector<size_t> primes;
			for (size_t i = 2; i <= root; i++) {
				if (base[i]) primes.push_back(i);
			}

			// segmented sieve, reported chunk by chunk through the warp
			const size_t chunks = 40;
			const size_t seg_size = std::max<size_t>(limit / chunks, 1);
			size_t count = 0;
			for (size_t c = 0; c < chunks; c++) {
				size_t lo = std::max(c * seg_size, size_t(2));
				size_t hi = std::min(lo + seg_size, limit);
				if (lo >= hi) break;

				std::vector<uint8_t> mark(hi - lo, 1);
				for (size_t p : primes) {
					size_t start = std::max(p * p, ((lo + p - 1) / p) * p);
					for (size_t k = start; k < hi; k += p) mark[k - lo] = 0;
				}
				for (uint8_t m : mark) count += m;

				// hop to the nginx warp to publish progress, then back to the pool
				co_await iris_switch(main);
				{
					std::lock_guard<std::mutex> guard(jobs_mutex);
					job->progress = (double)(c + 1) / (double)chunks;
				}
				event_bus->pub("jobs", job->id + "|" + job->kind + "|running|" +
					std::to_string(job->progress) + "|" +
					std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()));
				co_await iris_switch<ngx_warp_t>(nullptr);
			}

			result_text = std::to_string(count) + " primes <= " + std::to_string(limit);
		} else if (job->kind == "sleep_steps") {
			size_t steps = (size_t)(job->payload.count("steps") ? job->payload.at("steps") : 50.0);
			double ms = job->payload.count("ms") ? job->payload.at("ms") : 2000.0;
			steps = std::clamp<size_t>(steps, 1, 200);
			ms = std::clamp(ms, 100.0, 60000.0);
			const size_t step_ms = std::max<size_t>((size_t)(ms / (double)steps), 1);

			for (size_t s = 0; s < steps; s++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
				co_await iris_switch(main);
				{
					std::lock_guard<std::mutex> guard(jobs_mutex);
					job->progress = (double)(s + 1) / (double)steps;
				}
				event_bus->pub("jobs", job->id + "|" + job->kind + "|running|" +
					std::to_string(job->progress) + "|" +
					std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()));
				co_await iris_switch<ngx_warp_t>(nullptr);
			}

			result_text = "slept " + std::to_string(steps * step_ms) + " ms in " + std::to_string(steps) + " steps";
		} else {
			failed = true;
			result_text = "unknown job kind: " + job->kind;
		}

		co_await iris_switch(main);
		{
			std::lock_guard<std::mutex> guard(jobs_mutex);
			job->status = failed ? "failed" : "done";
			job->result = result_text;
			job->elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}
		event_bus->pub("jobs", job->id + "|" + job->kind + "|" + (failed ? "failed" : "done") + "|1.0|" +
			std::to_string(job->elapsed_ms));
		co_return;
	}

	size_t ngx_lua_cpp_t::get_hardware_concurrency() const noexcept {
		return std::thread::hardware_concurrency();
	}

	void ngx_lua_cpp_t::lua_registar(iris_lua_t lua, iris_lua_traits_t<ngx_lua_cpp_t>) {
		ngx_hooker_t::get_instance().registar(lua);

		lua.set_current_new<&iris_lua_t::place_new_object<ngx_lua_cpp_t>>("new");
		lua.set_current<&ngx_lua_cpp_t::start>("start");
		lua.set_current<&ngx_lua_cpp_t::stop>("stop");
		lua.set_current<&ngx_lua_cpp_t::is_running>("is_running");
		lua.set_current<&ngx_lua_cpp_t::get_hardware_concurrency>("get_hardware_concurrency");
		lua.set_current<&ngx_lua_cpp_t::sleep>("sleep");

		// demos
		lua.set_current<&ngx_lua_cpp_t::mandelbrot>("mandelbrot");
		lua.set_current<&ngx_lua_cpp_t::fetch>("fetch");
		lua.set_current<&ngx_lua_cpp_t::udp_echo>("udp_echo");
		lua.set_current<&ngx_lua_cpp_t::udp_listen>("udp_listen");
		lua.set_current<&ngx_lua_cpp_t::udp_recv>("udp_recv");
		lua.set_current<&ngx_lua_cpp_t::udp_send>("udp_send");
		lua.set_current<&ngx_lua_cpp_t::udp_reply>("udp_reply");
		lua.set_current<&ngx_lua_cpp_t::sha256>("sha256");
		lua.set_current<&ngx_lua_cpp_t::hmac_sha256>("hmac_sha256");
		lua.set_current<&ngx_lua_cpp_t::hmac_sha256_raw>("hmac_sha256_raw");
		lua.set_current<&ngx_lua_cpp_t::base64url_encode>("base64url_encode");
		lua.set_current<&ngx_lua_cpp_t::base64url_decode>("base64url_decode");
		lua.set_current<&ngx_lua_cpp_t::random_hex>("random_hex");
		lua.set_current<&ngx_lua_cpp_t::pub>("pub");
		lua.set_current<&ngx_lua_cpp_t::events>("events");
		lua.set_current<&ngx_lua_cpp_t::cache_set>("cache_set");
		lua.set_current<&ngx_lua_cpp_t::cache_get>("cache_get");
		lua.set_current<&ngx_lua_cpp_t::cache_stats>("cache_stats");
		lua.set_current<&ngx_lua_cpp_t::file_sha256>("file_sha256");
		lua.set_current<&ngx_lua_cpp_t::log_analyze>("log_analyze");
		lua.set_current<&ngx_lua_cpp_t::job_submit>("job_submit");
		lua.set_current<&ngx_lua_cpp_t::job_query>("job_query");
		lua.set_current<&ngx_lua_cpp_t::job_list>("job_list");

		lua.set_current<&ngx_lua_cpp_t::__async_worker__>("__async_worker__");
	}

	void* ngx_lua_cpp_t::__async_worker__(void* new_async_worker_ptr) {
		if (new_async_worker_ptr != nullptr && set_async_worker(*reinterpret_cast<std::shared_ptr<iris_async_worker_t<>>*>(new_async_worker_ptr))) {
			return new_async_worker_ptr;
		} else {
			return reinterpret_cast<void*>(&async_worker);
		}
	}

	bool ngx_lua_cpp_t::set_async_worker(std::shared_ptr<iris_async_worker_t<>> worker) {
		if (is_running())
			return false;

		std::swap(async_worker, worker);
		reset_main_warp();
		return true;
	}

	void ngx_lua_cpp_t::process_events() {
		if (async_worker->get_thread_count() <= 1 || async_worker->is_terminated()) {
			async_worker->make_current(main_thread_index);
			// if there is no worker threads, try polling from main_thread
			async_worker->poll();
			async_worker->make_current(~(size_t)0);
		}

		main_warp->poll<false>();
	}

	void ngx_warp_t::flush_warp() {
		ngx_hooker_t::get_instance().notify();
	}

	int ngx_lua_cpp_resume(lua_State* L, int narg) {
		return ngx_hooker_t::get_instance().ngx_lua_cpp_resume(L, narg);
	}

	int ngx_lua_cpp_yield(lua_State* L, int narg) {
		return ngx_hooker_t::get_instance().ngx_lua_cpp_yield(L, narg);
	}
}

extern "C" NGX_LUA_CPP_API int luaopen_ngx_lua_cpp(lua_State* L) {
	return iris::iris_lua_t::forward(L, +[](iris::iris_lua_t lua) {
		return lua.make_type<iris::ngx_lua_cpp_t>();
	});
}


