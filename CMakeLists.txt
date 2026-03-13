cmake_minimum_required(VERSION 3.23)
project(unet VERSION 0.1.0 LANGUAGES CXX)

option(UNET_BUILD_EXAMPLES "Build unet examples" ON)
option(UNET_BUILD_TESTS "Build unet tests" ON)
option(UNET_WARNINGS_AS_ERRORS "Treat warnings as errors for unet targets" OFF)
option(UNET_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers in Debug where supported" OFF)

add_library(unet STATIC
    src/address.cpp
    src/host.cpp
    src/services.cpp
    src/detail/platform.cpp
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
    target_link_libraries(unet_chat_server PRIVATE unet)

    add_executable(unet_chat_client examples/chat_client.cpp)
    target_link_libraries(unet_chat_client PRIVATE unet)

    add_executable(unet_p2p_file_node examples/p2p_file_node.cpp)
    target_link_libraries(unet_p2p_file_node PRIVATE unet)

    add_executable(unet_tcp_chat_server examples/tcp_chat_server.cpp)
    target_link_libraries(unet_tcp_chat_server PRIVATE unet)

    add_executable(unet_tcp_chat_client examples/tcp_chat_client.cpp)
    target_link_libraries(unet_tcp_chat_client PRIVATE unet)

    add_executable(unet_upnp_server examples/upnp_server.cpp)
    target_link_libraries(unet_upnp_server PRIVATE unet)
endif()

if(UNET_BUILD_TESTS)
    enable_testing()
    add_executable(unet_smoke_test tests/smoke_test.cpp)
    target_link_libraries(unet_smoke_test PRIVATE unet)
    add_test(NAME unet_smoke_test COMMAND unet_smoke_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_smoke_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_file_transfer_test tests/file_transfer_test.cpp)
    target_link_libraries(unet_file_transfer_test PRIVATE unet)
    add_test(NAME unet_file_transfer_test COMMAND unet_file_transfer_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_file_transfer_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_services_test tests/services_test.cpp)
    target_link_libraries(unet_services_test PRIVATE unet)
    add_test(NAME unet_services_test COMMAND unet_services_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_services_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_tcp_test tests/tcp_test.cpp)
    target_link_libraries(unet_tcp_test PRIVATE unet)
    add_test(NAME unet_tcp_test COMMAND unet_tcp_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unet_tcp_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unet_upnp_test tests/upnp_test.cpp)
    target_link_libraries(unet_upnp_test PRIVATE unet)
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
