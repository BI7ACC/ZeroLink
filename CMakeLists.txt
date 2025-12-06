cmake_minimum_required(VERSION 3.10)
project(ZeroLink C)

set(CMAKE_C_STANDARD 17)

# HACK: 明确告知CMake忽略旧的、已清空的源文件，以处理可能的构建缓存不一致问题。
set_source_files_properties(client.c server.c PROPERTIES HEADER_FILE_ONLY TRUE)

# --- 查找和链接 pthreads ---
find_package(Threads REQUIRED)

# --- 使用 pkg-config 查找库 ---
find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "/usr/lib/pkgconfig:/usr/share/pkgconfig")

pkg_check_modules(SODIUM REQUIRED libsodium)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
find_package(Curses REQUIRED)

# --- 查找 cJSON (带手动回退机制) ---
pkg_check_modules(CJSON QUIET cjson)
if(NOT CJSON_FOUND)
    message(STATUS "pkg-config could not find cJSON. Trying manual search...")
    find_path(CJSON_INCLUDE_DIR
            NAMES cJSON.h
            PATH_SUFFIXES cjson
            PATHS /usr/include /usr/local/include
    )
    find_library(CJSON_LIBRARY
            NAMES cjson
            PATHS /usr/lib /usr/local/lib
    )
    if(CJSON_INCLUDE_DIR AND CJSON_LIBRARY)
        set(CJSON_FOUND TRUE)
        set(CJSON_INCLUDE_DIRS ${CJSON_INCLUDE_DIR})
        set(CJSON_LIBRARIES ${CJSON_LIBRARY})
        message(STATUS "Found cJSON manually: ${CJSON_LIBRARIES}")
    else()
        message(FATAL_ERROR "cJSON not found by pkg-config or manually. Please install libcjson-dev or equivalent.")
    endif()
endif()


# --- 定义头文件包含路径 ---
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/client
    ${SODIUM_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
)

# --- 创建服务器可执行文件 ---
add_executable(server
    server/server_main.c
    server/bootstrap/bootstrap_server.c
)
target_link_libraries(server PRIVATE Threads::Threads)

# --- 创建客户端可执行文件 ---
add_executable(client
    client/client_main.c
    client/ui/ui.c
    client/logic/client_logic.c
)

target_link_libraries(client PRIVATE
    Threads::Threads
    ${SODIUM_LIBRARIES}
    ${CURSES_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${CJSON_LIBRARIES}
    m # for math functions if needed
)

# --- 清理占位符文件 (修正版) ---
file(GLOB_RECURSE PLACEHOLDERS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*/.placeholder"
    "${CMAKE_CURRENT_SOURCE_DIR}/client/*/.placeholder"
    "${CMAKE_CURRENT_SOURCE_DIR}/server/*/.placeholder"
)

if(PLACEHOLDERS)
    set(STAMP_FILES "")
    foreach(placeholder_file ${PLACEHOLDERS})
        # 为每个占位符创建一个唯一的标记文件名，存放在构建目录下
        string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" rel_path ${placeholder_file})
        string(REPLACE "/" "_" safe_path ${rel_path})
        set(stamp_file "${CMAKE_CURRENT_BINARY_DIR}/placeholders/${safe_path}.stamp")

        # 定义一个自定义命令：删除占位符文件，然后创建标记文件
        add_custom_command(
            OUTPUT ${stamp_file}
            COMMAND ${CMAKE_COMMAND} -E remove ${placeholder_file}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/placeholders"
            COMMAND ${CMAKE_COMMAND} -E touch ${stamp_file}
            COMMENT "Removing placeholder: ${rel_path}"
            VERBATIM
        )
        list(APPEND STAMP_FILES ${stamp_file})
    endforeach()

    # 定义一个自定义目标，它依赖于所有标记文件的创建
    add_custom_target(remove_placeholders ALL DEPENDS ${STAMP_FILES})
endif()
