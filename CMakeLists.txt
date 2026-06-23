cmake_minimum_required(VERSION 3.20)
project(astro_toolkit LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Set build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "astro_toolkit v${PROJECT_VERSION} | Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Compiler optimization flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Common flags for both GNU and Clang
  add_compile_options(-Wall -Wextra -Wpedantic -Werror=format-security)
  
  # Release-specific optimizations
  if(CMAKE_BUILD_TYPE MATCHES "Release")
    if(CMAKE_CROSSCOMPILING)
      add_compile_options(-O3 -flto=auto)
    else()
      add_compile_options(-O3 -march=native -flto=auto)
    endif()
    add_link_options(-flto=auto)
  else()
    # Debug build with sanitizers
    add_compile_options(-O0 -g -fsanitize=undefined -fsanitize=address)
    add_link_options(-fsanitize=undefined -fsanitize=address)
    message(STATUS "Debug build with AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  endif()
  
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  add_compile_options(/W4 /permissive- /std:c++latest)
  
  if(CMAKE_BUILD_TYPE MATCHES "Release")
    add_compile_options(/O2 /GL /Gy)
    add_link_options(/LTCG /OPT:REF /OPT:ICF)
  else()
    add_compile_options(/RTC1 /Zi)
  endif()
endif()

# ===== Qt6 =================================================================
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Concurrent Sql)
message(STATUS "Qt6 found: ${Qt6_VERSION}")

# ===== libcurl ==============================================================
find_package(CURL QUIET)
if(NOT CURL_FOUND)
  if(DEFINED CURL_INCLUDE_DIR AND DEFINED CURL_LIBRARY)
    add_library(CURL::libcurl UNKNOWN IMPORTED)
    set_target_properties(CURL::libcurl PROPERTIES
      IMPORTED_LOCATION "${CURL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")
    message(STATUS "libcurl (manual): ${CURL_LIBRARY}")
  else()
    message(FATAL_ERROR "libcurl not found. Please install libcurl development package:\n"
                        "  Ubuntu/Debian: sudo apt-get install libcurl4-openssl-dev\n"
                        "  Fedora: sudo dnf install libcurl-devel\n"
                        "  macOS: brew install curl")
  endif()
else()
  message(STATUS "libcurl: ${CURL_VERSION}")
endif()

# ===== nlohmann/json ========================================================
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  if(DEFINED NLOHMANN_JSON_INCLUDE_DIR)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    target_include_directories(nlohmann_json::nlohmann_json
      INTERFACE "${NLOHMANN_JSON_INCLUDE_DIR}")
    message(STATUS "nlohmann/json (manual): ${NLOHMANN_JSON_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "nlohmann_json not found. Please install the development package:\n"
                        "  Ubuntu/Debian: sudo apt-get install nlohmann-json3-dev\n"
                        "  Fedora: sudo dnf install nlohmann-json-devel\n"
                        "  macOS: brew install nlohmann-json")
  endif()
else()
  message(STATUS "nlohmann_json found")
endif()

# ===== cfitsio ==============================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(CFITSIO QUIET cfitsio)
if(NOT CFITSIO_FOUND)
  find_library(CFITSIO_LIBRARIES NAMES cfitsio)
  find_path(CFITSIO_INCLUDE_DIRS NAMES fitsio.h PATH_SUFFIXES cfitsio)
  if(NOT CFITSIO_LIBRARIES OR NOT CFITSIO_INCLUDE_DIRS)
    message(FATAL_ERROR "cfitsio not found. Please install the development package:\n"
                        "  Ubuntu/Debian: sudo apt-get install libcfitsio-dev\n"
                        "  Fedora: sudo dnf install cfitsio-devel\n"
                        "  macOS: brew install cfitsio")
  endif()
  message(STATUS "cfitsio (manual): ${CFITSIO_LIBRARIES}")
else()
  message(STATUS "cfitsio: ${CFITSIO_VERSION}")
endif()

# ===== GPSD (Optional, Linux-only) ==========================================
if(NOT WIN32)
  pkg_check_modules(GPSD QUIET libgps)
  if(GPSD_FOUND)
    add_compile_definitions(HAVE_GPSD)
    message(STATUS "GPSD support: ENABLED (${GPSD_VERSION})")
  else()
    message(STATUS "GPSD support: DISABLED (optional, install libgps-dev to enable)")
  endif()
else()
  message(STATUS "GPSD support: N/A (Windows platform)")
endif()

# ===== Build Target ========================================================
if(WIN32)
  add_executable(astro_toolkit WIN32 main.cpp)
else()
  add_executable(astro_toolkit main.cpp)
endif()

set_target_properties(astro_toolkit PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  EXPORT_COMPILE_COMMANDS ON
)

# Apply compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(astro_toolkit PRIVATE -Wall -Wextra -Wpedantic)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  target_compile_options(astro_toolkit PRIVATE /W4)
endif()

# ===== Linking ==============================================================
target_include_directories(astro_toolkit PRIVATE ${CFITSIO_INCLUDE_DIRS})

target_link_libraries(astro_toolkit PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::Concurrent
  Qt6::Sql
  CURL::libcurl
  nlohmann_json::nlohmann_json
  ${CFITSIO_LIBRARIES}
)

if(GPSD_FOUND)
  target_include_directories(astro_toolkit PRIVATE ${GPSD_INCLUDE_DIRS})
  target_link_libraries(astro_toolkit PRIVATE ${GPSD_LIBRARIES})
endif()

# ===== Platform-specific linking =============================================
if(WIN32)
  target_link_libraries(astro_toolkit PRIVATE ws2_32 crypt32)
endif()

# ===== Installation =========================================================
if(WIN32)
  install(TARGETS astro_toolkit
          RUNTIME DESTINATION .
  )
  # Bundle the starter catalogue beside the binary so it can be auto-loaded.
  install(FILES data/catalog.sqlite DESTINATION data OPTIONAL)
  message(STATUS "Install destination: Current directory (.)")
else()
  install(TARGETS astro_toolkit
          RUNTIME DESTINATION bin
          RENAME astro-toolkit
  )
  # Bundle the starter catalogue under <prefix>/share so the app can seed it on
  # first run (see findBundledCatalog() in main.cpp).
  install(FILES data/catalog.sqlite DESTINATION share/astro_toolkit OPTIONAL)
  message(STATUS "Install destination: /usr/local/bin/astro-toolkit (after 'sudo make install')")
endif()

# ===== Uninstall target ====================================================
# Note: cmake_uninstall.cmake.in is referenced from parent project
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../cpp_pr/cmake_uninstall.cmake.in")
  configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/../cpp_pr/cmake_uninstall.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
      IMMEDIATE @ONLY)
  
  add_custom_target(uninstall
      COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
  message(STATUS "Uninstall target available: make uninstall")
else()
  message(WARNING "cmake_uninstall.cmake.in not found. Uninstall target will be unavailable.")
endif()

# ===== Summary ==============================================================
message(STATUS "")
message(STATUS "======= Build Summary =======")
message(STATUS "Project:     astro_toolkit v${PROJECT_VERSION}")
message(STATUS "Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ Std:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Optional Features:")
message(STATUS "  GPSD:      ${GPSD_FOUND}")
message(STATUS "")
message(STATUS "Build with: cmake --build . -j$(nproc)")
message(STATUS "Install:    sudo cmake --install .")
