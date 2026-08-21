cmake_minimum_required(VERSION 3.15)

# ============================================================================
#  Jazz2 Resurrection - Lean PSP-only fork
# ----------------------------------------------------------------------------
#  This tree is PSP-native: the PSP is the ONLY target. It contains the exact
#  dependency closure reached from the PSP entry point (App/main.cpp), plus the
#  PSP GU backend (Sources/nCine/Backends/Psp). No other-platform backends, no
#  desktop entry point, no wildcards. Audio / multiplayer / save-load are kept
#  as interfaces + PSP stubs (see TODO.md, FEATURE_STATUS.md).
#
#  Build with ./build-psp.sh (PSPDEV Docker toolchain). See DEPENDENCIES.md.
# ============================================================================
project(jazz2_psp CXX ASM)

set(NEXT_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(NEXT_SOURCE_DIR "${NEXT_ROOT}/Sources")

# Explicit, migrated compile set (no globbing - see cmake/psp_sources.cmake).
include("${NEXT_ROOT}/cmake/psp_sources.cmake")

# --- The engine as a static library --------------------------------------------------------------
# PSP backend translation units live under the engine's Backends/Psp tree (their natural home):
#   PspGu        - GU frame lifecycle (clear/begin/end/swap)
#   PspEmitter   - RenderCommand::Issue -> GU draws (sprites, indexed CLUT tiles, host-vertex mesh)
#   PspBackend      - the backend seam: nCine::theApplication() + boot/step/shutdown entry points
#   PspAudioBackend - sceAudio output + software-mixed PCM gameplay effects
set(PSP_BACKEND_DIR "${NEXT_SOURCE_DIR}/nCine/Backends/Psp")
add_library(jazz2_psp_engine STATIC
	${PSP_ENGINE_SOURCES}
	"${NEXT_ROOT}/Tools/psp-bake/main.cpp"
	"${NEXT_ROOT}/Tools/psp-bake/TilesetBake.cpp"
	"${PSP_BACKEND_DIR}/PspGu.cpp"
	"${PSP_BACKEND_DIR}/PspVram.cpp"
	"${PSP_BACKEND_DIR}/PspEmitter.cpp"
	"${PSP_BACKEND_DIR}/PspDiagnostics.cpp"
	"${PSP_BACKEND_DIR}/PspAssetPack.cpp"
	"${PSP_BACKEND_DIR}/PspTextureStreamer.cpp"
	"${PSP_BACKEND_DIR}/PspBackend.cpp"
	"${PSP_BACKEND_DIR}/PspAudioBackend.cpp"
	"${PSP_BACKEND_DIR}/PspAdhoc.cpp"
	"${PSP_BACKEND_DIR}/PspInternetRelayTransport.cpp"
)

target_include_directories(jazz2_psp_engine PUBLIC
	"${NEXT_SOURCE_DIR}"
	"${NEXT_SOURCE_DIR}/Shared"
	"${NEXT_SOURCE_DIR}/Dependencies"
	"${NEXT_ROOT}/Tools/psp-bake"
)

# TILEMAP_USE_SINGLE_DRAW routes each tile layer through the batched host-vertex mesh path the GU
# emitter renders; it changes TileMap's member layout in the header, so it MUST match on every target
# that includes TileMap.h. See MIGRATION.md "Render backend" for the full rationale.
set(PSP_DEFS CMAKE_BUILD WITH_ZLIB WITH_AUDIO WITH_MULTIPLAYER DEATH_TRACE TILEMAP_USE_SINGLE_DRAW PSP_BAKE_LIBRARY)

# On-device debug logging: the debug.log/aemu.log writers, the log worker thread and its semaphores, and the
# periodic telemetry that formats messages for them. Off by default - the writers open, append to and flush a
# file on the Memory Stick, and some sit on the lobby/handshake path. Enable with -DPSP_DEBUG=ON to
# investigate on hardware. (The on-screen diagnostics overlay is NOT gated by this - it is a run-time toggle
# in the pause menu, just defaulted off.)
option(PSP_DEBUG "Build with on-device debug logging and diagnostics" OFF)
if(PSP_DEBUG)
	list(APPEND PSP_DEFS JAZZ2_PSP_DEBUG)
endif()

# Testing lever: force the low-memory streaming profile (async texture streaming, bounded sprite cache, 30 Hz
# present, 768 KiB display list, 60 fps cap) regardless of the detected model. A PSP-2000/3000 normally runs the
# eager-resident path; enable with -DPSP_FORCE_LOW_MEMORY=ON to exercise the streaming path on that hardware.
option(PSP_FORCE_LOW_MEMORY "Force the low-memory streaming profile regardless of detected PSP model" OFF)
if(PSP_FORCE_LOW_MEMORY)
	list(APPEND PSP_DEFS JAZZ2_PSP_FORCE_LOW_MEMORY)
endif()

# Testing lever: run in "legacy memory mode" — do NOT request the extended user partition (PBP MEMSIZE 0), so a
# PSP-2000/3000 is confined to the ~24 MiB standard partition exactly like a PSP-1000. PSP_HEAP_SIZE_KB(-1) then
# auto-sizes the heap to that smaller partition, giving real 32 MiB-class pressure. Combine with
# -DPSP_FORCE_LOW_MEMORY=ON to reproduce a 1000's eviction/streaming behaviour on 64 MiB hardware.
option(PSP_LEGACY_MEMORY "Do not claim the extended partition (MEMSIZE 0), simulating a 32 MiB PSP-1000" OFF)
if(PSP_LEGACY_MEMORY)
	set(PSP_MEMSIZE 0)
	list(APPEND PSP_DEFS JAZZ2_PSP_LEGACY_MEMORY)
else()
	set(PSP_MEMSIZE 1)
endif()

# Content/Animations + Content/Metadata can be linked into the EBOOT and unpacked next to it on first launch.
# That costs ~742 KiB of module image, which is carved out of the user partition BEFORE the heap is taken, and
# it buys nothing once the files exist on the Memory Stick - which they do after the first run, or after any
# `make deploy-psp-content`. Off by default; turn it on for a standalone redistributable EBOOT.
option(PSP_EMBED_CONTENT "Link Content/ into the EBOOT and unpack it on first launch" OFF)
if(PSP_EMBED_CONTENT)
	list(APPEND PSP_DEFS JAZZ2_PSP_EMBED_CONTENT)
endif()

set(PSP_ENGINE_FLAGS
	-std=gnu++17
	-fno-exceptions -fno-rtti
	-O2 -G0
	# On PSP int32_t is 'long'; make it 'int' (both 32-bit, ABI-identical) so the engine's
	# std::max/clamp(int32_t, int) template calls deduce, without editing hundreds of sites.
	-U__INT32_TYPE__ "-D__INT32_TYPE__=int"
	-U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int"
	-Wno-shift-count-overflow
)
target_compile_features(jazz2_psp_engine PUBLIC cxx_std_17)
target_compile_definitions(jazz2_psp_engine PUBLIC ${PSP_DEFS})
target_compile_options(jazz2_psp_engine PRIVATE ${PSP_ENGINE_FLAGS})

# Asset preparation is a one-time, CPU-bound workload. In particular, the fixed-size IMA ADPCM loop is
# called roughly 95,000 times for a half-rate retail music set; at -O3 GCC inlines and specializes it while
# growing this translation unit by only about 3 KiB. Keep the rest of the memory-sensitive game at -O2.
set_property(SOURCE "${NEXT_ROOT}/Tools/psp-bake/main.cpp" APPEND PROPERTY COMPILE_OPTIONS -O3)

# --- The executable: PSP entry point + the engine ------------------------------------------------
# English is the engine's compiled-in source language, so no translation catalogue is needed on PSP.
# Embed only the data required by the runtime and on-device baker.
set(PSP_APP_SOURCES
	"${NEXT_ROOT}/App/main.cpp"
	"${NEXT_ROOT}/Sources/nCine/Backends/Psp/EmbeddedContent.cpp")
if(PSP_EMBED_CONTENT)
	file(GLOB_RECURSE PSP_EMBEDDED_CONTENT_FILES CONFIGURE_DEPENDS
		"${NEXT_ROOT}/Content/Animations/*"
		"${NEXT_ROOT}/Content/Metadata/*")
	set(EMBEDDED_CONTENT_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/embedded_content.tar")
	set(EMBEDDED_CONTENT_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedContentVersion.h")
	add_custom_command(
		OUTPUT "${EMBEDDED_CONTENT_ARCHIVE}" "${EMBEDDED_CONTENT_VERSION_HEADER}"
		COMMAND "${CMAKE_COMMAND}" -E tar cf "${EMBEDDED_CONTENT_ARCHIVE}" --format=gnutar
			Content/Animations Content/Metadata
		COMMAND "${CMAKE_COMMAND}"
			"-DARCHIVE=${EMBEDDED_CONTENT_ARCHIVE}"
			"-DOUTPUT=${EMBEDDED_CONTENT_VERSION_HEADER}"
			-P "${NEXT_ROOT}/cmake/PspEmbeddedContentVersion.cmake"
		WORKING_DIRECTORY "${NEXT_ROOT}"
		DEPENDS ${PSP_EMBEDDED_CONTENT_FILES} "${NEXT_ROOT}/cmake/PspEmbeddedContentVersion.cmake"
		COMMENT "Packing engine Content into the EBOOT"
	)
	configure_file("${NEXT_ROOT}/Sources/nCine/Backends/Psp/EmbeddedContent.S.in"
		"${CMAKE_CURRENT_BINARY_DIR}/EmbeddedContent.S" @ONLY)
	set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/EmbeddedContent.S" PROPERTIES
		GENERATED TRUE OBJECT_DEPENDS "${EMBEDDED_CONTENT_ARCHIVE}")
	list(APPEND PSP_APP_SOURCES
		"${EMBEDDED_CONTENT_VERSION_HEADER}"
		"${CMAKE_CURRENT_BINARY_DIR}/EmbeddedContent.S")
endif()

add_executable(jazz2_psp_engine_app ${PSP_APP_SOURCES})
target_compile_options(jazz2_psp_engine_app PRIVATE ${PSP_ENGINE_FLAGS})
target_include_directories(jazz2_psp_engine_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT DEFINED PSP_BUILD_STAMP)
	set(PSP_BUILD_STAMP "unknown")
endif()
target_compile_definitions(jazz2_psp_engine_app PRIVATE PSP_BUILD_STAMP=\"${PSP_BUILD_STAMP}\")
target_link_libraries(jazz2_psp_engine_app
	jazz2_psp_engine
	xmp z
	pspaudio pspdisplay pspge pspgu pspctrl psppower psprtc psputility
	pspnet pspnet_adhoc pspnet_adhocctl pspnet_apctl pspnet_inet pspnet_resolver pspwlan
	pspsdk c m pspkubridge
)

# libpspkubridge is a legacy CFW stub archive. Normalize its import table once
# before PSPDEV's normal packaging fixup so the generated PBP gets a clean,
# validated table on the first build as well as incremental builds.
add_custom_command(TARGET jazz2_psp_engine_app POST_BUILD
	COMMAND "${CMAKE_COMMAND}"
		"-DFIXUP_TOOL=${PSPDEV}/bin/psp-fixup-imports"
		"-DTARGET_FILE=$<TARGET_FILE:jazz2_psp_engine_app>"
		-P "${NEXT_ROOT}/cmake/PspPreFixImports.cmake"
	COMMENT "Pre-normalizing KUBridge imports"
)

create_pbp_file(
	TARGET jazz2_psp_engine_app
	TITLE "Jazz Jackrabbit 2"
	VERSION "0.1.0"
	# Request the expanded user partition on PSP-2000+; PSP-1000 safely
	# ignores it. Keep this explicit because PSPDEV releases have used both
	# MEMSIZE=1 and MEMSIZE=2 as their create_pbp_file() default.
	# -DPSP_LEGACY_MEMORY=ON forces this to 0 to simulate a 32 MiB PSP-1000.
	MEMSIZE ${PSP_MEMSIZE}
	# Hand-made package art, kept in the source tree rather than Build/Assets/Cache: that directory is
	# generated by psp-bake and REBAKE=1 wipes it. (The ICON0.PNG psp-bake does emit there is unrelated - it
	# is the Sony *savedata* icon, a different slot.)
	ICON_PATH "${NEXT_ROOT}/App/Psp/Package/ICON0.PNG"
	BACKGROUND_PATH "${NEXT_ROOT}/App/Psp/Package/PIC1.PNG"
	PREVIEW_PATH NULL
	OUTPUT_DIR "${CMAKE_BINARY_DIR}/dist"
)
