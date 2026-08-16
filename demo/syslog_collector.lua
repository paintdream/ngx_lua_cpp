-- syslog_collector.lua
--
-- Demo A2: UDP syslog collector.
--
-- Listens on UDP 514 (the standard syslog port), parses RFC3164-style lines
-- and publishes every message to the ngx_lua_cpp event bus (topic "syslog")
-- plus keeps a small in-process ring of the most recent messages for the
-- HTTP API. The listener socket is owned by the C++ instance (udp_listen);
-- the poll loop runs inside chained ngx.timer handlers, so the nginx worker
-- is never blocked.
--
-- Wiring (http init_worker_by_lua_block in web/nginx.conf):
--   require("syslog_collector").start(514)
--
-- Send a test message from PowerShell:
--   $c = New-Object System.Net.Sockets.UdpClient; $b = [Text.Encoding]::UTF8.GetBytes('<134>Aug 17 00:00:01 demo-app hello from syslog!'); $c.Send($b, $b.Length, '127.0.0.1', 514) | Out-Null; $c.Close()

local M = {
    recent = {},   -- ring of { line = ..., from = ... }
}

local RECENT_MAX = 200

function M.start(port)
    local inst = require("init_ngx_lua_cpp")

    local err = inst:udp_listen(port)
    if err ~= "" then
        ngx.log(ngx.ERR, "syslog_collector: udp_listen(", port, ") failed: ", err)
        return
    end
    ngx.log(ngx.INFO, "syslog_collector: listening on udp ", port)

    local function loop(premature)
        if premature then
            return
        end

        local r = inst:udp_recv(port, 0)
        if r and r[1] then
            local line = r[2]
            local from = r[3] .. ":" .. r[4]

            -- crude RFC3164 parse: <PRI>MMM dd HH:MM:SS host tag: message
            local pri, rest = line:match("^<(%d+)>(.*)$")
            if not pri then
                rest = line
            end
            local ts, host, msg = rest:match("^(%S+ +%S+ %S+) (%S+) (.+)$")
            local entry = {
                line = line,
                from = from,
                pri = tonumber(pri) or 0,
                ts = ts or "",
                host = host or "",
                msg = msg or line,
            }
            table.insert(M.recent, entry)
            if #M.recent > RECENT_MAX then
                table.remove(M.recent, 1)
            end
            inst:pub("syslog", line .. "|" .. from)
        end

        -- pace the poll loop
        ngx.sleep(0.005)
        ngx.timer.at(0, loop)
    end

    ngx.timer.at(0, loop)
end

-- return the last n entries (newest last)
function M.get_recent(n)
    n = tonumber(n) or 50
    if n > RECENT_MAX then n = RECENT_MAX end
    local out = {}
    local start = math.max(1, #M.recent - n + 1)
    for i = start, #M.recent do
        out[#out + 1] = M.recent[i]
    end
    return out
end

return M