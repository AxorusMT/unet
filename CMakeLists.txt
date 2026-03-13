cmake_minimum_required(VERSION 3.23)
project(unetcode VERSION 0.1.0 LANGUAGES CXX)

option(UNET_BUILD_EXAMPLES "Build unetcode examples" ON)
option(UNET_BUILD_TESTS "Build unetcode tests" ON)
option(UNET_WARNINGS_AS_ERRORS "Treat warnings as errors for unetcode targets" OFF)
option(UNET_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers in Debug where supported" OFF)

add_library(unetcode STATIC
    src/address.cpp
    src/host.cpp
    src/services.cpp
    src/detail/platform.cpp
    src/detail/socket.cpp
)

target_include_directories(unetcode
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(unetcode PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(unetcode PRIVATE /W4 /permissive-)
    if(UNET_WARNINGS_AS_ERRORS)
        target_compile_options(unetcode PRIVATE /WX)
    endif()
else()
    target_compile_options(unetcode PRIVATE -Wall -Wextra -Wpedantic)
    if(UNET_WARNINGS_AS_ERRORS)
        target_compile_options(unetcode PRIVATE -Werror)
    endif()
endif()

if(WIN32)
    target_link_libraries(unetcode PRIVATE ws2_32)
endif()

if(UNET_ENABLE_SANITIZERS)
    if(MSVC)
        target_compile_options(unetcode PRIVATE $<$<CONFIG:Debug>:/fsanitize=address>)
        add_compile_options($<$<CONFIG:Debug>:/fsanitize=address>)
        get_filename_component(UNET_MSVC_ASAN_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
        add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
    endif()
endif()

if(UNET_BUILD_EXAMPLES)
    add_executable(unetcode_chat_server examples/chat_server.cpp)
    target_link_libraries(unetcode_chat_server PRIVATE unetcode)

    add_executable(unetcode_chat_client examples/chat_client.cpp)
    target_link_libraries(unetcode_chat_client PRIVATE unetcode)

    add_executable(unetcode_p2p_file_node examples/p2p_file_node.cpp)
    target_link_libraries(unetcode_p2p_file_node PRIVATE unetcode)
endif()

if(UNET_BUILD_TESTS)
    enable_testing()
    add_executable(unetcode_smoke_test tests/smoke_test.cpp)
    target_link_libraries(unetcode_smoke_test PRIVATE unetcode)
    add_test(NAME unetcode_smoke_test COMMAND unetcode_smoke_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unetcode_smoke_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unetcode_file_transfer_test tests/file_transfer_test.cpp)
    target_link_libraries(unetcode_file_transfer_test PRIVATE unetcode)
    add_test(NAME unetcode_file_transfer_test COMMAND unetcode_file_transfer_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unetcode_file_transfer_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()

    add_executable(unetcode_services_test tests/services_test.cpp)
    target_link_libraries(unetcode_services_test PRIVATE unetcode)
    add_test(NAME unetcode_services_test COMMAND unetcode_services_test)
    if(UNET_ENABLE_SANITIZERS AND MSVC)
        set_tests_properties(unetcode_services_test PROPERTIES
            ENVIRONMENT "PATH=${UNET_MSVC_ASAN_RUNTIME_DIR};$ENV{PATH}")
    endif()
endif()

install(TARGETS unetcode
    EXPORT unetcodeTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
