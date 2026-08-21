if(NOT DEFINED ARCHIVE OR NOT EXISTS "${ARCHIVE}")
	message(FATAL_ERROR "ARCHIVE must name the generated PSP embedded-content archive")
endif()
if(NOT DEFINED OUTPUT)
	message(FATAL_ERROR "OUTPUT must name the generated version header")
endif()

file(SHA256 "${ARCHIVE}" CONTENT_SHA256)
file(WRITE "${OUTPUT}"
	"#pragma once\n\n"
	"inline constexpr char PspEmbeddedContentVersion[] = \"${CONTENT_SHA256}\";\n")
