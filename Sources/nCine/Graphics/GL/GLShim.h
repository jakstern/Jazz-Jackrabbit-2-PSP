#pragma once

// PSP fake-OpenGL shim. pspdev ships pspgl (GL 1.x); nCine targets GL 3.x (UBO/VAO/sync),
// so on PSP we do NOT use real GL. This header provides the exact GL type and enum surface
// nCine's GL wrapper classes reference, purely so they compile; the GL call bodies are stubbed
// and all real drawing is emitted through the PSP GU backend at RenderCommand::Issue.

#include <cstdint>
#include <cstddef>
#include <type_traits>

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef void           GLvoid;
typedef char           GLchar;
typedef std::intptr_t  GLintptr;
typedef std::intptr_t  GLsizeiptr;
typedef std::int64_t   GLint64;
typedef std::uint64_t  GLuint64;
typedef struct __GLsync* GLsync;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);

#ifndef GL_ACTIVE_ATTRIBUTES
#define GL_ACTIVE_ATTRIBUTES 0x1000
#endif
#ifndef GL_ACTIVE_UNIFORM_BLOCKS
#define GL_ACTIVE_UNIFORM_BLOCKS 0x1001
#endif
#ifndef GL_ACTIVE_UNIFORMS
#define GL_ACTIVE_UNIFORMS 0x1002
#endif
#ifndef GL_ALPHA
#define GL_ALPHA 0x1003
#endif
#ifndef GL_AMD_
#define GL_AMD_ 0x1004
#endif
#ifndef GL_ARB_
#define GL_ARB_ 0x1005
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x1006
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x1007
#endif
#ifndef GL_ATC_RGBA_EXPLICIT_ALPHA_AMD
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD 0x1008
#endif
#ifndef GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD 0x1009
#endif
#ifndef GL_ATC_RGB_AMD
#define GL_ATC_RGB_AMD 0x100A
#endif
#ifndef GL_BACK
#define GL_BACK 0x100B
#endif
#ifndef GL_BGR
#define GL_BGR 0x100C
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x100D
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x100E
#endif
#ifndef GL_BLEND
#define GL_BLEND 0x100F
#endif
#ifndef GL_BLUE
#define GL_BLUE 0x1010
#endif
#ifndef GL_BOOL
#define GL_BOOL 0x1011
#endif
#ifndef GL_BOOL_VEC2
#define GL_BOOL_VEC2 0x1012
#endif
#ifndef GL_BOOL_VEC3
#define GL_BOOL_VEC3 0x1013
#endif
#ifndef GL_BOOL_VEC4
#define GL_BOOL_VEC4 0x1014
#endif
#ifndef GL_BUFFER
#define GL_BUFFER 0x1015
#endif
#ifndef GL_BUFFER_KHR
#define GL_BUFFER_KHR 0x1016
#endif
#ifndef GL_BYTE
#define GL_BYTE 0x1017
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x1019
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x101A
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x101B
#endif
#ifndef GL_COLOR_ATTACHMENT2
#define GL_COLOR_ATTACHMENT2 0x101C
#endif
#ifndef GL_COLOR_ATTACHMENT3
#define GL_COLOR_ATTACHMENT3 0x101D
#endif
#ifndef GL_COLOR_ATTACHMENT4
#define GL_COLOR_ATTACHMENT4 0x101E
#endif
#ifndef GL_COLOR_ATTACHMENT5
#define GL_COLOR_ATTACHMENT5 0x101F
#endif
#ifndef GL_COLOR_ATTACHMENT6
#define GL_COLOR_ATTACHMENT6 0x1020
#endif
#ifndef GL_COLOR_ATTACHMENT7
#define GL_COLOR_ATTACHMENT7 0x1021
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x1022
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x1023
#endif
#ifndef GL_COMPRESSED_R11_EAC
#define GL_COMPRESSED_R11_EAC 0x1024
#endif
#ifndef GL_COMPRESSED_RG11_EAC
#define GL_COMPRESSED_RG11_EAC 0x1025
#endif
#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x1026
#endif
#ifndef GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2
#define GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x1027
#endif
#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x1028
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_10
#define GL_COMPRESSED_RGBA_ASTC_10 0x1029
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_12
#define GL_COMPRESSED_RGBA_ASTC_12 0x102A
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_4
#define GL_COMPRESSED_RGBA_ASTC_4 0x102B
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_5
#define GL_COMPRESSED_RGBA_ASTC_5 0x102C
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_6
#define GL_COMPRESSED_RGBA_ASTC_6 0x102D
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_8
#define GL_COMPRESSED_RGBA_ASTC_8 0x102E
#endif
#ifndef GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x102F
#endif
#ifndef GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x1030
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x1031
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x1032
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x1033
#endif
#ifndef GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG 0x1034
#endif
#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x1035
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x1036
#endif
#ifndef GL_CONSTANT_ALPHA
#define GL_CONSTANT_ALPHA 0x1037
#endif
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x1038
#endif
#ifndef GL_CULL_FACE
#define GL_CULL_FACE 0x1039
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x103A
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR
#define GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR 0x103B
#endif
#ifndef GL_DEBUG_SEVERITY_HIGH
#define GL_DEBUG_SEVERITY_HIGH 0x103C
#endif
#ifndef GL_DEBUG_SEVERITY_HIGH_KHR
#define GL_DEBUG_SEVERITY_HIGH_KHR 0x103D
#endif
#ifndef GL_DEBUG_SEVERITY_LOW
#define GL_DEBUG_SEVERITY_LOW 0x103E
#endif
#ifndef GL_DEBUG_SEVERITY_LOW_KHR
#define GL_DEBUG_SEVERITY_LOW_KHR 0x103F
#endif
#ifndef GL_DEBUG_SEVERITY_MEDIUM
#define GL_DEBUG_SEVERITY_MEDIUM 0x1040
#endif
#ifndef GL_DEBUG_SEVERITY_MEDIUM_KHR
#define GL_DEBUG_SEVERITY_MEDIUM_KHR 0x1041
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x1042
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION_KHR
#define GL_DEBUG_SEVERITY_NOTIFICATION_KHR 0x1043
#endif
#ifndef GL_DEBUG_SOURCE_API
#define GL_DEBUG_SOURCE_API 0x1044
#endif
#ifndef GL_DEBUG_SOURCE_API_KHR
#define GL_DEBUG_SOURCE_API_KHR 0x1045
#endif
#ifndef GL_DEBUG_SOURCE_APPLICATION
#define GL_DEBUG_SOURCE_APPLICATION 0x1046
#endif
#ifndef GL_DEBUG_SOURCE_APPLICATION_KHR
#define GL_DEBUG_SOURCE_APPLICATION_KHR 0x1047
#endif
#ifndef GL_DEBUG_SOURCE_OTHER
#define GL_DEBUG_SOURCE_OTHER 0x1048
#endif
#ifndef GL_DEBUG_SOURCE_OTHER_KHR
#define GL_DEBUG_SOURCE_OTHER_KHR 0x1049
#endif
#ifndef GL_DEBUG_SOURCE_SHADER_COMPILER
#define GL_DEBUG_SOURCE_SHADER_COMPILER 0x104A
#endif
#ifndef GL_DEBUG_SOURCE_SHADER_COMPILER_KHR
#define GL_DEBUG_SOURCE_SHADER_COMPILER_KHR 0x104B
#endif
#ifndef GL_DEBUG_SOURCE_THIRD_PARTY
#define GL_DEBUG_SOURCE_THIRD_PARTY 0x104C
#endif
#ifndef GL_DEBUG_SOURCE_THIRD_PARTY_KHR
#define GL_DEBUG_SOURCE_THIRD_PARTY_KHR 0x104D
#endif
#ifndef GL_DEBUG_SOURCE_WINDOW_SYSTEM
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM 0x104E
#endif
#ifndef GL_DEBUG_SOURCE_WINDOW_SYSTEM_KHR
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM_KHR 0x104F
#endif
#ifndef GL_DEBUG_SUPPORTED
#define GL_DEBUG_SUPPORTED 0x1050
#endif
#ifndef GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR 0x1051
#endif
#ifndef GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR 0x1052
#endif
#ifndef GL_DEBUG_TYPE_ERROR
#define GL_DEBUG_TYPE_ERROR 0x1053
#endif
#ifndef GL_DEBUG_TYPE_ERROR_KHR
#define GL_DEBUG_TYPE_ERROR_KHR 0x1054
#endif
#ifndef GL_DEBUG_TYPE_MARKER
#define GL_DEBUG_TYPE_MARKER 0x1055
#endif
#ifndef GL_DEBUG_TYPE_MARKER_KHR
#define GL_DEBUG_TYPE_MARKER_KHR 0x1056
#endif
#ifndef GL_DEBUG_TYPE_OTHER
#define GL_DEBUG_TYPE_OTHER 0x1057
#endif
#ifndef GL_DEBUG_TYPE_OTHER_KHR
#define GL_DEBUG_TYPE_OTHER_KHR 0x1058
#endif
#ifndef GL_DEBUG_TYPE_PERFORMANCE
#define GL_DEBUG_TYPE_PERFORMANCE 0x1059
#endif
#ifndef GL_DEBUG_TYPE_PERFORMANCE_KHR
#define GL_DEBUG_TYPE_PERFORMANCE_KHR 0x105A
#endif
#ifndef GL_DEBUG_TYPE_POP_GROUP
#define GL_DEBUG_TYPE_POP_GROUP 0x105B
#endif
#ifndef GL_DEBUG_TYPE_POP_GROUP_KHR
#define GL_DEBUG_TYPE_POP_GROUP_KHR 0x105C
#endif
#ifndef GL_DEBUG_TYPE_PORTABILITY
#define GL_DEBUG_TYPE_PORTABILITY 0x105D
#endif
#ifndef GL_DEBUG_TYPE_PORTABILITY_KHR
#define GL_DEBUG_TYPE_PORTABILITY_KHR 0x105E
#endif
#ifndef GL_DEBUG_TYPE_PUSH_GROUP
#define GL_DEBUG_TYPE_PUSH_GROUP 0x105F
#endif
#ifndef GL_DEBUG_TYPE_PUSH_GROUP_KHR
#define GL_DEBUG_TYPE_PUSH_GROUP_KHR 0x1060
#endif
#ifndef GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR 0x1061
#endif
#ifndef GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR 0x1062
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x1063
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x1064
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x1065
#endif
#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT 0x1066
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 0x1067
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x1068
#endif
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F 0x1069
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x106A
#endif
#ifndef GL_DEPTH_TEST
#define GL_DEPTH_TEST 0x106B
#endif
#ifndef GL_DITHER
#define GL_DITHER 0x106C
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x106D
#endif
#ifndef GL_DST_ALPHA
#define GL_DST_ALPHA 0x106E
#endif
#ifndef GL_DST_COLOR
#define GL_DST_COLOR 0x106F
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x1070
#endif
#ifndef GL_ES
#define GL_ES 0x1071
#endif
#ifndef GL_ES_VERSION_3_0
#define GL_ES_VERSION_3_0 0x1072
#endif
#ifndef GL_ES_VERSION_3_1
#define GL_ES_VERSION_3_1 0x1073
#endif
#ifndef GL_ES_VERSION_3_2
#define GL_ES_VERSION_3_2 0x1074
#endif
#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x1075
#endif
#ifndef GL_EXT_
#define GL_EXT_ 0x1076
#endif
#ifndef GL_EXTENSIONS
#define GL_EXTENSIONS 0x1077
#endif
#ifndef GL_FALSE
#define GL_FALSE 0
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1078
#endif
#ifndef GL_FLOAT_MAT2
#define GL_FLOAT_MAT2 0x1079
#endif
#ifndef GL_FLOAT_MAT3
#define GL_FLOAT_MAT3 0x107A
#endif
#ifndef GL_FLOAT_MAT4
#define GL_FLOAT_MAT4 0x107B
#endif
#ifndef GL_FLOAT_VEC2
#define GL_FLOAT_VEC2 0x107C
#endif
#ifndef GL_FLOAT_VEC3
#define GL_FLOAT_VEC3 0x107D
#endif
#ifndef GL_FLOAT_VEC4
#define GL_FLOAT_VEC4 0x107E
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x107F
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x1080
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x1081
#endif
#ifndef GL_FRONT
#define GL_FRONT 0x1082
#endif
#ifndef GL_FRONT_AND_BACK
#define GL_FRONT_AND_BACK 0x1083
#endif
#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x1084
#endif
#ifndef GL_GREEN
#define GL_GREEN 0x1085
#endif
#ifndef GL_IMG_
#define GL_IMG_ 0x1086
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x1087
#endif
#ifndef GL_INT
#define GL_INT 0x1088
#endif
#ifndef GL_INT_VEC2
#define GL_INT_VEC2 0x1089
#endif
#ifndef GL_INT_VEC3
#define GL_INT_VEC3 0x108A
#endif
#ifndef GL_INT_VEC4
#define GL_INT_VEC4 0x108B
#endif
#ifndef GL_KHR_
#define GL_KHR_ 0x108C
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x108D
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x108E
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST 0x108F
#endif
#ifndef GL_LINE_STRIP
#define GL_LINE_STRIP 0x1090
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x1091
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x1093
#endif
#ifndef GL_MAP_FLUSH_EXPLICIT_BIT
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x1094
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x1095
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x1096
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x1097
#endif
#ifndef GL_MAX_COLOR_ATTACHMENTS
#define GL_MAX_COLOR_ATTACHMENTS 0x1098
#endif
#ifndef GL_MAX_FRAGMENT_UNIFORM_BLOCKS
#define GL_MAX_FRAGMENT_UNIFORM_BLOCKS 0x1099
#endif
#ifndef GL_MAX_LABEL_LENGTH
#define GL_MAX_LABEL_LENGTH 0x109A
#endif
#ifndef GL_MAX_LABEL_LENGTH_KHR
#define GL_MAX_LABEL_LENGTH_KHR 0x109B
#endif
#ifndef GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x109C
#endif
#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x109D
#endif
#ifndef GL_MAX_UNIFORM_BLOCK_SIZE
#define GL_MAX_UNIFORM_BLOCK_SIZE 0x109E
#endif
#ifndef GL_MAX_UNIFORM_BUFFER_BINDINGS
#define GL_MAX_UNIFORM_BUFFER_BINDINGS 0x109F
#endif
#ifndef GL_MAX_VERTEX_ATTRIBS
#define GL_MAX_VERTEX_ATTRIBS 0x10A0
#endif
#ifndef GL_MAX_VERTEX_ATTRIB_STRIDE
#define GL_MAX_VERTEX_ATTRIB_STRIDE 0x10A1
#endif
#ifndef GL_MAX_VERTEX_UNIFORM_BLOCKS
#define GL_MAX_VERTEX_UNIFORM_BLOCKS 0x10A2
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x10A3
#endif
#ifndef GL_NEAREST
#define GL_NEAREST 0x10A4
#endif
#ifndef GL_NEAREST_MIPMAP_LINEAR
#define GL_NEAREST_MIPMAP_LINEAR 0x10A5
#endif
#ifndef GL_NEAREST_MIPMAP_NEAREST
#define GL_NEAREST_MIPMAP_NEAREST 0x10A6
#endif
#ifndef GL_NO_ERROR
#define GL_NO_ERROR 0
#endif
#ifndef GL_NONE
#define GL_NONE 0
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x10A7
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x10A8
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS_OES
#define GL_NUM_PROGRAM_BINARY_FORMATS_OES 0x10A9
#endif
#ifndef GL_OES_
#define GL_OES_ 0x10AA
#endif
#ifndef GL_ONE
#define GL_ONE 1
#endif
#ifndef GL_ONE_MINUS_CONSTANT_ALPHA
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x10AB
#endif
#ifndef GL_ONE_MINUS_CONSTANT_COLOR
#define GL_ONE_MINUS_CONSTANT_COLOR 0x10AC
#endif
#ifndef GL_ONE_MINUS_DST_ALPHA
#define GL_ONE_MINUS_DST_ALPHA 0x10AD
#endif
#ifndef GL_ONE_MINUS_DST_COLOR
#define GL_ONE_MINUS_DST_COLOR 0x10AE
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x10AF
#endif
#ifndef GL_ONE_MINUS_SRC_COLOR
#define GL_ONE_MINUS_SRC_COLOR 0x10B0
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x10B1
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x10B2
#endif
#ifndef GL_PROGRAM
#define GL_PROGRAM 0x10B3
#endif
#ifndef GL_PROGRAM_BINARY_FORMATS
#define GL_PROGRAM_BINARY_FORMATS 0x10B4
#endif
#ifndef GL_PROGRAM_BINARY_FORMATS_OES
#define GL_PROGRAM_BINARY_FORMATS_OES 0x10B5
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x10B6
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH_OES
#define GL_PROGRAM_BINARY_LENGTH_OES 0x10B7
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x10B8
#endif
#ifndef GL_PROGRAM_KHR
#define GL_PROGRAM_KHR 0x10B9
#endif
#ifndef GL_PROGRAM_PIPELINE
#define GL_PROGRAM_PIPELINE 0x10BA
#endif
#ifndef GL_PROGRAM_PIPELINE_KHR
#define GL_PROGRAM_PIPELINE_KHR 0x10BB
#endif
#ifndef GL_QUERY
#define GL_QUERY 0x10BC
#endif
#ifndef GL_QUERY_KHR
#define GL_QUERY_KHR 0x10BD
#endif
#ifndef GL_R8
#define GL_R8 0x10BE
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x10BF
#endif
#ifndef GL_RED
#define GL_RED 0x10C0
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x10C1
#endif
#ifndef GL_RENDERER
#define GL_RENDERER 0x10C2
#endif
#ifndef GL_REPEAT
#define GL_REPEAT 0x10C3
#endif
#ifndef GL_RG
#define GL_RG 0x10C4
#endif
#ifndef GL_RG8
#define GL_RG8 0x10C5
#endif
#ifndef GL_RGB
#define GL_RGB 0x10C6
#endif
#ifndef GL_RGB16F
#define GL_RGB16F 0x10C7
#endif
#ifndef GL_RGB32F
#define GL_RGB32F 0x10C8
#endif
#ifndef GL_RGB565
#define GL_RGB565 0x10C9
#endif
#ifndef GL_RGB5_A1
#define GL_RGB5_A1 0x10CA
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x10CB
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x10CC
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x10CD
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x10CE
#endif
#ifndef GL_RGBA4
#define GL_RGBA4 0x10CF
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x10D0
#endif
#ifndef GL_SAMPLER
#define GL_SAMPLER 0x10D1
#endif
#ifndef GL_SAMPLER_1D
#define GL_SAMPLER_1D 0x10D2
#endif
#ifndef GL_SAMPLER_2D
#define GL_SAMPLER_2D 0x10D3
#endif
#ifndef GL_SAMPLER_3D
#define GL_SAMPLER_3D 0x10D4
#endif
#ifndef GL_SAMPLER_BUFFER
#define GL_SAMPLER_BUFFER 0x10D5
#endif
#ifndef GL_SAMPLER_CUBE
#define GL_SAMPLER_CUBE 0x10D6
#endif
#ifndef GL_SAMPLER_KHR
#define GL_SAMPLER_KHR 0x10D7
#endif
#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x10D8
#endif
#ifndef GL_SHADER
#define GL_SHADER 0x10D9
#endif
#ifndef GL_SHADER_KHR
#define GL_SHADER_KHR 0x10DA
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x10DB
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x10DC
#endif
#ifndef GL_SRC_ALPHA
#define GL_SRC_ALPHA 0x10DD
#endif
#ifndef GL_SRC_ALPHA_SATURATE
#define GL_SRC_ALPHA_SATURATE 0x10DE
#endif
#ifndef GL_SRC_COLOR
#define GL_SRC_COLOR 0x10DF
#endif
#ifndef GL_STENCIL_BUFFER_BIT
#define GL_STENCIL_BUFFER_BIT 0x10E0
#endif
#ifndef GL_STENCIL_TEST
#define GL_STENCIL_TEST 0x10E1
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x10E2
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x10E3
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x10E4
#endif
#ifndef GL_TEXTURE
#define GL_TEXTURE 0x10E5
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x10E6
#endif
#ifndef GL_TEXTURE_1D
#define GL_TEXTURE_1D 0x10E7
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x10E8
#endif
#ifndef GL_TEXTURE_3D
#define GL_TEXTURE_3D 0x10E9
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x10EA
#endif
#ifndef GL_TEXTURE_BUFFER
#define GL_TEXTURE_BUFFER 0x10EB
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x10EC
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x10ED
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x10EE
#endif
#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A 0x10EF
#endif
#ifndef GL_TEXTURE_SWIZZLE_B
#define GL_TEXTURE_SWIZZLE_B 0x10F0
#endif
#ifndef GL_TEXTURE_SWIZZLE_G
#define GL_TEXTURE_SWIZZLE_G 0x10F1
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x10F2
#endif
#ifndef GL_TEXTURE_SWIZZLE_RGBA
#define GL_TEXTURE_SWIZZLE_RGBA 0x10F3
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x10F4
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x10F5
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x10F6
#endif
#ifndef GL_TRANSFORM_FEEDBACK
#define GL_TRANSFORM_FEEDBACK 0x10F7
#endif
#ifndef GL_TRIANGLES
#define GL_TRIANGLES 0x10F8
#endif
#ifndef GL_TRIANGLE_STRIP
#define GL_TRIANGLE_STRIP 0x10F9
#endif
#ifndef GL_TRUE
#define GL_TRUE 1
#endif
#ifndef GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES 0x10FA
#endif
#ifndef GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS 0x10FB
#endif
#ifndef GL_UNIFORM_BLOCK_DATA_SIZE
#define GL_UNIFORM_BLOCK_DATA_SIZE 0x10FC
#endif
#ifndef GL_UNIFORM_BLOCK_INDEX
#define GL_UNIFORM_BLOCK_INDEX 0x10FD
#endif
#ifndef GL_UNIFORM_BLOCK_NAME_LENGTH
#define GL_UNIFORM_BLOCK_NAME_LENGTH 0x10FE
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x10FF
#endif
#ifndef GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x1100
#endif
#ifndef GL_UNIFORM_NAME_LENGTH
#define GL_UNIFORM_NAME_LENGTH 0x1101
#endif
#ifndef GL_UNIFORM_OFFSET
#define GL_UNIFORM_OFFSET 0x1102
#endif
#ifndef GL_UNIFORM_SIZE
#define GL_UNIFORM_SIZE 0x1103
#endif
#ifndef GL_UNIFORM_TYPE
#define GL_UNIFORM_TYPE 0x1104
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x1105
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x1106
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1107
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1108
#endif
#ifndef GL_UNSIGNED_INT_VEC2
#define GL_UNSIGNED_INT_VEC2 0x1109
#endif
#ifndef GL_UNSIGNED_INT_VEC3
#define GL_UNSIGNED_INT_VEC3 0x110A
#endif
#ifndef GL_UNSIGNED_INT_VEC4
#define GL_UNSIGNED_INT_VEC4 0x110B
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x110C
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x110D
#endif
#ifndef GL_UNSIGNED_SHORT_5_5_5_1
#define GL_UNSIGNED_SHORT_5_5_5_1 0x110E
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x110F
#endif
#ifndef GL_VALIDATE_STATUS
#define GL_VALIDATE_STATUS 0x1110
#endif
#ifndef GL_VENDOR
#define GL_VENDOR 0x1111
#endif
#ifndef GL_VERSION
#define GL_VERSION 0x1112
#endif
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x1113
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x1114
#endif
#ifndef GL_VERTEX_ARRAY_KHR
#define GL_VERTEX_ARRAY_KHR 0x1115
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x1116
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x1117
#endif
#ifndef GL_ZERO
#define GL_ZERO 0
#endif

// ---- Null-GL function layer (PSP) ----
// nCine's GL wrapper .cpp call the OpenGL API directly. PSP has no GL 3.x, so each such call
// resolves to a no-op variadic function template returning a type-erased Sink. Templates (not
// macros) are used so these never shadow nCine's own gl-prefixed helpers, which stay real.
namespace nCine { namespace PspGl {
	struct Sink {
		template<class T, class = typename std::enable_if<std::is_arithmetic<T>::value || std::is_enum<T>::value>::type>
		operator T() const { return T(); }
		// glMapBuffer/glMapBufferRange return a pointer the engine writes per-frame uniform/vertex data
		// into. On PSP that GL upload is a no-op, but the write must land somewhere valid, so hand back a
		// shared scratch buffer instead of null (the data is never read back - drawing goes through GU).
		operator void*() const { static unsigned char scratch[2 * 1024 * 1024]; return scratch; }
		operator GLsync() const { return nullptr; }
		operator const GLubyte*() const { static const GLubyte e[1] = {0}; return e; }
		operator const char*() const { return ""; }
	};
} }
template<class... A> inline ::nCine::PspGl::Sink glActiveTexture(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glAttachShader(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindBuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindBufferBase(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindBufferRange(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindFramebuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindRenderbuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindTexture(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBindVertexArray(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBlendEquation(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBlendFunc(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBlendFuncSeparate(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBufferData(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBufferStorage(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glBufferSubData(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCheckFramebufferStatus(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glClear(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glClearColor(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glClientWaitSync(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCompileShader(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCompressedTexImage2D(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCompressedTexSubImage2D(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCreateProgram(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCreateShader(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glCullFace(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDebugMessageCallback(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDebugMessageCallbackKHR(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDebugMessageInsert(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDebugMessageInsertKHR(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteBuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteFramebuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteProgram(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteRenderbuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteShader(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteSync(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteTextures(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDeleteVertexArrays(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDepthMask(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDetachShader(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDisable(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawArrays(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawArraysInstanced(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawBuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawElements(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawElementsBaseVertex(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawElementsInstanced(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glDrawElementsInstancedBaseVertex(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glEnable(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glEnableVertexAttribArray(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glFenceSync(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glFilter(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glFlushMappedBufferRange(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glFramebufferRenderbuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glFramebufferTexture2D(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGenBuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGenFramebuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGenRenderbuffers(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGenTextures(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGenVertexArrays(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetActiveAttrib(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetActiveUniform(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetActiveUniformBlockiv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetActiveUniformBlockName(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetActiveUniformName(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetActiveUniformsiv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetAttribLocation(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetError(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetIntegerv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetObjectLabel(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetObjectLabelKHR(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetProgramBinary(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetProgramBinaryOES(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetProgramInfoLog(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetProgramiv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetShaderInfoLog(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetShaderiv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetString(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetStringi(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetTexImage(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glGetUniformLocation(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glInfoStrings(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glInvalidateFramebuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glLinkProgram(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glMajor(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glMapBufferRange(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glMinor(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glObjectLabel(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glObjectLabelKHR(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glPixelStorei(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glPopDebugGroup(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glPopDebugGroupKHR(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glProgramBinary(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glProgramBinaryOES(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glProgramParameteri(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glPushDebugGroup(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glPushDebugGroupKHR(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glRenderbufferStorage(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glScissor(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glShaderSource(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexBuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexImage2D(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexParameterf(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexParameteri(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexParameteriv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexStorage2D(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glTexSubImage2D(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform1fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform1i(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform1iv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform2fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform2iv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform3fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform3iv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform4fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniform4iv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniformBlockBinding(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniformMatrix2fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniformMatrix3fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUniformMatrix4fv(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUnmapBuffer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glUseProgram(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glValidateProgram(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glVertexAttribIPointer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glVertexAttribPointer(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glViewport(A&&...) { return {}; }
template<class... A> inline ::nCine::PspGl::Sink glWrap(A&&...) { return {}; }
