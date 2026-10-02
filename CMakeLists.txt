cmake_minimum_required(VERSION 3.16)
project(DefectiveEngine C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The reference CPU is a 2015 laptop part, so no -march=native: the binary must run on baseline x86-64.
if(NOT MSVC)
  set(DFE_WARN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fwrapv)
  # Noise hashing relies on wrapping integer multiplication; without this GCC treats the overflow as undefined.
  set(CMAKE_C_FLAGS_RELEASE "-O2 -fno-strict-aliasing -DNDEBUG")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -fno-strict-aliasing")
else()
  set(DFE_WARN /W3 /wd4244 /wd4267 /wd4996)
endif()

set(THIRD ${CMAKE_SOURCE_DIR}/third_party)

# GLFW: X11 only on Linux so a build needs no Wayland protocol tooling.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD}/glfw EXCLUDE_FROM_ALL)

# LuaJIT is built by its own makefile in a copy under the build tree so the source tree stays clean.
set(LUAJIT_BIN ${CMAKE_BINARY_DIR}/luajit)
if(NOT EXISTS ${LUAJIT_BIN}/src/lj_arch.h)
  file(COPY ${THIRD}/luajit/ DESTINATION ${LUAJIT_BIN})
endif()
if(MSVC)
  set(LUAJIT_LIB ${LUAJIT_BIN}/src/lua51.lib)
  add_custom_command(OUTPUT ${LUAJIT_LIB}
    COMMAND cmd /c "msvcbuild.bat static" WORKING_DIRECTORY ${LUAJIT_BIN}/src)
else()
  set(LUAJIT_LIB ${LUAJIT_BIN}/src/libluajit.a)
  find_program(DFE_MAKE NAMES gmake make REQUIRED)
  add_custom_command(OUTPUT ${LUAJIT_LIB}
    COMMAND ${DFE_MAKE} -C ${LUAJIT_BIN}/src BUILDMODE=static amalg
    COMMENT "Building LuaJIT")
endif()
add_custom_target(luajit_build DEPENDS ${LUAJIT_LIB})
add_library(luajit_static STATIC IMPORTED GLOBAL)
set_target_properties(luajit_static PROPERTIES IMPORTED_LOCATION ${LUAJIT_LIB})
add_dependencies(luajit_static luajit_build)

add_library(dfe_third STATIC
  ${THIRD}/glad/src/gl.c
  ${THIRD}/lz4/lz4.c)
target_include_directories(dfe_third PUBLIC ${THIRD}/glad/include ${THIRD}/lz4 ${THIRD}/stb ${THIRD}/miniaudio ${THIRD}/fastnoise)

add_library(dfe_engine STATIC
  src/base.c
  src/render.c
  src/json.c
  src/registry.c
  src/world.c
  src/light.c
  src/gen.c
  src/save.c
  src/mesher.c
  src/scene.c
  src/lod.c
  src/mods.c
  src/script.c
  src/console.c
  src/selftest.c)
target_include_directories(dfe_engine PUBLIC src include ${LUAJIT_BIN}/src)
target_compile_options(dfe_engine PRIVATE ${DFE_WARN})
target_link_libraries(dfe_engine PUBLIC dfe_third glfw luajit_static)
if(UNIX)
  target_link_libraries(dfe_engine PUBLIC m pthread dl)
endif()

add_executable(dfe src/main.c)
target_compile_options(dfe PRIVATE ${DFE_WARN})
target_link_libraries(dfe PRIVATE dfe_engine)
# Plugins resolve the C API through the executable, so symbols must be exported.
set_target_properties(dfe PROPERTIES ENABLE_EXPORTS ON)
