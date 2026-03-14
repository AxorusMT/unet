cmake_minimum_required(VERSION 3.23)
project(unet VERSION 0.1.0 LANGUAGES CXX)

option(UNET_BUILD_EXAMPLES "Build unet examples" ON)
option(UNET_BUILD_TESTS "Build unet tests" ON)
option(UNET_WARNINGS_AS_ERRORS "Treat warnings as errors for unet targets" OFF)
option(UNET_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers in Debug where supported" OFF)
option(UNET_ENABLE_NATIVE_QUIC "Enable native QUIC backend via MsQuic" ON)

add_library(unet STATIC
    src/address.cpp
    src/host.cpp
    src/http3.cpp
    src/services.cpp
    src/detail/platform.cpp
    src/detail/quic_transport.cpp
    src/detail/socket.cpp
    src/detail/upnp.cpp
)

target_include_directories(unet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(unet PUBLIC cxx_std_23)

set(UNET_HAS_MSQUIC FALSE)
if(WIN32 AND UNET_ENABLE_NATIVE_QUIC)
    include(FetchContent)
    set(UNET_MSQUIC_ROOT "")
    set(UNET_MSQUIC_URL "https://www.nuget.org/api/v2/package/Microsoft.Native.Quic.MsQuic.Schannel")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(UNET_MSQUIC_ARCH "x64")
    else()
        set(UNET_MSQUIC_ARCH "x86")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(UNET_MSQUIC_ARCH "arm64")
    endif()

    FetchContent_Declare(
        unet_msquic_pkg
        URL "${UNET_MSQUIC_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(unet_msquic_pkg)
    set(UNET_MSQUIC_ROOT "${unet_msquic_pkg_SOURCE_DIR}")

    set(UNET_MSQUIC_INCLUDE_DIR "${UNET_MSQUIC_ROOT}/build/native/include")
    set(UNET_MSQUIC_LIB "${UNET_MSQUIC_ROOT}/build/native/lib/${UNET_MSQUIC_ARCH}/msquic.lib")
    set(UNET_MSQUIC_DLL "${UNET_MSQUIC_ROOT}/build/native/bin/${UNET_MSQUIC_ARCH}/msquic.dll")

    if(EXISTS "${UNET_MSQUIC_INCLUDE_DIR}/msquic.h" AND EXISTS "${UNET_MSQUIC_LIB}" AND EXISTS "${UNET_MSQUIC_DLL}")
        target_include_directories(unet PRIVATE "${UNET_MSQUIC_INCLUDE_DIR}")
        target_link_libraries(unet PRIVATE "${UNET_MSQUIC_LIB}")
        target_link_libraries(unet PRIVATE ntdll)
        target_compile_definitions(unet PRIVATE UNET_HAS_MSQUIC=1)
        set(UNET_HAS_MSQUIC TRUE)
        message(STATUS "Native QUIC enabled via MsQuic (${UNET_MSQUIC_ARCH})")
    else()
        message(FATAL_ERROR "MsQuic package is missing required files for architecture ${UNET_MSQUIC_ARCH}")
    endif()
endif()

if(MSVC)
    target_compile_options(unet PRIVATE /W4 /permissive-)
    if(UNET_WARNINGS_AS_ERRORS)
        target_compile_options(unet PRIVATE /WX)
    endif()
else()
    target_compile_options(unet PRIVATE -Wall -Wextra -Wpedantic)
    if(UNET_WARNINGS_AS_ERRORS)
        target_compile_options(unet PRIVATE -Werror)
    endif()
endif()

if(WIN32)
    target_link_libraries(unet PRIVATE ws2_32)
endif()

function(unet_link_target target_name)
    target_link_libraries(${target_name} PRIVATE unet)
    if(UNET_HAS_MSQUIC)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UNET_MSQUIC_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>")
    endif()
endfunction()

if(UNET_ENABLE_SANITIZERS)
    if(MSVC)
        target_compile_options(unet PRIVATE $<$<CONFIG:Debug>:/fsanitize=address>)
        add_compile_options($<$<CONFIG:Debug>:/fsanitize=address>)
        get_filename_component(UNET_MSVC_ASAN_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
        add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
    endif()
endif()

if(UNET_BUILD_EXAMPLES)
    add_executable(unet_chat_server examples/chat_server.cpp)
    unet_link_target(unet_chat_server)

    add_executable(unet_chat_client examples/chat_client.cpp)
    unet_link_target(unet_chat_client)

    add_executable(unet_p2p_file_node examples/p2p_file_node.cpp)
    unet_link_target(unet_p2p_file_node)

    add_executable(unet_tcp_chat_server examples/tcp_chat_server.cpp)
    unet_link_target(unet_tcp_chat_server)

    add_executable(unet_tcp_chat_client examples/tcp_chat_client.cpp)
    unet_link_target(unet_tcp_chat_client)

    add_executable(unet_upnp_server examples/upnp_server.cpp)
    unet_link_target(unet_upnp_server)
endif()

if(UNET_BUILD_TESTS)
    enable_testing()
    add_executable(unet_smoke_test tests/smoke_test.cpp)
    unet_link_target(unet_smoke_test)
    add_test(NAME unet_smoke_test COMMAND unet_smoke_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_smoke_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_file_transfer_test tests/file_transfer_test.cpp)
    unet_link_target(unet_file_transfer_test)
    add_test(NAME unet_file_transfer_test COMMAND unet_file_transfer_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_file_transfer_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_services_test tests/services_test.cpp)
    unet_link_target(unet_services_test)
    add_test(NAME unet_services_test COMMAND unet_services_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_services_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_tcp_test tests/tcp_test.cpp)
    unet_link_target(unet_tcp_test)
    add_test(NAME unet_tcp_test COMMAND unet_tcp_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_tcp_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_quic_test tests/quic_test.cpp)
    unet_link_target(unet_quic_test)
    add_test(NAME unet_quic_test COMMAND unet_quic_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_quic_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_http3_test tests/http3_test.cpp)
    unet_link_target(unet_http3_test)
    add_test(NAME unet_http3_test COMMAND unet_http3_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_http3_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_upnp_test tests/upnp_test.cpp)
    unet_link_target(unet_upnp_test)
    add_test(NAME unet_upnp_test COMMAND unet_upnp_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_upnp_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()
endif()

install(TARGETS unet
    EXPORT unetTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
