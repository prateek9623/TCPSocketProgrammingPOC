# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required (VERSION 3.8)

project ("TCPSocketProgrammingPOC")


find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)


file (GLOB COMMON_HEADERS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/Common/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/Common/*.hpp
)

add_executable (Server "Server/Server.cpp" ${COMMON_HEADERS})
target_include_directories(Server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Server spdlog::spdlog asio::asio)

add_executable (Client "Client/Client.cpp" ${COMMON_HEADERS})
target_include_directories(Client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Client spdlog::spdlog asio::asio)

