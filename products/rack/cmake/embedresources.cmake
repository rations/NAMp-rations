# Generate the embedded-resources translation unit: every art and font file as a byte array, plus
# the table src/gfx/resourcestore.cpp searches.
#
# Run with `cmake -P`, from the custom command in the top-level CMakeLists:
#
#   -DNAMPRACK_EMBED_ROOT=<dir>       the directory the paths below are relative to
#   -DNAMPRACK_EMBED_FILES=a;b;c      relative paths ("img/base.png"), the same list the bundle copies
#   -DNAMPRACK_EMBED_OUTPUT=<file>    the .cpp to write
#
# Byte arrays rather than an assembler .incbin or an ld -r -b binary blob: no extra toolchain
# language to enable, and the cost was measured in the donor project rather than assumed — 0.55 s
# to generate and 0.69 s to compile its largest layer at 960 KB. This project's largest is the
# cabinet at a comparable size, and the set only regenerates when a resource file changes.

foreach(_var NAMPRACK_EMBED_ROOT NAMPRACK_EMBED_FILES NAMPRACK_EMBED_OUTPUT)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "embedresources.cmake: -D${_var} is required")
    endif()
endforeach()

set(_body "")
set(_table "")
set(_index 0)

foreach(_rel IN LISTS NAMPRACK_EMBED_FILES)
    set(_path "${NAMPRACK_EMBED_ROOT}/${_rel}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "embedresources.cmake: missing resource ${_path}")
    endif()

    file(READ "${_path}" _hex HEX)
    string(LENGTH "${_hex}" _hexlen)
    if(_hexlen EQUAL 0)
        message(FATAL_ERROR "embedresources.cmake: ${_path} is empty")
    endif()
    string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")

    string(APPEND _body "const unsigned char kData${_index}[] = {${_bytes}};\n")
    string(APPEND _table "    {\"${_rel}\", kData${_index}, sizeof(kData${_index})},\n")
    math(EXPR _index "${_index} + 1")
endforeach()

if(_index EQUAL 0)
    message(FATAL_ERROR "embedresources.cmake: no files given")
endif()

file(WRITE "${NAMPRACK_EMBED_OUTPUT}"
"// GENERATED FILE - DO NOT EDIT.
//
// Written by cmake/embedresources.cmake from the project's resources directory. See
// src/gfx/resourcestore.h for what this is and why the plug-in does not link it.

#include \"gfx/resourcestore.h\"

namespace
{

${_body}
const Rations::EmbeddedResource kTable[] = {
${_table}};

} // namespace

namespace Rations
{

void installBuiltinResources()
{
    installEmbeddedResources(kTable, sizeof(kTable) / sizeof(kTable[0]));
}

} // namespace Rations
")
