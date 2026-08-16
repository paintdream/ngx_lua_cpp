-- udp_echo_server.lua
--
-- Demo 5 (UDP servers): drives the datagram listeners owned by ngx_lua_cpp.
--
-- nginx's stream module compiles "listen ... udp" out on Windows
-- (ngx_stream_core_module.c: `#if !(NGX_WIN32)`), so a UDP listener cannot be
-- declared in nginx.conf on this platform -- no matter how new the OpenResty
-- is. Instead the C++ instance binds the socket itself (udp_listen), a
-- background thread fills a bounded queue, and this module polls the queue
-- with udp_recv(port, 0) inside chained ngx.timer handlers. The nginx worker
-- is never blocked: udp_recv/udp_send hop to the C++ worker pool.
--
-- Wiring (http init_worker_by_lua_block in web/nginx.conf):
--   require("udp_echo_server").start(9000, "echo")   -- "pong [<payload>] ..."
--   require("udp_echo_server").start(9001, "hello")  -- fixed "hello-udp" reply

local M = {}

-- mode == "echo"  -> replies "pong [<payload>] lua_cpp=true from=<addr>:<port>"
-- mode == "hello" -> replies a fixed "hello-udp" datagram
function M.start(port, mode)
	local inst = require("init_ngx_lua_cpp")

	local err = inst:udp_listen(port)
	if err ~= "" then
		ngx.log(ngx.ERR, "udp_echo_server: udp_listen(", port, ") failed: ", err)
		return
	end
	ngx.log(ngx.INFO, "udp_echo_server: listening on udp ", port, " (mode=", mode or "echo", ")")

	local function loop(premature)
		if premature then
			return
		end

		-- timeout_ms == 0 -> non-blocking poll of the listener queue
		local r = inst:udp_recv(port, 0)
		if r and r[1] then
			local reply
			if mode == "hello" then
				reply = "hello-udp"
			else
				reply = "pong [" .. r[2] .. "] lua_cpp=true from=" .. r[3] .. ":" .. r[4]
			end
			-- reply FROM the listening socket so the source address is the
			-- port the client sent to (connected UDP clients require this)
			local ok, err = inst:udp_reply(port, reply)
			if not ok then
				ngx.log(ngx.WARN, "udp_echo_server: udp_reply(", port, ") failed: ", err)
			end
			-- publish traffic to the event bus (realtime panel)
			inst:pub("udp", port .. "|" .. r[2] .. "|" .. r[3] .. ":" .. r[4])
		end

		-- pace the poll loop (200 iterations/sec max)
		local ok, sleep_err = ngx.sleep(0.005)
		ngx.timer.at(0, loop)
	end

	ngx.timer.at(0, loop)
end

return M