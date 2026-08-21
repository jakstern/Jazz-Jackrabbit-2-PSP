cmake_minimum_required(VERSION 3.21)
project(psp_bake VERSION 0.1.0 LANGUAGES CXX)

option(PSP_BAKE_STATIC_LIBXMP "Link a static libxmp on Windows" OFF)
set(PSP_BAKE_RUNTIME_DIRS "" CACHE STRING "Additional directories containing Windows runtime DLLs")

# ============================================================================
#  psp-bake — HOST asset pipeline tool (NOT a PSP build).
#  Reuses a lean, GL-free subset of the engine's Compatibility + Shared IO code
#  to convert original JJ2 `Source/` into slim PSP cache metadata and PSP-GU-native assets.
# ============================================================================

find_package(ZLIB REQUIRED)

# Use the same tracker decoder as the PSP build. On Linux this is normally
# provided by libxmp-dev; on Windows it can be supplied by vcpkg or MSYS2.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
	pkg_check_modules(LIBXMP QUIET IMPORTED_TARGET libxmp)
endif()
if(TARGET PkgConfig::LIBXMP)
	set(PSP_BAKE_XMP_TARGET PkgConfig::LIBXMP)
else()
	find_path(LIBXMP_INCLUDE_DIR NAMES xmp.h)
	find_library(LIBXMP_LIBRARY NAMES xmp libxmp)
	if(NOT LIBXMP_INCLUDE_DIR OR NOT LIBXMP_LIBRARY)
		message(FATAL_ERROR
			"libxmp was not found. Install libxmp-dev (Debian/Ubuntu), libxmp-devel "
			"(Fedora/MSYS2), or libxmp via vcpkg, then configure again.")
	endif()
	add_library(psp_bake_libxmp UNKNOWN IMPORTED)
	set_target_properties(psp_bake_libxmp PROPERTIES
		IMPORTED_LOCATION "${LIBXMP_LIBRARY}"
		INTERFACE_INCLUDE_DIRECTORIES "${LIBXMP_INCLUDE_DIR}")
	set(PSP_BAKE_XMP_TARGET psp_bake_libxmp)
endif()

set(ENGINE "${CMAKE_CURRENT_LIST_DIR}/../../Sources")

add_executable(psp-bake
	Cli.cpp
	main.cpp
	TilesetBake.cpp
	# --- Compatibility conversion (the engine's own converters) ---
	${ENGINE}/Jazz2/Compatibility/JJ2Anims.cpp
	${ENGINE}/Jazz2/Compatibility/JJ2Block.cpp
	${ENGINE}/Jazz2/Compatibility/AnimSetMapping.cpp
	${ENGINE}/Jazz2/Compatibility/JJ2Data.cpp
	${ENGINE}/Jazz2/Compatibility/JJ2Episode.cpp
	${ENGINE}/Jazz2/Compatibility/JJ2Level.cpp
	${ENGINE}/Jazz2/Compatibility/JJ2Tileset.cpp
	${ENGINE}/Jazz2/Compatibility/JJ2Strings.cpp
	${ENGINE}/Jazz2/Compatibility/EventConverter.cpp
	# --- Shared IO / compression / containers the converters need ---
	${ENGINE}/Shared/IO/Stream.cpp
	${ENGINE}/Shared/IO/FileStream.cpp
	${ENGINE}/Shared/IO/FileSystem.cpp
	${ENGINE}/Shared/IO/MemoryStream.cpp
	${ENGINE}/Shared/IO/BoundedFileStream.cpp
	${ENGINE}/Shared/IO/PakFile.cpp
	${ENGINE}/Shared/IO/Compression/DeflateStream.cpp
	${ENGINE}/Shared/Containers/String.cpp
	${ENGINE}/Shared/Containers/StringView.cpp
	${ENGINE}/Shared/Containers/StringUtils.cpp
	${ENGINE}/Shared/Containers/SmallVector.cpp
	${ENGINE}/Shared/Containers/DateTime.cpp
	${ENGINE}/Shared/Base/Format.cpp
	${ENGINE}/Shared/Cryptography/xxHash.cpp
	${ENGINE}/Shared/Utf8.cpp
	${ENGINE}/Shared/Environment.cpp
	${ENGINE}/nCine/Base/Algorithms.cpp
	${ENGINE}/nCine/Primitives/Color.cpp
	# jsoncpp (metadata .res parsing for the sprite bake)
	${ENGINE}/Dependencies/jsoncpp/reader.cpp
	${ENGINE}/Dependencies/jsoncpp/value.cpp
	${ENGINE}/Dependencies/jsoncpp/writer.cpp
)

target_include_directories(psp-bake PRIVATE
	${ENGINE}
	${ENGINE}/Shared
	${ENGINE}/Dependencies
)

# CMAKE_BUILD + WITH_ZLIB match the engine's expectations; DEATH_TRACE is left
# undefined so the LOG* macros in the reused code compile to no-ops.
target_compile_definitions(psp-bake PRIVATE
	CMAKE_BUILD WITH_ZLIB
	PSP_BAKE_VERSION="${PROJECT_VERSION}"
)
target_compile_features(psp-bake PRIVATE cxx_std_17)
target_link_libraries(psp-bake PRIVATE ZLIB::ZLIB ${PSP_BAKE_XMP_TARGET})

if(WIN32)
	target_compile_definitions(psp-bake PRIVATE UNICODE _UNICODE)
	if(PSP_BAKE_STATIC_LIBXMP OR VCPKG_TARGET_TRIPLET MATCHES "-static")
		target_compile_definitions(psp-bake PRIVATE LIBXMP_STATIC)
	endif()
	target_link_libraries(psp-bake PRIVATE shell32)
endif()

# A package contains everything owned by this project. Users provide only the
# original JJ2 files; no engine checkout is needed at runtime.
include(GNUInstallDirs)
if(WIN32)
	if(CMAKE_CROSSCOMPILING)
		install(TARGETS psp-bake RUNTIME DESTINATION ".")
	else()
		install(TARGETS psp-bake
			RUNTIME_DEPENDENCIES
				DIRECTORIES ${PSP_BAKE_RUNTIME_DIRS}
				PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
				POST_EXCLUDE_REGEXES ".*[\\/]System32[\\/].*"
			RUNTIME DESTINATION ".")
	endif()
	set(PSP_BAKE_DATA_DESTINATION "Content")
else()
	install(TARGETS psp-bake RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
	set(PSP_BAKE_DATA_DESTINATION "${CMAKE_INSTALL_DATADIR}/psp-bake/Content")
endif()
install(DIRECTORY
	"${CMAKE_CURRENT_LIST_DIR}/../../Content/Animations"
	"${CMAKE_CURRENT_LIST_DIR}/../../Content/Metadata"
	DESTINATION "${PSP_BAKE_DATA_DESTINATION}")
install(FILES
	"${CMAKE_CURRENT_LIST_DIR}/README.md"
	"${CMAKE_CURRENT_LIST_DIR}/../../LICENSE"
	DESTINATION "${CMAKE_INSTALL_DOCDIR}")

include(CTest)
if(BUILD_TESTING)
	add_test(NAME psp-bake-version COMMAND psp-bake --version)
	add_test(NAME psp-bake-help COMMAND psp-bake --help)
	add_test(NAME psp-bake-rejects-unknown-option COMMAND psp-bake --definitely-invalid)
	set_tests_properties(psp-bake-rejects-unknown-option PROPERTIES WILL_FAIL TRUE)
endif()

set(CPACK_PACKAGE_NAME "psp-bake")
set(CPACK_PACKAGE_VENDOR "Jazz2 Resurrection")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Jazz Jackrabbit 2 asset converter for PSP")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE")
if(WIN32)
	set(CPACK_GENERATOR "ZIP")
else()
	set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
