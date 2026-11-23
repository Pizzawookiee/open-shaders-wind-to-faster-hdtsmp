cmake_minimum_required(VERSION 3.22)
project(OpenShadersFSMPWind VERSION 0.3.4 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An existing installation is FIRST-CLASS. CMake never rewrites that source tree.
set(CommonLibSSEPath_NG "" CACHE PATH "Existing CommonLibSSE-NG source checkout")
if(NOT CommonLibSSEPath_NG AND DEFINED ENV{CommonLibSSEPath_NG})
    set(CommonLibSSEPath_NG "$ENV{CommonLibSSEPath_NG}")
endif()
if(NOT CommonLibSSEPath_NG)
    message(FATAL_ERROR "Pass -DCommonLibSSEPath_NG=D:/Modding/<your CommonLibSSE-NG folder>")
endif()
if(NOT EXISTS "${CommonLibSSEPath_NG}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSEPath_NG must point to a source directory with CMakeLists.txt: ${CommonLibSSEPath_NG}")
endif()

set(BUILD_TESTS OFF CACHE BOOL "Build CommonLib's tests" FORCE)
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

add_subdirectory("${CommonLibSSEPath_NG}" "${CMAKE_BINARY_DIR}/CommonLibSSE-NG" EXCLUDE_FROM_ALL)
find_package(Bullet CONFIG REQUIRED)

# User-provided Bullet headers take precedence over vcpkg's include directory.
set(BULLET_INCLUDE_PATH "" CACHE PATH "Existing FSMP-compatible Bullet include/bullet folder")
if(BULLET_INCLUDE_PATH)
    if(NOT EXISTS "${BULLET_INCLUDE_PATH}/btBulletDynamicsCommon.h")
        message(FATAL_ERROR "BULLET_INCLUDE_PATH must contain btBulletDynamicsCommon.h: ${BULLET_INCLUDE_PATH}")
    endif()
    set(BULLET_INCLUDE_DIRS "${BULLET_INCLUDE_PATH}")
elseif(NOT BULLET_INCLUDE_DIRS)
    find_path(BULLET_INCLUDE_PATH btBulletDynamicsCommon.h
        HINTS "${CURRENT_INSTALLED_DIR}/include/bullet"
              "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/bullet")
    if(NOT BULLET_INCLUDE_PATH)
        message(FATAL_ERROR "Bullet 3 headers not located; pass -DBULLET_INCLUDE_PATH=.../include/bullet")
    endif()
    set(BULLET_INCLUDE_DIRS "${BULLET_INCLUDE_PATH}")
endif()

add_library(OpenShadersFSMPWind SHARED
    src/Plugin.cpp
    src/FSMPWorldABI.cpp
    src/WindMath.cpp
    src/WindArea.cpp
    src/WindShelter.cpp
    src/SelfShelter.cpp
    src/WindScene.cpp
)
target_include_directories(OpenShadersFSMPWind PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${BULLET_INCLUDE_DIRS}
)
target_compile_definitions(OpenShadersFSMPWind PRIVATE
    BT_USE_SSE_IN_API
    NOMINMAX WIN32_LEAN_AND_MEAN
)
target_compile_options(OpenShadersFSMPWind PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(OpenShadersFSMPWind PRIVATE
    CommonLibSSE::CommonLibSSE
    version
    dbghelp
)
set_target_properties(OpenShadersFSMPWind PROPERTIES
    OUTPUT_NAME "OpenShadersFSMPWind"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/SKSE/Plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/SKSE/Plugins"
)
# Deliberately don't link another Bullet runtime into this SKSE DLL: calls used
# here are inline, and all objects belong to the already-loaded FSMP Bullet.
# If a non-inline symbol becomes necessary, don't link arbitrary Bullet libraries
# without first checking whether that could introduce incompatible runtimes.

install(TARGETS OpenShadersFSMPWind RUNTIME DESTINATION SKSE/Plugins)
install(FILES "Data/SKSE/Plugins/OpenShadersFSMPWind.ini" DESTINATION SKSE/Plugins)

option(OSFSMPWIND_BUILD_MATH_TESTS "Build portable wind math sanity tests" OFF)
if(OSFSMPWIND_BUILD_MATH_TESTS)
    enable_testing()
    add_executable(WindMathTest tests/WindMathTest.cpp src/WindMath.cpp)
    add_executable(WindAreaTest tests/WindAreaTest.cpp src/WindMath.cpp src/WindArea.cpp src/WindShelter.cpp)
    target_include_directories(WindMathTest PRIVATE src)
    target_include_directories(WindAreaTest PRIVATE src)
    add_test(NAME WindMathTest COMMAND WindMathTest)
    add_test(NAME WindAreaTest COMMAND WindAreaTest)
    add_executable(SelfShelterTest tests/SelfShelterTest.cpp src/WindMath.cpp src/SelfShelter.cpp)
    target_include_directories(SelfShelterTest PRIVATE src)
    add_test(NAME SelfShelterTest COMMAND SelfShelterTest)
    add_executable(WindComponentsTest tests/WindComponentsTest.cpp)
    target_include_directories(WindComponentsTest PRIVATE src)
    add_test(NAME WindComponentsTest COMMAND WindComponentsTest)
endif()
