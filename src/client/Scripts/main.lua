-- Independent Windrose public chat client transport with native ImGui UI bridge.
local UEHelpers = require("UEHelpers")

local MOD = "PirateSignals"
local BUILD = "pirate-signals-client-1"
local NAMESPACE = "WRCHAT"
local PROTOCOL = "1"
local CLASS_TOKEN = "/Game/Mods/WindroseChatTransport/ModActor.ModActor_C"
local MAX_FRAME_BYTES = 4096
local MAX_MESSAGE_BYTES = 1024
local MAX_MESSAGE_CODEPOINTS = 300
local CLIENT_HISTORY_LIMIT = 100

local rpc_hook_registered = false
local active_actor = nil
local active_actor_name = ""
local client_session = ""
local hello_sent = false
local ready = false
local sequence = 0
local schedule_generation = 0
local history = {}
local pending_history = nil
local ui_notice = "Connecting"
local native_bridge_seen = false
local native_renderer_seen = false
local submit_message

local function log(message)
    print(string.format("[%s client %s] %s\n", MOD, BUILD, tostring(message)))
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
    return valid(left) and valid(right)
        and (left == right or full_name(left) == full_name(right))
end

local function is_transport_actor(actor)
    if not valid(actor) then return false end
    local ok, class = pcall(function() return actor:GetClass() end)
    return ok and valid(class) and string.find(full_name(class), CLASS_TOKEN, 1, true) ~= nil
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

local function local_controller()
    local ok, controller = pcall(function() return UEHelpers.GetPlayerController() end)
    if not ok or not valid(controller) then return nil end
    local local_ok, is_local = pcall(function() return controller:IsLocalPlayerController() end)
    return local_ok and is_local and controller or nil
end

local function in_client_lobby(object)
    return valid(object)
        and string.find(full_name(object), "/Game/Maps/Lobby/R5ClientLobby", 1, true) ~= nil
end

local function find_owned_transport()
    local controller = local_controller()
    if not controller then return nil end
    local controller_name = full_name(controller)
    for _, actor in ipairs(FindAllOf("ModActor_C") or {}) do
        if is_transport_actor(actor) then
            local owner_ok, owner = pcall(function() return unwrap(actor:GetOwner()) end)
            if owner_ok and valid(owner)
                and (same_object(owner, controller) or full_name(owner) == controller_name)
            then
                return actor
            end
        end
    end
    return nil
end

local function owned_by_controller(actor, controller)
    if not valid(actor) or not valid(controller) then return false end
    local owner_ok, owner = pcall(function() return unwrap(actor:GetOwner()) end)
    return owner_ok and valid(owner) and same_object(owner, controller)
end

local function native_function(name)
    local fn = _G[name]
    return type(fn) == "function" and fn or nil
end

local function native_call(name, ...)
    local fn = native_function(name)
    if not fn then return false, nil end
    local ok, result = pcall(fn, ...)
    if not ok then log("native bridge call failed " .. name .. ": " .. tostring(result)) end
    return ok, result
end

local function native_set_notice(message)
    ui_notice = tostring(message or "")
    native_call("WRChatNativeSetNotice", ui_notice)
end

local function native_set_ready(value)
    native_call("WRChatNativeSetReady", value == true)
end

local function native_hide()
    native_call("WRChatNativeHide")
end

local function native_add_item(item)
    native_call("WRChatNativeAddMessage", "<" .. tostring(item.name) .. "> " .. tostring(item.text))
end

local function native_sync_history()
    if not native_function("WRChatNativeResetHistory") then return end
    native_call("WRChatNativeResetHistory")
    for _, item in ipairs(history) do
        native_add_item(item)
    end
    native_set_ready(ready and valid(active_actor))
    native_call("WRChatNativeSetNotice", ui_notice)
end

local function send_server_frame(fields)
    if not valid(active_actor) then return false end
    local frame = table.concat(fields, "|")
    if #frame > MAX_FRAME_BYTES then log("outbound frame rejected locally: oversized") return false end
    local ok, err = pcall(function() active_actor:ServerPing(frame) end)
    if not ok then log("server frame send failed: " .. tostring(err)) end
    return ok
end

local function send_hello(actor)
    if not valid(actor) then return false end
    local name = full_name(actor)
    if name ~= active_actor_name then
        active_actor = actor
        active_actor_name = name
        client_session = string.format("%d-%d", os.time(), math.random(100000, 999999))
        hello_sent = false
        ready = false
        sequence = 0
        pending_history = nil
        native_set_ready(false)
        native_set_notice("Connecting")
    end
    if hello_sent then return true end
    hello_sent = send_server_frame({ NAMESPACE, PROTOCOL, "HELLO", client_session })
    if hello_sent then log("HELLO sent actor=" .. active_actor_name .. " session=" .. client_session) end
    return hello_sent
end

local function discover_and_hello()
    if ready and valid(active_actor) then return true end
    local actor = find_owned_transport()
    if not actor then return false end
    return send_hello(actor)
end

local function schedule_discovery(reason)
    schedule_generation = schedule_generation + 1
    local generation = schedule_generation
    for attempt = 1, 30 do
        ExecuteInGameThreadWithDelay(attempt * 1000, function()
            if generation ~= schedule_generation or (ready and valid(active_actor)) then return end
            if not discover_and_hello() and (attempt == 1 or attempt % 5 == 0) then
                log(string.format("waiting for owned transport reason=%s attempt=%d", reason, attempt))
            end
        end)
    end
end

local function has_forbidden_control(value)
    for index = 1, #value do
        local byte = string.byte(value, index)
        if byte < 32 or byte == 127 then return true end
    end
    return false
end

local function append_history(item)
    table.insert(history, item)
    while #history > CLIENT_HISTORY_LIMIT do table.remove(history, 1) end
    native_add_item(item)
end

local function parse_item(fields)
    if #fields ~= 7 then return nil end
    local id = tonumber(fields[4])
    local epoch = tonumber(fields[5])
    local name = decode_field(fields[6])
    local message = decode_field(fields[7])
    if not id or not epoch or name == nil or message == nil then return nil end
    return { id = id, epoch = epoch, name = name, text = message }
end

local function handle_client_frame(actor, payload)
    if not valid(active_actor) or not same_object(actor, active_actor) then
        log("ignored frame from stale or unowned transport actor=" .. full_name(actor))
        return
    end
    if #payload == 0 or #payload > MAX_FRAME_BYTES then return end
    local fields = split_frame(payload)
    if not fields or fields[1] ~= NAMESPACE then return end
    if fields[2] ~= PROTOCOL then
        ready = false
        native_set_ready(false)
        native_set_notice("Protocol mismatch: server requires " .. tostring(fields[2]))
        log("protocol mismatch: server=" .. tostring(fields[2]) .. " client=" .. PROTOCOL)
        return
    end

    local kind = fields[3]
    if kind == "ERROR" then
        local detail = decode_field(fields[5]) or ""
        native_set_notice(detail ~= "" and detail or tostring(fields[4]))
        log(string.format("server rejected request code=%s detail=%s", tostring(fields[4]), detail))
        return
    end

    if kind == "HISTORY_BEGIN" then
        pending_history = {
            expected = tonumber(fields[4]) or 0,
            last_id = tonumber(fields[5]) or 0,
            items = {},
        }
        log("history begin expected=" .. tostring(pending_history.expected))
        return
    end

    if kind == "HISTORY_ITEM" then
        local item = parse_item(fields)
        if pending_history and item then table.insert(pending_history.items, item) end
        return
    end

    if kind == "HISTORY_END" then
        if not pending_history then log("history end rejected: no active snapshot") return end
        local end_id = tonumber(fields[4]) or 0
        if #pending_history.items ~= pending_history.expected or end_id ~= pending_history.last_id then
            log("history snapshot rejected: count or boundary mismatch")
            pending_history = nil
            ready = false
            native_set_ready(false)
            native_set_notice("History synchronization failed")
            return
        end
        history = pending_history.items
        pending_history = nil
        ready = true
        ui_notice = ""
        native_sync_history()
        log(string.format("READY protocol=%s history=%d last_id=%d", PROTOCOL, #history, end_id))
        for _, item in ipairs(history) do
            log(string.format("HISTORY id=%d <%s> %s", item.id, item.name, item.text))
        end
        return
    end

    if kind == "MESSAGE" then
        local item = parse_item(fields)
        if not item then log("live message rejected: malformed") return end
        ui_notice = ""
        append_history(item)
        native_set_ready(true)
        log(string.format("MESSAGE id=%d <%s> %s", item.id, item.name, item.text))
    end
end

local function register_rpc_hook()
    if rpc_hook_registered then return end
    local ok, err = pcall(function()
        RegisterHook(CLASS_TOKEN .. ":ClientPong", function(context, param_payload)
            handle_client_frame(unwrap(context), text_value(param_payload))
        end)
    end)
    if ok then rpc_hook_registered = true log("client frame hook registered protocol=" .. PROTOCOL)
    else log("client frame hook failed: " .. tostring(err)) end
end

RegisterBeginPlayPostHook(function(context)
    local actor = unwrap(context)
    if is_transport_actor(actor) then register_rpc_hook() end
end)

RegisterHook("/Script/Engine.PlayerController:ClientRestart", function(context)
    local controller = unwrap(context)
    if not valid(controller) then return end
    local ok, is_local = pcall(function() return controller:IsLocalPlayerController() end)
    if ok and is_local then
        if ready and owned_by_controller(active_actor, controller) then
            log("ClientRestart retained ready owned transport actor=" .. active_actor_name)
            return
        end
        active_actor = nil
        active_actor_name = ""
        hello_sent = false
        ready = false
        pending_history = nil
        history = {}
        if in_client_lobby(controller) then native_hide() end
        native_set_ready(false)
        native_set_notice("Reconnecting")
        native_sync_history()
        schedule_discovery("ClientRestart")
    end
end)

RegisterEndPlayPreHook(function(context)
    local actor = unwrap(context)
    if same_object(actor, active_actor) then
        active_actor = nil
        active_actor_name = ""
        hello_sent = false
        ready = false
        pending_history = nil
        history = {}
        native_set_ready(false)
        native_set_notice("Disconnected")
        native_sync_history()
        ExecuteInGameThreadWithDelay(1000, function()
            local controller = local_controller()
            if in_client_lobby(controller) then native_hide() end
        end)
    end
end)

submit_message = function(message, source)
    if not ready or not valid(active_actor) then
        native_set_notice("Not connected")
        log(tostring(source) .. " send rejected locally: chat transport is not ready")
        return false
    end
    message = tostring(message or "")
    if message == "" then native_set_notice("Message is empty") return false end
    if #message > MAX_MESSAGE_BYTES then
        native_set_notice("Message is over 1024 UTF-8 bytes")
        return false
    end
    local codepoints = utf8.len(message)
    if not codepoints then native_set_notice("Message is not valid UTF-8") return false end
    if codepoints > MAX_MESSAGE_CODEPOINTS then
        native_set_notice("Message is over 300 characters")
        return false
    end
    if has_forbidden_control(message) then
        native_set_notice("Message contains unsupported control characters")
        return false
    end

    local next_sequence = sequence + 1
    if send_server_frame({ NAMESPACE, PROTOCOL, "SUBMIT", tostring(next_sequence), encode_field(message) }) then
        sequence = next_sequence
        native_set_notice("")
        log(string.format("SUBMIT sent source=%s sequence=%d bytes=%d", tostring(source), sequence, #message))
        return true
    end
    native_set_notice("Message could not be sent")
    return false
end

local function poll_native_bridge()
    if native_function("WRChatNativeTakeSubmission") then
        if not native_bridge_seen then
            native_bridge_seen = true
            native_hide()
            native_sync_history()
            log("native ImGui bridge detected")
        end
        local ok, renderer_ready = native_call("WRChatNativeAvailable")
        if ok and renderer_ready and not native_renderer_seen then
            native_renderer_seen = true
            log("native ImGui renderer ready")
        end
        for _ = 1, 8 do
            local take_ok, message = native_call("WRChatNativeTakeSubmission")
            if not take_ok or message == nil then break end
            submit_message(message, "imgui")
        end
    end
    ExecuteInGameThreadWithDelay(100, poll_native_bridge)
end
ExecuteInGameThreadWithDelay(100, poll_native_bridge)

RegisterConsoleCommandHandler("wrchat_send", function(_full_command, parameters)
    local message = table.concat(parameters or {}, " ")
    if message == "" then log("usage: wrchat_send <message>") return true end
    submit_message(message, "console")
    return true
end)

RegisterConsoleCommandHandler("wrchat_test", function()
    if not ready or not valid(active_actor) then
        log("test rejected locally: chat transport is not ready")
        return true
    end
    if submit_message("Pirate Signals transport test", "test") then
        log("fixed transport test submitted sequence=" .. tostring(sequence))
    end
    return true
end)

RegisterConsoleCommandHandler("wrchat_history", function()
    log("history count=" .. tostring(#history))
    for _, item in ipairs(history) do
        log(string.format("HISTORY id=%d <%s> %s", item.id, item.name, item.text))
    end
    return true
end)

RegisterConsoleCommandHandler("wrchat_status", function()
    log(string.format(
        "status ready=%s actor_valid=%s protocol=%s actor=%s history=%d sequence=%d native_bridge=%s renderer=%s",
        tostring(ready and valid(active_actor)), tostring(valid(active_actor)), PROTOCOL,
        active_actor_name ~= "" and active_actor_name or "<none>", #history, sequence,
        tostring(native_bridge_seen), tostring(native_renderer_seen)
    ))
    return true
end)

log("loaded protocol=" .. PROTOCOL .. "; native ImGui UI starts hidden; F8=one-line/expanded/hidden Enter=send")
