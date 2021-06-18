// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/stdcompat/optional.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <zircon/assert.h>

#include <array>
#include <cstddef>

namespace {

template <size_t N, class T>
std::array<T, N> ToStdArray(const T* c_arr) {
  std::array<T, N> result;
  std::copy(c_arr, c_arr + N, std::begin(result));
  return result;
}

}  // namespace

namespace scenic {

fuchsia::ui::scenic::Command NewCommand(fuchsia::ui::gfx::Command command) {
  fuchsia::ui::scenic::Command scenic_command;
  scenic_command.set_gfx(std::move(command));
  return scenic_command;
}

fuchsia::ui::scenic::Command NewCommand(fuchsia::ui::input::Command command) {
  fuchsia::ui::scenic::Command scenic_command;
  scenic_command.set_input(std::move(command));
  return scenic_command;
}

// Helper function for all resource creation functions.
fuchsia::ui::gfx::Command NewCreateResourceCmd(uint32_t id,
                                               fuchsia::ui::gfx::ResourceArgs resource) {
  fuchsia::ui::gfx::CreateResourceCmd create_resource;
  create_resource.id = id;
  create_resource.resource = std::move(resource);

  fuchsia::ui::gfx::Command command;
  command.set_create_resource(std::move(create_resource));

  return command;
}

fuchsia::ui::gfx::Command NewCreateMemoryCmd(uint32_t id, zx::vmo vmo, uint64_t allocation_size,
                                             fuchsia::images::MemoryType memory_type) {
  fuchsia::ui::gfx::MemoryArgs memory;
  memory.vmo = std::move(vmo);
  memory.allocation_size = allocation_size;
  memory.memory_type = memory_type;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_memory(std::move(memory));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateImageCmd(uint32_t id, uint32_t memory_id, uint32_t memory_offset,
                                            fuchsia::images::ImageInfo info) {
  fuchsia::ui::gfx::ImageArgs image;
  image.memory_id = memory_id;
  image.memory_offset = memory_offset;
  image.info = info;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_image(image);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateImage2Cmd(uint32_t id, uint32_t width, uint32_t height,
                                             uint32_t buffer_collection_id,
                                             uint32_t buffer_collection_index) {
  fuchsia::ui::gfx::ImageArgs2 image;
  image.width = width;
  image.height = height;
  image.buffer_collection_id = buffer_collection_id;
  image.buffer_collection_index = buffer_collection_index;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_image2(image);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateImage3Cmd(
    uint32_t id, uint32_t width, uint32_t height,
    fuchsia::scenic::allocation::BufferCollectionImportToken import_token,
    uint32_t buffer_collection_index) {
  fuchsia::ui::gfx::ImageArgs3 image;
  image.width = width;
  image.height = height;
  image.import_token = std::move(import_token);
  image.buffer_collection_index = buffer_collection_index;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_image3(std::move(image));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateImagePipeCmd(
    uint32_t id, fidl::InterfaceRequest<fuchsia::images::ImagePipe> request) {
  fuchsia::ui::gfx::ImagePipeArgs image_pipe;
  image_pipe.image_pipe_request = std::move(request);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_image_pipe(std::move(image_pipe));
  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateImagePipe2Cmd(
    uint32_t id, fidl::InterfaceRequest<fuchsia::images::ImagePipe2> request) {
  fuchsia::ui::gfx::ImagePipe2Args image_pipe;
  image_pipe.image_pipe_request = std::move(request);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_image_pipe2(std::move(image_pipe));
  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateImageCmd(uint32_t id, uint32_t memory_id, uint32_t memory_offset,
                                            fuchsia::images::PixelFormat format,
                                            fuchsia::images::ColorSpace color_space,
                                            fuchsia::images::Tiling tiling, uint32_t width,
                                            uint32_t height, uint32_t stride) {
  fuchsia::images::ImageInfo info;
  info.pixel_format = format;
  info.color_space = color_space;
  info.tiling = tiling;
  info.width = width;
  info.height = height;
  info.stride = stride;
  return NewCreateImageCmd(id, memory_id, memory_offset, info);
}

fuchsia::ui::gfx::Command NewCreateBufferCmd(uint32_t id, uint32_t memory_id,
                                             uint32_t memory_offset, uint32_t num_bytes) {
  fuchsia::ui::gfx::BufferArgs buffer;
  buffer.memory_id = memory_id;
  buffer.memory_offset = memory_offset;
  buffer.num_bytes = num_bytes;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_buffer(buffer);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateCompositorCmd(uint32_t id) {
  fuchsia::ui::gfx::CompositorArgs compositor;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_compositor(compositor);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateDisplayCompositorCmd(uint32_t id) {
  fuchsia::ui::gfx::DisplayCompositorArgs display_compositor;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_display_compositor(display_compositor);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateLayerStackCmd(uint32_t id) {
  fuchsia::ui::gfx::LayerStackArgs layer_stack;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_layer_stack(layer_stack);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateLayerCmd(uint32_t id) {
  fuchsia::ui::gfx::LayerArgs layer;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_layer(layer);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateSceneCmd(uint32_t id) {
  fuchsia::ui::gfx::SceneArgs scene;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_scene(scene);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateCameraCmd(uint32_t id, uint32_t scene_id) {
  fuchsia::ui::gfx::CameraArgs camera;
  camera.scene_id = scene_id;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_camera(camera);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateStereoCameraCmd(uint32_t id, uint32_t scene_id) {
  fuchsia::ui::gfx::StereoCameraArgs stereo_camera;
  stereo_camera.scene_id = scene_id;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_stereo_camera(stereo_camera);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateRendererCmd(uint32_t id) {
  fuchsia::ui::gfx::RendererArgs renderer;
  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_renderer(renderer);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateAmbientLightCmd(uint32_t id) {
  fuchsia::ui::gfx::AmbientLightArgs ambient_light;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_ambient_light(ambient_light);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateDirectionalLightCmd(uint32_t id) {
  fuchsia::ui::gfx::DirectionalLightArgs directional_light;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_directional_light(directional_light);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreatePointLightCmd(uint32_t id) {
  fuchsia::ui::gfx::PointLightArgs point_light;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_point_light(point_light);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateCircleCmd(uint32_t id, float radius) {
  fuchsia::ui::gfx::Value radius_value;
  radius_value.set_vector1(radius);

  fuchsia::ui::gfx::CircleArgs circle;
  circle.radius = std::move(radius_value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_circle(std::move(circle));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateRectangleCmd(uint32_t id, float width, float height) {
  fuchsia::ui::gfx::Value width_value;
  width_value.set_vector1(width);

  fuchsia::ui::gfx::Value height_value;
  height_value.set_vector1(height);

  fuchsia::ui::gfx::RectangleArgs rectangle;
  rectangle.width = std::move(width_value);
  rectangle.height = std::move(height_value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_rectangle(std::move(rectangle));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateRoundedRectangleCmd(uint32_t id, float width, float height,
                                                       float top_left_radius,
                                                       float top_right_radius,
                                                       float bottom_right_radius,
                                                       float bottom_left_radius) {
  fuchsia::ui::gfx::Value width_value;
  width_value.set_vector1(width);

  fuchsia::ui::gfx::Value height_value;
  height_value.set_vector1(height);

  fuchsia::ui::gfx::Value top_left_radius_value;
  top_left_radius_value.set_vector1(top_left_radius);

  fuchsia::ui::gfx::Value top_right_radius_value;
  top_right_radius_value.set_vector1(top_right_radius);

  fuchsia::ui::gfx::Value bottom_right_radius_value;
  bottom_right_radius_value.set_vector1(bottom_right_radius);

  fuchsia::ui::gfx::Value bottom_left_radius_value;
  bottom_left_radius_value.set_vector1(bottom_left_radius);

  fuchsia::ui::gfx::RoundedRectangleArgs rectangle;
  rectangle.width = std::move(width_value);
  rectangle.height = std::move(height_value);
  rectangle.top_left_radius = std::move(top_left_radius_value);
  rectangle.top_right_radius = std::move(top_right_radius_value);
  rectangle.bottom_right_radius = std::move(bottom_right_radius_value);
  rectangle.bottom_left_radius = std::move(bottom_left_radius_value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_rounded_rectangle(std::move(rectangle));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateVarCircleCmd(uint32_t id, uint32_t radius_var_id) {
  fuchsia::ui::gfx::Value radius_value;
  radius_value.set_variable_id(radius_var_id);

  fuchsia::ui::gfx::CircleArgs circle;
  circle.radius = std::move(radius_value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_circle(std::move(circle));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateVarRectangleCmd(uint32_t id, uint32_t width_var_id,
                                                   uint32_t height_var_id) {
  fuchsia::ui::gfx::Value width_value;
  width_value.set_variable_id(width_var_id);

  fuchsia::ui::gfx::Value height_value;
  height_value.set_variable_id(height_var_id);

  fuchsia::ui::gfx::RectangleArgs rectangle;
  rectangle.width = std::move(width_value);
  rectangle.height = std::move(height_value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_rectangle(std::move(rectangle));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateVarRoundedRectangleCmd(uint32_t id, uint32_t width_var_id,
                                                          uint32_t height_var_id,
                                                          uint32_t top_left_radius_var_id,
                                                          uint32_t top_right_radius_var_id,
                                                          uint32_t bottom_left_radius_var_id,
                                                          uint32_t bottom_right_radius_var_id) {
  fuchsia::ui::gfx::Value width_value;
  width_value.set_variable_id(width_var_id);

  fuchsia::ui::gfx::Value height_value;
  height_value.set_variable_id(height_var_id);

  fuchsia::ui::gfx::Value top_left_radius_value;
  top_left_radius_value.set_variable_id(top_left_radius_var_id);

  fuchsia::ui::gfx::Value top_right_radius_value;
  top_right_radius_value.set_variable_id(top_right_radius_var_id);

  fuchsia::ui::gfx::Value bottom_left_radius_value;
  bottom_left_radius_value.set_variable_id(bottom_left_radius_var_id);

  fuchsia::ui::gfx::Value bottom_right_radius_value;
  bottom_right_radius_value.set_variable_id(bottom_right_radius_var_id);

  fuchsia::ui::gfx::RoundedRectangleArgs rectangle;
  rectangle.width = std::move(width_value);
  rectangle.height = std::move(height_value);
  rectangle.top_left_radius = std::move(top_left_radius_value);
  rectangle.top_right_radius = std::move(top_right_radius_value);
  rectangle.bottom_left_radius = std::move(bottom_left_radius_value);
  rectangle.bottom_right_radius = std::move(bottom_right_radius_value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_rounded_rectangle(std::move(rectangle));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateMeshCmd(uint32_t id) {
  fuchsia::ui::gfx::MeshArgs mesh;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_mesh(mesh);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateMaterialCmd(uint32_t id) {
  fuchsia::ui::gfx::MaterialArgs material;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_material(material);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateClipNodeCmd(uint32_t id) {
  fuchsia::ui::gfx::ClipNodeArgs node;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_clip_node(node);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateEntityNodeCmd(uint32_t id) {
  fuchsia::ui::gfx::EntityNodeArgs node;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_entity_node(node);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateOpacityNodeCmdHACK(uint32_t id) {
  fuchsia::ui::gfx::OpacityNodeArgsHACK node;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_opacity_node(node);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateShapeNodeCmd(uint32_t id) {
  fuchsia::ui::gfx::ShapeNodeArgs node;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_shape_node(node);

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateViewCmd(uint32_t id, fuchsia::ui::views::ViewToken token,
                                           const cpp17::optional<std::string>& debug_name) {
  ZX_DEBUG_ASSERT(token.value);

  scenic::ViewRefPair ref_pair = scenic::ViewRefPair::New();

  fuchsia::ui::gfx::ViewArgs3 view;
  view.token = std::move(token);
  view.control_ref = std::move(ref_pair.control_ref);
  view.view_ref = std::move(ref_pair.view_ref);
  view.debug_name = debug_name ? fidl::StringPtr(*debug_name) : cpp17::nullopt;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_view3(std::move(view));
  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateViewCmd(uint32_t id, fuchsia::ui::views::ViewToken token,
                                           fuchsia::ui::views::ViewRefControl control_ref,
                                           fuchsia::ui::views::ViewRef view_ref,
                                           const cpp17::optional<std::string>& debug_name) {
  ZX_DEBUG_ASSERT(token.value);

  fuchsia::ui::gfx::ViewArgs3 view;
  view.token = std::move(token);
  view.control_ref = std::move(control_ref);
  view.view_ref = std::move(view_ref);
  view.debug_name = debug_name ? fidl::StringPtr(*debug_name) : cpp17::nullopt;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_view3(std::move(view));
  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateViewHolderCmd(uint32_t id,
                                                 fuchsia::ui::views::ViewHolderToken token,
                                                 const cpp17::optional<std::string>& debug_name) {
  ZX_DEBUG_ASSERT(token.value);

  fuchsia::ui::gfx::ViewHolderArgs view_holder;
  view_holder.token = std::move(token);
  view_holder.debug_name = debug_name ? fidl::StringPtr(*debug_name) : cpp17::nullopt;

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_view_holder(std::move(view_holder));
  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewCreateVariableCmd(uint32_t id, fuchsia::ui::gfx::Value value) {
  fuchsia::ui::gfx::VariableArgs variable;
  switch (value.Which()) {
    case ::fuchsia::ui::gfx::Value::Tag::kVector1:
      variable.type = fuchsia::ui::gfx::ValueType::kVector1;
      break;
    case ::fuchsia::ui::gfx::Value::Tag::kVector2:
      variable.type = fuchsia::ui::gfx::ValueType::kVector2;
      break;
    case ::fuchsia::ui::gfx::Value::Tag::kVector3:
      variable.type = fuchsia::ui::gfx::ValueType::kVector3;
      break;
    case ::fuchsia::ui::gfx::Value::Tag::kVector4:
      variable.type = fuchsia::ui::gfx::ValueType::kVector4;
      break;
    case fuchsia::ui::gfx::Value::Tag::kMatrix4x4:
      variable.type = fuchsia::ui::gfx::ValueType::kMatrix4;
      break;
    case fuchsia::ui::gfx::Value::Tag::kColorRgba:
      variable.type = fuchsia::ui::gfx::ValueType::kColorRgba;
      break;
    case fuchsia::ui::gfx::Value::Tag::kColorRgb:
      variable.type = fuchsia::ui::gfx::ValueType::kColorRgb;
      break;
    case fuchsia::ui::gfx::Value::Tag::kDegrees:
      variable.type = fuchsia::ui::gfx::ValueType::kVector1;
      break;
    case fuchsia::ui::gfx::Value::Tag::kTransform:
      variable.type = fuchsia::ui::gfx::ValueType::kFactoredTransform;
      break;
    case fuchsia::ui::gfx::Value::Tag::kQuaternion:
      variable.type = fuchsia::ui::gfx::ValueType::kQuaternion;
      break;
    case fuchsia::ui::gfx::Value::Tag::kVariableId:
      // A variable's initial value cannot be another variable.
      // This is also an invalid case, thus fall through.
    case fuchsia::ui::gfx::Value::Tag::Invalid:
      return fuchsia::ui::gfx::Command();
  }
  variable.initial_value = std::move(value);

  fuchsia::ui::gfx::ResourceArgs resource;
  resource.set_variable(std::move(variable));

  return NewCreateResourceCmd(id, std::move(resource));
}

fuchsia::ui::gfx::Command NewReleaseResourceCmd(uint32_t id) {
  fuchsia::ui::gfx::ReleaseResourceCmd release_resource;
  release_resource.id = id;

  fuchsia::ui::gfx::Command command;
  command.set_release_resource(release_resource);

  return command;
}

fuchsia::ui::gfx::Command NewExportResourceCmd(uint32_t resource_id, zx::eventpair export_token) {
  ZX_DEBUG_ASSERT(export_token);

  fuchsia::ui::gfx::ExportResourceCmdDeprecated export_resource;
  export_resource.id = resource_id;
  export_resource.token = std::move(export_token);

  fuchsia::ui::gfx::Command command;
  command.set_export_resource(std::move(export_resource));

  return command;
}

fuchsia::ui::gfx::Command NewImportResourceCmd(uint32_t resource_id,
                                               fuchsia::ui::gfx::ImportSpec spec,
                                               zx::eventpair import_token) {
  ZX_DEBUG_ASSERT(import_token);

  fuchsia::ui::gfx::ImportResourceCmdDeprecated import_resource;
  import_resource.id = resource_id;
  import_resource.token = std::move(import_token);
  import_resource.spec = spec;

  fuchsia::ui::gfx::Command command;
  command.set_import_resource(std::move(import_resource));

  return command;
}

fuchsia::ui::gfx::Command NewExportResourceCmdAsRequest(uint32_t resource_id,
                                                        zx::eventpair* out_import_token) {
  ZX_DEBUG_ASSERT(out_import_token);
  ZX_DEBUG_ASSERT(!*out_import_token);

  zx::eventpair export_token;
  zx_status_t status = zx::eventpair::create(0u, &export_token, out_import_token);
  ZX_DEBUG_ASSERT_MSG(status == ZX_OK, "event pair create failed: status=%d", status);
  return NewExportResourceCmd(resource_id, std::move(export_token));
}

fuchsia::ui::gfx::Command NewImportResourceCmdAsRequest(uint32_t resource_id,
                                                        fuchsia::ui::gfx::ImportSpec import_spec,
                                                        zx::eventpair* out_export_token) {
  ZX_DEBUG_ASSERT(out_export_token);
  ZX_DEBUG_ASSERT(!*out_export_token);

  zx::eventpair import_token;
  zx_status_t status = zx::eventpair::create(0u, &import_token, out_export_token);
  ZX_DEBUG_ASSERT_MSG(status == ZX_OK, "event pair create failed: status=%d", status);
  return NewImportResourceCmd(resource_id, import_spec, std::move(import_token));
}

fuchsia::ui::gfx::Command NewSetViewPropertiesCmd(uint32_t view_holder_id,
                                                  const std::array<float, 3>& bounding_box_min,
                                                  const std::array<float, 3>& bounding_box_max,
                                                  const std::array<float, 3>& inset_from_min,
                                                  const std::array<float, 3>& inset_from_max) {
  fuchsia::ui::gfx::SetViewPropertiesCmd set_view_properties;
  set_view_properties.view_holder_id = view_holder_id;
  auto& props = set_view_properties.properties;
  props.bounding_box.min = NewVector3(bounding_box_min);
  props.bounding_box.max = NewVector3(bounding_box_max);
  props.inset_from_min = NewVector3(inset_from_min);
  props.inset_from_max = NewVector3(inset_from_max);

  fuchsia::ui::gfx::Command command;
  command.set_set_view_properties(set_view_properties);

  return command;
}

fuchsia::ui::gfx::Command NewSetViewPropertiesCmd(uint32_t view_holder_id,
                                                  const fuchsia::ui::gfx::ViewProperties& props) {
  fuchsia::ui::gfx::SetViewPropertiesCmd set_view_properties;
  set_view_properties.view_holder_id = view_holder_id;
  set_view_properties.properties = props;

  fuchsia::ui::gfx::Command command;
  command.set_set_view_properties(set_view_properties);

  return command;
}

fuchsia::ui::gfx::Command NewAddChildCmd(uint32_t node_id, uint32_t child_id) {
  fuchsia::ui::gfx::AddChildCmd add_child;
  add_child.node_id = node_id;
  add_child.child_id = child_id;

  fuchsia::ui::gfx::Command command;
  command.set_add_child(add_child);

  return command;
}

fuchsia::ui::gfx::Command NewDetachCmd(uint32_t id) {
  fuchsia::ui::gfx::DetachCmd detach;
  detach.id = id;

  fuchsia::ui::gfx::Command command;
  command.set_detach(detach);

  return command;
}

fuchsia::ui::gfx::Command NewDetachChildrenCmd(uint32_t node_id) {
  fuchsia::ui::gfx::DetachChildrenCmd detach_children;
  detach_children.node_id = node_id;

  fuchsia::ui::gfx::Command command;
  command.set_detach_children(detach_children);

  return command;
}

fuchsia::ui::gfx::Command NewSetTranslationCmd(uint32_t node_id,
                                               const std::array<float, 3>& translation) {
  fuchsia::ui::gfx::SetTranslationCmd set_translation;
  set_translation.id = node_id;
  set_translation.value = NewVector3Value(translation);

  fuchsia::ui::gfx::Command command;
  command.set_set_translation(set_translation);

  return command;
}

fuchsia::ui::gfx::Command NewSetTranslationCmd(uint32_t node_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetTranslationCmd set_translation;
  set_translation.id = node_id;
  set_translation.value = NewVector3Value(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_translation(set_translation);

  return command;
}

fuchsia::ui::gfx::Command NewSetScaleCmd(uint32_t node_id, const std::array<float, 3>& scale) {
  fuchsia::ui::gfx::SetScaleCmd set_scale;
  set_scale.id = node_id;
  set_scale.value = NewVector3Value(scale);

  fuchsia::ui::gfx::Command command;
  command.set_set_scale(set_scale);

  return command;
}

fuchsia::ui::gfx::Command NewSetScaleCmd(uint32_t node_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetScaleCmd set_scale;
  set_scale.id = node_id;
  set_scale.value = NewVector3Value(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_scale(set_scale);

  return command;
}

fuchsia::ui::gfx::Command NewSetRotationCmd(uint32_t node_id,
                                            const std::array<float, 4>& quaternion) {
  fuchsia::ui::gfx::SetRotationCmd set_rotation;
  set_rotation.id = node_id;
  set_rotation.value = NewQuaternionValue(quaternion);

  fuchsia::ui::gfx::Command command;
  command.set_set_rotation(set_rotation);

  return command;
}

fuchsia::ui::gfx::Command NewSetRotationCmd(uint32_t node_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetRotationCmd set_rotation;
  set_rotation.id = node_id;
  set_rotation.value = NewQuaternionValue(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_rotation(set_rotation);

  return command;
}

fuchsia::ui::gfx::Command NewSetAnchorCmd(uint32_t node_id, const std::array<float, 3>& anchor) {
  fuchsia::ui::gfx::SetAnchorCmd set_anchor;
  set_anchor.id = node_id;
  set_anchor.value = NewVector3Value(anchor);

  fuchsia::ui::gfx::Command command;
  command.set_set_anchor(set_anchor);

  return command;
}

fuchsia::ui::gfx::Command NewSetAnchorCmd(uint32_t node_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetAnchorCmd set_anchor;
  set_anchor.id = node_id;
  set_anchor.value = NewVector3Value(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_anchor(set_anchor);

  return command;
}

fuchsia::ui::gfx::Command NewSetOpacityCmd(uint32_t node_id, float opacity) {
  fuchsia::ui::gfx::SetOpacityCmd set_opacity;
  set_opacity.node_id = node_id;
  set_opacity.opacity = opacity;

  fuchsia::ui::gfx::Command command;
  command.set_set_opacity(set_opacity);

  return command;
}

fuchsia::ui::gfx::Command NewSetEnableDebugViewBoundsCmd(uint32_t view_id, bool enable) {
  fuchsia::ui::gfx::SetEnableDebugViewBoundsCmd enable_cmd;
  enable_cmd.view_id = view_id;
  enable_cmd.enable = enable;

  fuchsia::ui::gfx::Command command;
  command.set_set_enable_view_debug_bounds(enable_cmd);
  return command;
}

fuchsia::ui::gfx::Command NewSetViewHolderBoundsColorCmd(uint32_t view_holder_id, uint8_t red,
                                                         uint8_t green, uint8_t blue) {
  fuchsia::ui::gfx::ColorRgbValue color;
  color.value.red = red;
  color.value.green = green;
  color.value.blue = blue;

  fuchsia::ui::gfx::SetViewHolderBoundsColorCmd bounds_color_cmd;
  bounds_color_cmd.view_holder_id = view_holder_id;
  bounds_color_cmd.color = color;

  fuchsia::ui::gfx::Command command;
  command.set_set_view_holder_bounds_color(bounds_color_cmd);
  return command;
}

fuchsia::ui::gfx::Command NewSetDisplayColorConversionCmdHACK(
    uint32_t compositor_id, const std::array<float, 3>& preoffsets,
    const std::array<float, 3 * 3>& matrix, const std::array<float, 3>& postoffsets) {
  fuchsia::ui::gfx::SetDisplayColorConversionCmdHACK color_conversion;
  color_conversion.compositor_id = compositor_id;
  color_conversion.preoffsets = preoffsets;
  color_conversion.matrix = matrix;
  color_conversion.postoffsets = postoffsets;

  fuchsia::ui::gfx::Command command;
  command.set_set_display_color_conversion(color_conversion);

  return command;
}

fuchsia::ui::gfx::Command NewSetDisplayMinimumRgbCmdHACK(uint8_t minimum) {
  fuchsia::ui::gfx::SetDisplayMinimumRgbCmdHACK minimum_cmd;
  minimum_cmd.min_value = minimum;
  fuchsia::ui::gfx::Command command;
  command.set_set_display_minimum_rgb(minimum_cmd);
  return command;
}

fuchsia::ui::gfx::Command NewSetDisplayRotationCmdHACK(uint32_t compositor_id,
                                                       uint32_t rotation_degrees) {
  fuchsia::ui::gfx::SetDisplayRotationCmdHACK display_rotation;
  display_rotation.compositor_id = compositor_id;
  display_rotation.rotation_degrees = rotation_degrees;

  fuchsia::ui::gfx::Command command;
  command.set_set_display_rotation(display_rotation);

  return command;
}

fuchsia::ui::gfx::Command NewSendSizeChangeHintCmdHACK(uint32_t node_id, float width_change_factor,
                                                       float height_change_factor) {
  fuchsia::ui::gfx::SendSizeChangeHintCmdHACK send_size_change_hint;
  send_size_change_hint.node_id = node_id;
  send_size_change_hint.width_change_factor = width_change_factor;
  send_size_change_hint.height_change_factor = height_change_factor;

  fuchsia::ui::gfx::Command command;
  command.set_send_size_change_hint_hack(send_size_change_hint);

  return command;
}

fuchsia::ui::gfx::Command NewSetShapeCmd(uint32_t node_id, uint32_t shape_id) {
  fuchsia::ui::gfx::SetShapeCmd set_shape;
  set_shape.node_id = node_id;
  set_shape.shape_id = shape_id;

  fuchsia::ui::gfx::Command command;
  command.set_set_shape(set_shape);

  return command;
}

fuchsia::ui::gfx::Command NewSetMaterialCmd(uint32_t node_id, uint32_t material_id) {
  fuchsia::ui::gfx::SetMaterialCmd set_material;
  set_material.node_id = node_id;
  set_material.material_id = material_id;

  fuchsia::ui::gfx::Command command;
  command.set_set_material(set_material);

  return command;
}

fuchsia::ui::gfx::Command NewSetClipCmd(uint32_t node_id, uint32_t clip_id, bool clip_to_self) {
  fuchsia::ui::gfx::SetClipCmd set_clip;
  set_clip.node_id = node_id;
  set_clip.clip_id = clip_id;
  set_clip.clip_to_self = clip_to_self;

  fuchsia::ui::gfx::Command command;
  command.set_set_clip(set_clip);

  return command;
}

fuchsia::ui::gfx::Command NewSetClipPlanesCmd(uint32_t node_id,
                                              std::vector<fuchsia::ui::gfx::Plane3> planes) {
  fuchsia::ui::gfx::SetClipPlanesCmd set_clip_planes;
  set_clip_planes.node_id = node_id;
  set_clip_planes.clip_planes = std::move(planes);

  fuchsia::ui::gfx::Command command;
  command.set_set_clip_planes(set_clip_planes);

  return command;
}

fuchsia::ui::gfx::Command NewSetTagCmd(uint32_t node_id, uint32_t tag_value) {
  fuchsia::ui::gfx::SetTagCmd set_tag;
  set_tag.node_id = node_id;
  set_tag.tag_value = tag_value;

  fuchsia::ui::gfx::Command command;
  command.set_set_tag(set_tag);

  return command;
}

fuchsia::ui::gfx::Command NewSetHitTestBehaviorCmd(
    uint32_t node_id, fuchsia::ui::gfx::HitTestBehavior hit_test_behavior) {
  fuchsia::ui::gfx::SetHitTestBehaviorCmd set_hit_test_behavior;
  set_hit_test_behavior.node_id = node_id;
  set_hit_test_behavior.hit_test_behavior = hit_test_behavior;

  fuchsia::ui::gfx::Command command;
  command.set_set_hit_test_behavior(set_hit_test_behavior);

  return command;
}

fuchsia::ui::gfx::Command NewSetSemanticVisibilityCmd(uint32_t node_id, bool visible) {
  fuchsia::ui::gfx::SetSemanticVisibilityCmd set_semantic_visibility;
  set_semantic_visibility.node_id = node_id;
  set_semantic_visibility.visible = visible;

  fuchsia::ui::gfx::Command command;
  command.set_set_semantic_visibility(set_semantic_visibility);

  return command;
}

fuchsia::ui::gfx::Command NewSetCameraCmd(uint32_t renderer_id, uint32_t camera_id) {
  fuchsia::ui::gfx::SetCameraCmd set_camera;
  set_camera.renderer_id = renderer_id;
  set_camera.camera_id = camera_id;

  fuchsia::ui::gfx::Command command;
  command.set_set_camera(set_camera);
  return command;
}

fuchsia::ui::gfx::Command NewSetTextureCmd(uint32_t material_id, uint32_t texture_id) {
  fuchsia::ui::gfx::SetTextureCmd set_texture;
  set_texture.material_id = material_id;
  set_texture.texture_id = texture_id;

  fuchsia::ui::gfx::Command command;
  command.set_set_texture(set_texture);
  return command;
}

fuchsia::ui::gfx::Command NewSetColorCmd(uint32_t material_id, uint8_t red, uint8_t green,
                                         uint8_t blue, uint8_t alpha) {
  fuchsia::ui::gfx::ColorRgbaValue color;
  color.value.red = red;
  color.value.green = green;
  color.value.blue = blue;
  color.value.alpha = alpha;
  color.variable_id = 0;
  fuchsia::ui::gfx::SetColorCmd set_color;
  set_color.material_id = material_id;
  set_color.color = color;

  fuchsia::ui::gfx::Command command;
  command.set_set_color(set_color);

  return command;
}

fuchsia::ui::gfx::MeshVertexFormat NewMeshVertexFormat(fuchsia::ui::gfx::ValueType position_type,
                                                       fuchsia::ui::gfx::ValueType normal_type,
                                                       fuchsia::ui::gfx::ValueType tex_coord_type) {
  fuchsia::ui::gfx::MeshVertexFormat vertex_format;
  vertex_format.position_type = position_type;
  vertex_format.normal_type = normal_type;
  vertex_format.tex_coord_type = tex_coord_type;
  return vertex_format;
}

fuchsia::ui::gfx::Command NewBindMeshBuffersCmd(
    uint32_t mesh_id, uint32_t index_buffer_id, fuchsia::ui::gfx::MeshIndexFormat index_format,
    uint64_t index_offset, uint32_t index_count, uint32_t vertex_buffer_id,
    fuchsia::ui::gfx::MeshVertexFormat vertex_format, uint64_t vertex_offset, uint32_t vertex_count,
    const std::array<float, 3>& bounding_box_min, const std::array<float, 3>& bounding_box_max) {
  fuchsia::ui::gfx::BindMeshBuffersCmd bind_mesh_buffers;
  bind_mesh_buffers.mesh_id = mesh_id;
  bind_mesh_buffers.index_buffer_id = index_buffer_id;
  bind_mesh_buffers.index_format = index_format;
  bind_mesh_buffers.index_offset = index_offset;
  bind_mesh_buffers.index_count = index_count;
  bind_mesh_buffers.vertex_buffer_id = vertex_buffer_id;
  bind_mesh_buffers.vertex_format = vertex_format;
  bind_mesh_buffers.vertex_offset = vertex_offset;
  bind_mesh_buffers.vertex_count = vertex_count;
  auto& bbox = bind_mesh_buffers.bounding_box;
  bbox.min.x = bounding_box_min[0];
  bbox.min.y = bounding_box_min[1];
  bbox.min.z = bounding_box_min[2];
  bbox.max.x = bounding_box_max[0];
  bbox.max.y = bounding_box_max[1];
  bbox.max.z = bounding_box_max[2];

  fuchsia::ui::gfx::Command command;
  command.set_bind_mesh_buffers(bind_mesh_buffers);

  return command;
}

fuchsia::ui::gfx::Command NewAddLayerCmd(uint32_t layer_stack_id, uint32_t layer_id) {
  fuchsia::ui::gfx::AddLayerCmd add_layer;
  add_layer.layer_stack_id = layer_stack_id;
  add_layer.layer_id = layer_id;

  fuchsia::ui::gfx::Command command;
  command.set_add_layer(add_layer);
  return command;
}

fuchsia::ui::gfx::Command NewRemoveLayerCmd(uint32_t layer_stack_id, uint32_t layer_id) {
  fuchsia::ui::gfx::RemoveLayerCmd remove_layer;
  remove_layer.layer_stack_id = layer_stack_id;
  remove_layer.layer_id = layer_id;

  fuchsia::ui::gfx::Command command;
  command.set_remove_layer(remove_layer);
  return command;
}

fuchsia::ui::gfx::Command NewRemoveAllLayersCmd(uint32_t layer_stack_id) {
  fuchsia::ui::gfx::RemoveAllLayersCmd remove_all_layers;
  remove_all_layers.layer_stack_id = layer_stack_id;

  fuchsia::ui::gfx::Command command;
  command.set_remove_all_layers(remove_all_layers);
  return command;
}

fuchsia::ui::gfx::Command NewSetLayerStackCmd(uint32_t compositor_id, uint32_t layer_stack_id) {
  fuchsia::ui::gfx::SetLayerStackCmd set_layer_stack;
  set_layer_stack.compositor_id = compositor_id;
  set_layer_stack.layer_stack_id = layer_stack_id;

  fuchsia::ui::gfx::Command command;
  command.set_set_layer_stack(set_layer_stack);
  return command;
}

fuchsia::ui::gfx::Command NewSetRendererCmd(uint32_t layer_id, uint32_t renderer_id) {
  fuchsia::ui::gfx::SetRendererCmd set_renderer;
  set_renderer.layer_id = layer_id;
  set_renderer.renderer_id = renderer_id;

  fuchsia::ui::gfx::Command command;
  command.set_set_renderer(set_renderer);
  return command;
}

fuchsia::ui::gfx::Command NewSetRendererParamCmd(uint32_t renderer_id,
                                                 fuchsia::ui::gfx::RendererParam param) {
  fuchsia::ui::gfx::SetRendererParamCmd param_command;
  param_command.renderer_id = renderer_id;
  param_command.param = std::move(param);

  fuchsia::ui::gfx::Command command;
  command.set_set_renderer_param(std::move(param_command));
  return command;
}

fuchsia::ui::gfx::Command NewSetSizeCmd(uint32_t node_id, const std::array<float, 2>& size) {
  fuchsia::ui::gfx::SetSizeCmd set_size;
  set_size.id = node_id;
  auto& value = set_size.value.value;
  value.x = size[0];
  value.y = size[1];
  set_size.value.variable_id = 0;

  fuchsia::ui::gfx::Command command;
  command.set_set_size(set_size);

  return command;
}

fuchsia::ui::gfx::Command NewSetCameraTransformCmd(uint32_t camera_id,
                                                   const std::array<float, 3>& eye_position,
                                                   const std::array<float, 3>& eye_look_at,
                                                   const std::array<float, 3>& eye_up) {
  fuchsia::ui::gfx::SetCameraTransformCmd set_command;
  set_command.camera_id = camera_id;
  set_command.eye_position = NewVector3Value(eye_position);
  set_command.eye_look_at = NewVector3Value(eye_look_at);
  set_command.eye_up = NewVector3Value(eye_up);

  fuchsia::ui::gfx::Command command;
  command.set_set_camera_transform(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetCameraProjectionCmd(uint32_t camera_id, const float fovy) {
  fuchsia::ui::gfx::SetCameraProjectionCmd set_command;
  set_command.camera_id = camera_id;
  set_command.fovy = NewFloatValue(fovy);

  fuchsia::ui::gfx::Command command;
  command.set_set_camera_projection(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetStereoCameraProjectionCmd(
    uint32_t camera_id, const std::array<float, 4 * 4>& left_projection,
    const std::array<float, 4 * 4>& right_projection) {
  fuchsia::ui::gfx::SetStereoCameraProjectionCmd set_command;
  set_command.camera_id = camera_id;
  set_command.left_projection = NewMatrix4Value(left_projection);
  set_command.right_projection = NewMatrix4Value(right_projection);

  fuchsia::ui::gfx::Command command;
  command.set_set_stereo_camera_projection(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetCameraClipSpaceTransformCmd(uint32_t camera_id, float x, float y,
                                                            float scale) {
  fuchsia::ui::gfx::SetCameraClipSpaceTransformCmd set_command;
  set_command.camera_id = camera_id;
  set_command.translation = {x, y};
  set_command.scale = scale;

  fuchsia::ui::gfx::Command command;
  command.set_set_camera_clip_space_transform(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetCameraPoseBufferCmd(uint32_t camera_id, uint32_t buffer_id,
                                                    uint32_t num_entries, int64_t base_time,
                                                    uint64_t time_interval) {
  fuchsia::ui::gfx::SetCameraPoseBufferCmd set_command;
  set_command.camera_id = camera_id;
  set_command.buffer_id = buffer_id;
  set_command.num_entries = num_entries;
  set_command.base_time = base_time;
  set_command.time_interval = time_interval;

  fuchsia::ui::gfx::Command command;
  command.set_set_camera_pose_buffer(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetCameraPoseBufferCmd(uint32_t camera_id, uint32_t buffer_id,
                                                    uint32_t num_entries, zx::time base_time,
                                                    zx::duration time_interval) {
  return NewSetCameraPoseBufferCmd(camera_id, buffer_id, num_entries, base_time.get(),
                                   time_interval.get());
}

fuchsia::ui::gfx::Command NewSetLightColorCmd(uint32_t light_id, const std::array<float, 3>& rgb) {
  fuchsia::ui::gfx::SetLightColorCmd set_command;
  set_command.light_id = light_id;
  set_command.color = NewColorRgbValue(rgb[0], rgb[1], rgb[2]);

  fuchsia::ui::gfx::Command command;
  command.set_set_light_color(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetLightColorCmd(uint32_t light_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetLightColorCmd set_command;
  set_command.light_id = light_id;
  set_command.color = NewColorRgbValue(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_light_color(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetLightDirectionCmd(uint32_t light_id,
                                                  const std::array<float, 3>& dir) {
  fuchsia::ui::gfx::SetLightDirectionCmd set_command;
  set_command.light_id = light_id;
  set_command.direction = NewVector3Value(dir);

  fuchsia::ui::gfx::Command command;
  command.set_set_light_direction(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetLightDirectionCmd(uint32_t light_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetLightDirectionCmd set_command;
  set_command.light_id = light_id;
  set_command.direction = NewVector3Value(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_light_direction(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetPointLightPositionCmd(uint32_t light_id,
                                                      const std::array<float, 3>& pos) {
  fuchsia::ui::gfx::SetPointLightPositionCmd set_command;
  set_command.light_id = light_id;
  set_command.position = NewVector3Value(pos);

  fuchsia::ui::gfx::Command command;
  command.set_set_point_light_position(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetPointLightPositionCmd(uint32_t light_id, uint32_t variable_id) {
  fuchsia::ui::gfx::SetPointLightPositionCmd set_command;
  set_command.light_id = light_id;
  set_command.position = NewVector3Value(variable_id);

  fuchsia::ui::gfx::Command command;
  command.set_set_point_light_position(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetPointLightFalloffCmd(uint32_t light_id, float falloff) {
  fuchsia::ui::gfx::SetPointLightFalloffCmd set_command;
  set_command.light_id = light_id;
  set_command.falloff = NewFloatValue(falloff);

  fuchsia::ui::gfx::Command command;
  command.set_set_point_light_falloff(set_command);

  return command;
}

fuchsia::ui::gfx::Command NewAddLightCmd(uint32_t scene_id, uint32_t light_id) {
  fuchsia::ui::gfx::AddLightCmd add_light_command;
  add_light_command.scene_id = scene_id;
  add_light_command.light_id = light_id;

  fuchsia::ui::gfx::Command command;
  command.set_add_light(add_light_command);

  return command;
}

fuchsia::ui::gfx::Command NewSceneAddAmbientLightCmd(uint32_t scene_id, uint32_t light_id) {
  fuchsia::ui::gfx::SceneAddAmbientLightCmd add_light_command;
  add_light_command.scene_id = scene_id;
  add_light_command.light_id = light_id;

  fuchsia::ui::gfx::Command command;
  command.set_scene__add_ambient_light(add_light_command);

  return command;
}

fuchsia::ui::gfx::Command NewSceneAddDirectionalLightCmd(uint32_t scene_id, uint32_t light_id) {
  fuchsia::ui::gfx::SceneAddDirectionalLightCmd add_light_command;
  add_light_command.scene_id = scene_id;
  add_light_command.light_id = light_id;

  fuchsia::ui::gfx::Command command;
  command.set_scene__add_directional_light(add_light_command);

  return command;
}

fuchsia::ui::gfx::Command NewSceneAddPointLightCmd(uint32_t scene_id, uint32_t light_id) {
  fuchsia::ui::gfx::SceneAddPointLightCmd add_light_command;
  add_light_command.scene_id = scene_id;
  add_light_command.light_id = light_id;

  fuchsia::ui::gfx::Command command;
  command.set_scene__add_point_light(add_light_command);

  return command;
}

fuchsia::ui::gfx::Command NewDetachLightCmd(uint32_t light_id) {
  fuchsia::ui::gfx::DetachLightCmd detach_light_command;
  detach_light_command.light_id = light_id;

  fuchsia::ui::gfx::Command command;
  command.set_detach_light(detach_light_command);

  return command;
}

fuchsia::ui::gfx::Command NewDetachLightsCmd(uint32_t scene_id) {
  fuchsia::ui::gfx::DetachLightsCmd detach_lights_command;
  detach_lights_command.scene_id = scene_id;

  fuchsia::ui::gfx::Command command;
  command.set_detach_lights(detach_lights_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetEventMaskCmd(uint32_t resource_id, uint32_t event_mask) {
  fuchsia::ui::gfx::SetEventMaskCmd set_event_mask_command;
  set_event_mask_command.id = resource_id;
  set_event_mask_command.event_mask = event_mask;

  fuchsia::ui::gfx::Command command;
  command.set_set_event_mask(set_event_mask_command);

  return command;
}

fuchsia::ui::gfx::Command NewSetLabelCmd(uint32_t resource_id, const std::string& label) {
  fuchsia::ui::gfx::SetLabelCmd set_label_command;
  set_label_command.id = resource_id;
  set_label_command.label = label.substr(0, fuchsia::ui::gfx::kLabelMaxLength);

  fuchsia::ui::gfx::Command command;
  command.set_set_label(std::move(set_label_command));

  return command;
}

fuchsia::ui::gfx::Command NewSetDisableClippingCmd(uint32_t renderer_id, bool disable_clipping) {
  fuchsia::ui::gfx::SetDisableClippingCmd set_disable_clipping_command;
  set_disable_clipping_command.renderer_id = renderer_id;
  set_disable_clipping_command.disable_clipping = disable_clipping;

  fuchsia::ui::gfx::Command command;
  command.set_set_disable_clipping(set_disable_clipping_command);

  return command;
}

fuchsia::ui::gfx::FloatValue NewFloatValue(float value) {
  fuchsia::ui::gfx::FloatValue val;
  val.variable_id = 0;
  val.value = value;
  return val;
}

fuchsia::ui::gfx::vec2 NewVector2(const std::array<float, 2>& value) {
  fuchsia::ui::gfx::vec2 val;
  val.x = value[0];
  val.y = value[1];
  return val;
}

fuchsia::ui::gfx::Vector2Value NewVector2Value(const std::array<float, 2> value) {
  fuchsia::ui::gfx::Vector2Value val;
  val.variable_id = 0;
  val.value = NewVector2(value);
  return val;
}

fuchsia::ui::gfx::Vector2Value NewVector2Value(uint32_t variable_id) {
  fuchsia::ui::gfx::Vector2Value val;
  val.variable_id = variable_id;
  return val;
}

fuchsia::ui::gfx::vec3 NewVector3(const std::array<float, 3>& value) {
  fuchsia::ui::gfx::vec3 val;
  val.x = value[0];
  val.y = value[1];
  val.z = value[2];
  return val;
}

fuchsia::ui::gfx::Vector3Value NewVector3Value(const std::array<float, 3>& value) {
  fuchsia::ui::gfx::Vector3Value val;
  val.variable_id = 0;
  val.value = NewVector3(value);
  return val;
}

fuchsia::ui::gfx::Vector3Value NewVector3Value(uint32_t variable_id) {
  fuchsia::ui::gfx::Vector3Value val;
  val.variable_id = variable_id;
  return val;
}

fuchsia::ui::gfx::vec4 NewVector4(const std::array<float, 4>& value) {
  fuchsia::ui::gfx::vec4 val;
  val.x = value[0];
  val.y = value[1];
  val.z = value[2];
  val.w = value[3];
  return val;
}

fuchsia::ui::gfx::Vector4Value NewVector4Value(const std::array<float, 4>& value) {
  fuchsia::ui::gfx::Vector4Value val;
  val.variable_id = 0;
  val.value = NewVector4(value);
  return val;
}

fuchsia::ui::gfx::Vector4Value NewVector4Value(uint32_t variable_id) {
  fuchsia::ui::gfx::Vector4Value val;
  val.variable_id = variable_id;
  return val;
}

fuchsia::ui::gfx::Quaternion NewQuaternion(const std::array<float, 4>& value) {
  fuchsia::ui::gfx::Quaternion val;
  val.x = value[0];
  val.y = value[1];
  val.z = value[2];
  val.w = value[3];
  return val;
}

fuchsia::ui::gfx::QuaternionValue NewQuaternionValue(const std::array<float, 4>& value) {
  fuchsia::ui::gfx::QuaternionValue val;
  val.variable_id = 0;
  val.value = NewQuaternion(value);
  return val;
}

fuchsia::ui::gfx::QuaternionValue NewQuaternionValue(uint32_t variable_id) {
  fuchsia::ui::gfx::QuaternionValue val;
  val.variable_id = variable_id;
  return val;
}

fuchsia::ui::gfx::mat4 NewMatrix4(const std::array<float, 4 * 4>& matrix) {
  fuchsia::ui::gfx::mat4 val;
  for (size_t index = 0; index < 4 * 4; index++) {
    val.matrix[index] = matrix[index];
  }
  return val;
}

fuchsia::ui::gfx::Matrix4Value NewMatrix4Value(const std::array<float, 4 * 4>& matrix) {
  fuchsia::ui::gfx::Matrix4Value val;
  val.variable_id = 0;
  val.value = NewMatrix4(matrix);
  return val;
}

fuchsia::ui::gfx::Matrix4Value NewMatrix4Value(uint32_t variable_id) {
  fuchsia::ui::gfx::Matrix4Value val;
  val.variable_id = variable_id;
  return val;
}

fuchsia::ui::gfx::ColorRgbValue NewColorRgbValue(float red, float green, float blue) {
  fuchsia::ui::gfx::ColorRgbValue val;
  val.variable_id = 0;
  auto& color = val.value;
  color.red = red;
  color.green = green;
  color.blue = blue;

  return val;
}

fuchsia::ui::gfx::ColorRgbValue NewColorRgbValue(uint32_t variable_id) {
  fuchsia::ui::gfx::ColorRgbValue val;
  val.variable_id = variable_id;

  return val;
}

fuchsia::ui::gfx::ColorRgbaValue NewColorRgbaValue(const std::array<uint8_t, 4>& value) {
  fuchsia::ui::gfx::ColorRgbaValue val;
  val.variable_id = 0;
  auto& color = val.value;
  color.red = value[0];
  color.green = value[1];
  color.blue = value[2];
  color.alpha = value[3];

  return val;
}

fuchsia::ui::gfx::ColorRgbaValue NewColorRgbaValue(uint32_t variable_id) {
  fuchsia::ui::gfx::ColorRgbaValue val;
  val.variable_id = variable_id;

  return val;
}

// TODO(mikejurka): this should be in an images util file
bool ImageInfoEquals(const fuchsia::images::ImageInfo& a, const fuchsia::images::ImageInfo& b) {
  return a.transform == b.transform && a.width == b.width && a.height == b.height &&
         a.stride == b.stride && a.pixel_format == b.pixel_format &&
         a.color_space == b.color_space && a.tiling == b.tiling && a.alpha_format == b.alpha_format;
}

}  // namespace scenic
