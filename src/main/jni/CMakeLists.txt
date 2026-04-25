cmake_minimum_required(VERSION 3.22.1)
project("mp_josemods")

set(JNI_DIR ${CMAKE_SOURCE_DIR})

# ============================================================
# PREBUILT: libdobby.a
# ============================================================
add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES
    IMPORTED_LOCATION ${JNI_DIR}/lib/${ANDROID_ABI}/libdobby.a)

# ============================================================
# MODULE 1: libMEOW.so — Overlay externo (APK)
# ============================================================
add_library(MEOW SHARED
    ${JNI_DIR}/main.cpp
    ${JNI_DIR}/src/ImGui/imgui.cpp
    ${JNI_DIR}/src/ImGui/imgui_draw.cpp
    ${JNI_DIR}/src/ImGui/imgui_tables.cpp
    ${JNI_DIR}/src/ImGui/imgui_widgets.cpp
    ${JNI_DIR}/src/ImGui/imgui_demo.cpp
    ${JNI_DIR}/src/ImGui/backends/imgui_impl_android.cpp
    ${JNI_DIR}/src/ImGui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(MEOW PRIVATE
    ${JNI_DIR}
    ${JNI_DIR}/include/ImGui
    ${JNI_DIR}/include/ImGui/backends
    ${JNI_DIR}/include/ImGui/font
    ${JNI_DIR}/include/Utils
    ${JNI_DIR}/include/Utils/Unity
)

target_compile_options(MEOW PRIVATE
    -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
    -Wno-error=c++11-narrowing -Wall
)

target_compile_definitions(MEOW PRIVATE)

set_target_properties(MEOW PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(MEOW
    log android EGL GLESv3
)

# ============================================================
# MODULE 2: libgl2.so — Hook injetado no jogo
# ============================================================
add_library(gl2 SHARED
    ${JNI_DIR}/GameHook.cpp
)

target_include_directories(gl2 PRIVATE
    ${JNI_DIR}
    ${JNI_DIR}/include/Utils
    ${JNI_DIR}/include/Utils/Unity
    ${JNI_DIR}/include/Hook/Dobby
)

target_compile_options(gl2 PRIVATE
    -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
    -Wno-error=c++11-narrowing -Wall
)

set_target_properties(gl2 PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(gl2
    log android dobby
)

# ============================================================
# MODULE 3: libzygisk.so — Zygisk Module
# ============================================================
add_library(zygisk SHARED
    ${JNI_DIR}/zygisk_main.cpp
    ${JNI_DIR}/GameHook.cpp
)

target_include_directories(zygisk PRIVATE
    ${JNI_DIR}
    ${JNI_DIR}/include/Utils
    ${JNI_DIR}/include/Utils/Unity
    ${JNI_DIR}/include/Hook/Dobby
)

target_compile_options(zygisk PRIVATE
    -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
    -Wno-error=c++11-narrowing -Wall
)

target_compile_definitions(zygisk PRIVATE
    ZYGISK_BUILD STEALTH_DEBUG
)

set_target_properties(zygisk PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(zygisk
    log android dobby
)

# ============================================================
# MODULE 4: libinjector.so — Ptrace injector
# ============================================================
add_library(injector SHARED
    ${JNI_DIR}/injector.c
)

target_compile_options(injector PRIVATE
    -w -fvisibility=hidden -O2 -DNDEBUG -fPIC
)

target_link_libraries(injector
    log dl
)
