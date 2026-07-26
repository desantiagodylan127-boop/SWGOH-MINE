cmake_minimum_required(VERSION 3.22)
project(heroes_offline_hook_compat LANGUAGES C CXX ASM)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver ON CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)

# This fork includes the upstream Android/ARM64 build and short-trampoline fixes
# proposed in jmpews/Dobby#302. Pinning keeps builds reproducible.
FetchContent_Declare(
  dobby
  GIT_REPOSITORY https://github.com/Linifadomra/Dobby.git
  GIT_TAG a418c6a7c493e6c599713a097ff823c84a40ba96
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(dobby)

add_library(shadowhook SHARED src/shadowhook_compat.cpp)
target_compile_features(shadowhook PRIVATE cxx_std_17)
target_compile_options(
  shadowhook
  PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall
    -Wextra
    -Werror
)
target_link_libraries(shadowhook PRIVATE dobby::dobby android dl log)
set_target_properties(shadowhook PROPERTIES OUTPUT_NAME shadowhook)
