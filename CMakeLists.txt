cmake_minimum_required(VERSION 3.20)
project(ColorizingDMDAIAssist VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The editor API (ColorizingDMD's plugin-api) and the VPX plugin SDK it builds on
# (vpinball's plugins/plugins), as the editor is built with.
set(COLORIZINGDMD_DIR "" CACHE PATH "ColorizingDMD sources")
set(VPINBALL_DIR "" CACHE PATH "vpinball sources, for the plugin SDK")
# Where a development editor looks for plugins (its build's vpx-plugins folder): the plugin is
# copied there after each build when set.
set(COLORIZINGDMD_PLUGIN_DIR "" CACHE PATH "Editor plugin folder to copy the plugin into")

if(NOT EXISTS "${COLORIZINGDMD_DIR}/plugin-api/ColorizingDMDPlugin.h")
    message(FATAL_ERROR "Set COLORIZINGDMD_DIR to the ColorizingDMD sources.")
endif()
if(NOT EXISTS "${VPINBALL_DIR}/plugins/plugins/MsgPlugin.h")
    message(FATAL_ERROR "Set VPINBALL_DIR to the vpinball sources (plugins/plugins).")
endif()

find_package(Threads REQUIRED)

add_library(AIAssistPlugin MODULE
    src/Editor.h
    src/McpServer.cpp
    src/McpServer.h
    src/Plugin.cpp
    src/Png.cpp
    src/Png.h
    src/Tools.cpp
    src/Tools.h
)
target_include_directories(AIAssistPlugin PRIVATE src)
target_include_directories(AIAssistPlugin SYSTEM PRIVATE
    "${COLORIZINGDMD_DIR}/plugin-api"
    "${VPINBALL_DIR}/plugins"
    "${PROJECT_SOURCE_DIR}/third-party"
    "${PROJECT_SOURCE_DIR}/third-party/httplib"
)
target_link_libraries(AIAssistPlugin PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(AIAssistPlugin PRIVATE ws2_32)
    # Windows 10 and later (httplib needs Vista or later).
    target_compile_definitions(AIAssistPlugin PRIVATE _WIN32_WINNT=0x0A00)
endif()
if(MSVC)
    target_compile_options(AIAssistPlugin PRIVATE /W4 /utf-8)
else()
    target_compile_options(AIAssistPlugin PRIVATE -Wall -Wextra)
endif()

# The file names plugin.cfg gives.
set_target_properties(AIAssistPlugin PROPERTIES PREFIX "" OUTPUT_NAME plugin-aiassist)
if(WIN32)
    set_target_properties(AIAssistPlugin PROPERTIES OUTPUT_NAME plugin-aiassist64)
elseif(APPLE)
    set_target_properties(AIAssistPlugin PROPERTIES SUFFIX ".dylib")
endif()

# The plugin's folder, as the editor's plugin folder holds it: AIAssist/plugin.cfg and the
# library.
set(plugin_folder "${CMAKE_BINARY_DIR}/AIAssist")
add_custom_command(TARGET AIAssistPlugin POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${plugin_folder}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/plugin.cfg" "${plugin_folder}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:AIAssistPlugin>" "${plugin_folder}/"
    VERBATIM
)
if(COLORIZINGDMD_PLUGIN_DIR)
    add_custom_command(TARGET AIAssistPlugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${plugin_folder}"
                "${COLORIZINGDMD_PLUGIN_DIR}/AIAssist"
        VERBATIM
    )
endif()
