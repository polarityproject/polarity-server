# polarity-keeper on its own (the server builds it on Linux; keeper.h). The mod's
# CMakeLists.txt builds it on Windows for local tests. This file works from
# the mod repository (cmake -S src/keeper) and from the bundle
# scripts/keeper-bundle.ps1 writes into polarity-server (same layout: the
# sources two levels up).
cmake_minimum_required(VERSION 3.20)
project(polarity_keeper CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
get_filename_component(POLARITY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

include(FetchContent)
# The same Weyvelength commit and client patches as the mod (and as the
# server's docker/server.Dockerfile WEYVELENGTH_COMMIT).
FetchContent_Declare(weyvelength
  GIT_REPOSITORY https://github.com/HeatXD/Weyvelength.git
  GIT_TAG 4d71aef28b9a59721a98198ab5b81c045cf26930
  PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${POLARITY_ROOT}/third_party/weyvelength.patch -P ${POLARITY_ROOT}/cmake/apply-patch.cmake
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(weyvelength)
execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${POLARITY_ROOT}/third_party/weyvelength-departed-links.patch
                        -P ${POLARITY_ROOT}/cmake/apply-patch.cmake
                WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE departed_links)
if(NOT departed_links EQUAL 0)
  message(FATAL_ERROR "cannot apply third_party/weyvelength-departed-links.patch")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${POLARITY_ROOT}/third_party/weyvelength-port-range.patch
                        -P ${POLARITY_ROOT}/cmake/apply-patch.cmake
                WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE port_range)
if(NOT port_range EQUAL 0)
  message(FATAL_ERROR "cannot apply third_party/weyvelength-port-range.patch")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH=${POLARITY_ROOT}/third_party/weyvelength-link-mux.patch
                        -P ${POLARITY_ROOT}/cmake/apply-patch.cmake
                WORKING_DIRECTORY ${weyvelength_SOURCE_DIR} RESULT_VARIABLE link_mux)
if(NOT link_mux EQUAL 0)
  message(FATAL_ERROR "cannot apply third_party/weyvelength-link-mux.patch")
endif()

set(NO_SERVER ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(${weyvelength_SOURCE_DIR}/Client/thirdparty/libjuice ${CMAKE_BINARY_DIR}/libjuice EXCLUDE_FROM_ALL)
set(WEYVE_SHARED_INCLUDES ${weyvelength_SOURCE_DIR}/Shared ${weyvelength_SOURCE_DIR}/Shared/thirdparty/asio/include)
add_library(weyvelength_client STATIC
  ${weyvelength_SOURCE_DIR}/Client/src/client.cpp
  ${weyvelength_SOURCE_DIR}/Client/src/p2p_mesh.cpp
  ${weyvelength_SOURCE_DIR}/Client/src/weyvelength.cpp)
target_include_directories(weyvelength_client
  PUBLIC ${weyvelength_SOURCE_DIR}/Client/include
  PRIVATE ${weyvelength_SOURCE_DIR}/Client/include/private ${WEYVE_SHARED_INCLUDES})
find_package(Threads REQUIRED)
target_link_libraries(weyvelength_client PRIVATE juice-static Threads::Threads)

find_package(OpenSSL 3 REQUIRED)
add_executable(polarity-keeper
  ${POLARITY_ROOT}/src/keeper/main.cpp
  ${POLARITY_ROOT}/src/keeper/keeper.cpp
  ${POLARITY_ROOT}/src/keeper/keeper_listing.cpp
  ${POLARITY_ROOT}/src/keeper/keeper_relay.cpp
  ${POLARITY_ROOT}/src/keeper/keeper_rooms.cpp
  ${POLARITY_ROOT}/src/keeper/keeper_tables.cpp
  ${POLARITY_ROOT}/src/keeper/sign.cpp
  ${POLARITY_ROOT}/src/core/keeper_protocol.cpp
  ${POLARITY_ROOT}/src/core/keeper_words.cpp
  ${POLARITY_ROOT}/src/core/lobby.cpp
  ${POLARITY_ROOT}/src/core/spectate_relay.cpp
  ${POLARITY_ROOT}/src/core/rules.cpp)
target_include_directories(polarity-keeper PRIVATE ${POLARITY_ROOT}/src)
target_link_libraries(polarity-keeper PRIVATE weyvelength_client OpenSSL::Crypto Threads::Threads)
install(TARGETS polarity-keeper RUNTIME DESTINATION bin)
