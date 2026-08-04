-- Independent Windrose dedicated-server public chat transport.
local UEHelpers = require("UEHelpers")

local MOD = "PirateSignals"
local BUILD = "pirate-signals-server-1"
local NAMESPACE = "WRCHAT"
local PROTOCOL = "1"
local CLASS_TOKEN = "/Game/Mods/WindroseChatTransport/ModActor.ModActor_C"

local MAX_FRAME_BYTES = 4096
local MAX_MESSAGE_BYTES = 1024
local MAX_MESSAGE_CODEPOINTS = 300
local MAX_NAME_BYTES = 96
local HISTORY_LIMIT = 100
local RATE_WINDOW_SECONDS = 10
local RATE_MAX_MESSAGES = 5

local transport_class = nil
local rpc_hook_registered = false
local peers_by_controller = {}
local sessions_by_actor = {}
local history = {}
local next_message_id = 1

local function log(message)
    print(string.format("[%s server %s] %s\n", MOD, BUILD, tostring(message)))
end

local function unwrap(value)
    if value == nil then return nil end
    local ok, result = pcall(function() return value:get() end)
    return ok and result or value
end

local function text_value(value)
    value = unwrap(value)
    if value == nil then return "" end
    if type(value) == "string" then return value end
    local ok, result = pcall(function() return value:ToString() end)
    return ok and tostring(result) or tostring(value)
end

local function valid(object)
    if object == nil then return false end
    local ok, result = pcall(function() return object:IsValid() end)
    return ok and result == true
end

local function full_name(object)
    if not valid(object) then return "" end
    local ok, result = pcall(function() return object:GetFullName() end)
    return ok and tostring(result) or ""
end

local function same_object(left, right)
    return valid(left)
        and valid(right)
        and (left == right or full_name(left) == full_name(right))
end

local function class_name(actor)
    if not valid(actor) then return "" end
    local ok, class = pcall(function() return actor:GetClass() end)
    return ok and valid(class) and full_name(class) or ""
end

local function is_transport_actor(actor)
    return string.find(class_name(actor), CLASS_TOKEN, 1, true) ~= nil
end

local function is_player_controller(actor)
    return string.find(class_name(actor), "BP_R5PlayerController_C", 1, true) ~= nil
end

local function encode_field(value)
    return (tostring(value or ""):gsub("([^A-Za-z0-9_.%-])", function(character)
        return string.format("%%%02X", string.byte(character))
    end))
end

local function decode_field(value)
    value = tostring(value or "")
    if string.find(value, "|", 1, true) then return nil end
    local index = 1
    local output = {}
    while index <= #value do
        local character = string.sub(value, index, index)
        if character == "%" then
            local hex = string.sub(value, index + 1, index + 2)
            if #hex ~= 2 or not string.match(hex, "^%x%x$") then return nil end
            table.insert(output, string.char(tonumber(hex, 16)))
            index = index + 3
        else
            table.insert(output, character)
            index = index + 1
        end
    end
    return table.concat(output)
end

local function split_frame(frame)
    local fields = {}
    for field in string.gmatch(frame .. "|", "(.-)|") do
        table.insert(fields, field)
        if #fields > 8 then return nil end
    end
    return fields
end

local function clean_identity(value)
    local name = tostring(value or "")
    name = name:gsub("[%z\1-\31\127]", "?")
    name = name:gsub("^%s+", ""):gsub("%s+$", "")
    if name == "" then name = "Player" end
    if #name > MAX_NAME_BYTES then name = string.sub(name, 1, MAX_NAME_BYTES) end
    return name
end

local function validate_message(value)
    if value == nil then return nil, "ENCODING" end
    local message = tostring(value)
    message = message:gsub("^%s+", ""):gsub("%s+$", "")
    if message == "" then return nil, "EMPTY" end
    if #message > MAX_MESSAGE_BYTES then return nil, "TOO_LONG" end
    if string.find(message, "[%z\1-\31\127]") then return nil, "CONTROL_CHAR" end
    local utf8_ok, count = pcall(function() return utf8.len(message) end)
    if not utf8_ok or count == nil then return nil, "INVALID_UTF8" end
    if count > MAX_MESSAGE_CODEPOINTS then return nil, "TOO_LONG" end
    return message, nil
end

local function authoritative_player_name(controller)
    if not valid(controller) then return nil end
    local ok, player_state = pcall(function() return unwrap(controller.PlayerState) end)
    if not ok or not valid(player_state) then return nil end
    ok, player_state = pcall(function() return player_state:GetPlayerName() end)
    if not ok then return nil end
    return clean_identity(text_value(player_state))
end

local function authority(actor)
    local ok, result = pcall(function() return actor:HasAuthority() end)
    return ok and result == true
end

local function send_frame(actor, fields)
    if not valid(actor) then return false end
    local frame = table.concat(fields, "|")
    if #frame > MAX_FRAME_BYTES then
        log("outbound frame rejected locally: oversized")
        return false
    end
    local ok, err = pcall(function() actor:ClientPong(frame) end)
    if not ok then log("client frame send failed: " .. tostring(err)) end
    return ok
end

local function send_error(actor, code, detail)
    send_frame(actor, { NAMESPACE, PROTOCOL, "ERROR", code, encode_field(detail) })
end

local function history_last_id()
    return #history > 0 and history[#history].id or 0
end

local function send_history(actor)
    send_frame(actor, {
        NAMESPACE, PROTOCOL, "HISTORY_BEGIN", tostring(#history), tostring(history_last_id())
    })
    for _, item in ipairs(history) do
        send_frame(actor, {
            NAMESPACE,
            PROTOCOL,
            "HISTORY_ITEM",
            tostring(item.id),
            tostring(item.epoch),
            encode_field(item.name),
            encode_field(item.text),
        })
    end
    send_frame(actor, { NAMESPACE, PROTOCOL, "HISTORY_END", tostring(history_last_id()) })
    log(string.format("history sent actor=%s count=%d", full_name(actor), #history))
end

local function rate_allowed(session, now)
    local retained = {}
    for _, timestamp in ipairs(session.rate_times) do
        if now - timestamp < RATE_WINDOW_SECONDS then table.insert(retained, timestamp) end
    end
    session.rate_times = retained
    if #retained >= RATE_MAX_MESSAGES then return false end
    table.insert(session.rate_times, now)
    return true
end

local function broadcast_message(item)
    local stale = {}
    local recipients = 0
    for key, actor in pairs(peers_by_controller) do
        if not valid(actor) then
            table.insert(stale, key)
        else
            local session = sessions_by_actor[full_name(actor)]
            if session and session.ready then
                if send_frame(actor, {
                    NAMESPACE,
                    PROTOCOL,
                    "MESSAGE",
                    tostring(item.id),
                    tostring(item.epoch),
                    encode_field(item.name),
                    encode_field(item.text),
                }) then recipients = recipients + 1 end
            end
        end
    end
    for _, key in ipairs(stale) do peers_by_controller[key] = nil end
    log(string.format("message broadcast id=%d recipients=%d", item.id, recipients))
end

local function resolve_transport_class()
    for _, path in ipairs({ "BlueprintGeneratedClass " .. CLASS_TOKEN, CLASS_TOKEN }) do
        local ok, class = pcall(function() return StaticFindObject(path) end)
        if ok and valid(class) then transport_class = class return class end
    end
    return valid(transport_class) and transport_class or nil
end

local function ensure_peer(controller)
    if not valid(controller) or not authority(controller) then return false end
    local class = resolve_transport_class()
    if not valid(class) then log("peer creation failed: transport class unavailable") return false end

    local key = full_name(controller)
    if valid(peers_by_controller[key]) then return true end

    local world_ok, world = pcall(function() return unwrap(controller:GetWorld()) end)
    if not world_ok or not valid(world) then world = UEHelpers.GetWorld() end
    if not valid(world) then log("peer creation failed: world unavailable") return false end

    local spawn_ok, actor = pcall(function() return world:SpawnActor(class, {}, {}) end)
    actor = spawn_ok and unwrap(actor) or nil
    if not valid(actor) then log("peer creation failed: dynamic spawn returned invalid") return false end

    local owner_ok, owner_err = pcall(function() actor:SetOwner(controller) end)
    if not owner_ok then log("peer ownership failed: " .. tostring(owner_err)) return false end

    local read_ok, owner = pcall(function() return unwrap(actor:GetOwner()) end)
    if not read_ok or not same_object(owner, controller) then
        log("peer ownership verification failed")
        return false
    end

    peers_by_controller[key] = actor
    sessions_by_actor[full_name(actor)] = {
        controller_key = key,
        ready = false,
        last_sequence = 0,
        rate_times = {},
    }
    log("peer ready actor=" .. full_name(actor) .. " owner=" .. key)
    return true
end

local function handle_client_frame(actor, payload)
    if not valid(actor) or not authority(actor) then return end
    if #payload == 0 or #payload > MAX_FRAME_BYTES then return end

    local fields = split_frame(payload)
    if not fields or fields[1] ~= NAMESPACE then return end
    if fields[2] ~= PROTOCOL then
        send_error(actor, "VERSION", "Server requires protocol " .. PROTOCOL)
        return
    end

    local owner_ok, controller = pcall(function() return unwrap(actor:GetOwner()) end)
    if not owner_ok or not valid(controller) then send_error(actor, "OWNER", "No server owner") return end
    local key = full_name(controller)
    if not same_object(peers_by_controller[key], actor) then
        send_error(actor, "SESSION", "Unrecognized transport actor")
        return
    end

    local session = sessions_by_actor[full_name(actor)]
    if not session then send_error(actor, "SESSION", "Missing server session") return end

    local kind = fields[3]
    if kind == "HELLO" then
        if #fields ~= 4 or not string.match(fields[4], "^[A-Za-z0-9_.%-]+$") then
            send_error(actor, "FRAME", "Invalid HELLO")
            return
        end
        session.ready = true
        session.client_session = fields[4]
        session.last_sequence = 0
        session.rate_times = {}
        send_history(actor)
        log("client ready owner=" .. key .. " session=" .. fields[4])
        return
    end

    if kind ~= "SUBMIT" or #fields ~= 5 then send_error(actor, "FRAME", "Unknown frame") return end
    if not session.ready then send_error(actor, "NOT_READY", "Send HELLO first") return end

    local sequence = tonumber(fields[4])
    if not sequence or sequence % 1 ~= 0 or sequence <= session.last_sequence then
        send_error(actor, "SEQUENCE", "Sequence must increase")
        return
    end
    session.last_sequence = sequence

    local message, validation_error = validate_message(decode_field(fields[5]))
    if not message then send_error(actor, validation_error, "Message rejected") return end
    if not rate_allowed(session, os.time()) then send_error(actor, "RATE_LIMIT", "Too many messages") return end

    local player_name = authoritative_player_name(controller)
    if not player_name then send_error(actor, "IDENTITY", "Player identity unavailable") return end

    local item = {
        id = next_message_id,
        epoch = os.time(),
        name = player_name,
        text = message,
    }
    next_message_id = next_message_id + 1
    table.insert(history, item)
    while #history > HISTORY_LIMIT do table.remove(history, 1) end
    broadcast_message(item)
    log(string.format("message accepted id=%d player=%s bytes=%d", item.id, player_name, #message))
end

local function register_rpc_hook()
    if rpc_hook_registered then return end
    local ok, err = pcall(function()
        RegisterHook(CLASS_TOKEN .. ":ServerPing", function(context, param_payload)
            handle_client_frame(unwrap(context), text_value(param_payload))
        end)
    end)
    if ok then rpc_hook_registered = true log("server frame hook registered protocol=" .. PROTOCOL)
    else log("server frame hook failed: " .. tostring(err)) end
end

RegisterBeginPlayPostHook(function(context)
    local actor = unwrap(context)
    if is_player_controller(actor) then ensure_peer(actor) return end
    if is_transport_actor(actor) and not valid(transport_class) then
        local ok, class = pcall(function() return actor:GetClass() end)
        if ok and valid(class) then transport_class = class register_rpc_hook() end
    end
end)

RegisterEndPlayPreHook(function(context, reason)
    local controller = unwrap(context)
    if not is_player_controller(controller) then return end
    local key = full_name(controller)
    local peer = peers_by_controller[key]
    if valid(peer) then
        sessions_by_actor[full_name(peer)] = nil
        local destroy_ok, destroy_err = pcall(function() peer:K2_DestroyActor() end)
        log(string.format(
            "peer cleanup owner=%s reason=%s destroyed=%s detail=%s",
            key, tostring(unwrap(reason)), tostring(destroy_ok), destroy_ok and "OK" or tostring(destroy_err)
        ))
    end
    peers_by_controller[key] = nil
end)

log(string.format(
    "loaded protocol=%s history_limit=%d max_chars=%d rate=%d/%ds",
    PROTOCOL, HISTORY_LIMIT, MAX_MESSAGE_CODEPOINTS, RATE_MAX_MESSAGES, RATE_WINDOW_SECONDS
))
