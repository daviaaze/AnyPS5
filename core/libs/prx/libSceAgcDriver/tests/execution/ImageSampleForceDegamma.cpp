#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
using AgcDriver::Graphics::ClearCachedTextures;

using AgcDriver::VulkanDevice;

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Size = 4;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Format8888Srgb = 130;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format8888SNorm = 57;
constexpr std::uint32_t FormatBc4UNorm = 175;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::uint32_t FilterPoint = 0;
constexpr std::uint32_t FilterBilinear = 1;
constexpr std::uint32_t FilterAnisoBilinear = 3;
constexpr std::uint32_t AnisoOverride = 1u << 29u;
constexpr std::uint32_t ForceDegamma = 1u << 20u;
constexpr std::uint32_t FormatBc3Srgb = 174;

alignas(256) std::array<float, Threads * 3> Input{};
alignas(256) std::array<float, Threads * 4> Output{};
alignas(256) std::array<float, Threads * 8> RaceOutputs{};
alignas(4096) std::array<std::uint8_t, 4096> Texels{};
constexpr std::uint32_t ArrayWords = 32;
constexpr std::uint32_t ArraySampleResult = 8;
alignas(256) std::array<std::uint32_t, Threads * ArrayWords> ArrayBuffer{};
alignas(256) constexpr std::array<std::uint32_t, 36> ArraySampleCode{
    0x34020087, 0xe0301000, 0x80000201, 0xe0301004, 0x80000301, 0xe0301008, 0x80000401, 0xe030100c,
    0x80000501, 0xe0301010, 0x80000601, 0xe0301014, 0x80000701, 0xbf8c3f70, 0xf09c0f28, 0x00610c02,
    0xf0001f10, 0x00011005, 0xbf8c3f70, 0xe0701020, 0x80000c01, 0xe0701024, 0x80000d01, 0xe0701028,
    0x80000e01, 0xe070102c, 0x80000f01, 0xe0701030, 0x80001001, 0xe0701034, 0x80001101, 0xe0701038,
    0x80001201, 0xe070103c, 0x80001301, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 13> Code{
    0x1614008c, 0xe03c1000, 0x8000010a, 0xbf8c3f70, 0xf0900f08, 0x00820401, 0xbf8c3f70,
    0x34160084, 0xe0781000, 0x8001040b, 0xbf810000, 0xbf810000, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

class WatchedTexture {
public:
    explicit WatchedTexture(std::size_t bytes = 65536, std::size_t committed = 0)
        : bytes(bytes), committedBytes(committed == 0 ? bytes : committed) {
        Require(bytes != 0 && bytes % 4096u == 0 && committedBytes != 0 &&
                committedBytes <= bytes && committedBytes % 4096u == 0,
            "invalid BC3 sampled-input geometry");
        memory = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        Require(memory != nullptr, "cannot reserve the BC3 sampled input");
#ifdef _WIN32
        GuestArena::GuestArenaCommit_nid_postfix(memory, committedBytes, PAGE_READWRITE, 65536);
#else
        Require(mmap(memory, committedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED,
            "cannot commit the BC3 sampled input");
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(memory, committedBytes);
#endif
        GuestAllocations::Mutation().Add(memory, bytes, true, true, true);
    }

    ~WatchedTexture() { Release(); }

    std::uint8_t* Data() const { return memory; }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(memory); }

    void Release() {
        if (memory == nullptr) return;
        GuestAllocations::Mutation().Remove(memory);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(memory, bytes);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(memory, committedBytes);
        munmap(memory, bytes);
#endif
        GuestArena::GuestArenaRelease_nid_postfix(memory, bytes);
        memory = nullptr;
    }

private:
    std::uint8_t* memory = nullptr;
    std::size_t bytes;
    std::size_t committedBytes;
};

std::array<std::uint32_t, 8> TextureDescriptor(std::uint64_t address, std::uint32_t format,
    std::uint32_t width, std::uint32_t height, std::uint32_t maxMip = 0, std::uint32_t baseLevel = 0,
    std::uint32_t type = Type2D, std::uint32_t lastArray = 0, std::uint32_t baseArray = 0,
    std::uint32_t tileMode = 0) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((width - 1u) & 3u) << 30u),
        ((width - 1u) >> 2u) | ((height - 1u) << 14u),
        0xfacu | (baseLevel << 12u) | (maxMip << 16u) | (tileMode << 20u) | (type << 28u),
        lastArray | (baseArray << 16u),
        maxMip << 4u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 8> TextureDescriptor(std::uint32_t format) {
    return TextureDescriptor(reinterpret_cast<std::uint64_t>(Texels.data()), format, Size, Size);
}

std::array<std::uint32_t, 4> SamplerDescriptor(std::uint32_t filter, std::uint32_t forced, std::uint32_t filters) {
    return {0x92u | forced, 0x00fff000u, (filter << 20u) | (filter << 22u) | (1u << 24u) | filters, 0u};
}

std::uint8_t Code8(std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
    return static_cast<std::uint8_t>((y * 61u + x * 17u + channel * 97u + 5u) % 256u);
}

void FillTexels() {
    const auto mips = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, Format8888Srgb, Size, Size, 1);
    Require(AgcDriver::Graphics::ComputeSurfaceSize(mips, 1) <= Texels.size(), "image sample force degamma: the texture does not fit its storage");
    Texels.fill(0);
    for (std::uint32_t y = 0; y < Size; ++y) {
        for (std::uint32_t x = 0; x < Size; ++x) {
            auto* texel = &Texels[mips[0].tiledOffset + static_cast<std::uint64_t>(y) * mips[0].pitchBytes + x * 4u];
            for (std::uint32_t channel = 0; channel < 4u; ++channel) texel[channel] = Code8(x, y, channel);
        }
    }
}

void FillInput(bool centers) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto x = tid % Size;
        const auto y = (tid / Size) % Size;
        Input[tid * 3u + 0u] = centers ? (static_cast<float>(x) + 0.5f) / static_cast<float>(Size) : (static_cast<float>((tid * 5u + 1u) % 13u) + 0.3f) / 13.0f;
        Input[tid * 3u + 1u] = centers ? (static_cast<float>(y) + 0.5f) / static_cast<float>(Size) : (static_cast<float>((tid * 7u + 3u) % 13u) + 0.6f) / 13.0f;
        Input[tid * 3u + 2u] = 0.0f;
    }
}
void FillCoordinate(float u, float v, float layer = 0.0f) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Input[tid * 3u] = u;
        Input[tid * 3u + 1u] = v;
        Input[tid * 3u + 2u] = layer;
    }
}

float SrgbToLinear(std::uint8_t code) {
    const float c = static_cast<float>(code) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::array<float, Threads * 4> Sample(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t filter, std::uint32_t forced, std::uint32_t filters = 0u, const std::array<std::uint32_t, 8>* image = nullptr, float* outputTarget = nullptr, bool wait = true) {
    std::vector<std::uint32_t> userData(20, 0u);
    auto* outputBytes = outputTarget != nullptr ? outputTarget : Output.data();
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(sizeof(Input)));
    const auto output = BufferDescriptor(outputBytes, static_cast<std::uint32_t>(Threads * 4u * sizeof(float)));
    const auto texture = image != nullptr ? *image : TextureDescriptor(format);
    const auto sampler = SamplerDescriptor(filter, forced, filters);
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 16);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    std::fill_n(outputBytes, Threads * 4u, -1.0f);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    if (!wait) return {};
    device.WaitIdle();
    if (outputTarget != nullptr) {
        std::array<float, Threads * 4> resultValues{};
        std::copy_n(outputBytes, resultValues.size(), resultValues.begin());
        return resultValues;
    }
    return Output;
}
std::array<float, 4> SampleArrayLayer(VulkanDevice& device, const std::array<std::uint32_t, 8>& image,
    float u, float v, float layer) {
    ArrayBuffer.fill(0xdeadbeefu);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        ArrayBuffer[tid * ArrayWords] = std::bit_cast<std::uint32_t>(u);
        ArrayBuffer[tid * ArrayWords + 1u] = std::bit_cast<std::uint32_t>(v);
        ArrayBuffer[tid * ArrayWords + 2u] = std::bit_cast<std::uint32_t>(layer);
        ArrayBuffer[tid * ArrayWords + 3u] = 0u;
        ArrayBuffer[tid * ArrayWords + 4u] = 0u;
        ArrayBuffer[tid * ArrayWords + 5u] = 0u;
    }
    std::vector<std::uint32_t> userData(16, 0u);
    const auto buffer = BufferDescriptor(ArrayBuffer.data(), static_cast<std::uint32_t>(ArrayBuffer.size() * 4u));
    const auto sampler = std::array<std::uint32_t, 4>{0u, 0xfffu << 12u, 1u << 26u, 0u};
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(image.begin(), image.end(), userData.begin() + 4);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 12);
    const auto code = std::span<const std::uint32_t>(ArraySampleCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    return {
        std::bit_cast<float>(ArrayBuffer[ArraySampleResult]),
        std::bit_cast<float>(ArrayBuffer[ArraySampleResult + 1u]),
        std::bit_cast<float>(ArrayBuffer[ArraySampleResult + 2u]),
        std::bit_cast<float>(ArrayBuffer[ArraySampleResult + 3u]),
    };
}


void RequireSame(const std::array<float, Threads * 4>& forced, const std::array<float, Threads * 4>& plain, const char* name, const char* first = "through FORCE_DEGAMMA", const char* second = "without it") {
    for (std::uint32_t index = 0; index < forced.size(); ++index) {
        Require(std::bit_cast<std::uint32_t>(forced[index]) == std::bit_cast<std::uint32_t>(plain[index]), std::string(name) + ": thread " + std::to_string(index / 4u) + " component " + std::to_string(index % 4u) + " sampled " + std::to_string(forced[index]) + " " + first + ", " + std::to_string(plain[index]) + " " + second);
    }
}

void RequireDecoded(const std::array<float, Threads * 4>& sampled, const char* name) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto x = tid % Size;
        const auto y = (tid / Size) % Size;
        for (std::uint32_t channel = 0; channel < 4u; ++channel) {
            const auto code = Code8(x, y, channel);
            const float expected = channel == 3u ? static_cast<float>(code) / 255.0f : SrgbToLinear(code);
            const float value = sampled[tid * 4u + channel];
            Require(std::fabs(value - expected) <= 0.005f, std::string(name) + " through FORCE_DEGAMMA: thread " + std::to_string(tid) + " component " + std::to_string(channel) + " sampled " + std::to_string(value) + ", expected " + std::to_string(expected));
        }
    }
}

void Reject(AgcDriver::VulkanDevice& device, std::uint32_t format, std::string_view reason) {
    try {
        static_cast<void>(Sample(device, format, FilterPoint, ForceDegamma));
    } catch (const std::exception& error) {
        Require(std::string_view(error.what()).find(reason) != std::string_view::npos, std::string("unexpected rejection: ") + error.what());
        return;
    }
    Require(false, std::string("expected rejection: ") + std::string(reason));
}

void FillBc3Solid(std::uint8_t* block, std::uint16_t rgb565) {
    block[0] = block[1] = 255;
    std::fill(block + 2, block + 8, std::uint8_t{0});
    block[8] = static_cast<std::uint8_t>(rgb565);
    block[9] = static_cast<std::uint8_t>(rgb565 >> 8u);
    block[10] = block[8];
    block[11] = block[9];
    std::fill(block + 12, block + 16, std::uint8_t{0});
}

void CheckSolid(const float* sampled, std::array<float, 4> expected, const char* name) {
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        for (std::uint32_t component = 0; component < 4; ++component) {
            const auto value = sampled[lane * 4u + component];
            Require(std::fabs(value - expected[component]) <= 0.005f,
                std::string(name) + ": lane " + std::to_string(lane) + " component " + std::to_string(component) +
                " sampled " + std::to_string(value) + ", expected " + std::to_string(expected[component]));
        }
    }
}

void residentBc3InputRace(VulkanDevice& device) {
    const auto context = device.graphicsContext();
    if (!context.textureCompressionBC || !context.sparseBinding ||
        !context.sparseResidencyBuffer || !context.sparseResidencyAliased) {
        if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr)
            throw std::runtime_error("resident BC3 input smoke requires BC compression and sparse aliased buffers");
        std::puts("resident BC3 snapshot race skipped: sparse resident buffers or BC compression unavailable");
        return;
    }
    ClearCachedTextures(context.device);
    WatchedTexture source;
    FillBc3Solid(source.Data(), 0x8410u);
    const auto address = source.Address();
    Require(AgcDriver::GuestMemory::Watched(address, 16), "the BC3 input is not write-watched");
    AgcDriver::GuestMemory::CollectWritesUncached(address, 16);

    const auto image = TextureDescriptor(address, FormatBc3Srgb, 4, 4);
    FillInput(true);
    static_cast<void>(Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &image, RaceOutputs.data(), false));
    const auto first = AgcDriver::Graphics::CachedSampledTexture(context, image);
    Require(first->HasResidentGpuInput(), "the first BC3 sample did not use the resident-mirror GPU input path");
    FillBc3Solid(source.Data(), 0xfc00u);
    AgcDriver::GuestMemory::CollectWritesUncached(address, 16);
    static_cast<void>(Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &image,
        RaceOutputs.data() + Threads * 4u, false));
    const auto second = AgcDriver::Graphics::CachedSampledTexture(context, image);
    Require(second->HasResidentGpuInput(), "the refreshed BC3 sample did not use the resident-mirror GPU input path");
    Require(source.Data()[8] == 0x00u && source.Data()[9] == 0xfcu,
        "sampling the private BC3 images changed the guest source bytes");
    source.Release();
    device.WaitIdle();

    CheckSolid(RaceOutputs.data(), {SrgbToLinear(132), SrgbToLinear(130), SrgbToLinear(132), 1.0f}, "first BC3 sRGB image");
    CheckSolid(RaceOutputs.data() + Threads * 4u, {1.0f, SrgbToLinear(130), 0.0f, 1.0f}, "refreshed BC3 sRGB image");
    ClearCachedTextures(context.device);
}

void residentBc3MipAndHolePixels(VulkanDevice& device) {
    const auto context = device.graphicsContext();
    if (!context.textureCompressionBC || !context.sparseBinding ||
        !context.sparseResidencyBuffer || !context.sparseResidencyAliased) return;
    ClearCachedTextures(context.device);
    {
        WatchedTexture source;
        const auto mips = AgcDriver::Graphics::ComputeMipLayout(
            AgcDriver::Graphics::TextureTileMode::kLinear, FormatBc3Srgb, 4, 4, 2);
        const auto layerBytes = AgcDriver::Graphics::ComputeSurfaceSize(mips, 1);
        FillBc3Solid(source.Data() + mips[0].tiledOffset, 0xfc00u);
        FillBc3Solid(source.Data() + mips[1].tiledOffset, 0x8410u);
        AgcDriver::GuestMemory::CollectWritesUncached(source.Address(), 32);
        const auto mipView = TextureDescriptor(source.Address(), FormatBc3Srgb, 4, 4, 1, 1);
        FillInput(true);
        const auto sampled = Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &mipView);
        CheckSolid(sampled.data(), {SrgbToLinear(132), SrgbToLinear(130), SrgbToLinear(132), 1.0f}, "BC3 sRGB base-mip-1 view");
        Require(AgcDriver::Graphics::CachedSampledTexture(context, mipView)->HasResidentGpuInput(),
            "the BC3 mip view did not use the resident-mirror GPU input path");

        FillBc3Solid(source.Data() + layerBytes + mips[0].tiledOffset, 0x001fu);
        FillBc3Solid(source.Data() + layerBytes + mips[1].tiledOffset, 0xfc00u);
        AgcDriver::GuestMemory::CollectWritesUncached(source.Address(), layerBytes * 2u);
        ClearCachedTextures(context.device);
        const auto layer0 = TextureDescriptor(source.Address(), FormatBc3Srgb, 4, 4, 1, 1, Type2DArray, 1, 0);
        const auto layer1 = TextureDescriptor(source.Address(), FormatBc3Srgb, 4, 4, 1, 1, Type2DArray, 1, 1);
        const auto sampledLayer0 = Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &layer0);
        const auto sampledLayer1 = Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &layer1);
        CheckSolid(sampledLayer0.data(), {SrgbToLinear(132), SrgbToLinear(130), SrgbToLinear(132), 1.0f}, "BC3 array layer 0 mip 1");
        CheckSolid(sampledLayer1.data(), {1.0f, SrgbToLinear(130), 0.0f, 1.0f}, "BC3 array layer 1 mip 1");
        Require(AgcDriver::Graphics::CachedSampledTexture(context, layer0)->HasResidentGpuInput() &&
                AgcDriver::Graphics::CachedSampledTexture(context, layer1)->HasResidentGpuInput(),
            "a BC3 array-layer view did not use the resident-mirror GPU input path");
        ClearCachedTextures(context.device);
    }
    {
        WatchedTexture source(65536, 4096);
        for (std::size_t offset = 0; offset < 4096; offset += 16) FillBc3Solid(source.Data() + offset, 0x8410u);
        AgcDriver::GuestMemory::CollectWritesUncached(source.Address(), 4096);
        const auto image = TextureDescriptor(source.Address(), FormatBc3Srgb, 128, 128);
        FillCoordinate(16.5f / 128.0f, 16.5f / 128.0f);
        const auto committed = Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &image);
        CheckSolid(committed.data(), {SrgbToLinear(132), SrgbToLinear(130), SrgbToLinear(132), 1.0f}, "BC3 committed sparse-mirror pixels");
        FillCoordinate(80.5f / 128.0f, 80.5f / 128.0f);
        const auto hole = Sample(device, FormatBc3Srgb, FilterPoint, 0u, 0u, &image);
        CheckSolid(hole.data(), {0.0f, 0.0f, 0.0f, 0.0f}, "BC3 uncommitted sparse-mirror pixels");
        Require(AgcDriver::Graphics::CachedSampledTexture(context, image)->HasResidentGpuInput(),
            "the sparse BC3 texture did not use the resident-mirror GPU input path");
        Require(source.Data()[0] == 255u && source.Data()[8] == 0x10u && source.Data()[9] == 0x84u,
            "sampling the sparse BC3 image changed its committed guest source");
        ClearCachedTextures(context.device);
    }
}

void residentBc3LargeInputSmoke(VulkanDevice& device) {
    if (std::getenv("APS5_BC3_LARGE_INPUT_SMOKE") == nullptr) return;
    const auto context = device.graphicsContext();
    Require(context.textureCompressionBC && context.sparseBinding &&
            context.sparseResidencyBuffer && context.sparseResidencyAliased,
        "the large BC3 input smoke requires BC compression and sparse aliased buffers");
    constexpr std::size_t expectedBytes = 369098752;
    constexpr std::size_t committedBytes = 177340416;
    const auto probe = TextureDescriptor(0x100000000ull, FormatBc3Srgb, 1024, 1024, 10, 0, Type2DArray, 255, 0, 9);
    const auto probeResource = AgcDriver::Graphics::DecodeTextureResource(probe);
    const auto geometry = AgcDriver::Graphics::DescribeSurface(probeResource);
    Require(geometry.guestBytes == expectedBytes,
        "the large BC3 input smoke descriptor does not match the 1024x1024x256, 11-mip guest byte count");
    ClearCachedTextures(context.device);
    WatchedTexture source(expectedBytes, committedBytes);
    const auto firstBlock = geometry.GuestOffset(0, geometry.mips.front());
    Require(firstBlock + 16u <= committedBytes, "the sampled BC3 mip block is outside committed memory");
    FillBc3Solid(source.Data() + firstBlock, 0x8410u);
    AgcDriver::GuestMemory::CollectWritesUncached(source.Address() + firstBlock, 16);
    const auto image = TextureDescriptor(source.Address(), FormatBc3Srgb, 1024, 1024, 10, 0,
        Type2DArray, 255, 0, 9);
    const auto sampled = SampleArrayLayer(device, image, 0.5f / 1024.0f, 0.5f / 1024.0f, 0.0f);
    const std::array<float, 4> expected{SrgbToLinear(132), SrgbToLinear(130), SrgbToLinear(132), 1.0f};
    for (std::uint32_t component = 0; component < expected.size(); ++component) {
        Require(std::fabs(sampled[component] - expected[component]) <= 0.005f,
            "large sparse BC3 sRGB array sample component " + std::to_string(component) +
            " is " + std::to_string(sampled[component]) + ", expected " + std::to_string(expected[component]));
    }
    Require(AgcDriver::Graphics::CachedSampledTexture(context, image)->HasResidentGpuInput(),
        "the large BC3 input smoke did not use the resident-mirror GPU input path");
    ClearCachedTextures(context.device);
}


}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        residentBc3InputRace(*device);
        residentBc3MipAndHolePixels(*device);
        residentBc3LargeInputSmoke(*device);
        FillTexels();
        FillInput(true);
        const auto forcedPoint = Sample(*device, Format8888Srgb, FilterPoint, ForceDegamma);
        RequireDecoded(forcedPoint, "8_8_8_8_SRGB");
        RequireSame(forcedPoint, Sample(*device, Format8888Srgb, FilterPoint, 0u), "8_8_8_8_SRGB, point");
        FillInput(false);
        const auto forcedBilinear = Sample(*device, Format8888Srgb, FilterBilinear, ForceDegamma);
        RequireSame(forcedBilinear, Sample(*device, Format8888Srgb, FilterBilinear, 0u), "8_8_8_8_SRGB, bilinear");
        RequireSame(Sample(*device, Format8888Srgb, FilterAnisoBilinear, ForceDegamma, AnisoOverride), forcedBilinear, "8_8_8_8_SRGB, single level", "anisotropically under ANISO_OVERRIDE", "bilinearly");
        FillInput(true);
        const auto unormPoint = Sample(*device, Format8888UNorm, FilterPoint, ForceDegamma);
        RequireDecoded(unormPoint, "8_8_8_8_UNORM");
        RequireSame(unormPoint, forcedPoint, "8_8_8_8_UNORM, point", "through FORCE_DEGAMMA", "as 8_8_8_8_SRGB");
        const auto unormPlain = Sample(*device, Format8888UNorm, FilterPoint, 0u);
        Require(unormPlain[0] != unormPoint[0] || unormPlain[4] != unormPoint[4], "8_8_8_8_UNORM without FORCE_DEGAMMA sampled the decoded values");
        FillInput(false);
        RequireSame(Sample(*device, Format8888UNorm, FilterBilinear, ForceDegamma), forcedBilinear, "8_8_8_8_UNORM, bilinear", "through FORCE_DEGAMMA", "as 8_8_8_8_SRGB");
        RequireSame(Sample(*device, Format32Float, FilterPoint, ForceDegamma), Sample(*device, Format32Float, FilterPoint, 0u), "32_FLOAT, point");
        RequireSame(Sample(*device, FormatBc4UNorm, FilterPoint, ForceDegamma), Sample(*device, FormatBc4UNorm, FilterPoint, 0u), "BC4_UNORM, point");
        FillInput(true);
        RequireSame(Sample(*device, Format8888SNorm, FilterPoint, ForceDegamma), forcedPoint, "8_8_8_8_SNORM, point", "through FORCE_DEGAMMA", "as 8_8_8_8_SRGB");
        FillInput(false);
        RequireSame(Sample(*device, Format8888SNorm, FilterBilinear, ForceDegamma), forcedBilinear, "8_8_8_8_SNORM, bilinear", "through FORCE_DEGAMMA", "as 8_8_8_8_SRGB");
        Reject(*device, Format32UInt, "guest format 20, which is not sRGB and has no sRGB view, is sampled through a sampler that forces sRGB decoding");
        std::puts("image sample force degamma tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
