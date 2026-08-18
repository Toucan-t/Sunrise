#include "runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../state/activity/runtime.h"
#include "activity_registry.h"

namespace sunrise::server::script {
namespace {

constexpr std::uint64_t kMaximumScriptBytes = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kReloadPollMs = 750;
constexpr std::size_t kTimerCapacity = 64;
constexpr std::size_t kScriptMessageCapacity = 512;

struct Timer final {
    std::uint64_t dueTick{};
    int callbackRef{LUA_NOREF};
    bool occupied{};
};

struct SessionContext final {
    lua_State* lua{};
    core::path::Buffer typeMainPath{};
    core::path::Buffer activityMainPath{};
    activity_registry::Definition definition{};
    std::array<Timer, kTimerCapacity> timers{};
    std::uint64_t sessionId{};
    std::uint64_t now{};
    std::uint64_t nextReloadPoll{};
    std::uint64_t observedTypeStamp{};
    std::uint64_t observedActivityStamp{};
    std::uint64_t scriptStateRevision{};
    std::int32_t regionIndex{state::activity::membership::kAbsentRegionIndex};
    std::uint32_t regionHash{};
    std::uint32_t scriptState{};
    std::int16_t activityIndex{state::activity::destination::kAbsentActivityIndex};
    int environmentRef{LUA_NOREF};
    int persistentStateRef{LUA_NOREF};
    bool occupied{};
    bool supported{};
    bool loadingCandidate{};
    bool reloadRequested{};
    bool stateEventPending{};
    std::uint32_t pendingPreviousState{};
    std::uint32_t pendingCurrentState{};
    std::uint64_t pendingStateRevision{};
};

core::path::Buffer g_scriptRoot{};
std::array<SessionContext, state::activity::kSessionCapacity> g_sessions{};
bool g_initialized{};
bool g_rootAvailable{};

[[nodiscard]] std::size_t sanitize_message(std::string_view input,
                                           std::array<char, kScriptMessageCapacity>& output) noexcept {
    output = {};
    const std::size_t count = std::min(input.size(), output.size() - 1);
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char value = static_cast<unsigned char>(input[index]);
        if (value == '\r' || value == '\n' || value == '\t') {
            output[index] = ' ';
        } else if (value < 0x20U) {
            output[index] = '?';
        } else {
            output[index] = static_cast<char>(value);
        }
    }
    return count;
}

void report(const SessionContext* context,
            std::string_view stage,
            std::string_view result,
            std::string_view detail = {}) noexcept {
    std::array<char, kScriptMessageCapacity> safe{};
    const std::size_t safeLength = sanitize_message(detail, safe);
    std::array<char, core::log::kLineCapacity> line{};
    const unsigned long long session = context != nullptr
                                           ? static_cast<unsigned long long>(context->sessionId)
                                           : 0ULL;
    const int written = safeLength == 0
                            ? std::snprintf(line.data(),
                                            line.size(),
                                            "ev=script stage=%.*s result=%.*s session=0x%llX",
                                            static_cast<int>(stage.size()),
                                            stage.data(),
                                            static_cast<int>(result.size()),
                                            result.data(),
                                            session)
                            : std::snprintf(line.data(),
                                            line.size(),
                                            "ev=script stage=%.*s result=%.*s session=0x%llX detail=%.*s",
                                            static_cast<int>(stage.size()),
                                            stage.data(),
                                            static_cast<int>(result.size()),
                                            result.data(),
                                            session,
                                            static_cast<int>(safeLength),
                                            safe.data());
    if (written > 0) {
        const std::size_t length = std::min(static_cast<std::size_t>(written), line.size() - 1);
        core::log::write(core::log::Channel::server,
                         result == "fail" ? core::log::Level::warn : core::log::Level::info,
                         {line.data(), length});
    }
}

void report_lua_error(SessionContext& context, std::string_view stage) noexcept {
    if (context.lua == nullptr) {
        report(&context, stage, "fail", "lua_state_missing");
        return;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(context.lua, -1, &length);
    report(&context,
           stage,
           "fail",
           text != nullptr ? std::string_view{text, length} : std::string_view{"unknown_lua_error"});
    lua_pop(context.lua, 1);
}

[[nodiscard]] bool directory_exists(const core::path::Buffer& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.chars.data());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

[[nodiscard]] bool script_path(std::wstring_view relative, core::path::Buffer& output) noexcept {
    output = g_scriptRoot;
    return core::path::append(output, L"\\") && core::path::append(output, relative);
}

[[nodiscard]] bool read_script(const core::path::Buffer& path, std::vector<char>& output) {
    output.clear();
    const HANDLE file = CreateFileW(path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER length{};
    const bool validLength = GetFileSizeEx(file, &length) != FALSE && length.QuadPart >= 0
                             && static_cast<std::uint64_t>(length.QuadPart) <= kMaximumScriptBytes;
    if (!validLength) {
        CloseHandle(file);
        return false;
    }
    output.resize(static_cast<std::size_t>(length.QuadPart));
    std::size_t offset = 0;
    bool readOk = true;
    while (offset < output.size()) {
        const std::size_t remaining = output.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (ReadFile(file, output.data() + offset, requested, &read, nullptr) == FALSE || read == 0) {
            readOk = false;
            break;
        }
        offset += static_cast<std::size_t>(read);
    }
    CloseHandle(file);
    if (!readOk) {
        output.clear();
    }
    return readOk;
}

[[nodiscard]] std::uint64_t file_stamp(const core::path::Buffer& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.chars.data(), GetFileExInfoStandard, &data) == FALSE
        || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return 0;
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return stamp.QuadPart;
}

void open_libraries(lua_State* state) noexcept {
    struct Library final {
        const char* name;
        lua_CFunction open;
    };
    constexpr std::array<Library, 6> libraries{{
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    }};
    for (const Library& library : libraries) {
        luaL_requiref(state, library.name, library.open, 1);
        lua_pop(state, 1);
    }
    lua_pushnil(state);
    lua_setglobal(state, "dofile");
    lua_pushnil(state);
    lua_setglobal(state, "loadfile");
}

[[nodiscard]] SessionContext* context_from_upvalue(lua_State* state) noexcept {
    return static_cast<SessionContext*>(lua_touserdata(state, lua_upvalueindex(1)));
}

int lua_log(lua_State* state) {
    SessionContext* context = context_from_upvalue(state);
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    report(context, "lua", "ok", std::string_view{text, length});
    return 0;
}

int lua_reload(lua_State* state) noexcept {
    SessionContext* context = context_from_upvalue(state);
    if (context != nullptr) {
        context->reloadRequested = true;
    }
    return 0;
}

int lua_after(lua_State* state) {
    SessionContext* context = context_from_upvalue(state);
    if (context == nullptr || !context->occupied) {
        return luaL_error(state, "sunrise.after requires an active activity session");
    }
    if (context->loadingCandidate) {
        return luaL_error(state, "sunrise.after cannot run while a script file is loading");
    }
    const lua_Number seconds = luaL_checknumber(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    if (!std::isfinite(static_cast<double>(seconds)) || seconds < 0.0 || seconds > 86'400.0) {
        return luaL_error(state, "sunrise.after delay must be between 0 and 86400 seconds");
    }
    Timer* slot = nullptr;
    for (Timer& timer : context->timers) {
        if (!timer.occupied) {
            slot = &timer;
            break;
        }
    }
    if (slot == nullptr) {
        return luaL_error(state, "sunrise timer capacity reached");
    }
    const double milliseconds = std::ceil(static_cast<double>(seconds) * 1000.0);
    lua_pushvalue(state, 2);
    slot->callbackRef = luaL_ref(state, LUA_REGISTRYINDEX);
    slot->dueTick = context->now + static_cast<std::uint64_t>(milliseconds);
    slot->occupied = true;
    return 0;
}

int lua_activity_session_id(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    std::array<char, 32> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "0x%016llX",
                                      context != nullptr
                                          ? static_cast<unsigned long long>(context->sessionId)
                                          : 0ULL);
    lua_pushlstring(state, text.data(), written > 0 ? static_cast<std::size_t>(written) : 0U);
    return 1;
}

int lua_activity_name(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    const std::string_view name = context != nullptr ? context->definition.destination
                                                      : std::string_view{};
    lua_pushlstring(state, name.data(), name.size());
    return 1;
}

int lua_activity_type(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    const std::string_view name = context != nullptr ? context->definition.typeName
                                                      : std::string_view{};
    lua_pushlstring(state, name.data(), name.size());
    return 1;
}

int lua_activity_script_name(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    const std::string_view name = context != nullptr ? context->definition.activityName
                                                      : std::string_view{};
    lua_pushlstring(state, name.data(), name.size());
    return 1;
}

int lua_activity_id(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    lua_pushinteger(state,
                    static_cast<lua_Integer>(context != nullptr ? context->activityIndex : -1));
    return 1;
}

int lua_activity_region_index(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    lua_pushinteger(state,
                    static_cast<lua_Integer>(context != nullptr ? context->regionIndex : -1));
    return 1;
}

int lua_activity_region_hash(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    lua_pushinteger(state,
                    static_cast<lua_Integer>(context != nullptr ? context->regionHash : 0U));
    return 1;
}

int lua_activity_state(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    lua_pushinteger(state,
                    static_cast<lua_Integer>(context != nullptr ? context->scriptState : 0U));
    return 1;
}

int lua_activity_state_revision(lua_State* state) {
    const SessionContext* context = context_from_upvalue(state);
    lua_pushinteger(state,
                    static_cast<lua_Integer>(context != nullptr ? context->scriptStateRevision : 0));
    return 1;
}

int apply_script_state(lua_State* state, SessionContext& context, std::uint32_t requested) {
    std::uint32_t previous = 0;
    std::uint64_t revision = 0;
    const state::activity::script_state::MutationResult result =
        state::activity::set_script_state(context.sessionId, requested, previous, revision);
    const bool accepted = result == state::activity::script_state::MutationResult::ok
                          || result == state::activity::script_state::MutationResult::unchanged;
    if (accepted) {
        context.scriptState = requested;
        context.scriptStateRevision = revision;
        if (requested != previous) {
            context.stateEventPending = true;
            context.pendingPreviousState = previous;
            context.pendingCurrentState = requested;
            context.pendingStateRevision = revision;
        }
        std::array<char, 160> detail{};
        const int written = std::snprintf(detail.data(),
                                          detail.size(),
                                          "old=%u new=%u revision=%llu client_publish=pending",
                                          previous,
                                          requested,
                                          static_cast<unsigned long long>(revision));
        report(&context,
               "state",
               result == state::activity::script_state::MutationResult::unchanged ? "unchanged"
                                                                                  : "advanced",
               written > 0 ? std::string_view{detail.data(), static_cast<std::size_t>(written)}
                           : std::string_view{});
    }
    lua_pushboolean(state, accepted ? 1 : 0);
    lua_pushinteger(state, static_cast<lua_Integer>(previous));
    lua_pushinteger(state, static_cast<lua_Integer>(revision));
    return 3;
}

int lua_activity_set_state(lua_State* state) {
    SessionContext* context = context_from_upvalue(state);
    if (context == nullptr || !context->occupied) {
        return luaL_error(state, "activity.set_state requires an active activity session");
    }
    const lua_Integer value = luaL_checkinteger(state, 1);
    if (value < 0 || static_cast<unsigned long long>(value) > 0xFFFFFFFFULL) {
        return luaL_error(state, "activity state must fit an unsigned 32-bit value");
    }
    return apply_script_state(state, *context, static_cast<std::uint32_t>(value));
}

int lua_activity_advance_state(lua_State* state) {
    SessionContext* context = context_from_upvalue(state);
    if (context == nullptr || !context->occupied) {
        return luaL_error(state, "activity.advance_state requires an active activity session");
    }
    if (context->scriptState == 0xFFFFFFFFU) {
        return luaL_error(state, "activity state is already at the 32-bit maximum");
    }
    return apply_script_state(state, *context, context->scriptState + 1U);
}

void push_context_closure(lua_State* state, SessionContext& context, lua_CFunction function) {
    lua_pushlightuserdata(state, &context);
    lua_pushcclosure(state, function, 1);
}

void install_api(SessionContext& context) noexcept {
    lua_State* state = context.lua;
    lua_newtable(state);
    push_context_closure(state, context, lua_log);
    lua_setfield(state, -2, "log");
    push_context_closure(state, context, lua_after);
    lua_setfield(state, -2, "after");
    push_context_closure(state, context, lua_reload);
    lua_setfield(state, -2, "reload");
    lua_setglobal(state, "sunrise");

    lua_newtable(state);
    push_context_closure(state, context, lua_activity_session_id);
    lua_setfield(state, -2, "session_id");
    push_context_closure(state, context, lua_activity_name);
    lua_setfield(state, -2, "name");
    push_context_closure(state, context, lua_activity_type);
    lua_setfield(state, -2, "type");
    push_context_closure(state, context, lua_activity_script_name);
    lua_setfield(state, -2, "script_name");
    push_context_closure(state, context, lua_activity_id);
    lua_setfield(state, -2, "id");
    push_context_closure(state, context, lua_activity_region_index);
    lua_setfield(state, -2, "region_index");
    push_context_closure(state, context, lua_activity_region_hash);
    lua_setfield(state, -2, "region_hash");
    push_context_closure(state, context, lua_activity_state);
    lua_setfield(state, -2, "state");
    push_context_closure(state, context, lua_activity_state_revision);
    lua_setfield(state, -2, "state_revision");
    push_context_closure(state, context, lua_activity_set_state);
    lua_setfield(state, -2, "set_state");
    push_context_closure(state, context, lua_activity_advance_state);
    lua_setfield(state, -2, "advance_state");
    lua_setglobal(state, "activity");
}

void reset_persistent_state(SessionContext& context) noexcept {
    if (context.persistentStateRef != LUA_NOREF) {
        luaL_unref(context.lua, LUA_REGISTRYINDEX, context.persistentStateRef);
        context.persistentStateRef = LUA_NOREF;
    }
    lua_newtable(context.lua);
    context.persistentStateRef = luaL_ref(context.lua, LUA_REGISTRYINDEX);
    lua_getglobal(context.lua, "sunrise");
    lua_rawgeti(context.lua, LUA_REGISTRYINDEX, context.persistentStateRef);
    lua_setfield(context.lua, -2, "state");
    lua_pop(context.lua, 1);
}

void clear_timers(SessionContext& context) noexcept {
    if (context.lua == nullptr) {
        context.timers = {};
        return;
    }
    for (Timer& timer : context.timers) {
        if (timer.occupied && timer.callbackRef != LUA_NOREF) {
            luaL_unref(context.lua, LUA_REGISTRYINDEX, timer.callbackRef);
        }
        timer = {};
        timer.callbackRef = LUA_NOREF;
    }
}

[[nodiscard]] bool push_callback(SessionContext& context, const char* name) noexcept {
    if (context.lua == nullptr || context.environmentRef == LUA_NOREF) {
        return false;
    }
    lua_rawgeti(context.lua, LUA_REGISTRYINDEX, context.environmentRef);
    lua_getfield(context.lua, -1, name);
    lua_remove(context.lua, -2);
    if (!lua_isfunction(context.lua, -1)) {
        lua_pop(context.lua, 1);
        return false;
    }
    return true;
}

void call_callback(SessionContext& context, const char* name) noexcept {
    if (!push_callback(context, name)) {
        return;
    }
    if (lua_pcall(context.lua, 0, 0, 0) != LUA_OK) {
        report_lua_error(context, name);
    }
}

void call_region_callback(SessionContext& context) noexcept {
    if (!push_callback(context, "on_region_entered")) {
        return;
    }
    lua_pushinteger(context.lua, static_cast<lua_Integer>(context.regionIndex));
    lua_pushinteger(context.lua, static_cast<lua_Integer>(context.regionHash));
    if (lua_pcall(context.lua, 2, 0, 0) != LUA_OK) {
        report_lua_error(context, "on_region_entered");
    }
}

void dispatch_state_event(SessionContext& context) noexcept {
    if (!context.stateEventPending) {
        return;
    }
    const std::uint32_t previous = context.pendingPreviousState;
    const std::uint32_t current = context.pendingCurrentState;
    const std::uint64_t revision = context.pendingStateRevision;
    context.stateEventPending = false;
    if (!push_callback(context, "on_state_changed")) {
        return;
    }
    lua_pushinteger(context.lua, static_cast<lua_Integer>(previous));
    lua_pushinteger(context.lua, static_cast<lua_Integer>(current));
    lua_pushinteger(context.lua, static_cast<lua_Integer>(revision));
    if (lua_pcall(context.lua, 3, 0, 0) != LUA_OK) {
        report_lua_error(context, "on_state_changed");
    }
}

[[nodiscard]] int create_environment(SessionContext& context) noexcept {
    lua_newtable(context.lua);
    const int environment = lua_gettop(context.lua);
    lua_pushvalue(context.lua, environment);
    lua_setfield(context.lua, environment, "_G");
    lua_newtable(context.lua);
    lua_pushglobaltable(context.lua);
    lua_setfield(context.lua, -2, "__index");
    lua_setmetatable(context.lua, environment);
    lua_pushvalue(context.lua, environment);
    const int reference = luaL_ref(context.lua, LUA_REGISTRYINDEX);
    lua_pop(context.lua, 1);
    return reference;
}

[[nodiscard]] bool execute_file(SessionContext& context,
                                const core::path::Buffer& path,
                                std::string_view chunkName,
                                int environmentRef) {
    std::vector<char> source{};
    if (!read_script(path, source)) {
        report(&context, "load", "fail", chunkName);
        return false;
    }
    const char* sourceData = source.empty() ? "" : source.data();
    if (luaL_loadbufferx(context.lua, sourceData, source.size(), chunkName.data(), "t") != LUA_OK) {
        report_lua_error(context, "compile");
        return false;
    }
    lua_rawgeti(context.lua, LUA_REGISTRYINDEX, environmentRef);
    if (lua_setupvalue(context.lua, -2, 1) == nullptr) {
        lua_pop(context.lua, 1);
        report(&context, "load", "fail", "chunk_has_no_environment");
        return false;
    }
    if (lua_pcall(context.lua, 0, 0, 0) != LUA_OK) {
        report_lua_error(context, "execute");
        return false;
    }
    return true;
}

[[nodiscard]] int load_candidate(SessionContext& context) noexcept {
    int candidate = LUA_NOREF;
    try {
        candidate = create_environment(context);
        context.loadingCandidate = true;
        const bool typeLoaded = execute_file(context,
                                             context.typeMainPath,
                                             "@activities/type/main.lua",
                                             candidate);
        const bool activityLoaded = typeLoaded
                                    && execute_file(context,
                                                    context.activityMainPath,
                                                    "@activities/activity/main.lua",
                                                    candidate);
        context.loadingCandidate = false;
        if (!activityLoaded) {
            luaL_unref(context.lua, LUA_REGISTRYINDEX, candidate);
            return LUA_NOREF;
        }
        return candidate;
    } catch (...) {
        context.loadingCandidate = false;
        if (candidate != LUA_NOREF) {
            luaL_unref(context.lua, LUA_REGISTRYINDEX, candidate);
        }
        report(&context, "load", "fail", "allocation_exception");
        return LUA_NOREF;
    }
}

[[nodiscard]] bool commit_environment(SessionContext& context, bool reloading) noexcept {
    const int candidate = load_candidate(context);
    if (candidate == LUA_NOREF) {
        return false;
    }
    clear_timers(context);
    const int previous = context.environmentRef;
    context.environmentRef = candidate;
    if (previous != LUA_NOREF) {
        luaL_unref(context.lua, LUA_REGISTRYINDEX, previous);
    }
    report(&context,
           reloading ? "reload" : "load",
           "ok",
           context.definition.activityName);
    call_callback(context, reloading ? "on_reload" : "on_start");
    return true;
}

void destroy_context(SessionContext& context, std::string_view reason) noexcept {
    if (!context.occupied) {
        return;
    }
    if (context.lua != nullptr && context.environmentRef != LUA_NOREF) {
        call_callback(context, "on_stop");
    }
    clear_timers(context);
    if (context.lua != nullptr && context.environmentRef != LUA_NOREF) {
        luaL_unref(context.lua, LUA_REGISTRYINDEX, context.environmentRef);
    }
    if (context.lua != nullptr && context.persistentStateRef != LUA_NOREF) {
        luaL_unref(context.lua, LUA_REGISTRYINDEX, context.persistentStateRef);
    }
    if (context.lua != nullptr) {
        lua_close(context.lua);
    }
    report(&context, "activity", "stopped", reason);
    context = {};
    context.regionIndex = state::activity::membership::kAbsentRegionIndex;
    context.activityIndex = state::activity::destination::kAbsentActivityIndex;
    context.environmentRef = LUA_NOREF;
    context.persistentStateRef = LUA_NOREF;
}

[[nodiscard]] SessionContext* find_context(std::uint64_t sessionId) noexcept {
    for (SessionContext& context : g_sessions) {
        if (context.occupied && context.sessionId == sessionId) {
            return &context;
        }
    }
    return nullptr;
}

[[nodiscard]] SessionContext* allocate_context() noexcept {
    for (SessionContext& context : g_sessions) {
        if (!context.occupied) {
            return &context;
        }
    }
    return nullptr;
}

void start_context(SessionContext& context,
                   const state::activity::JoinedSessionSnapshot& snapshot,
                   std::uint64_t now) noexcept {
    context = {};
    context.occupied = true;
    context.sessionId = snapshot.context.sessionId;
    context.now = now;
    context.regionIndex = snapshot.context.regionIndex;
    context.regionHash = snapshot.context.regionHash;
    context.activityIndex = snapshot.activityIndex;
    context.scriptState = snapshot.scriptState;
    context.scriptStateRevision = snapshot.scriptStateRevision;
    context.environmentRef = LUA_NOREF;
    context.persistentStateRef = LUA_NOREF;

    const std::string_view destination{snapshot.context.destination.data(),
                                       snapshot.context.destinationLength};
    if (!activity_registry::find(destination, context.definition)) {
        report(&context, "select", "skipped", destination);
        return;
    }
    context.supported = true;
    if (!script_path(context.definition.typeMain, context.typeMainPath)
        || !script_path(context.definition.activityMain, context.activityMainPath)) {
        report(&context, "select", "fail", "script_path_overflow");
        context.supported = false;
        return;
    }
    context.observedTypeStamp = file_stamp(context.typeMainPath);
    context.observedActivityStamp = file_stamp(context.activityMainPath);
    context.lua = luaL_newstate();
    if (context.lua == nullptr) {
        report(&context, "select", "fail", "lua_state_allocation");
        context.supported = false;
        return;
    }
    open_libraries(context.lua);
    install_api(context);
    reset_persistent_state(context);
    std::array<char, 224> detail{};
    const int detailWritten = std::snprintf(
        detail.data(),
        detail.size(),
        "destination=%.*s activity=%d type=%.*s script=%.*s state=%u revision=%llu",
        static_cast<int>(destination.size()),
        destination.data(),
        static_cast<int>(context.activityIndex),
        static_cast<int>(context.definition.typeName.size()),
        context.definition.typeName.data(),
        static_cast<int>(context.definition.activityName.size()),
        context.definition.activityName.data(),
        context.scriptState,
        static_cast<unsigned long long>(context.scriptStateRevision));
    report(&context,
           "session",
           "attached",
           detailWritten > 0
               ? std::string_view{detail.data(),
                                  std::min(static_cast<std::size_t>(detailWritten),
                                           detail.size() - 1)}
               : std::string_view{});
    if (commit_environment(context, false) && context.regionIndex >= 0) {
        call_region_callback(context);
    }
    dispatch_state_event(context);
}

void service_timers(SessionContext& context, std::uint64_t now) noexcept {
    if (context.lua == nullptr || context.environmentRef == LUA_NOREF) {
        return;
    }
    for (Timer& timer : context.timers) {
        if (!timer.occupied || timer.dueTick > now) {
            continue;
        }
        const int callback = timer.callbackRef;
        timer = {};
        timer.callbackRef = LUA_NOREF;
        lua_rawgeti(context.lua, LUA_REGISTRYINDEX, callback);
        luaL_unref(context.lua, LUA_REGISTRYINDEX, callback);
        if (lua_pcall(context.lua, 0, 0, 0) != LUA_OK) {
            report_lua_error(context, "timer");
        }
    }
}

void service_reload(SessionContext& context, std::uint64_t now) noexcept {
    if (!context.supported || context.lua == nullptr
        || (now < context.nextReloadPoll && !context.reloadRequested)) {
        return;
    }
    context.nextReloadPoll = now + kReloadPollMs;
    const std::uint64_t typeStamp = file_stamp(context.typeMainPath);
    const std::uint64_t activityStamp = file_stamp(context.activityMainPath);
    const bool changed = context.reloadRequested || typeStamp != context.observedTypeStamp
                         || activityStamp != context.observedActivityStamp;
    context.reloadRequested = false;
    if (!changed) {
        return;
    }
    context.observedTypeStamp = typeStamp;
    context.observedActivityStamp = activityStamp;
    const bool reloading = context.environmentRef != LUA_NOREF;
    if (!commit_environment(context, reloading)) {
        report(&context,
               reloading ? "reload" : "load",
               reloading ? "kept_previous" : "retry_failed",
               context.definition.activityName);
        return;
    }
    if (!reloading && context.regionIndex >= 0) {
        call_region_callback(context);
    }
}

[[nodiscard]] bool snapshot_contains(const state::activity::JoinedSessionSnapshots& snapshots,
                                     std::size_t count,
                                     std::uint64_t sessionId) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (snapshots[index].context.sessionId == sessionId) {
            return true;
        }
    }
    return false;
}

void update_context(SessionContext& context,
                    const state::activity::JoinedSessionSnapshot& snapshot,
                    std::uint64_t now) noexcept {
    context.now = now;
    if (snapshot.scriptState != context.scriptState
        || snapshot.scriptStateRevision != context.scriptStateRevision) {
        context.pendingPreviousState = context.scriptState;
        context.pendingCurrentState = snapshot.scriptState;
        context.pendingStateRevision = snapshot.scriptStateRevision;
        context.scriptState = snapshot.scriptState;
        context.scriptStateRevision = snapshot.scriptStateRevision;
        context.stateEventPending = true;
    }
    const bool enteredRegion = snapshot.context.regionIndex >= 0
                               && snapshot.context.regionIndex != context.regionIndex;
    const bool regionChanged = snapshot.context.regionIndex != context.regionIndex
                               || snapshot.context.regionHash != context.regionHash;
    if (regionChanged) {
        context.regionIndex = snapshot.context.regionIndex;
        context.regionHash = snapshot.context.regionHash;
        if (enteredRegion && context.supported && context.lua != nullptr) {
            call_region_callback(context);
        }
    }
    service_reload(context, now);
    service_timers(context, now);
    dispatch_state_event(context);
}

} // namespace

bool initialize(void* module) noexcept {
    if (g_initialized) {
        return g_rootAvailable;
    }
    g_initialized = true;
    if (module == nullptr || !core::path::module_directory(module, g_scriptRoot)
        || !core::path::append(g_scriptRoot, L"scripts")) {
        report(nullptr, "initialize", "fail", "module_script_root");
        return false;
    }
    g_rootAvailable = directory_exists(g_scriptRoot);
    if (!g_rootAvailable) {
        report(nullptr, "initialize", "fail", "scripts_directory_missing");
        return false;
    }
    report(nullptr, "initialize", "ok", "module_relative_session_scripts");
    return true;
}

void service(std::uint64_t now) noexcept {
    if (!g_initialized || !g_rootAvailable) {
        return;
    }
    state::activity::JoinedSessionSnapshots snapshots{};
    const std::size_t count = state::activity::joined_sessions_snapshot(snapshots);

    for (SessionContext& context : g_sessions) {
        if (context.occupied && !snapshot_contains(snapshots, count, context.sessionId)) {
            destroy_context(context, "session_left_or_evicted");
        }
    }

    for (std::size_t index = 0; index < count; ++index) {
        const auto& snapshot = snapshots[index];
        SessionContext* context = find_context(snapshot.context.sessionId);
        if (context == nullptr) {
            context = allocate_context();
            if (context == nullptr) {
                report(nullptr, "activity", "fail", "script_context_capacity");
                continue;
            }
            start_context(*context, snapshot, now);
            continue;
        }
        update_context(*context, snapshot, now);
    }
}

void shutdown() noexcept {
    if (!g_initialized) {
        return;
    }
    for (SessionContext& context : g_sessions) {
        destroy_context(context, "server_shutdown");
    }
    g_scriptRoot = {};
    g_rootAvailable = false;
    g_initialized = false;
    report(nullptr, "shutdown", "ok");
}

} // namespace sunrise::server::script
