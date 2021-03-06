include_guard(DIRECTORY)

include(GNUInstallDirs)
include(FetchContent)
include(GitHeadSHA)

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../../config/version.txt GALOIS_VERSION)
string(REGEX REPLACE "[ \t\n]" "" GALOIS_VERSION ${GALOIS_VERSION})
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+)" "\\1" GALOIS_VERSION_MAJOR ${GALOIS_VERSION})
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+)" "\\2" GALOIS_VERSION_MINOR ${GALOIS_VERSION})
string(REGEX REPLACE "([0-9]+)\\.([0-9]+)\\.([0-9]+)" "\\3" GALOIS_VERSION_PATCH ${GALOIS_VERSION})
set(GALOIS_COPYRIGHT_YEAR "2018") # Also in COPYRIGHT
set(GALOIS_GIT_SHA "${GIT_HEAD_SHA}")

if (NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type selected, default to Release")
  # cmake default flags with relwithdebinfo is -O2 -g
  # cmake default flags with release is -O3 -DNDEBUG
  set(CMAKE_BUILD_TYPE "Release")
endif ()

###### Options (alternatively pass as options to cmake -DName=Value) ######
###### General features ######
set(GALOIS_ENABLE_PAPI OFF CACHE BOOL "Use PAPI counters for profiling")
set(GALOIS_ENABLE_VTUNE OFF CACHE BOOL "Use VTune for profiling")
set(GALOIS_STRICT_CONFIG OFF CACHE BOOL "Instead of falling back gracefully, fail")
set(GALOIS_GRAPH_LOCATION "" CACHE PATH "Location of inputs for tests if downloaded/stored separately.")
set(CXX_CLANG_TIDY "" CACHE STRING "Semi-colon separated list of clang-tidy command and arguments")
set(CMAKE_CXX_COMPILER_LAUNCHER "" CACHE STRING "Semi-colon separated list of command and arguments to wrap compiler invocations (e.g., ccache)")
set(GALOIS_USE_ARCH "sandybridge" CACHE STRING "Semi-colon separated list of processor architectures to atttempt to optimize for; use the first valid configuration ('none' to disable)")
set(GALOIS_USE_SANITIZER "" CACHE STRING "Semi-colon separated list of sanitizers to use (Memory, MemoryWithOrigins, Address, Undefined, Thread)")
# This option is automatically handled by CMake.
# It makes add_library build a shared lib unless STATIC is explicitly specified.
# Putting this here is mostly just a placeholder so people know it's an option.
# Currently this is really only intended to change anything for the libgalois_shmem target.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")
# This option is added by include(CTest). We define it here to let people know
# that this is a standard option.
set(BUILD_TESTING ON CACHE BOOL "Build tests")
# Set here to override the cmake default of "/usr/local" because
# "/usr/local/lib" is not a default search location for ld.so
set(CMAKE_INSTALL_PREFIX "/usr" CACHE STRING "install prefix")

###### Developer features ######
set(GALOIS_PER_ROUND_STATS OFF CACHE BOOL "Report statistics of each round of execution")
set(GALOIS_NUM_TEST_GPUS "" CACHE STRING "Number of test GPUs to use (on a single machine) for running the tests.")
set(GALOIS_USE_LCI OFF CACHE BOOL "Use LCI network runtime instead of MPI")
set(GALOIS_NUM_TEST_THREADS "" CACHE STRING "Maximum number of threads to use when running tests (default: number of physical core)")
set(GALOIS_AUTO_CONAN OFF CACHE BOOL "Automatically call conan from cmake rather than manually (experimental)")
# GALOIS_FORCE_NON_STATIC is a transitional flag intended to turn symbol export
# errors into linker errors while the codebase transitions to hidden visibility
# by default.
set(GALOIS_FORCE_NON_STATIC OFF CACHE BOOL "Allow libraries intended to be used statically to be built as shared if BUILD_SHARED_LIBS=ON")
mark_as_advanced(GALOIS_FORCE_NON_STATIC)

if (NOT GALOIS_NUM_TEST_THREADS)
  cmake_host_system_information(RESULT GALOIS_NUM_TEST_THREADS QUERY NUMBER_OF_PHYSICAL_CORES)
endif ()
if (GALOIS_NUM_TEST_THREADS LESS_EQUAL 0)
  set(GALOIS_NUM_TEST_THREADS 1)
endif ()

if (NOT GALOIS_NUM_TEST_GPUS)
  if (GALOIS_ENABLE_GPU)
    set(GALOIS_NUM_TEST_GPUS 1)
  else ()
    set(GALOIS_NUM_TEST_GPUS 0)
  endif ()
endif ()

###### Configure (users don't need to go beyond here) ######

# Enable KATANA_IS_MAIN_PROJECT if this file is included in the root project.
# KATANA_IS_MAIN_PROJECT is enabled for Katana library builds and disabled if
# Katana is included as a sub-project of another build.
if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(KATANA_IS_MAIN_PROJECT ON)
else ()
  set(KATANA_IS_MAIN_PROJECT OFF)
endif ()

if (KATANA_IS_MAIN_PROJECT)
  include(CTest)
endif ()

###### Install dependencies ######

find_package(PkgConfig REQUIRED)

if (GALOIS_AUTO_CONAN)
  include(${CMAKE_CURRENT_LIST_DIR}/conan.cmake)
  # config/conanfile.py is relative to the current project, so it will be either enterprise or open depending on who
  # includes us.
  conan_cmake_run(CONANFILE config/conanfile.py
      BASIC_SETUP
      CMAKE_TARGETS
      NO_OUTPUT_DIRS
      BUILD missing)
  include("${CMAKE_CURRENT_BINARY_DIR}/conan_paths.cmake")
endif ()


###### Configure compiler ######

# generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) #...without compiler extensions like gnu++11
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hidden symbols break MacOS
if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif ()

# Always include debug info
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-g>")

# GCC
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 7)
    message(FATAL_ERROR "gcc must be version 7 or higher. Found ${CMAKE_CXX_COMPILER_VERSION}.")
  endif ()

  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>")

  if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 9)
    # Avoid warnings from openmpi
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-cast-function-type>")
    # Avoid warnings from boost::counting_iterator (1.71.0)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-copy>")
  endif ()

  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Werror>")
  endif ()
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 7)
    message(FATAL_ERROR "clang must be version 7 or higher. Found ${CMAKE_CXX_COMPILER_VERSION}.")
  endif ()

  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>")

  if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 10)
    # Avoid warnings from boost::counting_iterator (1.71.0)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-copy>")
  endif ()

  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Werror>")
  endif ()
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>")

  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Werror>")
  endif ()
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.0.1)
    message(FATAL_ERROR "icpc must be 19.0.1 or higher. Found ${CMAKE_CXX_COMPILER_VERSION}.")
  endif ()

  # Avoid warnings when using noinline for methods defined inside class defintion.
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-wd2196>")
endif ()

# Enable architecture-specific optimizations
include(CheckArchFlags)
if (ARCH_FLAGS_FOUND)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${ARCH_CXX_FLAGS}>")
  add_compile_options("$<$<COMPILE_LANGUAGE:C>:${ARCH_C_FLAGS}>")
  add_link_options(${ARCH_LINK_FLAGS})
endif ()

if (CXX_CLANG_TIDY)
  set(CMAKE_CXX_CLANG_TIDY ${CXX_CLANG_TIDY} "-header-filter=.*${PROJECT_SOURCE_DIR}.*")
  # Ignore warning flags intended for the CXX program. This only works because
  # the two compilers we care about, clang and gcc, both understand
  # -Wno-unknown-warning-option.
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-warning-option>")
endif ()

###### Configure features ######

if (GALOIS_ENABLE_VTUNE)
  find_package(VTune REQUIRED PATHS /opt/intel/vtune_amplifier)
  include_directories(${VTune_INCLUDE_DIRS})
  add_definitions(-DGALOIS_ENABLE_VTUNE)
endif ()

if (GALOIS_ENABLE_PAPI)
  find_package(PAPI REQUIRED)
  include_directories(${PAPI_INCLUDE_DIRS})
  add_definitions(-DGALOIS_ENABLE_PAPI)
endif ()

find_package(NUMA)

find_package(Threads REQUIRED)

include(CheckMmap)

include(CheckHugePages)
if (NOT HAVE_HUGEPAGES AND GALOIS_STRICT_CONFIG)
  message(FATAL_ERROR "Need huge pages")
endif ()

find_package(Boost 1.58.0 REQUIRED COMPONENTS filesystem serialization iostreams)

find_package(mongoc-1.0 1.6)
if (NOT mongoc-1.0_FOUND)
  message(STATUS "Library mongoc not found, not building MongoDB support")
endif ()

find_package(MySQL 8.0)
if (NOT MySQL_FOUND)
  message(STATUS "Library MySQL not found, not building MySQL support")
endif ()

foreach (llvm_step RANGE 2)
  # Range is [0, end]. Start version search from highest compatible version
  # first.
  math(EXPR llvm_ver "11 - ${llvm_step}")
  find_package(LLVM ${llvm_ver} QUIET CONFIG)
  if (LLVM_FOUND)
    break()
  endif ()
endforeach ()
if (LLVM_FOUND)
  message(STATUS "Library llvm found ${LLVM_DIR}")
else ()
  message(FATAL_ERROR "Searched for LLVM 7 through 11 but did not find any compatible version")
endif ()
if (NOT DEFINED LLVM_ENABLE_RTTI)
  message(FATAL_ERROR "Could not determine if LLVM has RTTI enabled.")
endif ()
if (NOT ${LLVM_ENABLE_RTTI})
  message(FATAL_ERROR "Galois requires a build of LLVM that includes RTTI."
      "Most package managers do this already, but if you built LLVM"
      "from source you need to configure it with `-DLLVM_ENABLE_RTTI=ON`")
endif ()
target_include_directories(LLVMSupport INTERFACE ${LLVM_INCLUDE_DIRS})

include(HandleSanitizer)

include(CheckEndian)

# Testing-only dependencies
if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTING)
  find_package(benchmark REQUIRED)
endif ()

###### Test Inputs ######

if (GALOIS_GRAPH_LOCATION)
  set(BASEINPUT "${GALOIS_GRAPH_LOCATION}")
  set(BASE_VERIFICATION "${GALOIS_GRAPH_LOCATION}")
  set(GALOIS_ENABLE_INPUTS OFF)
  message(STATUS "Using graph input and verification logs location ${GALOIS_GRAPH_LOCATION}")
else ()
  set(BASEINPUT "${PROJECT_BINARY_DIR}/inputs/current")
  set(BASE_VERIFICATION "${PROJECT_BINARY_DIR}/inputs/current")
  set(GALOIS_ENABLE_INPUTS ON)
endif ()
# Set a common graph location for any nested projects.
set(GALOIS_GRAPH_LOCATION ${BASEINPUT})

###### Documentation ######

find_package(Doxygen)

###### Source finding ######

add_custom_target(lib)
add_custom_target(apps)

# Core libraries (lib)

# Allow build tree libraries and executables to see preload customizations like
# in libtsuba-fs without having to set LD_PRELOAD or similar explicitly.
list(PREPEND CMAKE_BUILD_RPATH ${PROJECT_BINARY_DIR})

# Allow installed libraries and executables to pull in deployment specific
# modifications like vendored runtime libraries (e.g., MPI).
list(PREPEND CMAKE_INSTALL_RPATH /usr/local/katana/lib)

###### Installation ######

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/GaloisConfigVersion.cmake
    VERSION ${GALOIS_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    ${CMAKE_CURRENT_LIST_DIR}/../GaloisConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/GaloisConfig.cmake
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Galois"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_BINDIR
)
install(
    FILES
    "${CMAKE_CURRENT_BINARY_DIR}/GaloisConfigVersion.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/GaloisConfig.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/FindNUMA.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Galois"
    COMPONENT dev
)
install(
    EXPORT GaloisTargets
    NAMESPACE Galois::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Galois"
    COMPONENT dev
)

set_property(GLOBAL PROPERTY KATANA_DOXYGEN_DIRECTORIES)
# Invoke this after all the documentation directories have been added to KATANA_DOXYGEN_DIRECTORIES.
function(add_katana_doxygen_target)
  if (NOT TARGET doc AND DOXYGEN_FOUND)
    get_property(doc_dirs GLOBAL PROPERTY KATANA_DOXYGEN_DIRECTORIES)
    list(JOIN doc_dirs "\" \"" DOXYFILE_SOURCE_DIR)

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(doc ${DOXYGEN_EXECUTABLE}
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile WORKING_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR})
  endif ()
endfunction()
