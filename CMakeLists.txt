cmake_minimum_required(VERSION 3.27)

set(PROJECT_NAME "symboli")
project(${PROJECT_NAME})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

set(CMAKE_C_STANDARD 99)

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wall -g -O0 -pedantic")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")

if(WIN32)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -Wl,--subsystem,windows")
endif()

set(VENDOR_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor/)

# ==============================================================================
set(SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/source/)
set(INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include/)

set(SOURCES
    ${SOURCE_DIR}main.c
    ${SOURCE_DIR}types.c
    ${SOURCE_DIR}compiler.c
    ${SOURCE_DIR}vm.c
    ${SOURCE_DIR}parser.c
    ${SOURCE_DIR}builtins.c
)

include_directories(${INCLUDE_DIR})

# ==============================================================================
if(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_executable(${PROJECT_NAME} ${SOURCES})
    else()
        add_executable(${PROJECT_NAME} WIN32 ${SOURCES})
    endif()
else()
    add_executable(${PROJECT_NAME} ${SOURCES})
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME} PRIVATE DEBUG=1)
else()
endif()

target_include_directories(${PROJECT_NAME}
    PUBLIC
    ${INCLUDE_DIR}
)

# target_link_libraries(${PROJECT_NAME}
#     PRIVATE
# )

