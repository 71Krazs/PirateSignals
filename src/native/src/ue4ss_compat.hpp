#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace RC
{
using CharType = wchar_t;
using StringType = std::basic_string<CharType>;
using StringViewType = std::basic_string_view<CharType>;
#define STR(value) L##value

namespace Input
{
enum Key : uint8_t
{
    RETURN = 0x0D,
    F8 = 0x77,
};
using EventCallbackCallable = std::function<void()>;
struct Handler
{
    using ModifierKeyArray = std::array<uint8_t, 3>;
};
} // namespace Input

namespace LuaMadeSimple
{
class __declspec(dllimport) Lua
{
public:
    using LuaFunction = int (*)(const Lua&);

    void register_function(const std::string& name, const LuaFunction&) const;
    [[nodiscard]] lua_State* get_lua_state() const;
    [[nodiscard]] int32_t get_stack_size() const;
    [[nodiscard]] bool is_nil(int32_t force_index = 1) const;
    void set_nil() const;
    [[nodiscard]] bool is_string(int32_t force_index = 1) const;
    [[nodiscard]] std::string_view get_string(int32_t force_index = 1) const;
    void set_string(std::string_view value) const;
    [[nodiscard]] bool is_integer(int32_t force_index = 1) const;
    [[nodiscard]] int64_t get_integer(int32_t force_index = 1) const;
    [[nodiscard]] bool is_bool(int32_t force_index = 1) const;
    [[nodiscard]] bool get_bool(int32_t force_index = 1) const;
    void set_bool(bool value) const;
};
} // namespace LuaMadeSimple

class __declspec(dllimport) CppUserModBase
{
protected:
    std::vector<std::shared_ptr<void>> GUITabs{};

public:
    StringType ModName{}, ModVersion{}, ModDescription{}, ModAuthors{}, ModIntendedSDKVersion{};
    CppUserModBase();
    virtual ~CppUserModBase();
    virtual void on_update() {}
    virtual void on_unreal_init() {}
    virtual void on_ui_init() {}
    virtual void on_program_start() {}
    virtual void on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) {}
    virtual void on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) {}
    virtual void on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) {}
    virtual void on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, std::vector<LuaMadeSimple::Lua*>&) {}
    virtual void on_dll_load(StringViewType) {}
    virtual void render_tab() {}
    virtual void on_lua_start(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) {}
    virtual void on_lua_start(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) {}
    virtual void on_lua_stop(StringViewType, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) {}
    virtual void on_lua_stop(LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua&, LuaMadeSimple::Lua*) {}
    virtual void on_cpp_mods_loaded() {}

protected:
    void register_keydown_event(Input::Key, const Input::EventCallbackCallable&, uint8_t custom_data = 0);
};
} // namespace RC
