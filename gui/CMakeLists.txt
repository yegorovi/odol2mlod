cmake_minimum_required(VERSION 3.21)
project(odol2mlod_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(odol2mlod-gui WIN32
	main.cpp
	mainwindow.cpp mainwindow.h
)

target_link_libraries(odol2mlod-gui PRIVATE Qt6::Widgets)

if(MINGW)
	target_link_options(odol2mlod-gui PRIVATE -static-libgcc -static-libstdc++)
endif()

set_target_properties(odol2mlod-gui PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
	RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}
)

# рядом с gui-сборкой кладётся конвертер, который окно запускает процессом
add_custom_command(TARGET odol2mlod-gui POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${CMAKE_CURRENT_SOURCE_DIR}/../odol2mlod.exe
		$<TARGET_FILE_DIR:odol2mlod-gui>
)
