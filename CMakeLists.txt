cmake_minimum_required(VERSION 3.18)
project(xeus-lix LANGUAGES CXX)

# modern CMake policies ensure consistent and predictable behavior.
cmake_policy(SET CMP0167 NEW) # suppress FindBoost module removal warning.
cmake_policy(SET CMP0148 NEW) # use modern Python discovery.

# provides standard installation directory variables (e.g., CMAKE_INSTALL_BINDIR).
include(GNUInstallDirs)

# set the C++ standard for the project.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# dependency discovery

# use PkgConfig to locate Lix and its individual component libraries.
find_package(PkgConfig REQUIRED)
pkg_search_module(LIX_MAIN REQUIRED lix-main)
pkg_search_module(LIX_EXPR REQUIRED lix-expr)
pkg_search_module(LIX_STORE REQUIRED lix-store)
pkg_search_module(LIX_UTIL REQUIRED lix-util)
pkg_search_module(LIX_CMD REQUIRED lix-cmd)
pkg_search_module(KJASYNC REQUIRED kj-async)

# find direct dependencies for Xeus and other utilities.
find_package(xeus REQUIRED)
find_package(xeus-zmq REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED)

# kernel executable target

# define the kernel executable and its source files.
add_executable(xlix
    src/main.cpp
    src/lix_interpreter.cpp
    src/lix_eval_helpers.cpp
    src/lix_repl_commands.cpp
    src/lix_logger.cpp
)

# explicitly state the C++ standard requirement for the target.
target_compile_features(xlix PRIVATE cxx_std_20)

# add all required include directories for the kernel.
target_include_directories(xlix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIX_MAIN_INCLUDE_DIRS}
    ${LIX_EXPR_INCLUDE_DIRS}
    ${LIX_STORE_INCLUDE_DIRS}
    ${LIX_UTIL_INCLUDE_DIRS}
    ${LIX_CMD_INCLUDE_DIRS}
    ${KJASYNC_INCLUDE_DIRS}
)

# link the kernel against all required libraries.
target_link_libraries(xlix PRIVATE
    xeus
    xeus-zmq
    nlohmann_json::nlohmann_json
    Boost::boost
    ${LIX_MAIN_LIBRARIES}
    ${LIX_EXPR_LIBRARIES}
    ${LIX_STORE_LIBRARIES}
    ${LIX_UTIL_LIBRARIES}
    ${LIX_CMD_LIBRARIES}
    ${KJASYNC_LIBRARIES}
)

# installation rules

# define the final, absolute path where the kernel executable will be installed.
# this is used to configure the kernel.json file correctly.
set(XLEX_EXECUTABLE "${CMAKE_INSTALL_FULL_BINDIR}/xlix")

# configure the kernel spec file (kernel.json) by substituting the executable path.
set(KERNEL_SPEC_DIR_BUILD "${CMAKE_BINARY_DIR}/share/jupyter/kernels/lix")
file(MAKE_DIRECTORY "${KERNEL_SPEC_DIR_BUILD}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/kernelspec/kernel.json.in"
    "${KERNEL_SPEC_DIR_BUILD}/kernel.json"
    @ONLY
)

# define the installation directory for the kernel spec.
set(KERNEL_SPEC_DIR_INSTALL "share/jupyter/kernels/lix")

# install the kernel executable to the standard binary directory (e.g., $out/bin).
install(TARGETS xlix RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# install the generated kernel.json and any other spec files to the correct location.
install(
    DIRECTORY "${KERNEL_SPEC_DIR_BUILD}/"
    DESTINATION "${KERNEL_SPEC_DIR_INSTALL}"
)
