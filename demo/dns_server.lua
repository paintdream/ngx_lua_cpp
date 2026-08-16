-- dns_server.lua
--
-- Demo A1: minimal DNS-over-UDP forwarder + local override server.
--
-- Listens on UDP 53 (or any port), parses the incoming query header and
-- QNAME in plain Lua (LuaJIT has no string.pack, so bytes are assembled
-- with string.char / bit ops), then either:
--   * answers from a static override table (A records built by hand), or
--   * forwards the raw query to an upstream resolver via inst:udp_echo()
--     (the C++ worker-pool datagram client) and relays the response.
-- The upstream response is cached in the C++ LRU cache for 30 seconds.
--
-- The listener + sockets are owned by the C++ instance; this module only
-- implements the DNS wire logic. The nginx worker is never blocked.

local M = {}

local bit = require("bit")

local OVERRIDES = {
    ["myapp.test."] = "127.0.0.1",
    ["demo.test."] = "127.0.0.1",
    ["gateway.test."] = "127.0.0.1",
}

local CACHE_TTL_MS = 30000

local function b16(v) return bit.band(v, 0xff) end

-- encode a query name like "www.example.com" (with or without trailing dot)
local function encode_name(name)
    if name:sub(-1) ~= "." then
        name = name .. "."
    end
    local out = {}
    for label in name:gmatch("([^.]+)%.") do
        out[#out + 1] = string.char(#label) .. label
    end
    out[#out + 1] = "\0"
    return table.concat(out)
end

-- decode a query name from the question section; returns name, next_offset
local function decode_name(msg, offset)
    local labels = {}
    while true do
        local len = msg:byte(offset)
        if not len then return nil end
        if len == 0 then
            offset = offset + 1
            break
        end
        if bit.band(len, 0xc0) == 0xc0 then
            -- compression pointer: skip 2 bytes (we do not follow pointers
            -- for the question section, which is always uncompressed)
            offset = offset + 2
            break
        end
        labels[#labels + 1] = msg:sub(offset + 1, offset + len)
        offset = offset + 1 + len
    end
    if #labels == 0 then
        return nil, offset
    end
    return table.concat(labels, ".") .. ".", offset
end

-- build an A-record response for a query (header echoed, QR=1 RD=1 RA=1)
local function build_a_response(query, qname, qtype, qclass, ip)
    local id = query:sub(1, 2)
    local question = query:sub(13) -- original question section (uncompressed)
    local ttl = 60
    local o1, o2, o3, o4 = ip:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if not o1 then return nil end

    local header = id .. string.char(0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00)
    local answer = string.char(0xc0, 0x0c) ..           -- name pointer to offset 12
        string.char(0x00, 0x01, 0x00, 0x01) ..          -- type A, class IN
        string.char(b16(bit.rshift(ttl, 24)), b16(bit.rshift(ttl, 16)), b16(bit.rshift(ttl, 8)), b16(ttl)) ..
        string.char(0x00, 0x04) ..                      -- rdlength = 4
        string.char(tonumber(o1), tonumber(o2), tonumber(o3), tonumber(o4))
    return header .. question .. answer
end

-- build a DNS query for an arbitrary name/type (for the test API)
function M.build_query(inst, name, qtype)
    qtype = qtype or 1
    local header = string.char(0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
    local q = encode_name(name)
    local qt = string.char(b16(bit.rshift(qtype, 8)), b16(qtype), 0x00, 0x01)
    return header .. q .. qt
end

-- crude response reader: returns ancount, first A record ip (or nil), raw hex
function M.parse_response(msg)
    if not msg or #msg < 13 then
        return 0, nil
    end
    -- 0-based header: ANCOUNT at bytes 6-7 -> 1-based 7-8
    local ancount = bit.bor(bit.lshift(msg:byte(7) or 0, 8), msg:byte(8) or 0)
    -- question section: skip name + 4 bytes (name starts at 1-based 13)
    local off = 13
    while true do
        local len = msg:byte(off)
        if not len then return ancount, nil end
        if len == 0 then
            off = off + 1
            break
        end
        if bit.band(len, 0xc0) == 0xc0 then
            off = off + 2
            break
        end
        off = off + 1 + len
    end
    off = off + 4 -- qtype + qclass
    -- first answer record: expect a compression pointer to offset 12
    if ancount >= 1 and msg:byte(off) == 0xc0 and msg:byte(off + 1) == 0x0c then
        local type_hi, type_lo = msg:byte(off + 2), msg:byte(off + 3)
        local rtype = bit.bor(bit.lshift(type_hi or 0, 8), type_lo or 0)
        local rdlen = bit.bor(bit.lshift(msg:byte(off + 10) or 0, 8), msg:byte(off + 11) or 0)
        if rtype == 1 and rdlen == 4 then
            local ip = table.concat({
                msg:byte(off + 12), msg:byte(off + 13), msg:byte(off + 14), msg:byte(off + 15),
            }, ".")
            return ancount, ip
        end
    end
    return ancount, nil
end

function M.start(port, upstream)
    local inst = require("init_ngx_lua_cpp")

    local err = inst:udp_listen(port)
    if err ~= "" then
        ngx.log(ngx.ERR, "dns_server: udp_listen(", port, ") failed: ", err)
        return
    end
    ngx.log(ngx.INFO, "dns_server: listening on udp ", port, " upstream=", upstream)

    local function loop(premature)
        if premature then
            return
        end

        local r = inst:udp_recv(port, 0)
        if r and r[1] then
            local query = r[2]
            -- header is 12 bytes; the question name starts at 1-based 13
            local qname, qend = decode_name(query, 13)
            if qname then
                local qtype = bit.bor(bit.lshift(query:byte(qend) or 0, 8), query:byte(qend + 1) or 0)
                local qclass = bit.bor(bit.lshift(query:byte(qend + 2) or 0, 8), query:byte(qend + 3) or 0)

                local response
                local ip = OVERRIDES[qname]
                if ip then
                    response = build_a_response(query, qname, qtype, qclass, ip)
                    inst:pub("dns", port .. "|" .. qname .. "|override|" .. ip)
                else
                    -- forward to upstream with a 2s timeout, cached 30s
                    local cache_key = "dns:" .. qname .. ":" .. qtype
                    local got = inst:cache_get(cache_key)
                    if got[1] then
                        response = got[2]
                        inst:pub("dns", port .. "|" .. qname .. "|cache")
                    else
                        local fwd = inst:udp_echo(upstream, 53, query, 2000)
                        if fwd[1] then
                            response = fwd[2]
                            inst:cache_set(cache_key, response, CACHE_TTL_MS)
                            inst:pub("dns", port .. "|" .. qname .. "|forward|" .. fwd[3])
                        else
                            ngx.log(ngx.WARN, "dns_server: upstream ", upstream, " failed: ", fwd[4])
                            inst:pub("dns", port .. "|" .. qname .. "|error|" .. fwd[4])
                        end
                    end
                end

                if response then
                    inst:udp_reply(port, response)
                end
            end
        end

        ngx.sleep(0.005)
        ngx.timer.at(0, loop)
    end

    ngx.timer.at(0, loop)
end

return M