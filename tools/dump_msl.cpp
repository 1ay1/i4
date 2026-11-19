// tools/dump_msl.cpp — print mayag's Metal shader source (for `xcrun metal`
// in CI: the shader is otherwise compiled only at run time, on a Mac).
#include <cstdio>
#include <string>

#include <mayag/render/shader_source.hpp>

int main(int argc, char** argv) {
    const bool fetch = argc > 1 && std::string(argv[1]) == "1";
    std::printf("#define MG_FB_FETCH %d\n%s%s", fetch ? 1 : 0, mayag::render::shaders::msl_kernel.data(),
                mayag::render::shaders::msl_shaders.data());
}
