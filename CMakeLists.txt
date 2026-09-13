cmake_minimum_required(VERSION 3.25)

project(STLInspector VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

find_package(Vulkan REQUIRED COMPONENTS glslc)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_impl_sdl2.cpp
    third_party/imgui/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_link_libraries(imgui PUBLIC SDL2::SDL2-static Vulkan::Vulkan)

add_library(vkbootstrap STATIC third_party/vkbootstrap/VkBootstrap.cpp)
target_include_directories(vkbootstrap PUBLIC third_party/vkbootstrap)
target_link_libraries(vkbootstrap PUBLIC Vulkan::Vulkan ${CMAKE_DL_LIBS})

add_library(stl_core STATIC
    src/stl.cpp
    src/platform.cpp
    src/browser.cpp
)
target_include_directories(stl_core PUBLIC src third_party/glm)
target_compile_definitions(stl_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE NOMINMAX WIN32_LEAN_AND_MEAN)

if(WIN32)
    target_link_libraries(stl_core PUBLIC ole32 shell32 uuid psapi)
endif()

add_executable(STLInspector
    src/main.cpp
    src/renderer.cpp
    src/application.cpp
)
target_include_directories(STLInspector PRIVATE third_party/vma)
target_compile_definitions(STLInspector PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(STLInspector PRIVATE stl_core imgui vkbootstrap)

if(MSVC)
    target_compile_options(stl_core PRIVATE /W4 /utf-8)
    target_compile_options(STLInspector PRIVATE /W4 /utf-8 /bigobj)
    target_link_options(STLInspector PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
    )
endif()

foreach(shader mesh.vert mesh.frag)
    set(output "${CMAKE_BINARY_DIR}/shaders/${shader}.spv")

    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
        COMMAND Vulkan::glslc --target-env=vulkan1.3 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}" -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        VERBATIM
    )

    list(APPEND SHADERS "${output}")
endforeach()

add_custom_target(inspector_shaders DEPENDS ${SHADERS})
add_dependencies(STLInspector inspector_shaders)

add_custom_command(TARGET STLInspector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "$<TARGET_FILE_DIR:STLInspector>/shaders"
)

include(CTest)

if(BUILD_TESTING)
    add_executable(stl_tests tests/stl_tests.cpp)
    target_link_libraries(stl_tests PRIVATE stl_core)
    add_test(NAME stl_parser COMMAND stl_tests)
endif()

install(TARGETS STLInspector RUNTIME DESTINATION .)
install(DIRECTORY "${CMAKE_BINARY_DIR}/shaders" DESTINATION .)
install(FILES README.md THIRD_PARTY_NOTICES.md VALIDATION.md DESTINATION .)
install(DIRECTORY licenses/ DESTINATION licenses)
