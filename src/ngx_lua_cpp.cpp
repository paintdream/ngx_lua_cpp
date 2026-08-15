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
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <thread>

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
			if (offset_http_co_ctx_event_queue != 0) {
				for (auto* co_ctx : pending_lua_http_co_ctxs) {
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

			do {
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

	ngx_lua_cpp_t::ngx_lua_cpp_t() : async_worker(std::make_shared<iris_async_worker_t<>>()) {
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

	void ngx_lua_cpp_t::stop_impl() {
		async_worker->terminate();
		async_worker->join();

		// manually polling events
		while (main_warp->poll()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		main_thread_index = ~(size_t)0;
		reset_main_warp();
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

