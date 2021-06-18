// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_UI_SCENIC_CPP_COMMANDS_H_
#define LIB_UI_SCENIC_CPP_COMMANDS_H_

#include <fuchsia/images/cpp/fidl.h>
#include <fuchsia/scenic/allocation/cpp/fidl.h>
#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/time.h>

#include <string>

namespace scenic {

constexpr float kZeroesFloat3[3] = {0.f, 0.f, 0.f};
constexpr float kOnesFloat3[3] = {1.f, 1.f, 1.f};
// A quaterion that has no rotation.
constexpr float kQuaternionDefault[4] = {0.f, 0.f, 0.f, 1.f};

// Helper function for wrapping a GFX command as a Scenic command.
fuchsia::ui::scenic::Command NewCommand(fuchsia::ui::gfx::Command command);

// Helper function for wrapping an input command as a Scenic command.
fuchsia::ui::scenic::Command NewCommand(fuchsia::ui::input::Command command);

// Resource creation.
fuchsia::ui::gfx::Command NewCreateMemoryCmd(uint32_t id, zx::vmo vmo, uint64_t allocation_size,
                                             fuchsia::images::MemoryType memory_type);
fuchsia::ui::gfx::Command NewCreateImageCmd(uint32_t id, uint32_t memory_id, uint32_t memory_offset,
                                            fuchsia::images::ImageInfo info);
fuchsia::ui::gfx::Command NewCreateImage2Cmd(uint32_t id, uint32_t width, uint32_t height,
                                             uint32_t buffer_collection_id,
                                             uint32_t buffer_collection_index);
fuchsia::ui::gfx::Command NewCreateImage3Cmd(
    uint32_t id, uint32_t width, uint32_t height,
    fuchsia::scenic::allocation::BufferCollectionImportToken import_token,
    uint32_t buffer_collection_index);
fuchsia::ui::gfx::Command NewCreateImageCmd(uint32_t id, uint32_t memory_id, uint32_t memory_offset,
                                            fuchsia::images::PixelFormat format,
                                            fuchsia::images::ColorSpace color_space,
                                            fuchsia::images::Tiling tiling, uint32_t width,
                                            uint32_t height, uint32_t stride);
fuchsia::ui::gfx::Command NewCreateImagePipeCmd(
    uint32_t id, fidl::InterfaceRequest<fuchsia::images::ImagePipe> request);
fuchsia::ui::gfx::Command NewCreateImagePipe2Cmd(
    uint32_t id, fidl::InterfaceRequest<fuchsia::images::ImagePipe2> request);
fuchsia::ui::gfx::Command NewCreateBufferCmd(uint32_t id, uint32_t memory_id,
                                             uint32_t memory_offset, uint32_t num_bytes);

fuchsia::ui::gfx::Command NewCreateCompositorCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateDisplayCompositorCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateLayerStackCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateLayerCmd(uint32_t id);

fuchsia::ui::gfx::Command NewCreateSceneCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateCameraCmd(uint32_t id, uint32_t scene_id);
fuchsia::ui::gfx::Command NewCreateStereoCameraCmd(uint32_t id, uint32_t scene_id);
fuchsia::ui::gfx::Command NewCreateRendererCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateAmbientLightCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateDirectionalLightCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreatePointLightCmd(uint32_t id);

fuchsia::ui::gfx::Command NewCreateCircleCmd(uint32_t id, float radius);
fuchsia::ui::gfx::Command NewCreateRectangleCmd(uint32_t id, float width, float height);
fuchsia::ui::gfx::Command NewCreateRoundedRectangleCmd(uint32_t id, float width, float height,
                                                       float top_left_radius,
                                                       float top_right_radius,
                                                       float bottom_right_radius,
                                                       float bottom_left_radius);

// Variant of NewCreateCircleCmd that uses a variable radius instead of a
// constant one set at construction time.
fuchsia::ui::gfx::Command NewCreateVarCircleCmd(uint32_t id, uint32_t radius_var_id);
// Variant of NewCreateRectangleCmd that uses a variable width/height
// instead of constant ones set at construction time.
fuchsia::ui::gfx::Command NewCreateVarRectangleCmd(uint32_t id, uint32_t width_var_id,
                                                   uint32_t height_var_id);
// Variant of NewCreateRoundedRectangleCmd that uses a variable
// width/height/etc. instead of constant ones set at construction time.
fuchsia::ui::gfx::Command NewCreateVarRoundedRectangleCmd(uint32_t id, uint32_t width_var_id,
                                                          uint32_t height_var_id,
                                                          uint32_t top_left_radius_var_id,
                                                          uint32_t top_right_radius_var_id,
                                                          uint32_t bottom_left_radius_var_id,
                                                          uint32_t bottom_right_radius_var_id);

fuchsia::ui::gfx::Command NewCreateMeshCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateMaterialCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateClipNodeCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateEntityNodeCmd(uint32_t id);
fuchsia::ui::gfx::Command NewCreateOpacityNodeCmdHACK(uint32_t id);
fuchsia::ui::gfx::Command NewCreateShapeNodeCmd(uint32_t id);

fuchsia::ui::gfx::Command NewCreateViewCmd(uint32_t id, fuchsia::ui::views::ViewToken token,
                                           const cpp17::optional<std::string>& debug_name);
fuchsia::ui::gfx::Command NewCreateViewCmd(uint32_t id, fuchsia::ui::views::ViewToken token,
                                           fuchsia::ui::views::ViewRefControl control_ref,
                                           fuchsia::ui::views::ViewRef view_ref,
                                           const cpp17::optional<std::string>& debug_name);
fuchsia::ui::gfx::Command NewCreateViewHolderCmd(uint32_t id,
                                                 fuchsia::ui::views::ViewHolderToken token,
                                                 const cpp17::optional<std::string>& debug_name);

fuchsia::ui::gfx::Command NewCreateVariableCmd(uint32_t id, fuchsia::ui::gfx::Value value);

fuchsia::ui::gfx::Command NewReleaseResourceCmd(uint32_t id);

// Export & Import operations.
fuchsia::ui::gfx::Command NewExportResourceCmd(uint32_t resource_id, zx::eventpair export_token);
fuchsia::ui::gfx::Command NewImportResourceCmd(uint32_t resource_id,
                                               fuchsia::ui::gfx::ImportSpec spec,
                                               zx::eventpair import_token);

// Exports the resource and returns an import token in |out_import_token|
// which allows it to be imported into other sessions.
fuchsia::ui::gfx::Command NewExportResourceCmdAsRequest(uint32_t resource_id,
                                                        zx::eventpair* out_import_token);

// Imports the resource and returns an export token in |out_export_token|
// by which another session can export a resource to associate with this import.
fuchsia::ui::gfx::Command NewImportResourceCmdAsRequest(uint32_t resource_id,
                                                        fuchsia::ui::gfx::ImportSpec import_spec,
                                                        zx::eventpair* out_export_token);

// View/ViewHolder commands.
fuchsia::ui::gfx::Command NewSetViewPropertiesCmd(uint32_t view_holder_id,
                                                  const std::array<float, 3>& bounding_box_min,
                                                  const std::array<float, 3>& bounding_box_max,
                                                  const std::array<float, 3>& inset_from_min,
                                                  const std::array<float, 3>& inset_from_max);
fuchsia::ui::gfx::Command NewSetViewPropertiesCmd(uint32_t view_holder_id,
                                                  const fuchsia::ui::gfx::ViewProperties& props);

// Node operations.
fuchsia::ui::gfx::Command NewAddChildCmd(uint32_t node_id, uint32_t child_id);
fuchsia::ui::gfx::Command NewAddPartCmd(uint32_t node_id, uint32_t part_id);
fuchsia::ui::gfx::Command NewDetachCmd(uint32_t node_id);
fuchsia::ui::gfx::Command NewDetachChildrenCmd(uint32_t node_id);
fuchsia::ui::gfx::Command NewSetTranslationCmd(uint32_t node_id,
                                               const std::array<float, 3>& translation);
fuchsia::ui::gfx::Command NewSetTranslationCmd(uint32_t node_id, uint32_t variable_id);
fuchsia::ui::gfx::Command NewSetScaleCmd(uint32_t node_id, const std::array<float, 3>& scale);
fuchsia::ui::gfx::Command NewSetScaleCmd(uint32_t node_id, uint32_t variable_id);
fuchsia::ui::gfx::Command NewSetRotationCmd(uint32_t node_id,
                                            const std::array<float, 4>& quaternion);
fuchsia::ui::gfx::Command NewSetRotationCmd(uint32_t node_id, uint32_t variable_id);
fuchsia::ui::gfx::Command NewSetAnchorCmd(uint32_t node_id, const std::array<float, 3>& anchor);
fuchsia::ui::gfx::Command NewSetAnchorCmd(uint32_t node_id, uint32_t variable_id);

fuchsia::ui::gfx::Command NewSetOpacityCmd(uint32_t node_id, float opacity);
fuchsia::ui::gfx::Command NewSendSizeChangeHintCmdHACK(uint32_t node_id, float width_change_factor,
                                                       float height_change_factor);
fuchsia::ui::gfx::Command NewSetShapeCmd(uint32_t node_id, uint32_t shape_id);
fuchsia::ui::gfx::Command NewSetMaterialCmd(uint32_t node_id, uint32_t material_id);
fuchsia::ui::gfx::Command NewSetClipCmd(uint32_t node_id, uint32_t clip_id, bool clip_to_self);
fuchsia::ui::gfx::Command NewSetClipPlanesCmd(uint32_t node_id,
                                              std::vector<fuchsia::ui::gfx::Plane3> planes);
fuchsia::ui::gfx::Command NewSetTagCmd(uint32_t node_id, uint32_t tag_value);
fuchsia::ui::gfx::Command NewSetHitTestBehaviorCmd(
    uint32_t node_id, fuchsia::ui::gfx::HitTestBehavior hit_test_behavior);
fuchsia::ui::gfx::Command NewSetSemanticVisibilityCmd(uint32_t node_id, bool visibility);

// Display Commands.
fuchsia::ui::gfx::Command NewSetEnableDebugViewBoundsCmd(uint32_t view_id, bool enable);
fuchsia::ui::gfx::Command NewSetViewHolderBoundsColorCmd(uint32_t view_holder_id, uint8_t red,
                                                         uint8_t green, uint8_t blue);

fuchsia::ui::gfx::Command NewSetDisplayColorConversionCmdHACK(
    uint32_t compositor_id, const std::array<float, 3>& preoffsets,
    const std::array<float, 3 * 3>& matrix, const std::array<float, 3>& postoffsets);

fuchsia::ui::gfx::Command NewSetDisplayMinimumRgbCmdHACK(uint8_t minimum);

fuchsia::ui::gfx::Command NewSetDisplayRotationCmdHACK(uint32_t compositor_id,
                                                       uint32_t rotation_degrees);

// Camera and lighting operations.

fuchsia::ui::gfx::Command NewSetCameraCmd(uint32_t renderer_id, uint32_t camera_id);
fuchsia::ui::gfx::Command NewSetCameraTransformCmd(uint32_t camera_id,
                                                   const std::array<float, 3>& eye_position,
                                                   const std::array<float, 3>& eye_look_at,
                                                   const std::array<float, 3>& eye_up);
fuchsia::ui::gfx::Command NewSetCameraProjectionCmd(uint32_t camera_id, const float fovy);
fuchsia::ui::gfx::Command NewSetCameraClipSpaceTransformCmd(uint32_t camera_id, float x, float y,
                                                            float scale);

fuchsia::ui::gfx::Command NewSetCameraPoseBufferCmd(uint32_t camera_id, uint32_t buffer_id,
                                                    uint32_t num_entries, int64_t base_time,
                                                    uint64_t time_interval);

// Overloaded |NewSetCameraPoseBufferCmd()| to support `zx::time` and `zx::duration`.
fuchsia::ui::gfx::Command NewSetCameraPoseBufferCmd(uint32_t camera_id, uint32_t buffer_id,
                                                    uint32_t num_entries, zx::time base_time,
                                                    zx::duration time_interval);

fuchsia::ui::gfx::Command NewSetStereoCameraProjectionCmd(
    uint32_t camera_id, const std::array<float, 4 * 4>& left_projection,
    const std::array<float, 4 * 4>& right_projection);

fuchsia::ui::gfx::Command NewSetLightColorCmd(uint32_t light_id, const std::array<float, 3>& rgb);
fuchsia::ui::gfx::Command NewSetLightColorCmd(uint32_t light_id, uint32_t variable_id);
fuchsia::ui::gfx::Command NewSetLightDirectionCmd(uint32_t light_id,
                                                  const std::array<float, 3>& direction);
fuchsia::ui::gfx::Command NewSetLightDirectionCmd(uint32_t light_id, uint32_t variable_id);
fuchsia::ui::gfx::Command NewSetPointLightPositionCmd(uint32_t light_id,
                                                      const std::array<float, 3>& position);
fuchsia::ui::gfx::Command NewSetPointLightPositionCmd(uint32_t light_id, uint32_t variable_id);
fuchsia::ui::gfx::Command NewSetPointLightPositionCmd(uint32_t light_id,
                                                      const std::array<float, 3>& position);
fuchsia::ui::gfx::Command NewSetPointLightFalloffCmd(uint32_t light_id, float falloff);
fuchsia::ui::gfx::Command NewAddLightCmd(uint32_t scene_id, uint32_t light_id);
fuchsia::ui::gfx::Command NewSceneAddAmbientLightCmd(uint32_t scene_id, uint32_t light_id);
fuchsia::ui::gfx::Command NewSceneAddDirectionalLightCmd(uint32_t scene_id, uint32_t light_id);
fuchsia::ui::gfx::Command NewSceneAddPointLightCmd(uint32_t scene_id, uint32_t light_id);
fuchsia::ui::gfx::Command NewDetachLightCmd(uint32_t light_id);
fuchsia::ui::gfx::Command NewDetachLightsCmd(uint32_t scene_id);

// Material operations.
fuchsia::ui::gfx::Command NewSetTextureCmd(uint32_t material_id, uint32_t texture_id);
fuchsia::ui::gfx::Command NewSetColorCmd(uint32_t material_id, uint8_t red, uint8_t green,
                                         uint8_t blue, uint8_t alpha);

// Mesh operations.
fuchsia::ui::gfx::MeshVertexFormat NewMeshVertexFormat(fuchsia::ui::gfx::ValueType position_type,
                                                       fuchsia::ui::gfx::ValueType normal_type,
                                                       fuchsia::ui::gfx::ValueType tex_coord_type);
// These arguments are documented in commands.fidl; see BindMeshBuffersCmd.
fuchsia::ui::gfx::Command NewBindMeshBuffersCmd(
    uint32_t mesh_id, uint32_t index_buffer_id, fuchsia::ui::gfx::MeshIndexFormat index_format,
    uint64_t index_offset, uint32_t index_count, uint32_t vertex_buffer_id,
    fuchsia::ui::gfx::MeshVertexFormat vertex_format, uint64_t vertex_offset, uint32_t vertex_count,
    const std::array<float, 3>& bounding_box_min, const std::array<float, 3>& bounding_box_max);

// Layer / LayerStack / Compositor operations.
fuchsia::ui::gfx::Command NewAddLayerCmd(uint32_t layer_stack_id, uint32_t layer_id);
fuchsia::ui::gfx::Command NewRemoveLayerCmd(uint32_t layer_stack_id, uint32_t layer_id);
fuchsia::ui::gfx::Command NewRemoveAllLayersCmd(uint32_t layer_stack_id);
fuchsia::ui::gfx::Command NewSetLayerStackCmd(uint32_t compositor_id, uint32_t layer_stack_id);
fuchsia::ui::gfx::Command NewSetRendererCmd(uint32_t layer_id, uint32_t renderer_id);
fuchsia::ui::gfx::Command NewSetRendererParamCmd(uint32_t renderer_id,
                                                 fuchsia::ui::gfx::RendererParam param);
fuchsia::ui::gfx::Command NewSetSizeCmd(uint32_t node_id, const std::array<float, 2>& size);

// Event operations.
fuchsia::ui::gfx::Command NewSetEventMaskCmd(uint32_t resource_id, uint32_t event_mask);

// Diagnostic operations.
fuchsia::ui::gfx::Command NewSetLabelCmd(uint32_t resource_id, const std::string& label);

// Debugging operations.
fuchsia::ui::gfx::Command NewSetDisableClippingCmd(uint32_t renderer_id, bool disable_clipping);

// Basic types.
// All functions with C-style array arguments are deprecated. Use the std::array version instead.
fuchsia::ui::gfx::FloatValue NewFloatValue(float value);
fuchsia::ui::gfx::Vector2Value NewVector2Value(const std::array<float, 2>& value);
fuchsia::ui::gfx::Vector2Value NewVector2Value(uint32_t variable_id);
fuchsia::ui::gfx::Vector3Value NewVector3Value(const std::array<float, 3>& value);
fuchsia::ui::gfx::Vector3Value NewVector3Value(uint32_t variable_id);
fuchsia::ui::gfx::Vector4Value NewVector4Value(const std::array<float, 4>& value);
fuchsia::ui::gfx::Vector4Value NewVector4Value(uint32_t variable_id);
fuchsia::ui::gfx::QuaternionValue NewQuaternionValue(const std::array<float, 4>& value);
fuchsia::ui::gfx::QuaternionValue NewQuaternionValue(uint32_t variable_id);
fuchsia::ui::gfx::Matrix4Value NewMatrix4Value(const std::array<float, 4 * 4>& matrix);
fuchsia::ui::gfx::Matrix4Value NewMatrix4Value(uint32_t variable_id);
fuchsia::ui::gfx::ColorRgbValue NewColorRgbValue(float red, float green, float blue);
fuchsia::ui::gfx::ColorRgbValue NewColorRgbValue(uint32_t variable_id);
fuchsia::ui::gfx::ColorRgbaValue NewColorRgbaValue(const std::array<uint8_t, 4>& value);
fuchsia::ui::gfx::ColorRgbaValue NewColorRgbaValue(uint32_t variable_id);
fuchsia::ui::gfx::QuaternionValue NewQuaternionValue(const std::array<float, 4>& value);
fuchsia::ui::gfx::vec2 NewVector2(const std::array<float, 2>& value);
fuchsia::ui::gfx::vec3 NewVector3(const std::array<float, 3>& value);
fuchsia::ui::gfx::vec4 NewVector4(const std::array<float, 4>& value);

// Utilities.

bool ImageInfoEquals(const fuchsia::images::ImageInfo& a, const fuchsia::images::ImageInfo& b);

}  // namespace scenic

#endif  // LIB_UI_SCENIC_CPP_COMMANDS_H_
