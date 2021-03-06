// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <tuple>

#include <boost/functional/hash.hpp>

#include "common/bit_cast.h"
#include "common/cityhash.h"
#include "common/common_types.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace Vulkan {

namespace {

constexpr std::size_t POINT = 0;
constexpr std::size_t LINE = 1;
constexpr std::size_t POLYGON = 2;
constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
    POINT,   // Points
    LINE,    // Lines
    LINE,    // LineLoop
    LINE,    // LineStrip
    POLYGON, // Triangles
    POLYGON, // TriangleStrip
    POLYGON, // TriangleFan
    POLYGON, // Quads
    POLYGON, // QuadStrip
    POLYGON, // Polygon
    LINE,    // LinesAdjacency
    LINE,    // LineStripAdjacency
    POLYGON, // TrianglesAdjacency
    POLYGON, // TriangleStripAdjacency
    POLYGON, // Patches
};

} // Anonymous namespace

void FixedPipelineState::Fill(const Maxwell& regs, bool has_extended_dynamic_state) {
    const std::array enabled_lut = {regs.polygon_offset_point_enable,
                                    regs.polygon_offset_line_enable,
                                    regs.polygon_offset_fill_enable};
    const u32 topology_index = static_cast<u32>(regs.draw.topology.Value());

    raw = 0;
    primitive_restart_enable.Assign(regs.primitive_restart.enabled != 0 ? 1 : 0);
    depth_bias_enable.Assign(enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]] != 0 ? 1 : 0);
    depth_clamp_disabled.Assign(regs.view_volume_clip_control.depth_clamp_disabled.Value());
    ndc_minus_one_to_one.Assign(regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1 : 0);
    polygon_mode.Assign(PackPolygonMode(regs.polygon_mode_front));
    patch_control_points_minus_one.Assign(regs.patch_vertices - 1);
    tessellation_primitive.Assign(static_cast<u32>(regs.tess_mode.prim.Value()));
    tessellation_spacing.Assign(static_cast<u32>(regs.tess_mode.spacing.Value()));
    tessellation_clockwise.Assign(regs.tess_mode.cw.Value());
    logic_op_enable.Assign(regs.logic_op.enable != 0 ? 1 : 0);
    logic_op.Assign(PackLogicOp(regs.logic_op.operation));
    rasterize_enable.Assign(regs.rasterize_enable != 0 ? 1 : 0);
    topology.Assign(regs.draw.topology);

    alpha_raw = 0;
    const auto test_func =
        regs.alpha_test_enabled == 1 ? regs.alpha_test_func : Maxwell::ComparisonOp::Always;
    alpha_test_func.Assign(PackComparisonOp(test_func));
    alpha_test_ref = Common::BitCast<u32>(regs.alpha_test_ref);

    point_size = Common::BitCast<u32>(regs.point_size);

    for (std::size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        binding_divisors[index] =
            regs.instanced_arrays.IsInstancingEnabled(index) ? regs.vertex_array[index].divisor : 0;
    }

    for (std::size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        const auto& input = regs.vertex_attrib_format[index];
        auto& attribute = attributes[index];
        attribute.raw = 0;
        attribute.enabled.Assign(input.IsConstant() ? 0 : 1);
        attribute.buffer.Assign(input.buffer);
        attribute.offset.Assign(input.offset);
        attribute.type.Assign(static_cast<u32>(input.type.Value()));
        attribute.size.Assign(static_cast<u32>(input.size.Value()));
    }

    for (std::size_t index = 0; index < std::size(attachments); ++index) {
        attachments[index].Fill(regs, index);
    }

    const auto& transform = regs.viewport_transform;
    std::transform(transform.begin(), transform.end(), viewport_swizzles.begin(),
                   [](const auto& viewport) { return static_cast<u16>(viewport.swizzle.raw); });

    if (!has_extended_dynamic_state) {
        no_extended_dynamic_state.Assign(1);
        dynamic_state.Fill(regs);
    }
}

void FixedPipelineState::BlendingAttachment::Fill(const Maxwell& regs, std::size_t index) {
    const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];

    raw = 0;
    mask_r.Assign(mask.R);
    mask_g.Assign(mask.G);
    mask_b.Assign(mask.B);
    mask_a.Assign(mask.A);

    // TODO: C++20 Use templated lambda to deduplicate code

    if (!regs.independent_blend_enable) {
        const auto& src = regs.blend;
        if (!src.enable[index]) {
            return;
        }
        equation_rgb.Assign(PackBlendEquation(src.equation_rgb));
        equation_a.Assign(PackBlendEquation(src.equation_a));
        factor_source_rgb.Assign(PackBlendFactor(src.factor_source_rgb));
        factor_dest_rgb.Assign(PackBlendFactor(src.factor_dest_rgb));
        factor_source_a.Assign(PackBlendFactor(src.factor_source_a));
        factor_dest_a.Assign(PackBlendFactor(src.factor_dest_a));
        enable.Assign(1);
        return;
    }

    if (!regs.blend.enable[index]) {
        return;
    }
    const auto& src = regs.independent_blend[index];
    equation_rgb.Assign(PackBlendEquation(src.equation_rgb));
    equation_a.Assign(PackBlendEquation(src.equation_a));
    factor_source_rgb.Assign(PackBlendFactor(src.factor_source_rgb));
    factor_dest_rgb.Assign(PackBlendFactor(src.factor_dest_rgb));
    factor_source_a.Assign(PackBlendFactor(src.factor_source_a));
    factor_dest_a.Assign(PackBlendFactor(src.factor_dest_a));
    enable.Assign(1);
}

void FixedPipelineState::DynamicState::Fill(const Maxwell& regs) {
    u32 packed_front_face = PackFrontFace(regs.front_face);
    if (regs.screen_y_control.triangle_rast_flip != 0) {
        // Flip front face
        packed_front_face = 1 - packed_front_face;
    }

    raw1 = 0;
    raw2 = 0;
    front.action_stencil_fail.Assign(PackStencilOp(regs.stencil_front_op_fail));
    front.action_depth_fail.Assign(PackStencilOp(regs.stencil_front_op_zfail));
    front.action_depth_pass.Assign(PackStencilOp(regs.stencil_front_op_zpass));
    front.test_func.Assign(PackComparisonOp(regs.stencil_front_func_func));
    if (regs.stencil_two_side_enable) {
        back.action_stencil_fail.Assign(PackStencilOp(regs.stencil_back_op_fail));
        back.action_depth_fail.Assign(PackStencilOp(regs.stencil_back_op_zfail));
        back.action_depth_pass.Assign(PackStencilOp(regs.stencil_back_op_zpass));
        back.test_func.Assign(PackComparisonOp(regs.stencil_back_func_func));
    } else {
        back.action_stencil_fail.Assign(front.action_stencil_fail);
        back.action_depth_fail.Assign(front.action_depth_fail);
        back.action_depth_pass.Assign(front.action_depth_pass);
        back.test_func.Assign(front.test_func);
    }
    stencil_enable.Assign(regs.stencil_enable);
    depth_write_enable.Assign(regs.depth_write_enabled);
    depth_bounds_enable.Assign(regs.depth_bounds_enable);
    depth_test_enable.Assign(regs.depth_test_enable);
    front_face.Assign(packed_front_face);
    depth_test_func.Assign(PackComparisonOp(regs.depth_test_func));
    cull_face.Assign(PackCullFace(regs.cull_face));
    cull_enable.Assign(regs.cull_test_enabled != 0 ? 1 : 0);

    for (std::size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        const auto& input = regs.vertex_array[index];
        VertexBinding& binding = vertex_bindings[index];
        binding.raw = 0;
        binding.enabled.Assign(input.IsEnabled() ? 1 : 0);
        binding.stride.Assign(static_cast<u16>(input.stride.Value()));
    }
}

std::size_t FixedPipelineState::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<std::size_t>(hash);
}

bool FixedPipelineState::operator==(const FixedPipelineState& rhs) const noexcept {
    return std::memcmp(this, &rhs, Size()) == 0;
}

u32 FixedPipelineState::PackComparisonOp(Maxwell::ComparisonOp op) noexcept {
    // OpenGL enums go from 0x200 to 0x207 and the others from 1 to 8
    // If we substract 0x200 to OpenGL enums and 1 to the others we get a 0-7 range.
    // Perfect for a hash.
    const u32 value = static_cast<u32>(op);
    return value - (value >= 0x200 ? 0x200 : 1);
}

Maxwell::ComparisonOp FixedPipelineState::UnpackComparisonOp(u32 packed) noexcept {
    // Read PackComparisonOp for the logic behind this.
    return static_cast<Maxwell::ComparisonOp>(packed + 1);
}

u32 FixedPipelineState::PackStencilOp(Maxwell::StencilOp op) noexcept {
    switch (op) {
    case Maxwell::StencilOp::Keep:
    case Maxwell::StencilOp::KeepOGL:
        return 0;
    case Maxwell::StencilOp::Zero:
    case Maxwell::StencilOp::ZeroOGL:
        return 1;
    case Maxwell::StencilOp::Replace:
    case Maxwell::StencilOp::ReplaceOGL:
        return 2;
    case Maxwell::StencilOp::Incr:
    case Maxwell::StencilOp::IncrOGL:
        return 3;
    case Maxwell::StencilOp::Decr:
    case Maxwell::StencilOp::DecrOGL:
        return 4;
    case Maxwell::StencilOp::Invert:
    case Maxwell::StencilOp::InvertOGL:
        return 5;
    case Maxwell::StencilOp::IncrWrap:
    case Maxwell::StencilOp::IncrWrapOGL:
        return 6;
    case Maxwell::StencilOp::DecrWrap:
    case Maxwell::StencilOp::DecrWrapOGL:
        return 7;
    }
    return 0;
}

Maxwell::StencilOp FixedPipelineState::UnpackStencilOp(u32 packed) noexcept {
    static constexpr std::array LUT = {Maxwell::StencilOp::Keep,     Maxwell::StencilOp::Zero,
                                       Maxwell::StencilOp::Replace,  Maxwell::StencilOp::Incr,
                                       Maxwell::StencilOp::Decr,     Maxwell::StencilOp::Invert,
                                       Maxwell::StencilOp::IncrWrap, Maxwell::StencilOp::DecrWrap};
    return LUT[packed];
}

u32 FixedPipelineState::PackCullFace(Maxwell::CullFace cull) noexcept {
    // FrontAndBack is 0x408, by substracting 0x406 in it we get 2.
    // Individual cull faces are in 0x404 and 0x405, substracting 0x404 we get 0 and 1.
    const u32 value = static_cast<u32>(cull);
    return value - (value == 0x408 ? 0x406 : 0x404);
}

Maxwell::CullFace FixedPipelineState::UnpackCullFace(u32 packed) noexcept {
    static constexpr std::array LUT = {Maxwell::CullFace::Front, Maxwell::CullFace::Back,
                                       Maxwell::CullFace::FrontAndBack};
    return LUT[packed];
}

u32 FixedPipelineState::PackFrontFace(Maxwell::FrontFace face) noexcept {
    return static_cast<u32>(face) - 0x900;
}

Maxwell::FrontFace FixedPipelineState::UnpackFrontFace(u32 packed) noexcept {
    return static_cast<Maxwell::FrontFace>(packed + 0x900);
}

u32 FixedPipelineState::PackPolygonMode(Maxwell::PolygonMode mode) noexcept {
    return static_cast<u32>(mode) - 0x1B00;
}

Maxwell::PolygonMode FixedPipelineState::UnpackPolygonMode(u32 packed) noexcept {
    return static_cast<Maxwell::PolygonMode>(packed + 0x1B00);
}

u32 FixedPipelineState::PackLogicOp(Maxwell::LogicOperation op) noexcept {
    return static_cast<u32>(op) - 0x1500;
}

Maxwell::LogicOperation FixedPipelineState::UnpackLogicOp(u32 packed) noexcept {
    return static_cast<Maxwell::LogicOperation>(packed + 0x1500);
}

u32 FixedPipelineState::PackBlendEquation(Maxwell::Blend::Equation equation) noexcept {
    switch (equation) {
    case Maxwell::Blend::Equation::Add:
    case Maxwell::Blend::Equation::AddGL:
        return 0;
    case Maxwell::Blend::Equation::Subtract:
    case Maxwell::Blend::Equation::SubtractGL:
        return 1;
    case Maxwell::Blend::Equation::ReverseSubtract:
    case Maxwell::Blend::Equation::ReverseSubtractGL:
        return 2;
    case Maxwell::Blend::Equation::Min:
    case Maxwell::Blend::Equation::MinGL:
        return 3;
    case Maxwell::Blend::Equation::Max:
    case Maxwell::Blend::Equation::MaxGL:
        return 4;
    }
    return 0;
}

Maxwell::Blend::Equation FixedPipelineState::UnpackBlendEquation(u32 packed) noexcept {
    static constexpr std::array LUT = {
        Maxwell::Blend::Equation::Add, Maxwell::Blend::Equation::Subtract,
        Maxwell::Blend::Equation::ReverseSubtract, Maxwell::Blend::Equation::Min,
        Maxwell::Blend::Equation::Max};
    return LUT[packed];
}

u32 FixedPipelineState::PackBlendFactor(Maxwell::Blend::Factor factor) noexcept {
    switch (factor) {
    case Maxwell::Blend::Factor::Zero:
    case Maxwell::Blend::Factor::ZeroGL:
        return 0;
    case Maxwell::Blend::Factor::One:
    case Maxwell::Blend::Factor::OneGL:
        return 1;
    case Maxwell::Blend::Factor::SourceColor:
    case Maxwell::Blend::Factor::SourceColorGL:
        return 2;
    case Maxwell::Blend::Factor::OneMinusSourceColor:
    case Maxwell::Blend::Factor::OneMinusSourceColorGL:
        return 3;
    case Maxwell::Blend::Factor::SourceAlpha:
    case Maxwell::Blend::Factor::SourceAlphaGL:
        return 4;
    case Maxwell::Blend::Factor::OneMinusSourceAlpha:
    case Maxwell::Blend::Factor::OneMinusSourceAlphaGL:
        return 5;
    case Maxwell::Blend::Factor::DestAlpha:
    case Maxwell::Blend::Factor::DestAlphaGL:
        return 6;
    case Maxwell::Blend::Factor::OneMinusDestAlpha:
    case Maxwell::Blend::Factor::OneMinusDestAlphaGL:
        return 7;
    case Maxwell::Blend::Factor::DestColor:
    case Maxwell::Blend::Factor::DestColorGL:
        return 8;
    case Maxwell::Blend::Factor::OneMinusDestColor:
    case Maxwell::Blend::Factor::OneMinusDestColorGL:
        return 9;
    case Maxwell::Blend::Factor::SourceAlphaSaturate:
    case Maxwell::Blend::Factor::SourceAlphaSaturateGL:
        return 10;
    case Maxwell::Blend::Factor::Source1Color:
    case Maxwell::Blend::Factor::Source1ColorGL:
        return 11;
    case Maxwell::Blend::Factor::OneMinusSource1Color:
    case Maxwell::Blend::Factor::OneMinusSource1ColorGL:
        return 12;
    case Maxwell::Blend::Factor::Source1Alpha:
    case Maxwell::Blend::Factor::Source1AlphaGL:
        return 13;
    case Maxwell::Blend::Factor::OneMinusSource1Alpha:
    case Maxwell::Blend::Factor::OneMinusSource1AlphaGL:
        return 14;
    case Maxwell::Blend::Factor::ConstantColor:
    case Maxwell::Blend::Factor::ConstantColorGL:
        return 15;
    case Maxwell::Blend::Factor::OneMinusConstantColor:
    case Maxwell::Blend::Factor::OneMinusConstantColorGL:
        return 16;
    case Maxwell::Blend::Factor::ConstantAlpha:
    case Maxwell::Blend::Factor::ConstantAlphaGL:
        return 17;
    case Maxwell::Blend::Factor::OneMinusConstantAlpha:
    case Maxwell::Blend::Factor::OneMinusConstantAlphaGL:
        return 18;
    }
    return 0;
}

Maxwell::Blend::Factor FixedPipelineState::UnpackBlendFactor(u32 packed) noexcept {
    static constexpr std::array LUT = {
        Maxwell::Blend::Factor::Zero,
        Maxwell::Blend::Factor::One,
        Maxwell::Blend::Factor::SourceColor,
        Maxwell::Blend::Factor::OneMinusSourceColor,
        Maxwell::Blend::Factor::SourceAlpha,
        Maxwell::Blend::Factor::OneMinusSourceAlpha,
        Maxwell::Blend::Factor::DestAlpha,
        Maxwell::Blend::Factor::OneMinusDestAlpha,
        Maxwell::Blend::Factor::DestColor,
        Maxwell::Blend::Factor::OneMinusDestColor,
        Maxwell::Blend::Factor::SourceAlphaSaturate,
        Maxwell::Blend::Factor::Source1Color,
        Maxwell::Blend::Factor::OneMinusSource1Color,
        Maxwell::Blend::Factor::Source1Alpha,
        Maxwell::Blend::Factor::OneMinusSource1Alpha,
        Maxwell::Blend::Factor::ConstantColor,
        Maxwell::Blend::Factor::OneMinusConstantColor,
        Maxwell::Blend::Factor::ConstantAlpha,
        Maxwell::Blend::Factor::OneMinusConstantAlpha,
    };
    return LUT[packed];
}

} // namespace Vulkan
