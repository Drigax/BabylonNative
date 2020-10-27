#include "ShaderCompiler.h"
#include "ShaderCompilerCommon.h"
#include "ShaderCompilerTraversers.h"
#include "ResourceLimits.h"
#include <arcana/experimental/array.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_parser.hpp>
#include <spirv_glsl.hpp>
#include <bgfx/bgfx.h>

namespace bgfx::gl
{
    void setAttribName(bgfx::Attrib::Enum, const char*);
}

namespace Babylon
{
    extern const TBuiltInResource DefaultTBuiltInResource;

    namespace
    {
        void AddShader(glslang::TProgram& program, glslang::TShader& shader, std::string_view source)
        {
            const std::array<const char*, 1> sources{source.data()};
            shader.setStrings(sources.data(), gsl::narrow_cast<int>(sources.size()));

            if (!shader.parse(&DefaultTBuiltInResource, 310, EProfile::EEsProfile, true, true, EShMsgDefault))
            {
                throw std::runtime_error(shader.getInfoDebugLog());
            }

            program.addShader(&shader);
        }

        std::pair<std::unique_ptr<spirv_cross::Parser>, std::unique_ptr<spirv_cross::Compiler>> CompileShader(glslang::TProgram& program, EShLanguage stage, std::string& glsl)
        {
            std::vector<uint32_t> spirv;
            glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);

            auto parser = std::make_unique<spirv_cross::Parser>(std::move(spirv));
            parser->parse();

            auto compiler = std::make_unique<spirv_cross::CompilerGLSL>(parser->get_parsed_ir());

            spirv_cross::CompilerGLSL::Options options = compiler->get_common_options();

            options.version = 300;
            options.es = true;

            compiler->set_common_options(options);

            glsl = compiler->compile();

            return{std::move(parser), std::move(compiler)};
        }
    }

    ShaderCompiler::ShaderCompiler()
    {
        bgfx::gl::setAttribName(bgfx::Attrib::Position, "position");
        bgfx::gl::setAttribName(bgfx::Attrib::Normal, "normal");
        bgfx::gl::setAttribName(bgfx::Attrib::Tangent, "tangent");
        bgfx::gl::setAttribName(bgfx::Attrib::Bitangent, "__unsupported__");
        bgfx::gl::setAttribName(bgfx::Attrib::Color0, "color");
        bgfx::gl::setAttribName(bgfx::Attrib::Color1, "__unsupported__");
        bgfx::gl::setAttribName(bgfx::Attrib::Color2, "__unsupported__");
        bgfx::gl::setAttribName(bgfx::Attrib::Color3, "__unsupported__");
        bgfx::gl::setAttribName(bgfx::Attrib::Indices, "matricesIndices");
        bgfx::gl::setAttribName(bgfx::Attrib::Weight, "matricesWeights");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord0, "uv");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord1, "uv2");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord2, "uv3");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord3, "uv4");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord4, "uv5");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord5, "uv6");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord6, "__unsupported__");
        bgfx::gl::setAttribName(bgfx::Attrib::TexCoord7, "__unsupported__");

        glslang::InitializeProcess();
    }

    ShaderCompiler::~ShaderCompiler()
    {
        glslang::FinalizeProcess();
    }

    ShaderCompiler::BgfxShaderInfo ShaderCompiler::Compile(std::string_view vertexSource, std::string_view fragmentSource)
    {
        glslang::TProgram program;

        glslang::TShader vertexShader{EShLangVertex};
        AddShader(program, vertexShader, vertexSource);

        glslang::TShader fragmentShader{EShLangFragment};
        AddShader(program, fragmentShader, fragmentSource);

        glslang::SpvVersion spv{};
        spv.spv = 0x10000;
        vertexShader.getIntermediate()->setSpv(spv);
        fragmentShader.getIntermediate()->setSpv(spv);

        if (!program.link(EShMsgDefault))
        {
            throw std::exception();
        }

        ShaderCompilerTraversers::IdGenerator ids{};
        auto cutScope = ShaderCompilerTraversers::ChangeUniformTypes(program, ids);
        std::unordered_map<std::string, std::string> replacementNameToOriginal = {};
        ShaderCompilerTraversers::AssignLocationsAndNamesToVertexVaryings(program, ids, replacementNameToOriginal);

        std::string vertexGLSL(vertexSource.data(), vertexSource.size());
        auto [vertexParser, vertexCompiler] = CompileShader(program, EShLangVertex, vertexGLSL);

        std::string fragmentGLSL(fragmentSource.data(), fragmentSource.size());
        auto [fragmentParser, fragmentCompiler] = CompileShader(program, EShLangFragment, fragmentGLSL);

        return ShaderCompilerCommon::CreateBgfxShader(
            {std::move(vertexParser), std::move(vertexCompiler), gsl::make_span(reinterpret_cast<uint8_t*>(vertexGLSL.data()), vertexGLSL.size()), std::move(replacementNameToOriginal)},
            {std::move(fragmentParser), std::move(fragmentCompiler), gsl::make_span(reinterpret_cast<uint8_t*>(fragmentGLSL.data()), fragmentGLSL.size()), {}});
    }
}
