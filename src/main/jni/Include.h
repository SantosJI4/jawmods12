#include "Utils.h"
#include "jniStuff.h"
#include "ByNameModding/Tools.h"
#include "ByNameModding/fake_dlfcn.h"
#include "ByNameModding/Il2Cpp.h"
#include "include/Utils/MonoString.h"
#include "Toggle.h"

int glWidth, glHeight;

/*
const-string v0, "MEOW"
invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
*/
