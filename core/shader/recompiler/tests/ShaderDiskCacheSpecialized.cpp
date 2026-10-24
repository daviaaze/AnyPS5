#include "ShaderDiskCache.hpp"
#include "Recompiler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    using namespace ShaderRecompiler;
    using namespace ShaderRecompiler::ShaderDiskCache;

    const std::vector<std::uint32_t> sourceWords{
        0x07230203u, 0x00010000u, 0u, 5u, 0u,
        (2u << 16u) | 17u, 1u,
        (3u << 16u) | 14u, 0u, 1u,
        (5u << 16u) | 15u, 5u, 3u, 0x6e69616du, 0u,
        (6u << 16u) | 16u, 3u, 17u, 1u, 1u, 1u,
        (2u << 16u) | 19u, 1u,
        (3u << 16u) | 33u, 2u, 1u,
        (5u << 16u) | 54u, 1u, 3u, 0u, 2u,
        (2u << 16u) | 248u, 4u,
        (1u << 16u) | 253u,
        (1u << 16u) | 56u,
    };
    CompiledShaderArtifact artifact;
    artifact.spirv = SharedSpirv(sourceWords);
    constexpr std::uint32_t classes[]{2u, 1u};
    constexpr PipelineSpecializationConstant constants[]{{17u, 29u}};
    const SpirvTarget target{
        0x00403000u, 0x00010600u, 32u, RuntimeAbi::Version, {}, {}, false,
        {1024u, 1024u, 64u}, 1024u, 32768u, std::nullopt, std::nullopt, true
    };
    const auto built = BuildSpecializedModuleForCache(artifact, classes, constants, target);

    const SpecializationKeyInput input{
        .source = artifact.spirv.ContentHash(),
        .sourceWords = artifact.spirv.size(),
        .vertexInputPatches = artifact.vertexInputPatches,
        .stencilReferences = !artifact.stencilReferences.empty(),
        .classes = classes,
        .constants = constants,
        .vulkanVersion = target.vulkanVersion,
        .spirvVersion = target.spirvVersion,
        .nonConstantImageOffsets = target.nonConstantImageOffsets,
        .spirvTools = ANYPS5_ENABLE_SPIRV_TOOLS != 0,
    };
    std::vector<std::byte> key;
    BuildSpecializationKey(input, key);
    const auto file = EncodeSpecializedEntry(key, built.spirv.Words());
    if (file.empty()) {
        std::fprintf(stderr, "specialized module encoding failed\n");
        return EXIT_FAILURE;
    }

    std::vector<std::uint32_t> reloaded;
    if (DecodeSpecializedEntry(file, key, reloaded) != LoadStatus::Loaded ||
        !std::equal(built.spirv.begin(), built.spirv.end(), reloaded.begin(), reloaded.end())) {
        std::fprintf(stderr, "specialized module round trip changed SPIR-V\n");
        return EXIT_FAILURE;
    }
    const auto before = ScanSpecializedModuleBindings(built.spirv.Words());
    const auto after = ScanSpecializedModuleBindings(reloaded);
    if (before.bindings != after.bindings || before.pushData != after.pushData ||
        before.bindings != built.bindings || before.pushData != built.pushData) {
        std::fprintf(stderr, "specialized module round trip changed descriptor bindings or push-data metadata\n");
        return EXIT_FAILURE;
    }

    auto changedConstant = input;
    PipelineSpecializationConstant alteredConstant{17u, 30u};
    changedConstant.constants = std::span(&alteredConstant, 1u);
    std::vector<std::byte> changedKey;
    BuildSpecializationKey(changedConstant, changedKey);
    std::vector<std::uint32_t> ignored;
    if (changedKey == key || DecodeSpecializedEntry(file, changedKey, ignored) != LoadStatus::KeyMismatch) {
        std::fprintf(stderr, "changed specialization constant did not produce a cache miss\n");
        return EXIT_FAILURE;
    }

    auto changedTarget = input;
    changedTarget.vulkanVersion++;
    BuildSpecializationKey(changedTarget, changedKey);
    if (changedKey == key || DecodeSpecializedEntry(file, changedKey, ignored) != LoadStatus::KeyMismatch) {
        std::fprintf(stderr, "changed target did not produce a cache miss\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
