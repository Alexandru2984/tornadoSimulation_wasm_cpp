cmake_minimum_required(VERSION 3.14)
project(tornado_sim VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ─────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# Build the game itself. OFF builds only the (GLM-only) unit tests, which needs
# no GLFW/X11/OpenGL headers — handy on headless servers and in quick CI jobs.
option(TORNADO_BUILD_GAME "Build the tornado game executable" ON)

# ── Detect Emscripten ─────────────────────────────────────────────────
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Emscripten")
    set(USING_EMSCRIPTEN TRUE)
else()
    set(USING_EMSCRIPTEN FALSE)
endif()

# ── Dependencies via FetchContent ─────────────────────────────────────
# Release tarballs pinned by SHA-256: a tampered or re-tagged upstream archive
# fails the download instead of silently ending up in the build.
include(FetchContent)

# glad 0.1.x still declares cmake_minimum_required(VERSION 3.0), which CMake 4
# refuses outright. Let third-party projects configure with 3.5 policies.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Extract with the archive timestamps replaced (silences CMP0135).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# GLM is always needed (header-only math library)
FetchContent_Declare(
    glm
    URL      https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
)
# Header-only use: don't build GLM's own test suite.
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_ENABLE_CXX_17 ON CACHE BOOL "" FORCE)

if(NOT USING_EMSCRIPTEN AND TORNADO_BUILD_GAME)
    # Native build: fetch GLFW and GLAD
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # X11 is enough everywhere (Wayland sessions run it through XWayland) and
    # avoids requiring wayland-scanner + protocol headers just to build.
    option(GLFW_BUILD_WAYLAND "Build GLFW with Wayland support" OFF)
    FetchContent_Declare(
        glfw
        URL      https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
        URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    )
    FetchContent_Declare(
        glad
        URL      https://github.com/Dav1dde/glad/archive/refs/tags/v0.1.36.tar.gz
        URL_HASH SHA256=8470ed1b0e9fbe88e10c34770505c8a1dc8ccb78cadcf673331aaf5224f963d2
    )
    FetchContent_MakeAvailable(glfw glad glm)
    find_package(OpenGL REQUIRED)
else()
    FetchContent_MakeAvailable(glm)
endif()

# ── Main executable ───────────────────────────────────────────────────
if(NOT TORNADO_BUILD_GAME)
    # Tests-only configuration (see option above).
elseif(USING_EMSCRIPTEN)
    add_executable(tornado src/main.cpp)
    # --- Emscripten / WebGL 2 build ---
    # Emit tornado.js/.wasm/.data only. The page is web/index.html, a plain
    # static file: emcc's --shell-file minifies it and forces an inline
    # {{{ SCRIPT }}} tag, while web/app.js loads tornado.js itself after
    # checking WebGL 2 support (so unsupported browsers get a real message).
    set_target_properties(tornado PROPERTIES SUFFIX ".js")

    target_compile_definitions(tornado PRIVATE "PLATFORM_EMSCRIPTEN")

    target_link_options(tornado PRIVATE
        "SHELL:-s USE_GLFW=3"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MIN_WEBGL_VERSION=2"
        "SHELL:-s MAX_WEBGL_VERSION=2"
        "SHELL:-s ENVIRONMENT=web"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/shaders@shaders"
        "SHELL:-s EXPORTED_FUNCTIONS=['_main','_touch_set_move','_touch_set_look','_touch_set_cursor','_set_quality','_toggle_sound','_set_paused','_restart_game','_get_score_points','_get_game_phase','_start_time_attack','_set_difficulty','_notify_user_gesture','_start_run','_enter_menu','_continue_endless','_set_reduced_motion','_get_run_stat','_set_language','_start_daily']"
    )

    # Force a relink when a preloaded shader changes — otherwise editing only
    # GLSL leaves a stale tornado.data behind.
    file(GLOB TORNADO_SHADERS "${CMAKE_SOURCE_DIR}/shaders/*.glsl")
    set_target_properties(tornado PROPERTIES LINK_DEPENDS "${TORNADO_SHADERS}")

    # Copy the static site (index.html, app.js, style.css, icons, ...) next
    # to the build output on every build, so build-wasm/ is servable as-is.
    add_custom_target(web_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/web ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Copying web/ into the build directory")

    target_link_libraries(tornado PRIVATE glm::glm)
else()
    # --- Native / desktop build ---
    add_executable(tornado src/main.cpp)
    target_include_directories(tornado PRIVATE ${glad_SOURCE_DIR}/include)
    target_compile_definitions(tornado PRIVATE "GLFW_INCLUDE_NONE")
    target_link_libraries(tornado PRIVATE glfw glad OpenGL::GL glm::glm)

    # Copy shaders + assets to build dir so the binary finds them
    add_custom_command(TARGET tornado POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:tornado>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/vertex.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/vertex.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/fragment.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/fragment.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/particle_vertex.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/particle_vertex.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/particle_fragment.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/particle_fragment.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/sky_vertex.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/sky_vertex.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/sky_fragment.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/sky_fragment.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/rain_vertex.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/rain_vertex.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/rain_fragment.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/rain_fragment.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/hud_vertex.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/hud_vertex.glsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/shaders/hud_fragment.glsl
            $<TARGET_FILE_DIR:tornado>/shaders/hud_fragment.glsl
    )
endif()

# ── Tests (GLM math — works on both platforms) ───────────────────────
add_executable(test_math src/test_math.cpp)
target_link_libraries(test_math PRIVATE glm::glm)

# Register test_math with CTest
enable_testing()
add_test(NAME test_math COMMAND test_math)

message(STATUS "Configured tornado_sim | Emscripten=${USING_EMSCRIPTEN}")
