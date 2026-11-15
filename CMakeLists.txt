cmake_minimum_required(VERSION 3.24)
project(bomberman_recomp C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	message(FATAL_ERROR "Build with clang / clang-cl: the generated code relies on [[clang::musttail]].")
endif()

set(GENERATED_DIR ${CMAKE_SOURCE_DIR}/game/generated)
if(NOT EXISTS ${GENERATED_DIR}/ppu_funcs.h)
	message(FATAL_ERROR "Run the recompiler first:\n  python -I recomp/recompile.py NPUB30051/USRDIR/EBOOT.elf game/generated")
endif()

# --- third-party: SDL3 (window/input/audio), Vulkan headers, volk (Vulkan loader) ---
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG release-3.2.10
	GIT_SHALLOW TRUE)
FetchContent_Declare(VulkanHeaders
	GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
	GIT_TAG vulkan-sdk-1.4.304.0
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR do-not-add)      # headers only
FetchContent_Declare(volk
	GIT_REPOSITORY https://github.com/zeux/volk.git
	GIT_TAG vulkan-sdk-1.4.304.0
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR do-not-add)      # built below as a single source file
FetchContent_Declare(imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG v1.91.8
	GIT_SHALLOW TRUE)
FetchContent_Declare(stb    # stb_image: trophy icons (PNG) from TROPHY.TRP
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
	SOURCE_SUBDIR do-not-add)      # header only
FetchContent_Declare(glslang
	GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
	GIT_TAG 15.1.0
	GIT_SHALLOW TRUE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
# glslang: GLSL -> SPIR-V compiler only (RSX shaders are translated to GLSL at runtime)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3 VulkanHeaders volk glslang imgui stb)

add_library(volk STATIC ${volk_SOURCE_DIR}/volk.c)
target_include_directories(volk PUBLIC ${volk_SOURCE_DIR} ${vulkanheaders_SOURCE_DIR}/include)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)

# Dear ImGui for the overlay (profile menu, notices), drawn over the game by the presenter.
# Its font is embedded so the executable needs no extra files.
add_library(imgui STATIC
	${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
	${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp ${CMAKE_BINARY_DIR}/overlay_font.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static volk)
file(READ ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf FONT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FONT_BYTES "${FONT_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/overlay_font.cpp
	"// generated from imgui/misc/fonts/Roboto-Medium.ttf (Apache License 2.0)\n"
	"extern const unsigned char g_overlay_font[] = {${FONT_BYTES}};\n"
	"extern const unsigned int g_overlay_font_size = sizeof(g_overlay_font);\n")

# --- our code ---
if(MSVC)
	set(OUR_FLAGS /EHsc /W3 -msse4.1 -Wno-unused-label -Wno-unused-variable /Z7)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
	# keep a PDB in release builds so crash addresses can be symbolized
	add_link_options(/DEBUG /OPT:REF /OPT:ICF)
else()
	set(OUR_FLAGS -msse4.1 -Wno-unused-label -g)
endif()

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.cpp runtime/src/*/*.cpp)
add_library(ps3runtime OBJECT ${RUNTIME_SOURCES})
target_include_directories(ps3runtime PUBLIC runtime/include runtime/src)
target_include_directories(ps3runtime PRIVATE ${stb_SOURCE_DIR})
target_compile_options(ps3runtime PRIVATE ${OUR_FLAGS})
target_link_libraries(ps3runtime PUBLIC SDL3::SDL3-static volk imgui glslang::glslang glslang::glslang-default-resource-limits)

# recompiled PPU code, plus recompiled SPU programs (recomp/spu_recompile.py) in spu/
file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${GENERATED_DIR}/*.cpp ${GENERATED_DIR}/spu/*.cpp)
add_library(ppucode OBJECT ${GENERATED_SOURCES})
target_include_directories(ppucode PUBLIC runtime/include runtime/src ${GENERATED_DIR})
target_compile_options(ppucode PRIVATE ${OUR_FLAGS})

# HLE functions register themselves from static constructors, so the runtime is
# linked as object files rather than a static library (which would drop them).
add_executable(bomberman $<TARGET_OBJECTS:ps3runtime> $<TARGET_OBJECTS:ppucode>)

# The executable's icon (Explorer, taskbar, title bar; SDL uses the first icon resource for
# its window class): the Bomberman head from the game's trophy icons (tools/media/make_icon.py).
file(GLOB TROPHY_TRP ${CMAKE_SOURCE_DIR}/NPUB30051/TROPDIR/*/TROPHY.TRP)
if(TROPHY_TRP)
	list(GET TROPHY_TRP 0 TROPHY_TRP)
	add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/bomberman.ico
		COMMAND python -I ${CMAKE_SOURCE_DIR}/tools/media/make_icon.py ${TROPHY_TRP} ${CMAKE_BINARY_DIR}/bomberman.ico
		DEPENDS ${CMAKE_SOURCE_DIR}/tools/media/make_icon.py ${TROPHY_TRP})
	file(WRITE ${CMAKE_BINARY_DIR}/bomberman.rc "1 ICON \"bomberman.ico\"\n")
	set_source_files_properties(${CMAKE_BINARY_DIR}/bomberman.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/bomberman.ico)
	target_sources(bomberman PRIVATE ${CMAKE_BINARY_DIR}/bomberman.rc)
else()
	message(STATUS "No NPUB30051/TROPDIR/*/TROPHY.TRP: building without an icon")
endif()

target_link_libraries(bomberman PRIVATE SDL3::SDL3-static volk imgui glslang::glslang glslang::glslang-default-resource-limits)
