idf_component_register(
    SRCS
        src/ALACDecoder.cpp
        src/ALACBitUtilities.c
        src/EndianPortable.c
        src/ag_dec.c
        src/dp_dec.c
        src/matrix_dec.c
        src/alac_wrapper.cpp
    INCLUDE_DIRS include
)

set_source_files_properties(src/ALACDecoder.cpp PROPERTIES COMPILE_FLAGS -Wno-unused-parameter)
