find_package(Threads REQUIRED)
add_library(anyps5_host_runtime INTERFACE)
target_link_libraries(anyps5_host_runtime INTERFACE Threads::Threads)

foreach(submodule IN ITEMS glslang SPIRV-Headers stb libjpeg-turbo)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/3rdparty/${submodule}/CMakeLists.txt" AND
       NOT EXISTS "${PROJECT_SOURCE_DIR}/3rdparty/${submodule}/stb_image.h")
        message(FATAL_ERROR "Missing ${submodule} submodule. Run git submodule update --init --recursive.")
    endif()
endforeach()

add_shader_recompiler(shader_recompiler_host anyps5_host_runtime)
add_png_decoder(png_decoder_host anyps5_host_runtime)
add_jpeg_decoder(jpeg_decoder_host anyps5_host_runtime)
if(APPLE)
    add_subdirectory(metal)
    add_subdirectory(arm64)
endif()

set(agcSourceDirectory "${PROJECT_SOURCE_DIR}/core/libs/prx/libSceAgcDriver")
add_executable(agc_shader_replay "${agcSourceDirectory}/tools/ShaderReplay.cpp")
target_link_libraries(agc_shader_replay PRIVATE shader_recompiler_host)
if(ANYPS5_ENABLE_SPIRV_TOOLS)
    target_link_libraries(agc_shader_replay PRIVATE SPIRV-Tools-static)
endif()

if(BUILD_TESTING)
    foreach(shaderTest IN ITEMS Structurizer SpirvModule DppWrite MaskedSelect
            Vop1ControlDecode ValidateProgram ShaderDiskCache ControlFlow
            RdnaTruncatedModifier Vop1SdwaMoveDecode PixelPositionExport)
        add_test_executable(host_${shaderTest}_tests "${agcSourceDirectory}/tests/${shaderTest}.cpp")
        target_link_libraries(host_${shaderTest}_tests PRIVATE shader_recompiler_host)
        add_test(NAME host_${shaderTest} COMMAND host_${shaderTest}_tests)
    endforeach()
endif()
