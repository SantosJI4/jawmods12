#include "xdl.h"
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#define DO_API(r, n, p) r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

void init_il2cpp_api() {
    #define DO_API(r, n, p) n = (r (*) p)xdl_sym(xdl_open("libil2cpp.so", XDL_DEFAULT), #n, nullptr)
    #include "il2cpp-api-functions.h"
    #undef DO_API
}

void APIHook() {
    init_il2cpp_api();
}