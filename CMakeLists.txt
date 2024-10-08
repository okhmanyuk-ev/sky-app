cmake_minimum_required(VERSION 3.10)
project(sky-app)

set(PRODUCT_NAME "${PROJECT_NAME}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBS_FOLDER "libs")

# main

if(BUILD_DEVELOPER)
	add_definitions(-DBUILD_DEVELOPER)
endif()

add_definitions(-DPROJECT_NAME="${PROJECT_NAME}")
add_definitions(-DPRODUCT_NAME="${PRODUCT_NAME}")

file(GLOB_RECURSE ALL_SRC
	src/*.cpp
	src/*.hpp
	src/*.h
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ALL_SRC})

if(WIN32)
    add_definitions(-DNOMINMAX)

    add_executable(${PROJECT_NAME}
        ${ALL_SRC}
    )

	set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
    set_target_properties(${PROJECT_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
elseif(EMSCRIPTEN)
	set(ASSETS_FOLDER_NAME assets)
	set(ASSETS_FOLDER_PATH ${CMAKE_SOURCE_DIR}/${ASSETS_FOLDER_NAME})

	add_executable(${PROJECT_NAME} ${ALL_SRC} ${ASSETS_FOLDER_PATH})

	set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "index")
	set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/output)
	set(CMAKE_EXECUTABLE_SUFFIX .html)

	target_link_options(${PROJECT_NAME} PUBLIC --preload-file ${ASSETS_FOLDER_PATH}@${ASSETS_FOLDER_NAME})
endif()

target_include_directories(${PROJECT_NAME} PUBLIC src)

# lua

file(GLOB LUA_SRC
	lib/lua/src/*.c
	lib/lua/src/*.h
)

target_include_directories(${PROJECT_NAME} PUBLIC lib/lua/src)
add_library(lua STATIC ${LUA_SRC})
target_link_libraries(${PROJECT_NAME} lua)
set_property(TARGET lua PROPERTY FOLDER ${LIBS_FOLDER})

# sol

target_include_directories(${PROJECT_NAME} PUBLIC lib/sol)

# curl

if (NOT EMSCRIPTEN)
	set(BUILD_CURL_EXE OFF)
	set(BUILD_SHARED_LIBS OFF)
	set(CURL_DISABLE_INSTALL ON)
	set(SHARE_LIB_OBJECT OFF)
	add_subdirectory(lib/curl)
	target_link_libraries(${PROJECT_NAME} CURL::libcurl)
	set_property(TARGET libcurl_static PROPERTY FOLDER ${LIBS_FOLDER})
endif()

# sky

add_subdirectory(sky)
target_link_libraries(${PROJECT_NAME} sky)
copy_required_libs(${PROJECT_NAME})
