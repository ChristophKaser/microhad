/* Compile source only: fixture checks must never execute automation code. */
#include <stdio.h>
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
int main(int argc, char **argv) {
    lua_State *state = luaL_newstate();
    int result;
    if (!state || argc != 2) return 2;
    result = luaL_loadfilex(state, argv[1], "t");
    if (result != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(state, -1));
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
