/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#define SK_OPTS_NS skslc_standalone
#include "include/core/SkGraphics.h"
#include "include/core/SkStream.h"
#include "include/private/SkStringView.h"
#include "src/core/SkCpu.h"
#include "src/core/SkOpts.h"
#include "src/opts/SkChecksum_opts.h"
#include "src/opts/SkVM_opts.h"
#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/SkSLFileOutputStream.h"
#include "src/sksl/SkSLStringStream.h"
#include "src/sksl/SkSLUtil.h"
#include "src/sksl/codegen/SkSLPipelineStageCodeGenerator.h"
#include "src/sksl/codegen/SkSLVMCodeGenerator.h"
#include "src/sksl/ir/SkSLProgram.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"
#include "src/sksl/tracing/SkVMDebugTrace.h"
#include "src/utils/SkShaderUtils.h"
#include "src/utils/SkVMVisualizer.h"

#include "spirv-tools/libspirv.hpp"

#include <fstream>
#include <limits.h>
#include <optional>
#include <stdarg.h>
#include <stdio.h>

void SkDebugf(const char format[], ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

namespace SkOpts {
    decltype(hash_fn) hash_fn = skslc_standalone::hash_fn;
    decltype(interpret_skvm) interpret_skvm = skslc_standalone::interpret_skvm;
}

enum class ResultCode {
    kSuccess = 0,
    kCompileError = 1,
    kInputError = 2,
    kOutputError = 3,
    kConfigurationError = 4,
};

static std::unique_ptr<SkWStream> as_SkWStream(SkSL::OutputStream& s) {
    struct Adapter : public SkWStream {
    public:
        Adapter(SkSL::OutputStream& out) : fOut(out), fBytesWritten(0) {}

        bool write(const void* buffer, size_t size) override {
            fOut.write(buffer, size);
            fBytesWritten += size;
            return true;
        }
        void flush() override {}
        size_t bytesWritten() const override { return fBytesWritten; }

    private:
        SkSL::OutputStream& fOut;
        size_t fBytesWritten;
    };

    return std::make_unique<Adapter>(s);
}

static bool consume_suffix(std::string* str, const char suffix[]) {
    if (!skstd::ends_with(*str, suffix)) {
        return false;
    }
    str->resize(str->length() - strlen(suffix));
    return true;
}

class ShaderCapsTestFactory : public SkSL::ShaderCapsFactory {
public:
    static std::unique_ptr<SkSL::ShaderCaps> AddAndTrueToLoopCondition() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fAddAndTrueToLoopCondition = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> BuiltinDeterminantSupport() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fBuiltinDeterminantSupport = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> BuiltinFMASupport() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fBuiltinFMASupport = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> CannotUseFractForNegativeValues() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fCanUseFractForNegativeValues = false;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> CannotUseFragCoord() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fCanUseFragCoord = false;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> CannotUseMinAndAbsTogether() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fCanUseMinAndAbsTogether = false;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> EmulateAbsIntFunction() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fEmulateAbsIntFunction = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> FramebufferFetchSupport() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fFBFetchSupport = true;
        result->fFBFetchColorName = "gl_LastFragData[0]";
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> IncompleteShortIntPrecision() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 310es";
        result->fUsesPrecisionModifiers = true;
        result->fIncompleteShortIntPrecision = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> MustForceNegatedAtanParamToFloat() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fMustForceNegatedAtanParamToFloat = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> MustForceNegatedLdexpParamToMultiply() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fMustForceNegatedLdexpParamToMultiply = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> MustGuardDivisionEvenAfterExplicitZeroCheck() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fMustGuardDivisionEvenAfterExplicitZeroCheck = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> RemovePowWithConstantExponent() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fRemovePowWithConstantExponent = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> RewriteDoWhileLoops() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fRewriteDoWhileLoops = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> RewriteMatrixComparisons() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fRewriteMatrixComparisons = true;
        result->fUsesPrecisionModifiers = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> RewriteMatrixVectorMultiply() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fRewriteMatrixVectorMultiply = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> RewriteSwitchStatements() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fRewriteSwitchStatements = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> SampleMaskSupport() {
        std::unique_ptr<SkSL::ShaderCaps> result = Default();
        result->fSampleMaskSupport = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> ShaderDerivativeExtensionString() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fShaderDerivativeSupport = true;
        result->fShaderDerivativeExtensionString = "GL_OES_standard_derivatives";
        result->fUsesPrecisionModifiers = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> UnfoldShortCircuitAsTernary() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fUnfoldShortCircuitAsTernary = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> UsesPrecisionModifiers() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 400";
        result->fUsesPrecisionModifiers = true;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> Version110() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 110";
        result->fGLSLGeneration = SkSL::GLSLGeneration::k110;
        return result;
    }

    static std::unique_ptr<SkSL::ShaderCaps> Version450Core() {
        std::unique_ptr<SkSL::ShaderCaps> result = MakeShaderCaps();
        result->fVersionDeclString = "#version 450 core";
        return result;
    }
};

// Given a string containing an SkSL program, searches for a #pragma settings comment, like so:
//    /*#pragma settings Default Sharpen*/
// The passed-in Settings object will be updated accordingly. Any number of options can be provided.
static bool detect_shader_settings(const std::string& text,
                                   SkSL::ProgramSettings* settings,
                                   const SkSL::ShaderCaps** caps,
                                   std::unique_ptr<SkSL::SkVMDebugTrace>* debugTrace) {
    using Factory = ShaderCapsTestFactory;

    // Find a matching comment and isolate the name portion.
    static constexpr char kPragmaSettings[] = "/*#pragma settings ";
    const char* settingsPtr = strstr(text.c_str(), kPragmaSettings);
    if (settingsPtr != nullptr) {
        // Subtract one here in order to preserve the leading space, which is necessary to allow
        // consumeSuffix to find the first item.
        settingsPtr += strlen(kPragmaSettings) - 1;

        const char* settingsEnd = strstr(settingsPtr, "*/");
        if (settingsEnd != nullptr) {
            std::string settingsText{settingsPtr, size_t(settingsEnd - settingsPtr)};

            // Apply settings as requested. Since they can come in any order, repeat until we've
            // consumed them all.
            for (;;) {
                const size_t startingLength = settingsText.length();

                if (consume_suffix(&settingsText, " AddAndTrueToLoopCondition")) {
                    static SkSL::ShaderCaps* s_addAndTrueCaps =
                            Factory::AddAndTrueToLoopCondition().release();
                    *caps = s_addAndTrueCaps;
                }
                if (consume_suffix(&settingsText, " BuiltinDeterminantSupport")) {
                    static SkSL::ShaderCaps* s_detCaps =
                            Factory::BuiltinDeterminantSupport().release();
                    *caps = s_detCaps;
                }
                if (consume_suffix(&settingsText, " BuiltinFMASupport")) {
                    static SkSL::ShaderCaps* s_fmaCaps = Factory::BuiltinFMASupport().release();
                    *caps = s_fmaCaps;
                }
                if (consume_suffix(&settingsText, " CannotUseFractForNegativeValues")) {
                    static SkSL::ShaderCaps* s_negativeFractCaps =
                            Factory::CannotUseFractForNegativeValues().release();
                    *caps = s_negativeFractCaps;
                }
                if (consume_suffix(&settingsText, " CannotUseFragCoord")) {
                    static SkSL::ShaderCaps* s_noFragCoordCaps =
                            Factory::CannotUseFragCoord().release();
                    *caps = s_noFragCoordCaps;
                }
                if (consume_suffix(&settingsText, " CannotUseMinAndAbsTogether")) {
                    static SkSL::ShaderCaps* s_minAbsCaps =
                            Factory::CannotUseMinAndAbsTogether().release();
                    *caps = s_minAbsCaps;
                }
                if (consume_suffix(&settingsText, " Default")) {
                    static SkSL::ShaderCaps* s_defaultCaps = Factory::Default().release();
                    *caps = s_defaultCaps;
                }
                if (consume_suffix(&settingsText, " EmulateAbsIntFunction")) {
                    static SkSL::ShaderCaps* s_emulateAbsIntCaps =
                            Factory::EmulateAbsIntFunction().release();
                    *caps = s_emulateAbsIntCaps;
                }
                if (consume_suffix(&settingsText, " FramebufferFetchSupport")) {
                    static SkSL::ShaderCaps* s_fbFetchSupport =
                            Factory::FramebufferFetchSupport().release();
                    *caps = s_fbFetchSupport;
                }
                if (consume_suffix(&settingsText, " IncompleteShortIntPrecision")) {
                    static SkSL::ShaderCaps* s_incompleteShortIntCaps =
                            Factory::IncompleteShortIntPrecision().release();
                    *caps = s_incompleteShortIntCaps;
                }
                if (consume_suffix(&settingsText, " MustGuardDivisionEvenAfterExplicitZeroCheck")) {
                    static SkSL::ShaderCaps* s_div0Caps =
                            Factory::MustGuardDivisionEvenAfterExplicitZeroCheck().release();
                    *caps = s_div0Caps;
                }
                if (consume_suffix(&settingsText, " MustForceNegatedAtanParamToFloat")) {
                    static SkSL::ShaderCaps* s_negativeAtanCaps =
                            Factory::MustForceNegatedAtanParamToFloat().release();
                    *caps = s_negativeAtanCaps;
                }
                if (consume_suffix(&settingsText, " MustForceNegatedLdexpParamToMultiply")) {
                    static SkSL::ShaderCaps* s_negativeLdexpCaps =
                            Factory::MustForceNegatedLdexpParamToMultiply().release();
                    *caps = s_negativeLdexpCaps;
                }
                if (consume_suffix(&settingsText, " RemovePowWithConstantExponent")) {
                    static SkSL::ShaderCaps* s_powCaps =
                            Factory::RemovePowWithConstantExponent().release();
                    *caps = s_powCaps;
                }
                if (consume_suffix(&settingsText, " RewriteDoWhileLoops")) {
                    static SkSL::ShaderCaps* s_rewriteLoopCaps =
                            Factory::RewriteDoWhileLoops().release();
                    *caps = s_rewriteLoopCaps;
                }
                if (consume_suffix(&settingsText, " RewriteSwitchStatements")) {
                    static SkSL::ShaderCaps* s_rewriteSwitchCaps =
                            Factory::RewriteSwitchStatements().release();
                    *caps = s_rewriteSwitchCaps;
                }
                if (consume_suffix(&settingsText, " RewriteMatrixVectorMultiply")) {
                    static SkSL::ShaderCaps* s_rewriteMatVecMulCaps =
                            Factory::RewriteMatrixVectorMultiply().release();
                    *caps = s_rewriteMatVecMulCaps;
                }
                if (consume_suffix(&settingsText, " RewriteMatrixComparisons")) {
                    static SkSL::ShaderCaps* s_rewriteMatrixComparisons =
                            Factory::RewriteMatrixComparisons().release();
                    *caps = s_rewriteMatrixComparisons;
                }
                if (consume_suffix(&settingsText, " ShaderDerivativeExtensionString")) {
                    static SkSL::ShaderCaps* s_derivativeCaps =
                            Factory::ShaderDerivativeExtensionString().release();
                    *caps = s_derivativeCaps;
                }
                if (consume_suffix(&settingsText, " UnfoldShortCircuitAsTernary")) {
                    static SkSL::ShaderCaps* s_ternaryCaps =
                            Factory::UnfoldShortCircuitAsTernary().release();
                    *caps = s_ternaryCaps;
                }
                if (consume_suffix(&settingsText, " UsesPrecisionModifiers")) {
                    static SkSL::ShaderCaps* s_precisionCaps =
                            Factory::UsesPrecisionModifiers().release();
                    *caps = s_precisionCaps;
                }
                if (consume_suffix(&settingsText, " Version110")) {
                    static SkSL::ShaderCaps* s_version110Caps = Factory::Version110().release();
                    *caps = s_version110Caps;
                }
                if (consume_suffix(&settingsText, " Version450Core")) {
                    static SkSL::ShaderCaps* s_version450CoreCaps =
                            Factory::Version450Core().release();
                    *caps = s_version450CoreCaps;
                }
                if (consume_suffix(&settingsText, " AllowNarrowingConversions")) {
                    settings->fAllowNarrowingConversions = true;
                }
                if (consume_suffix(&settingsText, " ForceHighPrecision")) {
                    settings->fForceHighPrecision = true;
                }
                if (consume_suffix(&settingsText, " NoInline")) {
                    settings->fInlineThreshold = 0;
                }
                if (consume_suffix(&settingsText, " NoOptimize")) {
                    settings->fOptimize = false;
                    settings->fInlineThreshold = 0;
                }
                if (consume_suffix(&settingsText, " NoRTFlip")) {
                    settings->fForceNoRTFlip = true;
                }
                if (consume_suffix(&settingsText, " NoTraceVarInSkVMDebugTrace")) {
                    settings->fAllowTraceVarInSkVMDebugTrace = false;
                }
                if (consume_suffix(&settingsText, " InlineThresholdMax")) {
                    settings->fInlineThreshold = INT_MAX;
                }
                if (consume_suffix(&settingsText, " Sharpen")) {
                    settings->fSharpenTextures = true;
                }
                if (consume_suffix(&settingsText, " SkVMDebugTrace")) {
                    settings->fOptimize = false;
                    *debugTrace = std::make_unique<SkSL::SkVMDebugTrace>();
                }

                if (settingsText.empty()) {
                    break;
                }
                if (settingsText.length() == startingLength) {
                    printf("Unrecognized #pragma settings: %s\n", settingsText.c_str());
                    return false;
                }
            }
        }
    }

    return true;
}

/**
 * Displays a usage banner; used when the command line arguments don't make sense.
 */
static void show_usage() {
    printf("usage: skslc <input> <output> <flags>\n"
           "       skslc <worklist>\n"
           "\n"
           "Allowed flags:\n"
           "--settings:   honor embedded /*#pragma settings*/ comments.\n"
           "--nosettings: ignore /*#pragma settings*/ comments\n");
}

static bool set_flag(std::optional<bool>* flag, const char* name, bool value) {
    if (flag->has_value()) {
        printf("%s flag was specified multiple times\n", name);
        return false;
    }
    *flag = value;
    return true;
}

/**
 * Handle a single input.
 */
ResultCode processCommand(const std::vector<std::string>& args) {
    std::optional<bool> honorSettings;
    std::vector<std::string> paths;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--settings") {
            if (!set_flag(&honorSettings, "settings", true)) {
                return ResultCode::kInputError;
            }
        } else if (arg == "--nosettings") {
            if (!set_flag(&honorSettings, "settings", false)) {
                return ResultCode::kInputError;
            }
        } else if (!skstd::starts_with(arg, "--")) {
            paths.push_back(arg);
        } else {
            show_usage();
            return ResultCode::kInputError;
        }
    }
    if (paths.size() != 2) {
        show_usage();
        return ResultCode::kInputError;
    }

    if (!honorSettings.has_value()) {
        honorSettings = true;
    }

    const std::string& inputPath = paths[0];
    const std::string& outputPath = paths[1];
    SkSL::ProgramKind kind;
    if (skstd::ends_with(inputPath, ".vert")) {
        kind = SkSL::ProgramKind::kVertex;
    } else if (skstd::ends_with(inputPath, ".frag") || skstd::ends_with(inputPath, ".sksl")) {
        kind = SkSL::ProgramKind::kFragment;
    } else if (skstd::ends_with(inputPath, ".compute")) {
        kind = SkSL::ProgramKind::kCompute;
    } else if (skstd::ends_with(inputPath, ".rtb")) {
        kind = SkSL::ProgramKind::kRuntimeBlender;
    } else if (skstd::ends_with(inputPath, ".rtcf")) {
        kind = SkSL::ProgramKind::kRuntimeColorFilter;
    } else if (skstd::ends_with(inputPath, ".rts")) {
        kind = SkSL::ProgramKind::kRuntimeShader;
    } else {
        printf("input filename must end in '.vert', '.frag', '.rtb', '.rtcf', "
               "'.rts' or '.sksl'\n");
        return ResultCode::kInputError;
    }

    std::ifstream in(inputPath);
    std::string text((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    if (in.rdstate()) {
        printf("error reading '%s'\n", inputPath.c_str());
        return ResultCode::kInputError;
    }

    SkSL::ProgramSettings settings;
    auto standaloneCaps = SkSL::ShaderCapsFactory::Standalone();
    const SkSL::ShaderCaps* caps = standaloneCaps.get();
    std::unique_ptr<SkSL::SkVMDebugTrace> debugTrace;
    if (*honorSettings) {
        if (!detect_shader_settings(text, &settings, &caps, &debugTrace)) {
            return ResultCode::kInputError;
        }
    }

    // This tells the compiler where the rt-flip uniform will live should it be required. For
    // testing purposes we don't care where that is, but the compiler will report an error if we
    // leave them at their default invalid values, or if the offset overlaps another uniform.
    settings.fRTFlipOffset  = 16384;
    settings.fRTFlipSet     = 0;
    settings.fRTFlipBinding = 0;

    auto emitCompileError = [&](SkSL::FileOutputStream& out, const char* errorText) {
        // Overwrite the compiler output, if any, with an error message.
        out.close();
        SkSL::FileOutputStream errorStream(outputPath.c_str());
        errorStream.writeText("### Compilation failed:\n\n");
        errorStream.writeText(errorText);
        errorStream.close();
        // Also emit the error directly to stdout.
        puts(errorText);
    };

    auto compileProgram = [&](const auto& writeFn) -> ResultCode {
        SkSL::FileOutputStream out(outputPath.c_str());
        SkSL::Compiler compiler(caps);
        if (!out.isValid()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        std::unique_ptr<SkSL::Program> program = compiler.convertProgram(kind, text, settings);
        if (!program || !writeFn(compiler, *program, out)) {
            emitCompileError(out, compiler.errorText().c_str());
            return ResultCode::kCompileError;
        }
        if (!out.close()) {
            printf("error writing '%s'\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        return ResultCode::kSuccess;
    };

    auto compileProgramForSkVM = [&](const auto& writeFn) -> ResultCode {
        if (kind == SkSL::ProgramKind::kVertex) {
            printf("%s: SkVM does not support vertex programs\n", outputPath.c_str());
            return ResultCode::kOutputError;
        }
        if (kind == SkSL::ProgramKind::kFragment) {
            // Handle .sksl and .frag programs as runtime shaders.
            kind = SkSL::ProgramKind::kRuntimeShader;
        }
        return compileProgram(writeFn);
    };

    if (skstd::ends_with(outputPath, ".spirv")) {
        return compileProgram(
                [](SkSL::Compiler& compiler, SkSL::Program& program, SkSL::OutputStream& out) {
                    return compiler.toSPIRV(program, out);
                });
    } else if (skstd::ends_with(outputPath, ".asm.frag") ||
               skstd::ends_with(outputPath, ".asm.vert")) {
        return compileProgram(
                [](SkSL::Compiler& compiler, SkSL::Program& program, SkSL::OutputStream& out) {
                    // Compile program to SPIR-V assembly in a string-stream.
                    SkSL::StringStream assembly;
                    if (!compiler.toSPIRV(program, assembly)) {
                        return false;
                    }
                    // Convert the string-stream to a SPIR-V disassembly.
                    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
                    const std::string& spirv(assembly.str());
                    std::string disassembly;
                    if (!tools.Disassemble((const uint32_t*)spirv.data(),
                                           spirv.size() / 4, &disassembly)) {
                        return false;
                    }
                    // Finally, write the disassembly to our output stream.
                    out.write(disassembly.data(), disassembly.size());
                    return true;
                });
    } else if (skstd::ends_with(outputPath, ".glsl")) {
        return compileProgram(
                [](SkSL::Compiler& compiler, SkSL::Program& program, SkSL::OutputStream& out) {
                    return compiler.toGLSL(program, out);
                });
    } else if (skstd::ends_with(outputPath, ".metal")) {
        return compileProgram(
                [](SkSL::Compiler& compiler, SkSL::Program& program, SkSL::OutputStream& out) {
                    return compiler.toMetal(program, out);
                });
    } else if (skstd::ends_with(outputPath, ".hlsl")) {
        return compileProgram(
                [](SkSL::Compiler& compiler, SkSL::Program& program, SkSL::OutputStream& out) {
                    return compiler.toHLSL(program, out);
                });
    } else if (skstd::ends_with(outputPath, ".wgsl")) {
        return compileProgram(
                [](SkSL::Compiler& compiler, SkSL::Program& program, SkSL::OutputStream& out) {
                    return compiler.toWGSL(program, out);
                });
    } else if (skstd::ends_with(outputPath, ".skvm")) {
        return compileProgramForSkVM(
                [&](SkSL::Compiler&, SkSL::Program& program, SkSL::OutputStream& out) {
                    skvm::Builder builder{skvm::Features{}};
                    if (!SkSL::testingOnly_ProgramToSkVMShader(program, &builder,
                                                               debugTrace.get())) {
                        return false;
                    }

                    std::unique_ptr<SkWStream> redirect = as_SkWStream(out);
                    if (debugTrace) {
                        debugTrace->dump(redirect.get());
                    }
                    builder.done().dump(redirect.get());
                    return true;
                });
    } else if (skstd::ends_with(outputPath, ".stage")) {
        return compileProgram(
                [](SkSL::Compiler&, SkSL::Program& program, SkSL::OutputStream& out) {
                    class Callbacks : public SkSL::PipelineStage::Callbacks {
                    public:
                        std::string getMangledName(const char* name) override {
                            return std::string(name) + "_0";
                        }

                        std::string declareUniform(const SkSL::VarDeclaration* decl) override {
                            fOutput += decl->description();
                            return std::string(decl->var().name());
                        }

                        void defineFunction(const char* decl,
                                            const char* body,
                                            bool /*isMain*/) override {
                            fOutput += std::string(decl) + "{" + body + "}";
                        }

                        void declareFunction(const char* decl) override {
                            fOutput += std::string(decl) + ";";
                        }

                        void defineStruct(const char* definition) override {
                            fOutput += definition;
                        }

                        void declareGlobal(const char* declaration) override {
                            fOutput += declaration;
                        }

                        std::string sampleShader(int index, std::string coords) override {
                            return "child_" + std::to_string(index) + ".eval(" + coords + ")";
                        }

                        std::string sampleColorFilter(int index, std::string color) override {
                            return "child_" + std::to_string(index) + ".eval(" + color + ")";
                        }

                        std::string sampleBlender(int index,
                                                  std::string src,
                                                  std::string dst) override {
                            return "child_" + std::to_string(index) + ".eval(" + src + ", " +
                                   dst + ")";
                        }

                        std::string toLinearSrgb(std::string color) override {
                            return "toLinearSrgb(" + color + ")";
                        }
                        std::string fromLinearSrgb(std::string color) override {
                            return "fromLinearSrgb(" + color + ")";
                        }

                        std::string fOutput;
                    };
                    // The .stage output looks almost like valid SkSL, but not quite.
                    // The PipelineStageGenerator bridges the gap between the SkSL in `program`,
                    // and the C++ FP builder API (see GrSkSLFP). In that API, children don't need
                    // to be declared (so they don't emit declarations here). Children are sampled
                    // by index, not name - so all children here are just "child_N".
                    // The input color and coords have names in the original SkSL (as parameters to
                    // main), but those are ignored here. References to those variables become
                    // "_coords" and "_inColor". At runtime, those variable names are irrelevant
                    // when the new SkSL is emitted inside the FP - references to those variables
                    // are replaced with strings from EmitArgs, and might be varyings or differently
                    // named parameters.
                    Callbacks callbacks;
                    SkSL::PipelineStage::ConvertProgram(program, "_coords", "_inColor",
                                                        "_canvasColor", &callbacks);
                    out.writeString(SkShaderUtils::PrettyPrint(callbacks.fOutput));
                    return true;
                });
    } else if (skstd::ends_with(outputPath, ".html")) {
        settings.fAllowTraceVarInSkVMDebugTrace = false;

        SkCpu::CacheRuntimeFeatures();
        return compileProgramForSkVM(
            [&](SkSL::Compiler&, SkSL::Program& program, SkSL::OutputStream& out) {
                if (!debugTrace) {
                    debugTrace = std::make_unique<SkSL::SkVMDebugTrace>();
                    debugTrace->setSource(text.c_str());
                }
                auto visualizer = std::make_unique<skvm::viz::Visualizer>(debugTrace.get());
                skvm::Builder builder(skvm::Features{}, /*createDuplicates=*/true);
                if (!SkSL::testingOnly_ProgramToSkVMShader(program, &builder, debugTrace.get())) {
                    return false;
                }

                std::unique_ptr<SkWStream> redirect = as_SkWStream(out);
                skvm::Program p = builder.done(
                        /*debug_name=*/nullptr, /*allow_jit=*/false, std::move(visualizer));
                p.visualize(redirect.get());
                return true;
            });
    } else {
        printf("expected output path to end with one of: .glsl, .html, .metal, .hlsl, .wgsl, "
               ".spirv, .asm.vert, .asm.frag, .skvm, .stage (got '%s')\n",
               outputPath.c_str());
        return ResultCode::kConfigurationError;
    }
    return ResultCode::kSuccess;
}

/**
 * Processes multiple inputs in a single invocation of skslc.
 */
ResultCode processWorklist(const char* worklistPath) {
    std::string inputPath(worklistPath);
    if (!skstd::ends_with(inputPath, ".worklist")) {
        printf("expected .worklist file, found: %s\n\n", worklistPath);
        show_usage();
        return ResultCode::kConfigurationError;
    }

    // The worklist contains one line per argument to pass to skslc. When a blank line is reached,
    // those arguments will be passed to `processCommand`.
    auto resultCode = ResultCode::kSuccess;
    std::vector<std::string> args = {"skslc"};
    std::ifstream in(worklistPath);
    for (std::string line; std::getline(in, line); ) {
        if (in.rdstate()) {
            printf("error reading '%s'\n", worklistPath);
            return ResultCode::kInputError;
        }

        if (!line.empty()) {
            // We found an argument. Remember it.
            args.push_back(std::move(line));
        } else {
            // We found a blank line. If we have any arguments stored up, process them as a command.
            if (!args.empty()) {
                ResultCode outcome = processCommand(args);
                resultCode = std::max(resultCode, outcome);

                // Clear every argument except the first ("skslc").
                args.resize(1);
            }
        }
    }

    // If the worklist ended with a list of arguments but no blank line, process those now.
    if (args.size() > 1) {
        ResultCode outcome = processCommand(args);
        resultCode = std::max(resultCode, outcome);
    }

    // Return the "worst" status we encountered. For our purposes, compilation errors are the least
    // serious, because they are expected to occur in unit tests. Other types of errors are not
    // expected at all during a build.
    return resultCode;
}

int main(int argc, const char** argv) {
    if (argc == 2) {
        // Worklists are the only two-argument case for skslc, and we don't intend to support
        // nested worklists, so we can process them here.
        return (int)processWorklist(argv[1]);
    } else {
        // Process non-worklist inputs.
        std::vector<std::string> args;
        for (int index=0; index<argc; ++index) {
            args.push_back(argv[index]);
        }

        return (int)processCommand(args);
    }
}
