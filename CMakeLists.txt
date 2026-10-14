cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(TelegramBotApi VERSION 10.3 LANGUAGES CXX)

# apply patches from td-patches to TDLib sources; already applied patches are skipped
option(TELEGRAM_BOT_API_APPLY_TD_PATCHES "Apply patches from td-patches to TDLib sources" ON)
if (TELEGRAM_BOT_API_APPLY_TD_PATCHES)
  file(GLOB TD_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/td-patches/*.patch")
  list(SORT TD_PATCHES)
  if (TD_PATCHES)
    find_package(Git)
    if (NOT GIT_FOUND)
      message(FATAL_ERROR "Git is needed to apply patches from td-patches to TDLib sources. Apply them manually and "
                          "pass -DTELEGRAM_BOT_API_APPLY_TD_PATCHES=OFF to CMake.")
    endif()
  endif()
  # an empty private repository is used, so the patches are applied in the same way whether td is a submodule,
  # a plain directory inside another repository, or a copy with a broken .git link
  set(TD_PATCHES_GIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/td-patches-git")
  if (TD_PATCHES AND NOT EXISTS "${TD_PATCHES_GIT_DIR}/HEAD")
    execute_process(COMMAND "${GIT_EXECUTABLE}" init --quiet --bare "${TD_PATCHES_GIT_DIR}"
                    RESULT_VARIABLE TD_PATCHES_GIT_INIT_RESULT)
    if (NOT TD_PATCHES_GIT_INIT_RESULT EQUAL 0)
      message(FATAL_ERROR "Failed to create ${TD_PATCHES_GIT_DIR}")
    endif()
  endif()
  set(TD_GIT_APPLY "${GIT_EXECUTABLE}" "--git-dir=${TD_PATCHES_GIT_DIR}" --work-tree=. apply --whitespace=nowarn)
  foreach (TD_PATCH ${TD_PATCHES})
    execute_process(COMMAND ${TD_GIT_APPLY} --reverse --check "${TD_PATCH}"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/td"
                    RESULT_VARIABLE TD_PATCH_IS_APPLIED OUTPUT_QUIET ERROR_QUIET)
    if (NOT TD_PATCH_IS_APPLIED EQUAL 0)
      execute_process(COMMAND ${TD_GIT_APPLY} "${TD_PATCH}"
                      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/td"
                      RESULT_VARIABLE TD_PATCH_RESULT ERROR_VARIABLE TD_PATCH_ERROR)
      if (NOT TD_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${TD_PATCH} to TDLib sources: ${TD_PATCH_ERROR}")
      endif()
      message(STATUS "Applied ${TD_PATCH} to TDLib sources")
    endif()
  endforeach()
endif()

add_subdirectory(td EXCLUDE_FROM_ALL)

if (NOT DEFINED CMAKE_MODULE_PATH)
  set(CMAKE_MODULE_PATH "")
endif()
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/td/CMake" "${CMAKE_MODULE_PATH}")

if (NOT DEFINED CMAKE_INSTALL_BINDIR)
  set(CMAKE_INSTALL_BINDIR "bin")
endif()

if (POLICY CMP0074)
  # use environment variables to find libraries
  cmake_policy(SET CMP0074 NEW)
endif()

include(PreventInSourceBuild)
prevent_in_source_build()

if (TELEGRAM_BOT_API_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IPO_SUPPORTED LANGUAGES CXX)
  if (IPO_SUPPORTED)
    string(REPLACE ";" " " CXX_FLAGS_IPO "${CMAKE_CXX_COMPILE_OPTIONS_IPO}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS_IPO}")

    string(REPLACE ";" " " LINK_FLAGS_IPO "${CMAKE_CXX_LINK_OPTIONS_IPO}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINK_FLAGS_IPO}")
  endif()
endif()

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if (THREADS_HAVE_PTHREAD_ARG)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include(TdSetUpCompiler)
td_set_up_compiler()

if (CLANG OR GCC)
  if (MEMPROF)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-no-pie CXX_NO_PIE_FLAG)
    if (CXX_NO_PIE_FLAG)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
    elseif (APPLE)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-no_pie")
    endif()
    include(AddCXXCompilerFlag)
    add_cxx_compiler_flag("-static-libstdc++")
    add_cxx_compiler_flag("-static-libgcc")
  endif()
endif()

set(TELEGRAM_BOT_API_SOURCE
  telegram-bot-api/telegram-bot-api.cpp

  telegram-bot-api/BotMetrics.cpp
  telegram-bot-api/Client.cpp
  telegram-bot-api/ClientManager.cpp
  telegram-bot-api/CpuSampler.cpp
  telegram-bot-api/HeapSampler.cpp
  telegram-bot-api/HttpConnection.cpp
  telegram-bot-api/HttpStatConnection.cpp
  telegram-bot-api/Query.cpp
  telegram-bot-api/Stats.cpp
  telegram-bot-api/Watchdog.cpp
  telegram-bot-api/WebhookActor.cpp

  telegram-bot-api/BotMetrics.h
  telegram-bot-api/Client.h
  telegram-bot-api/ClientManager.h
  telegram-bot-api/ClientParameters.h
  telegram-bot-api/CpuSampler.h
  telegram-bot-api/HeapSampler.h
  telegram-bot-api/HttpConnection.h
  telegram-bot-api/HttpServer.h
  telegram-bot-api/HttpStatConnection.h
  telegram-bot-api/Query.h
  telegram-bot-api/Stats.h
  telegram-bot-api/Watchdog.h
  telegram-bot-api/WebhookActor.h
)

add_executable(telegram-bot-api ${TELEGRAM_BOT_API_SOURCE})
target_include_directories(telegram-bot-api PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(telegram-bot-api PRIVATE memprof tdactor tdcore tddb tdnet tdutils)
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
  # timer_create is in librt before glibc 2.34
  include(CheckLibraryExists)
  check_library_exists(rt timer_create "" TELEGRAM_BOT_API_HAVE_LIBRT)
  if (TELEGRAM_BOT_API_HAVE_LIBRT)
    target_link_libraries(telegram-bot-api PRIVATE rt)
  endif()
endif()

# accounting of memory usage of each bot replaces memory allocation functions; it requires dynamic linking with glibc
option(TELEGRAM_BOT_API_HEAP_SAMPLING "Account memory usage of each bot by replacing memory allocation functions" ON)
set(TELEGRAM_BOT_API_HEAP_SAMPLING_ENABLED OFF)
if (TELEGRAM_BOT_API_HEAP_SAMPLING AND CMAKE_SYSTEM_NAME MATCHES "Linux" AND NOT MEMPROF AND
    NOT "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "(^| )-static( |$)")
  set(TELEGRAM_BOT_API_HEAP_SAMPLING_ENABLED ON)
  target_compile_definitions(telegram-bot-api PRIVATE TELEGRAM_BOT_API_HEAP_SAMPLING)
  target_link_libraries(telegram-bot-api PRIVATE ${CMAKE_DL_LIBS})
endif()

# test-only tools for test/restart/restart_test.py
add_executable(restart-test-fake-auth EXCLUDE_FROM_ALL test/restart/fake_auth.cpp)
target_link_libraries(restart-test-fake-auth PRIVATE tdmtproto tddb tdutils)
add_executable(restart-test-fake-updates EXCLUDE_FROM_ALL test/restart/fake_updates.cpp)
target_link_libraries(restart-test-fake-updates PRIVATE tddb tdutils)

# verification of memory accounting: test/heap/heap_sampler_test.cpp
add_executable(heap-sampler-test EXCLUDE_FROM_ALL test/heap/heap_sampler_test.cpp telegram-bot-api/HeapSampler.cpp)
target_include_directories(heap-sampler-test PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_definitions(heap-sampler-test PRIVATE TELEGRAM_BOT_API_HEAP_SAMPLING)
target_link_libraries(heap-sampler-test PRIVATE tdactor tdutils ${CMAKE_DL_LIBS})

install(TARGETS telegram-bot-api RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (MSVC AND VCPKG_TOOLCHAIN)
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/" DESTINATION "${CMAKE_INSTALL_BINDIR}" FILES_MATCHING PATTERN "*.dll" PATTERN "*.pdb")
endif()
