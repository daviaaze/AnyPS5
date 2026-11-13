#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourcePlan.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>
#include <span>

namespace AgcDriver::Graphics {

inline constexpr std::uint32_t PipelinePushConstantBytes = 256;
inline constexpr std::uint32_t PipelinePushSlotBytes = 128;
inline constexpr std::uint32_t ComputePushConstantBytes = 128;

inline std::uint32_t PushBlockBytes(bool libraries) {
    return libraries ? PipelinePushConstantBytes : PipelinePushSlotBytes;
}

inline VkShaderStageFlags PreRasterizationPushStages(bool meshShader, bool tessellation, bool geometry) {
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT;
    if (tessellation) stages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (geometry) stages |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (meshShader) stages |= VK_SHADER_STAGE_MESH_BIT_EXT;
    return stages;
}

inline std::uint32_t FixedPushOffset(ShaderRecompiler::ShaderStage stage) {
    return stage == ShaderRecompiler::ShaderStage::Fragment ? PipelinePushSlotBytes : 0u;
}

inline std::uint32_t StagePushOffset(std::uint32_t cursor, ShaderRecompiler::ShaderStage stage, bool fixedSlots) {
    return fixedSlots && stage == ShaderRecompiler::ShaderStage::Fragment ? PipelinePushSlotBytes : cursor;
}

// Per-record scalar bytes only; compiled reflection and module identity stay immutable.
struct ShaderRuntimeData {
    struct BindingWords {
        std::size_t bindingIndex;
        std::vector<std::uint32_t> words;
    };
    std::vector<std::byte> pushConstants;
    std::vector<BindingWords> bindingWords;

    explicit ShaderRuntimeData(const ShaderRecompiler::RecompileResult& program) { Reset(program); }
    ShaderRuntimeData() = default;

    void Patch(std::span<const std::pair<std::uint32_t, std::uint32_t>> patches) {
        for (const auto& [slot, value] : patches) {
            const auto at = static_cast<std::size_t>(slot) * sizeof(value);
            if (at + sizeof(value) <= pushConstants.size()) std::memcpy(pushConstants.data() + at, &value, sizeof(value));
            for (auto& binding : bindingWords) if (slot < binding.words.size()) binding.words[slot] = value;
        }
    }

    void Reset(const ShaderRecompiler::RecompileResult& program) {
        pushConstants.assign(program.pushConstants.begin(), program.pushConstants.end());
        const auto count = static_cast<std::size_t>(std::count_if(program.bindings.begin(), program.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; }));
        if (count == 0u) {
            std::vector<BindingWords>{}.swap(bindingWords);
            return;
        }
        bindingWords.clear();
        bindingWords.reserve(count);
        for (std::size_t i = 0; i < program.bindings.size(); ++i) {
            const auto& binding = program.bindings[i];
            if (binding.role != ShaderRecompiler::DescriptorRole::ShaderData) continue;
            bindingWords.push_back({i, std::vector<std::uint32_t>(binding.guestDescriptor.begin(), binding.guestDescriptor.end())});
        }
    }

    std::span<const std::uint32_t> DataWords(const ShaderRecompiler::RecompileResult& program, const ShaderRecompiler::DescriptorBinding& binding) const {
        if (binding.role != ShaderRecompiler::DescriptorRole::ShaderData) return binding.guestDescriptor.Span();
        const auto index = static_cast<std::size_t>(&binding - program.bindings.data());
        for (const auto& candidate : bindingWords) if (candidate.bindingIndex == index) return candidate.words;
        return binding.guestDescriptor.Span();
    }

};

struct CompiledShader {
    ShaderRecompiler::ShaderStage stage;
    const ShaderRecompiler::RecompileResult* program;
    std::uint32_t pushConstantOffset;
    const std::vector<ShaderRecompiler::RegisteredImageView>* registeredImageViews = nullptr;
    const ShaderRuntimeData* runtimeData = nullptr;

    std::span<const std::byte> PushConstants() const {
        return runtimeData != nullptr ? std::span<const std::byte>(runtimeData->pushConstants) : std::span<const std::byte>(program->pushConstants);
    }

    std::span<const std::uint32_t> DataWords(const ShaderRecompiler::DescriptorBinding& binding) const {
        return runtimeData != nullptr ? runtimeData->DataWords(*program, binding) : binding.guestDescriptor.Span();
    }
};

inline std::uint32_t KeyedPushOffset(const CompiledShader& shader) {
    return shader.program->pushConstants.empty() ? 0u : shader.pushConstantOffset;
}

inline VkShaderStageFlagBits VulkanStage(ShaderRecompiler::ShaderStage stage) {
    switch (stage) {
        case ShaderRecompiler::ShaderStage::Vertex:
        case ShaderRecompiler::ShaderStage::Local: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderRecompiler::ShaderStage::TessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderRecompiler::ShaderStage::TessellationEvaluation: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderRecompiler::ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderRecompiler::ShaderStage::Mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
        case ShaderRecompiler::ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderRecompiler::ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: throw std::runtime_error("AGC graphics: unsupported compiled shader stage");
    }
}

inline VkPipelineStageFlags PipelineStages(std::span<const CompiledShader> shaders) {
    VkPipelineStageFlags result = 0;
    for (const auto& shader : shaders) {
        switch (VulkanStage(shader.stage)) {
            case VK_SHADER_STAGE_VERTEX_BIT: result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT; break;
            case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: result |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT; break;
            case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: result |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT; break;
            case VK_SHADER_STAGE_GEOMETRY_BIT: result |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT; break;
            case VK_SHADER_STAGE_MESH_BIT_EXT: result |= VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT; break;
            case VK_SHADER_STAGE_FRAGMENT_BIT: result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; break;
            default: throw std::runtime_error("AGC graphics: invalid graphics pipeline stage");
        }
    }
    return result;
}

inline VkShaderStageFlags PushConstantStages(std::span<const CompiledShader> shaders) {
    VkShaderStageFlags result = 0;
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        if (!shader.program->pushConstants.empty() || shader.program->readsDepthRemapPush || shader.stage == ShaderRecompiler::ShaderStage::Mesh || shader.stage == ShaderRecompiler::ShaderStage::TessellationControl) result |= VulkanStage(shader.stage);
    }
    return result;
}

inline std::array<std::byte, PipelinePushConstantBytes> AssemblePushConstants(std::span<const CompiledShader> shaders) {
    std::array<std::byte, PipelinePushConstantBytes> result{};
    std::array<bool, PipelinePushConstantBytes> occupied{};
    for (const auto& shader : shaders) {
        Require(shader.program != nullptr, "missing compiled shader");
        const auto bytes = shader.PushConstants();
        if (bytes.empty()) continue;
        Require(bytes.size() % 4 == 0 && shader.pushConstantOffset % 4 == 0, "shader push constant range is not DWORD aligned");
        Require(shader.pushConstantOffset < PipelinePushConstantBytes && bytes.size() <= PipelinePushConstantBytes - shader.pushConstantOffset, "shader push constant range lies outside the pipeline push constant block");
        Require(bytes.size() <= PipelinePushSlotBytes - shader.pushConstantOffset % PipelinePushSlotBytes, "shader push constant range crosses a push constant slot");
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto position = shader.pushConstantOffset + i;
            Require(!occupied[position], "shader push constant ranges of different stages overlap");
            occupied[position] = true;
            result[position] = bytes[i];
        }
    }
    return result;
}

}

#endif
