cmake_minimum_required(VERSION 3.16)
project(hwsaga VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Download SDL2 and link it statically: produces a self-contained executable
# (used for the Windows and macOS release builds).
option(HWSAGA_BUNDLE_SDL "Download SDL2 and link it statically" OFF)

if(HWSAGA_BUNDLE_SDL)
	include(FetchContent)
	set(SDL_SHARED OFF CACHE BOOL "" FORCE)
	set(SDL_STATIC ON CACHE BOOL "" FORCE)
	set(SDL_TEST OFF CACHE BOOL "" FORCE)
	set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
	# SDL 2.30's PipeWire backend does not compile against newer PipeWire
	# headers with GCC 15; PulseAudio/ALSA cover Linux audio
	set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(SDL2
		URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
		URL_HASH SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
	FetchContent_MakeAvailable(SDL2)
	set(HWSAGA_SDL_LIBS SDL2::SDL2main SDL2::SDL2-static)
else()
	find_package(SDL2 REQUIRED)
	if(TARGET SDL2::SDL2)
		set(HWSAGA_SDL_LIBS $<$<TARGET_EXISTS:SDL2::SDL2main>:SDL2::SDL2main> SDL2::SDL2)
	else()
		set(HWSAGA_SDL_LIBS ${SDL2_LIBRARIES})
		include_directories(${SDL2_INCLUDE_DIRS})
	endif()
endif()

set(HWSAGA_SOURCES
	src/main.cpp
	src/cpu.cpp
	src/machine.cpp
	src/loader.cpp
	src/hle.cpp
	src/io.cpp
	src/vfs.cpp
	src/cdaudio.cpp
	src/midi.cpp
	src/host.cpp
)

if(APPLE)
	add_executable(hwsaga MACOSX_BUNDLE ${HWSAGA_SOURCES})
	set_target_properties(hwsaga PROPERTIES
		OUTPUT_NAME "Hoehlenwelt-Saga"
		MACOSX_BUNDLE_BUNDLE_NAME "Die Höhlenwelt-Saga"
		MACOSX_BUNDLE_GUI_IDENTIFIER "org.hwsaga.hoehlenwelt"
		MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
else()
	add_executable(hwsaga ${HWSAGA_SOURCES})
endif()

target_link_libraries(hwsaga PRIVATE ${HWSAGA_SDL_LIBS})

# no console window on Windows (SDL2main provides WinMain); diagnostics are
# still available with -DHWSAGA_CONSOLE=ON
option(HWSAGA_CONSOLE "Windows: build as console program" OFF)
if(WIN32 AND NOT HWSAGA_CONSOLE)
	set_target_properties(hwsaga PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(MSVC)
	target_compile_options(hwsaga PRIVATE /W3 /utf-8)
	target_compile_definitions(hwsaga PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(WIN32)
	target_compile_options(hwsaga PRIVATE -Wall -Wextra -Wno-unused-parameter)
	target_link_options(hwsaga PRIVATE -static)
else()
	target_compile_options(hwsaga PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS hwsaga RUNTIME DESTINATION bin BUNDLE DESTINATION .)

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "hwsaga")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Taktloss")
    
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Engine für die Höhlenwelt-Saga")
    set(CPACK_DEBIAN_PACKAGE_SECTION "games")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)