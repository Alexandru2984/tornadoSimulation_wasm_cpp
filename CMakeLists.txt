cmake_minimum_required(VERSION 3.14)
project(tornado_sim VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ─────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# ── Detect Emscripten ─────────────────────────────────────────────────
if("${CMAKE_SYSTEM_NAME}" STREQUAL "Emscripten")
    set(USING_EMSCRIPTEN TRUE)
else()
    set(USING_EMSCRIPTEN FALSE)
endif()

# ── Dependencies via FetchContent ─────────────────────────────────────
include(FetchContent)

# GLM is always needed (header-only math library)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        0.9.9.8
)

if(NOT USING_EMSCRIPTEN)
    # Native build: fetch GLFW and GLAD
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.3.8
    )
    FetchContent_Declare(
        glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG        v0.1.36
    )
    FetchContent_MakeAvailable(glfw glad glm)
    find_package(OpenGL REQUIRED)
else()
    FetchContent_MakeAvailable(glm)
endif()

# ── Main executable ───────────────────────────────────────────────────
add_executable(tornado src/main.cpp src/tinygltf_impl.cpp)

target_include_directories(tornado PRIVATE ${CMAKE_SOURCE_DIR}/vendor)

if(USING_EMSCRIPTEN)
    # --- Emscripten / WebGL 2 build ---
    set_target_properties(tornado PROPERTIES SUFFIX ".html")

    target_compile_definitions(tornado PRIVATE "PLATFORM_EMSCRIPTEN")

    target_link_options(tornado PRIVATE
        "SHELL:-s USE_GLFW=3"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MIN_WEBGL_VERSION=2"
        "SHELL:-s MAX_WEBGL_VERSION=2"
        "SHELL:-s ENVIRONMENT=web"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets@assets"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/shaders@shaders"
        "SHELL:-s EXPORTED_FUNCTIONS=['_main','_touch_set_move','_touch_set_look','_touch_set_cursor','_set_quality','_toggle_sound','_set_paused','_restart_game','_get_score_points']"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    )

    # Use custom HTML shell if it exists
    if(EXISTS "${CMAKE_SOURCE_DIR}/web/shell.html")
        target_link_options(tornado PRIVATE
            "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html"
        )
    endif()

    target_link_libraries(tornado PRIVATE glm::glm)
else()
    # --- Native / desktop build ---
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
    add_custom_command(TARGET tornado POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:tornado>/assets
    )
endif()

# ── Tests (GLM math — works on both platforms) ───────────────────────
add_executable(test_math src/test_math.cpp)
target_link_libraries(test_math PRIVATE glm::glm)

# Register test_math with CTest
enable_testing()
add_test(NAME test_math COMMAND test_math)

message(STATUS "Configured tornado_sim | Emscripten=${USING_EMSCRIPTEN}")
