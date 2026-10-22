#include "script_benchmark.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <stdint.h>
#include <time.h>

#if MICROHAD_SCRIPT_RUNTIME == 1
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#elif MICROHAD_SCRIPT_RUNTIME == 2
extern "C" {
#include "quickjs.h"
}
#endif

namespace {
const size_t kHeapLimit = 1024U * 1024U;
const int64_t kDeadlineNanoseconds = static_cast<int64_t>(5) * 1000 * 1000 * 1000;

int64_t monotonicNanoseconds() {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<int64_t>(value.tv_sec) * static_cast<int64_t>(1000000000) + value.tv_nsec;
}

long residentKilobytes() {
    std::ifstream input("/proc/self/status");
    std::string key;
    long value;
    while (input >> key >> value) {
        if (key == "VmRSS:") return value;
        input.ignore(4096, '\n');
    }
    return -1;
}

std::string jsonEscape(const std::string& input) {
    std::string output;
    for (std::string::size_type i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '\\' || ch == '"') { output += '\\'; output += ch; }
        else if (ch == '\n') output += "\\n";
        else if (ch == '\r') output += "\\r";
        else if (ch == '\t') output += "\\t";
        else if (static_cast<unsigned char>(ch) >= 0x20) output += ch;
    }
    return output;
}

bool readFile(const std::string& filename, std::string* contents, std::string* error) {
    std::ifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input) { *error = "cannot read script"; return false; }
    std::ostringstream buffer; buffer << input.rdbuf(); *contents = buffer.str();
    if (contents->size() > 128U * 1024U) { *error = "script exceeds 128 KiB"; return false; }
    return true;
}

struct BenchmarkState {
    unsigned long ticks;
    unsigned long actions;
    int64_t deadline;
    size_t used;
    BenchmarkState() : ticks(0), actions(0), deadline(0), used(0) {}
};

#if MICROHAD_SCRIPT_RUNTIME == 1
void* limitedAlloc(void* opaque, void* pointer, size_t oldSize, size_t newSize) {
    BenchmarkState* state = static_cast<BenchmarkState*>(opaque);
    if (newSize == 0) { if (oldSize <= state->used) state->used -= oldSize; free(pointer); return 0; }
    const size_t next = state->used - (oldSize <= state->used ? oldSize : state->used) + newSize;
    if (next > kHeapLimit) return 0;
    void* resized = realloc(pointer, newSize);
    if (resized != 0) state->used = next;
    return resized;
}

int luaTick(lua_State* state) {
    BenchmarkState* benchmark = static_cast<BenchmarkState*>(lua_touserdata(state, lua_upvalueindex(1)));
    ++benchmark->ticks; lua_pushnumber(state, static_cast<lua_Number>(benchmark->ticks)); return 1;
}

int luaAction(lua_State* state) {
    BenchmarkState* benchmark = static_cast<BenchmarkState*>(lua_touserdata(state, lua_upvalueindex(1)));
    ++benchmark->actions; lua_pushboolean(state, 1); return 1;
}

void luaLimitHook(lua_State* state, lua_Debug*) {
    void* opaque = 0; lua_getallocf(state, &opaque);
    BenchmarkState* benchmark = static_cast<BenchmarkState*>(opaque);
    if (monotonicNanoseconds() > benchmark->deadline) luaL_error(state, "script execution deadline exceeded");
}

bool runLua(const std::string& source, const std::string& filename, unsigned long runs, BenchmarkState* state, std::string* error) {
    lua_State* lua = lua_newstate(limitedAlloc, state);
    if (lua == 0) { *error = "cannot create Lua state"; return false; }
    luaL_requiref(lua, "_G", luaopen_base, 1); lua_pop(lua, 1);
    luaL_requiref(lua, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(lua, 1);
    luaL_requiref(lua, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(lua, 1);
    luaL_requiref(lua, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(lua, 1);
    lua_pushnil(lua); lua_setglobal(lua, "dofile"); lua_pushnil(lua); lua_setglobal(lua, "loadfile");
    lua_pushnil(lua); lua_setglobal(lua, "load"); lua_pushnil(lua); lua_setglobal(lua, "collectgarbage");
    lua_newtable(lua); lua_pushlightuserdata(lua, state); lua_pushcclosure(lua, luaTick, 1); lua_setfield(lua, -2, "tick");
    lua_pushlightuserdata(lua, state); lua_pushcclosure(lua, luaAction, 1); lua_setfield(lua, -2, "action"); lua_setglobal(lua, "benchmark");
    lua_sethook(lua, luaLimitHook, LUA_MASKCOUNT, 1000);
    state->deadline = monotonicNanoseconds() + kDeadlineNanoseconds;
    if (luaL_loadbufferx(lua, source.data(), source.size(), filename.c_str(), "t") != LUA_OK || lua_pcall(lua, 0, 0, 0) != LUA_OK) {
        *error = lua_tostring(lua, -1) == 0 ? "Lua script failed" : lua_tostring(lua, -1); lua_close(lua); return false;
    }
    lua_getglobal(lua, "run");
    if (!lua_isfunction(lua, -1)) { *error = "script must define run()"; lua_close(lua); return false; }
    for (unsigned long i = 0; i < runs; ++i) {
        state->deadline = monotonicNanoseconds() + kDeadlineNanoseconds;
        lua_pushvalue(lua, -1);
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) { *error = lua_tostring(lua, -1) == 0 ? "Lua run() failed" : lua_tostring(lua, -1); lua_close(lua); return false; }
    }
    lua_close(lua); return true;
}
#elif MICROHAD_SCRIPT_RUNTIME == 2
JSValue quickJsTick(JSContext* context, JSValueConst, int, JSValueConst*) {
    BenchmarkState* state = static_cast<BenchmarkState*>(JS_GetContextOpaque(context));
    ++state->ticks; return JS_NewInt64(context, static_cast<int64_t>(state->ticks));
}

JSValue quickJsAction(JSContext* context, JSValueConst, int, JSValueConst*) {
    BenchmarkState* state = static_cast<BenchmarkState*>(JS_GetContextOpaque(context));
    ++state->actions; return JS_TRUE;
}

int quickJsInterrupt(JSRuntime* runtime, void* opaque) {
    (void)runtime; BenchmarkState* state = static_cast<BenchmarkState*>(opaque);
    return monotonicNanoseconds() > state->deadline;
}

std::string quickJsError(JSContext* context) {
    JSValue exception = JS_GetException(context); const char* text = JS_ToCString(context, exception);
    std::string output = text == 0 ? "QuickJS script failed" : text;
    if (text != 0) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
    return output;
}

bool runQuickJs(const std::string& source, const std::string& filename, unsigned long runs, BenchmarkState* state, std::string* error) {
    JSRuntime* runtime = JS_NewRuntime();
    if (runtime == 0) { *error = "cannot create QuickJS runtime"; return false; }
    JS_SetMemoryLimit(runtime, kHeapLimit); JS_SetMaxStackSize(runtime, 256U * 1024U); JS_SetInterruptHandler(runtime, quickJsInterrupt, state);
    JSContext* context = JS_NewContext(runtime);
    if (context == 0) { JS_FreeRuntime(runtime); *error = "cannot create QuickJS context"; return false; }
    JS_SetContextOpaque(context, state);
    JSValue global = JS_GetGlobalObject(context); JSValue benchmark = JS_NewObject(context);
    JS_SetPropertyStr(context, benchmark, "tick", JS_NewCFunction(context, quickJsTick, "tick", 0));
    JS_SetPropertyStr(context, benchmark, "action", JS_NewCFunction(context, quickJsAction, "action", 1)); JS_SetPropertyStr(context, global, "benchmark", benchmark); JS_FreeValue(context, global);
    state->deadline = monotonicNanoseconds() + kDeadlineNanoseconds;
    JSValue evaluated = JS_Eval(context, source.data(), source.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(evaluated)) { *error = quickJsError(context); JS_FreeContext(context); JS_FreeRuntime(runtime); return false; }
    JS_FreeValue(context, evaluated);
    global = JS_GetGlobalObject(context); JSValue run = JS_GetPropertyStr(context, global, "run"); JS_FreeValue(context, global);
    if (!JS_IsFunction(context, run)) { JS_FreeValue(context, run); JS_FreeContext(context); JS_FreeRuntime(runtime); *error = "script must define run()"; return false; }
    for (unsigned long i = 0; i < runs; ++i) {
        state->deadline = monotonicNanoseconds() + kDeadlineNanoseconds;
        JSValue result = JS_Call(context, run, JS_UNDEFINED, 0, 0);
        if (JS_IsException(result)) { JS_FreeValue(context, result); *error = quickJsError(context); JS_FreeValue(context, run); JS_FreeContext(context); JS_FreeRuntime(runtime); return false; }
        JS_FreeValue(context, result);
    }
    JS_FreeValue(context, run); JS_FreeContext(context); JS_FreeRuntime(runtime); return true;
}
#endif
}

bool runScriptBenchmark(const std::string& runtime, const std::string& filename, unsigned long runs, std::string* json, std::string* error) {
    std::string source;
    if (runs == 0 || runs > 1000000UL || !readFile(filename, &source, error)) return false;
    BenchmarkState state; struct rusage before, after; getrusage(RUSAGE_SELF, &before); const int64_t started = monotonicNanoseconds();
#if MICROHAD_SCRIPT_RUNTIME == 1
    const bool result = runtime == "lua" && runLua(source, filename, runs, &state, error);
#elif MICROHAD_SCRIPT_RUNTIME == 2
    const bool result = runtime == "quickjs" && runQuickJs(source, filename, runs, &state, error);
#else
    const bool result = false; *error = "this binary has no script runtime";
#endif
    const int64_t elapsed = monotonicNanoseconds() - started; getrusage(RUSAGE_SELF, &after);
    if (!result) return false;
    std::ostringstream output;
    output << "{\"runtime\":\"" << jsonEscape(runtime) << "\",\"runs\":" << runs << ",\"elapsed_ns\":" << elapsed
           << ",\"ticks\":" << state.ticks << ",\"actions\":" << state.actions << ",\"max_rss_kb\":" << after.ru_maxrss << ",\"rss_kb\":" << residentKilobytes()
           << ",\"user_us\":" << (after.ru_utime.tv_sec-before.ru_utime.tv_sec)*1000000L+(after.ru_utime.tv_usec-before.ru_utime.tv_usec)
           << ",\"system_us\":" << (after.ru_stime.tv_sec-before.ru_stime.tv_sec)*1000000L+(after.ru_stime.tv_usec-before.ru_stime.tv_usec) << "}";
    *json = output.str(); return true;
}
