/* Copyright (c) 2017 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Based on the following files from the Granite rendering engine:
// - vulkan/command_buffer.cpp

#include "src/ui/lib/escher/third_party/granite/vk/command_buffer.h"

#include <algorithm>

#include "src/ui/lib/escher/escher.h"
#include "src/ui/lib/escher/impl/vulkan_utils.h"
#include "src/ui/lib/escher/util/bit_ops.h"
#include "src/ui/lib/escher/util/hasher.h"
#include "src/ui/lib/escher/util/image_utils.h"
#include "src/ui/lib/escher/util/trace_macros.h"
#include "src/ui/lib/escher/vk/impl/descriptor_set_allocator.h"
#include "src/ui/lib/escher/vk/impl/framebuffer.h"
#include "src/ui/lib/escher/vk/impl/framebuffer_allocator.h"
#include "src/ui/lib/escher/vk/pipeline_builder.h"
#include "src/ui/lib/escher/vk/shader_program.h"
#include "src/ui/lib/escher/vk/texture.h"

// TODO(fxbug.dev/7174): We currently wrap an old-style impl::CommandBuffer.  This
// facilitates making the same "keep alive" functionality available to users of
// the old and new CommandBuffer classes.  Once there are no direct users of the
// old-style CommandBuffers, fold the necessary functionality into this class
// and delete the old one.
#include "src/ui/lib/escher/impl/command_buffer_pool.h"

namespace escher {

CommandBufferPtr CommandBuffer::NewForType(Escher* escher, Type type, bool use_protected_memory) {
  switch (type) {
    case Type::kGraphics:
      return NewForGraphics(escher, use_protected_memory);
    case Type::kCompute:
      return NewForCompute(escher, use_protected_memory);
    case Type::kTransfer:
      FX_DCHECK(!use_protected_memory);
      return NewForTransfer(escher);
    default:
      FX_LOGS(ERROR) << "Unrecognized CommandBuffer type requested. Cannot "
                     << "create CommandBuffer.";
      return nullptr;
  }
}

CommandBufferPtr CommandBuffer::NewForGraphics(Escher* escher, bool use_protected_memory) {
  return fxl::AdoptRef(new CommandBuffer(
      escher->GetWeakPtr(), Type::kGraphics,
      use_protected_memory ? escher->protected_command_buffer_pool()->GetCommandBuffer()
                           : escher->command_buffer_pool()->GetCommandBuffer()));
}

CommandBufferPtr CommandBuffer::NewForCompute(Escher* escher, bool use_protected_memory) {
  return fxl::AdoptRef(new CommandBuffer(
      escher->GetWeakPtr(), Type::kCompute,
      use_protected_memory ? escher->protected_command_buffer_pool()->GetCommandBuffer()
                           : escher->command_buffer_pool()->GetCommandBuffer()));
}

CommandBufferPtr CommandBuffer::NewForTransfer(Escher* escher) {
  auto pool = escher->transfer_command_buffer_pool() ? escher->transfer_command_buffer_pool()
                                                     : escher->command_buffer_pool();
  return fxl::AdoptRef(
      new CommandBuffer(escher->GetWeakPtr(), Type::kTransfer, pool->GetCommandBuffer()));
}

CommandBuffer::CommandBuffer(EscherWeakPtr escher, Type type, impl::CommandBuffer* impl)
    : escher_(std::move(escher)),
      type_(type),
      impl_(impl),
      vk_(impl_->vk()),
      vk_device_(escher_->vk_device()),
      pipeline_state_(escher_->pipeline_builder()->GetWeakPtr()) {
  BeginCompute();
}

bool CommandBuffer::Submit(CommandBufferFinishedCallback callback) {
  vk::Queue queue;
  switch (type_) {
    case Type::kGraphics:
    case Type::kCompute:
      queue = escher_->device()->vk_main_queue();
      break;
    case Type::kTransfer:
      queue = escher_->device()->vk_transfer_queue();
      break;
    default:
      FX_DCHECK(false);
      return false;
  }
  return Submit(queue, std::move(callback));
}

void CommandBuffer::BeginCompute() {
  is_compute_ = true;
  BeginGraphicsOrComputeContext();
}

void CommandBuffer::BeginGraphics() {
  is_compute_ = false;
  BeginGraphicsOrComputeContext();
}

void CommandBuffer::BeginGraphicsOrComputeContext() {
  TRACE_DURATION("gfx", "CommandBuffer::BeginGraphicsOrComputeContext");
  dirty_ = ~0u;
  dirty_descriptor_sets_ = ~0u;
  current_vk_pipeline_ = vk::Pipeline();
  current_vk_pipeline_layout_ = vk::PipelineLayout();
  current_pipeline_layout_ = nullptr;
  current_program_ = nullptr;
  for (uint32_t i = 0; i < VulkanLimits::kNumDescriptorSets; ++i) {
    DescriptorSetBindings* set = GetDescriptorSetBindings(i);
    memset(set->uids, 0, sizeof(set->uids));
    memset(set->secondary_uids, 0, sizeof(set->secondary_uids));
  }
  memset(&index_binding_, 0, sizeof(index_binding_));

  pipeline_state_.BeginGraphicsOrComputeContext();
}

void CommandBuffer::BeginRenderPass(const RenderPassInfo& info) {
  TRACE_DURATION("gfx", "CommandBuffer::BeginRenderPass");
  FX_DCHECK(!IsInRenderPass());
  FX_DCHECK(pipeline_state_.current_subpass() == 0);

  framebuffer_ = escher_->framebuffer_allocator()->ObtainFramebuffer(
      info, allow_renderpass_and_pipeline_creation_);
  FX_CHECK(framebuffer_) << "Lazy render-pass "
                         << (allow_renderpass_and_pipeline_creation_ ? "IS" : "IS NOT")
                         << " allowed.";
  auto& render_pass = framebuffer_->render_pass();
  pipeline_state_.set_render_pass(render_pass.get());
  impl_->KeepAlive(framebuffer_.get());
  impl_->KeepAlive(render_pass.get());

  vk::Rect2D rect = info.render_area;
  impl::ClipToRect(&rect, {{0, 0}, framebuffer_->extent()});

  // Provide room for a clear value for each color attachment, plus one extra
  // for the depth attachment.
  vk::ClearValue clear_values[VulkanLimits::kNumColorAttachments + 1];
  uint32_t num_clear_values = 0;

  // Gather clear values for each color attachment that is flagged as needing
  // clearing.
  for (uint32_t i = 0; i < info.num_color_attachments; ++i) {
    FX_DCHECK(info.color_attachments[i]);
    if (info.clear_attachments & (1u << i)) {
      clear_values[i].color = info.clear_color[i];
      num_clear_values = i + 1;
    }
  }

  // Add a clear value for the depth attachment, but only if it exists and
  // requires clearing.
  if (info.depth_stencil_attachment &&
      (info.op_flags & RenderPassInfo::kClearDepthStencilOp) != 0) {
    clear_values[info.num_color_attachments].depthStencil = info.clear_depth_stencil;
    num_clear_values = info.num_color_attachments + 1;
  }

  // Set the |layout_| of all color and depth stencil attachments to
  // corresponding |finalLayout| values previously stored in render pass.
  for (uint32_t i = 0; i < info.num_color_attachments; ++i) {
    FX_DCHECK(info.color_attachments[i]);
    info.color_attachments[i]->image()->set_layout(render_pass->GetColorAttachmentFinalLayout(i));
  }
  if (info.depth_stencil_attachment) {
    info.depth_stencil_attachment->image()->set_layout(
        render_pass->GetDepthStencilAttachmentFinalLayout());
  }

  vk::RenderPassBeginInfo begin_info;
  begin_info.renderPass = render_pass->vk();
  begin_info.framebuffer = framebuffer_->vk();
  begin_info.renderArea = rect;
  begin_info.clearValueCount = num_clear_values;
  begin_info.pClearValues = clear_values;

  // Escher's use-cases are not amenable to creating reusable secondary command
  // buffers.  Therefore, we always encode commands for the render pass inline,
  // in the same command buffer.
  vk_.beginRenderPass(begin_info, vk::SubpassContents::eInline);

  // BeginGraphics() will dirty everything; no need to dirty anything here.
  scissor_ = rect;
  viewport_ = vk::Viewport(0.0f, 0.0f, static_cast<float>(framebuffer_->width()),
                           static_cast<float>(framebuffer_->height()), 0.0f, 1.0f);

  BeginGraphics();
}

void CommandBuffer::EndRenderPass() {
  TRACE_DURATION("gfx", "CommandBuffer::EndRenderPass");
  FX_DCHECK(IsInRenderPass());

  vk().endRenderPass();

  framebuffer_ = nullptr;
  pipeline_state_.set_render_pass(nullptr);
  pipeline_state_.reset_current_subpass();

  BeginCompute();
}

bool CommandBuffer::IsInRenderPass() {
  FX_DCHECK(!framebuffer_ == !pipeline_state_.render_pass());
  return static_cast<bool>(pipeline_state_.render_pass());
}

void CommandBuffer::NextSubpass() {
  FX_DCHECK(IsInRenderPass());
  SetDirty(kDirtyPipelineBit | kDirtyPushConstantsBit);
  pipeline_state_.IncrementSubpass();
  vk().nextSubpass(vk::SubpassContents::eInline);
}

void CommandBuffer::BufferBarrier(const BufferPtr& buffer, vk::PipelineStageFlags src_stages,
                                  vk::AccessFlags src_access, vk::PipelineStageFlags dst_stages,
                                  vk::AccessFlags dst_access) {
  impl_->KeepAlive(buffer.get());

  vk::BufferMemoryBarrier barrier;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer->vk();
  barrier.offset = 0U;
  barrier.size = VK_WHOLE_SIZE;

  vk().pipelineBarrier(src_stages, dst_stages, {}, {}, {std::move(barrier)}, {});
}

void CommandBuffer::ImageBarrier(const ImagePtr& image, vk::ImageLayout old_layout,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags src_stages,
                                 vk::AccessFlags src_access, vk::PipelineStageFlags dst_stages,
                                 vk::AccessFlags dst_access, uint32_t src_queue_family_index,
                                 uint32_t dst_queue_family_index) {
  // Render passes may also cause image layout transitions.  We haven't worked
  // through all of the corner cases with respect to our per-image layout
  // tracking.  Therefore, since we don't currently need image barriers during
  // render passes, we disallow them.
  FX_DCHECK(!IsInRenderPass());

  FX_DCHECK(!image->is_transient());
  FX_DCHECK(image->layout() == old_layout || old_layout == vk::ImageLayout::eUndefined ||
            old_layout == vk::ImageLayout::ePreinitialized);

  impl_->KeepAlive(image.get());

  vk::ImageMemoryBarrier barrier;
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.image = image->vk();
  barrier.subresourceRange.aspectMask =
      image_utils::FormatToColorOrDepthStencilAspectFlags(image->format());
  barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
  barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
  barrier.srcQueueFamilyIndex = src_queue_family_index;
  barrier.dstQueueFamilyIndex = dst_queue_family_index;

  image->set_layout(new_layout);

  vk().pipelineBarrier(src_stages, dst_stages, {}, 0, nullptr, 0, nullptr, 1, &barrier);
}

void CommandBuffer::PushConstants(const void* data, vk::DeviceSize offset, vk::DeviceSize range) {
  FX_DCHECK(offset + range <= VulkanLimits::kPushConstantSize);
  memcpy(bindings_.push_constant_data + offset, data, range);
  SetDirty(kDirtyPushConstantsBit);
}

void CommandBuffer::BindUniformBuffer(uint32_t set, uint32_t binding, const BufferPtr& buffer) {
  BindUniformBuffer(set, binding, buffer.get(), 0, buffer->size());
}

void CommandBuffer::BindUniformBuffer(uint32_t set, uint32_t binding, const BufferPtr& buffer,
                                      vk::DeviceSize offset, vk::DeviceSize range) {
  BindUniformBuffer(set, binding, buffer.get(), offset, range);
}

void CommandBuffer::BindUniformBuffer(uint32_t set_index, uint32_t binding, Buffer* buffer,
                                      vk::DeviceSize offset, vk::DeviceSize range) {
  auto set = GetDescriptorSetBindings(set_index);
  auto b = GetDescriptorBindingInfo(set, binding);

  impl_->KeepAlive(buffer);

  if (buffer->uid() == set->uids[binding] && b->buffer.offset == offset &&
      b->buffer.range == range) {
    return;
  }

  b->buffer = vk::DescriptorBufferInfo{buffer->vk(), offset, range};
  set->uids[binding] = buffer->uid();
  dirty_descriptor_sets_ |= 1u << set_index;
}

void CommandBuffer::BindTexture(unsigned set_index, unsigned binding, const Texture* texture) {
  auto set = GetDescriptorSetBindings(set_index);
  auto b = GetDescriptorBindingInfo(set, binding);

  auto& image = texture->image();
  FX_DCHECK(image->info().usage & vk::ImageUsageFlagBits::eSampled);

  impl_->KeepAlive(texture);

  vk::ImageLayout vk_layout = image->layout();
  if (texture->uid() == set->uids[binding] && b->image.fp.imageLayout == vk_layout &&
      // TODO(fxbug.dev/7174): if we reify Samplers as a separate resource type, then use
      // the sampler's uid instead of the texture.
      texture->uid() == set->secondary_uids[binding]) {
    // The image, layout, and sampler are all unchanged, so we do not need to
    // update any bindings, nor mark the descriptor set as dirty.
    return;
  }

  b->image.fp.imageLayout = vk_layout;
  b->image.fp.imageView = texture->vk_float_view();
  b->image.fp.sampler = texture->sampler()->vk();
  b->image.integer.imageLayout = vk_layout;
  b->image.integer.imageView = texture->vk_integer_view();
  b->image.integer.sampler = texture->sampler()->vk();
  set->uids[binding] = texture->uid();
  // TODO(fxbug.dev/7174): if we reify Samplers as a separate resource type,
  // then use the sampler's uid instead of the texture.
  set->secondary_uids[binding] = texture->uid();
  dirty_descriptor_sets_ |= 1u << set_index;
}

void CommandBuffer::BindInputAttachment(unsigned set_index, unsigned binding,
                                        const ImageView* view) {
  auto set = GetDescriptorSetBindings(set_index);
  auto b = GetDescriptorBindingInfo(set, binding);

  auto& image = view->image();
  impl_->KeepAlive(view);

  vk::ImageLayout vk_layout = image->layout();
  if (view->uid() == set->uids[binding] && b->image.fp.imageLayout == vk_layout &&
      // TODO(fxbug.dev/7174): if we reify Samplers as a separate resource type, then use
      // the sampler's uid instead of the texture.
      view->uid() == set->secondary_uids[binding]) {
    // The image, layout, and sampler are all unchanged, so we do not need to
    // update any bindings, nor mark the descriptor set as dirty.
    return;
  }

  b->image.fp.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  b->image.fp.imageView = view->vk_float_view();
  b->image.fp.sampler = VK_NULL_HANDLE;

  // According to the Vulkan Spec:
  //    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL specifies a layout allowing
  //    read-only access in a shader as a sampled image, combined image/sampler,
  //    or input attachment. This layout is valid only for image subresources of
  //    images created with the VK_IMAGE_USAGE_SAMPLED_BIT or
  //    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT usage bits enabled.
  // So we set the layout as eShaderReadOnlyoptimal here.
  b->image.integer.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  b->image.integer.imageView = view->vk_integer_view();
  b->image.integer.sampler = VK_NULL_HANDLE;

  set->uids[binding] = view->uid();
  // TODO(fxbug.dev/7174): if we reify Samplers as a separate resource type,
  // then use the sampler's uid instead of the texture.
  set->secondary_uids[binding] = view->uid();
  dirty_descriptor_sets_ |= 1u << set_index;
}

void CommandBuffer::BindVertices(uint32_t binding, vk::Buffer buffer, vk::DeviceSize offset,
                                 vk::DeviceSize stride, vk::VertexInputRate step_rate) {
  FX_DCHECK(IsInRenderPass());

  if (pipeline_state_.BindVertices(binding, buffer, offset, stride, step_rate)) {
    // Pipeline change is required.
    SetDirty(kDirtyStaticVertexBit);
  }
}

void CommandBuffer::BindVertices(uint32_t binding, Buffer* buffer, vk::DeviceSize offset,
                                 vk::DeviceSize stride, vk::VertexInputRate step_rate) {
  impl_->KeepAlive(buffer);
  BindVertices(binding, buffer->vk(), offset, stride, step_rate);
}

void CommandBuffer::BindIndices(vk::Buffer buffer, vk::DeviceSize offset,
                                vk::IndexType index_type) {
  if (index_binding_.buffer == buffer && index_binding_.offset == offset &&
      index_binding_.index_type == index_type) {
    // Bindings are unchanged.
    return;
  }
  TRACE_DURATION("gfx", "escher::CommandBuffer::BindIndices");

  // Index buffer changes never require a new pipeline to be generated, so it is
  // OK to make this change immediately.
  index_binding_.buffer = buffer;
  index_binding_.offset = offset;
  index_binding_.index_type = index_type;
  vk().bindIndexBuffer(buffer, offset, index_type);
}

void CommandBuffer::BindIndices(const BufferPtr& buffer, vk::DeviceSize offset,
                                vk::IndexType index_type) {
  BindIndices(buffer->vk(), offset, index_type);
  impl_->KeepAlive(buffer.get());
}

void CommandBuffer::DrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                                int32_t vertex_offset, uint32_t first_instance) {
  TRACE_DURATION("gfx", "escher::CommandBuffer::DrawIndexed");
  FX_DCHECK(current_program_);
  FX_DCHECK(!is_compute_);
  FX_DCHECK(index_binding_.buffer);

  FlushRenderState();

  TRACE_DURATION("gfx", "escher::CommandBuffer::DrawIndexed[vulkan]");
  vk().drawIndexed(index_count, instance_count, first_index, vertex_offset, first_instance);
}

// Wraps vkCmdDraw() which is able to render raw vertices without using an index buffer.
void CommandBuffer::Draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                         uint32_t first_instance) {
  TRACE_DURATION("gfx", "escher::CommandBuffer::Draw");
  FX_DCHECK(current_program_);
  FX_DCHECK(!is_compute_);

  FlushRenderState();

  TRACE_DURATION("gfx", "escher::CommandBuffer::Draw[vulkan]");
  vk().draw(vertex_count, instance_count, first_vertex, first_instance);
}

void CommandBuffer::Dispatch(uint32_t groupXCount, uint32_t groupYCount, uint32_t groupZCount) {
  TRACE_DURATION("gfx", "escher::CommandBuffer::Dispatch");
  FX_DCHECK(current_program_);
  FX_DCHECK(is_compute_);
  FX_DCHECK(!IsInRenderPass());

  FlushComputeState();

  TRACE_DURATION("gfx", "escher::CommandBuffer::Dispatch[vulkan]");
  vk().dispatch(groupXCount, groupYCount, groupZCount);
}

void CommandBuffer::FlushComputeState() {
  TRACE_DURATION("gfx", "escher::CommandBuffer::ComputeStateState");

  FX_DCHECK(current_pipeline_layout_);
  FX_DCHECK(current_program_);

  // We've invalidated pipeline state, update the VkPipeline.
  if (GetAndClearDirty(kDirtyStaticStateBit | kDirtyPipelineBit)) {
    // Update |current_pipeline_|, keeping track of the old one so we can see if
    // there was a change.
    vk::Pipeline previous_pipeline = current_vk_pipeline_;
    FlushComputePipeline();
    if (previous_pipeline != current_vk_pipeline_) {
      vk().bindPipeline(vk::PipelineBindPoint::eCompute, current_vk_pipeline_);

      // According to the Vulkan spec (Section 9.9 "Dynamic State"), it would
      // theoretically be possible to track which state is statically vs.
      // dynamically set for a given pipeline, and only set kDirtyDynamicBits
      // when the new pipeline leaves some of the previous state in an undefined
      // state.  However, it is not worth the additional complexity.
      SetDirty(kDirtyDynamicBits);
    }
  }

  FlushDescriptorSets();

  if (GetAndClearDirty(kDirtyPushConstantsBit)) {
    TRACE_DURATION("gfx", "escher::CommandBuffer::FlushComputeState[push_constants]");
    // The push constants were invalidated (perhaps by being explicitly set, or
    // perhaps by a change in the descriptor set layout; it doesn't matter).
    uint32_t num_ranges = current_pipeline_layout_->spec().num_push_constant_ranges();
    for (unsigned i = 0; i < num_ranges; ++i) {
      auto& range = current_pipeline_layout_->spec().push_constant_ranges()[i];
      vk().pushConstants(current_vk_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                         range.offset, range.size, bindings_.push_constant_data + range.offset);
    }
  }
}

void CommandBuffer::FlushRenderState() {
  TRACE_DURATION("gfx", "escher::CommandBuffer::FlushRenderState");

  FX_DCHECK(current_pipeline_layout_);
  FX_DCHECK(current_program_);

  // We've invalidated pipeline state, update the VkPipeline.
  if (GetAndClearDirty(kDirtyStaticStateBit | kDirtyPipelineBit | kDirtyStaticVertexBit)) {
    // Update |current_pipeline_|, keeping track of the old one so we can see if
    // there was a change.
    vk::Pipeline previous_pipeline = current_vk_pipeline_;
    FlushGraphicsPipeline();
    if (previous_pipeline != current_vk_pipeline_) {
      vk().bindPipeline(vk::PipelineBindPoint::eGraphics, current_vk_pipeline_);

      // According to the Vulkan spec (Section 9.9 "Dynamic State"), it would
      // theoretically be possible to track which state is statically vs.
      // dynamically set for a given pipeline, and only set kDirtyDynamicBits
      // when the new pipeline leaves some of the previous state in an undefined
      // state.  However, it is not worth the additional complexity.
      SetDirty(kDirtyDynamicBits);
    }
  }

  FlushDescriptorSets();

  const PipelineStaticState* static_pipeline_state = pipeline_state_.static_state();

  if (GetAndClearDirty(kDirtyPushConstantsBit)) {
    TRACE_DURATION("gfx", "escher::CommandBuffer::FlushRenderState[push_constants]");
    // The push constants were invalidated (perhaps by being explicitly set, or
    // perhaps by a change in the descriptor set layout; it doesn't matter).
    uint32_t num_ranges = current_pipeline_layout_->spec().num_push_constant_ranges();
    for (unsigned i = 0; i < num_ranges; ++i) {
      auto& range = current_pipeline_layout_->spec().push_constant_ranges()[i];
      vk().pushConstants(current_vk_pipeline_layout_, range.stageFlags, range.offset, range.size,
                         bindings_.push_constant_data + range.offset);
    }
  }
  if (GetAndClearDirty(kDirtyViewportBit)) {
    TRACE_DURATION("gfx", "escher::CommandBuffer::FlushRenderState[viewport]");
    vk().setViewport(0, 1, &viewport_);
  }
  if (GetAndClearDirty(kDirtyScissorBit)) {
    TRACE_DURATION("gfx", "escher::CommandBuffer::FlushRenderState[scissor]");
    vk().setScissor(0, 1, &scissor_);
  }
  if (static_pipeline_state->depth_bias_enable && GetAndClearDirty(kDirtyDepthBiasBit)) {
    TRACE_DURATION("gfx", "escher::CommandBuffer::FlushRenderState[depth_bias]");
    vk().setDepthBias(dynamic_state_.depth_bias_constant, 0.0f, dynamic_state_.depth_bias_slope);
  }
  if (static_pipeline_state->stencil_test && GetAndClearDirty(kDirtyStencilMasksAndReferenceBit)) {
    TRACE_DURATION("gfx", "escher::CommandBuffer::FlushRenderState[stencil]");
    vk().setStencilCompareMask(vk::StencilFaceFlagBits::eFront, dynamic_state_.front_compare_mask);
    vk().setStencilReference(vk::StencilFaceFlagBits::eFront, dynamic_state_.front_reference);
    vk().setStencilWriteMask(vk::StencilFaceFlagBits::eFront, dynamic_state_.front_write_mask);
    vk().setStencilCompareMask(vk::StencilFaceFlagBits::eBack, dynamic_state_.back_compare_mask);
    vk().setStencilReference(vk::StencilFaceFlagBits::eBack, dynamic_state_.back_reference);
    vk().setStencilWriteMask(vk::StencilFaceFlagBits::eBack, dynamic_state_.back_write_mask);
  }

  // Bind all vertex buffers that are both active and dirty.
  pipeline_state_.FlushVertexBuffers(vk());
}

void CommandBuffer::FlushComputePipeline() {
  TRACE_DURATION("gfx", "escher::CommandBuffer::FlushComputePipeline");
  const bool log_pipeline_creation = !allow_renderpass_and_pipeline_creation_;
  current_vk_pipeline_ = pipeline_state_.FlushComputePipeline(
      current_pipeline_layout_.get(), current_program_, log_pipeline_creation);
}

void CommandBuffer::FlushGraphicsPipeline() {
  TRACE_DURATION("gfx", "escher::CommandBuffer::FlushGraphicsPipeline");
  const bool log_pipeline_creation = !allow_renderpass_and_pipeline_creation_;
  current_vk_pipeline_ = pipeline_state_.FlushGraphicsPipeline(
      current_pipeline_layout_.get(), current_program_, log_pipeline_creation);
}

void CommandBuffer::FlushDescriptorSets() {
  TRACE_DURATION("gfx", "escher::CommandBuffer::FlushDescriptorSets");
  auto& spec = current_pipeline_layout_->spec();
  uint32_t sets_to_flush = spec.descriptor_set_mask() & dirty_descriptor_sets_;
  ForEachBitIndex(sets_to_flush, [this](uint32_t set_index) { FlushDescriptorSet(set_index); });
  // All descriptor sets that weren't flushed remain dirty.
  dirty_descriptor_sets_ &= ~sets_to_flush;
}

void CommandBuffer::FlushDescriptorSet(uint32_t set_index) {
  const impl::DescriptorSetLayout& set_layout =
      current_pipeline_layout_->spec().descriptor_set_layouts(set_index);

  const auto set_bindings = GetDescriptorSetBindings(set_index);
  uint32_t num_dynamic_offsets = 0;
  uint32_t dynamic_offsets[VulkanLimits::kNumBindings];
  Hasher h;

  h.u32(set_layout.fp_mask);

  // UBOs
  ForEachBitIndex(set_layout.uniform_buffer_mask, [&](uint32_t binding) {
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    FX_DCHECK(b->buffer.buffer) << "No buffer for uniform binding " << set_index << "," << binding;
    FX_DCHECK(set_bindings->uids[binding]);

    h.u64(set_bindings->uids[binding]);
    h.u32(static_cast<uint32_t>(b->buffer.range));
    dynamic_offsets[num_dynamic_offsets++] = static_cast<uint32_t>(b->buffer.offset);
  });

  // SSBOs
  ForEachBitIndex(set_layout.storage_buffer_mask, [&](uint32_t binding) {
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    FX_DCHECK(b->buffer.buffer);
    FX_DCHECK(set_bindings->uids[binding]);

    h.u64(set_bindings->uids[binding]);
    h.u32(static_cast<uint32_t>(b->buffer.offset));
    h.u32(static_cast<uint32_t>(b->buffer.range));
  });

  // Sampled buffers
  ForEachBitIndex(set_layout.sampled_buffer_mask, [&](uint32_t binding) {
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    FX_DCHECK(b->buffer_view);
    FX_DCHECK(set_bindings->uids[binding]);

    h.u64(set_bindings->uids[binding]);
  });

  // Sampled images
  ForEachBitIndex(set_layout.sampled_image_mask, [&](uint32_t binding) {
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    FX_DCHECK(b->image.fp.imageView);
    FX_DCHECK(b->image.fp.sampler);
    FX_DCHECK(set_bindings->uids[binding]);

    h.u64(set_bindings->uids[binding]);
    h.u64(set_bindings->secondary_uids[binding]);
    h.u32(EnumCast(b->image.fp.imageLayout));
  });

  // Storage images
  ForEachBitIndex(set_layout.storage_image_mask, [&](uint32_t binding) {
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    FX_DCHECK(b->image.fp.imageView);
    FX_DCHECK(set_bindings->uids[binding]);

    h.u64(set_bindings->uids[binding]);
    h.u32(EnumCast(b->image.fp.imageLayout));
  });

  // Input attachments
  ForEachBitIndex(set_layout.input_attachment_mask, [&](uint32_t binding) {
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    FX_DCHECK(b->image.fp.imageView);
    FX_DCHECK(set_bindings->uids[binding]);

    h.u64(set_bindings->uids[binding]);
    h.u32(EnumCast(b->image.fp.imageLayout));
  });

  Hash hash = h.value();
  auto pair = current_pipeline_layout_->GetDescriptorSetAllocator(set_index)->Get(hash);
  vk::DescriptorSet vk_set = pair.first;

  // The descriptor set was not found in the cache; rebuild.
  if (!pair.second) {
    WriteDescriptors(set_index, vk_set, set_layout);
  }

  vk().bindDescriptorSets(
      IsInRenderPass() ? vk::PipelineBindPoint::eGraphics : vk::PipelineBindPoint::eCompute,
      current_vk_pipeline_layout_, set_index, 1, &vk_set, num_dynamic_offsets, dynamic_offsets);
}

void CommandBuffer::WriteDescriptors(uint32_t set_index, vk::DescriptorSet vk_set,
                                     const impl::DescriptorSetLayout& set_layout) {
  const auto set_bindings = GetDescriptorSetBindings(set_index);
  uint32_t write_count = 0;
  uint32_t buffer_info_count = 0;
  vk::WriteDescriptorSet writes[VulkanLimits::kNumBindings];
  vk::DescriptorBufferInfo buffer_info[VulkanLimits::kNumBindings];

  ForEachBitIndex(set_layout.uniform_buffer_mask, [&](uint32_t binding) {
    auto& write = writes[write_count++];
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    write.dstArrayElement = 0;
    write.dstBinding = binding;
    write.dstSet = vk_set;

    // Offsets are applied dynamically.
    auto& buffer = buffer_info[buffer_info_count++];
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    buffer = b->buffer;
    buffer.offset = 0;
    write.pBufferInfo = &buffer;
  });

  ForEachBitIndex(set_layout.storage_buffer_mask, [&](uint32_t binding) {
    auto& write = writes[write_count++];
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.dstArrayElement = 0;
    write.dstBinding = binding;
    write.dstSet = vk_set;
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    write.pBufferInfo = &b->buffer;
  });

  ForEachBitIndex(set_layout.sampled_buffer_mask, [&](uint32_t binding) {
    auto& write = writes[write_count++];
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eUniformTexelBuffer;
    write.dstArrayElement = 0;
    write.dstBinding = binding;
    write.dstSet = vk_set;
    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    write.pTexelBufferView = &b->buffer_view;
  });

  vk::DescriptorType image_descriptor_type;
  auto write_image_descriptor = [&](uint32_t binding) {
    auto& write = writes[write_count++];
    write.descriptorCount = 1;
    write.descriptorType = image_descriptor_type;
    write.dstArrayElement = 0;
    write.dstBinding = binding;
    write.dstSet = vk_set;

    const auto b = GetDescriptorBindingInfo(set_bindings, binding);
    if (set_layout.fp_mask & (1u << binding)) {
      write.pImageInfo = &b->image.fp;
    } else {
      write.pImageInfo = &b->image.integer;
    }
  };

  image_descriptor_type = vk::DescriptorType::eCombinedImageSampler;
  ForEachBitIndex(set_layout.sampled_image_mask, write_image_descriptor);

  image_descriptor_type = vk::DescriptorType::eStorageImage;
  ForEachBitIndex(set_layout.storage_image_mask, write_image_descriptor);

  image_descriptor_type = vk::DescriptorType::eInputAttachment;
  ForEachBitIndex(set_layout.input_attachment_mask, write_image_descriptor);

  vk_device().updateDescriptorSets(write_count, writes, 0, nullptr);
}

void CommandBuffer::SetToDefaultState(DefaultState default_state) {
  SetDirty(kDirtyStaticStateBit);
  pipeline_state_.SetToDefaultState(default_state);
}

void CommandBuffer::SaveState(CommandBuffer::SavedStateFlags flags,
                              CommandBuffer::SavedState* state) const {
  TRACE_DURATION("gfx", "escher::CommandBuffer::SaveState");

  for (unsigned i = 0; i < VulkanLimits::kNumDescriptorSets; ++i) {
    if (flags & (kSavedBindingsBit0 << i)) {
      memcpy(&state->bindings.descriptor_sets[i], &bindings_.descriptor_sets[i],
             sizeof(bindings_.descriptor_sets[i]));
    }
  }
  if (flags & kSavedViewportBit) {
    state->viewport = viewport_;
  }
  if (flags & kSavedScissorBit) {
    state->scissor = scissor_;
  }
  if (flags & kSavedRenderStateBit) {
    memcpy(&state->static_state, pipeline_state_.static_state(), sizeof(PipelineStaticState));
    state->potential_static_state = *pipeline_state_.potential_static_state();
    state->dynamic_state = dynamic_state_;
  }

  if (flags & kSavedPushConstantBit) {
    memcpy(state->bindings.push_constant_data, bindings_.push_constant_data,
           sizeof(bindings_.push_constant_data));
  }

  state->flags = flags;
}

void CommandBuffer::RestoreState(const CommandBuffer::SavedState& state) {
  TRACE_DURATION("gfx", "escher::CommandBuffer::RestoreState");

  for (unsigned i = 0; i < VulkanLimits::kNumDescriptorSets; ++i) {
    if (state.flags & (kSavedBindingsBit0 << i)) {
      if (memcmp(&state.bindings.descriptor_sets[i], &bindings_.descriptor_sets[i],
                 sizeof(bindings_.descriptor_sets[i]))) {
        memcpy(&bindings_.descriptor_sets[i], &state.bindings.descriptor_sets[i],
               sizeof(bindings_.descriptor_sets[i]));
        dirty_descriptor_sets_ |= 1u << i;
      }
    }
  }

  if (state.flags & kSavedPushConstantBit) {
    if (memcmp(state.bindings.push_constant_data, bindings_.push_constant_data,
               sizeof(bindings_.push_constant_data))) {
      memcpy(bindings_.push_constant_data, state.bindings.push_constant_data,
             sizeof(bindings_.push_constant_data));
      SetDirty(kDirtyPushConstantsBit);
    }
  }

  if ((state.flags & kSavedViewportBit) && memcmp(&state.viewport, &viewport_, sizeof(viewport_))) {
    viewport_ = state.viewport;
    SetDirty(kDirtyViewportBit);
  }

  if ((state.flags & kSavedScissorBit) && memcmp(&state.scissor, &scissor_, sizeof(scissor_))) {
    scissor_ = state.scissor;
    SetDirty(kDirtyScissorBit);
  }

  if (state.flags & kSavedRenderStateBit) {
    if (memcmp(&state.static_state, pipeline_state_.static_state(), sizeof(state.static_state))) {
      memcpy(pipeline_state_.static_state(), &state.static_state, sizeof(state.static_state));
      SetDirty(kDirtyStaticStateBit);
    }

    if (memcmp(&state.potential_static_state, pipeline_state_.potential_static_state(),
               sizeof(state.potential_static_state))) {
      memcpy(pipeline_state_.potential_static_state(), &state.potential_static_state,
             sizeof(state.potential_static_state));
      SetDirty(kDirtyStaticStateBit);
    }

    if (memcmp(&state.dynamic_state, &dynamic_state_, sizeof(dynamic_state_))) {
      memcpy(&dynamic_state_, &state.dynamic_state, sizeof(dynamic_state_));
      SetDirty(kDirtyStencilMasksAndReferenceBit | kDirtyDepthBiasBit);
    }
  }
}

void CommandBuffer::ClearAttachmentRect(uint32_t attachment, const vk::ClearRect& rect,
                                        const vk::ClearValue& value, vk::ImageAspectFlags aspect) {
  FX_DCHECK(IsInRenderPass());
  vk::ClearAttachment att = {};
  att.clearValue = value;
  att.colorAttachment = attachment;
  att.aspectMask = aspect;
  vk().clearAttachments(1, &att, 1, &rect);
}

void CommandBuffer::ClearColorAttachmentRect(uint32_t subpass_color_attachment_index,
                                             vk::Offset2D offset, vk::Extent2D extent,
                                             const vk::ClearColorValue& value) {
  ClearAttachmentRect(
      subpass_color_attachment_index,
      vk::ClearRect(vk::Rect2D(offset, extent), 0 /*baseArrayLayer*/, 1 /*layerCount*/), value,
      vk::ImageAspectFlagBits::eColor);
}

void CommandBuffer::ClearDepthStencilAttachmentRect(vk::Offset2D offset, vk::Extent2D extent,
                                                    const vk::ClearDepthStencilValue& value,
                                                    vk::ImageAspectFlags aspect) {
  ClearAttachmentRect(
      0, vk::ClearRect(vk::Rect2D(offset, extent), 0 /*baseArrayLayer*/, 1 /*layerCount*/), value,
      aspect);
}

void CommandBuffer::Blit(const ImagePtr& src_image, vk::Offset2D src_offset,
                         vk::Extent2D src_extent, const ImagePtr& dst_image,
                         vk::Offset2D dst_offset, vk::Extent2D dst_extent, vk::Filter filter) {
  impl_->KeepAlive(src_image);
  impl_->KeepAlive(dst_image);

  vk::ImageBlit blit;
  blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  blit.srcSubresource.mipLevel = 0;
  blit.srcSubresource.baseArrayLayer = 0;
  blit.srcSubresource.layerCount = 1;
  blit.dstSubresource = blit.srcSubresource;
  blit.srcOffsets[0] = vk::Offset3D(src_offset.x, src_offset.y, 0);
  blit.srcOffsets[1] =
      vk::Offset3D(src_offset.x + src_extent.width, src_offset.y + src_extent.height, 1);
  blit.dstOffsets[0] = vk::Offset3D(dst_offset.x, dst_offset.y, 0);
  blit.dstOffsets[1] =
      vk::Offset3D(dst_offset.x + dst_extent.width, dst_offset.y + dst_extent.height, 1);

  vk().blitImage(src_image->vk(), vk::ImageLayout::eTransferSrcOptimal, dst_image->vk(),
                 vk::ImageLayout::eTransferDstOptimal, 1, &blit, filter);
}

void CommandBuffer::SetShaderProgram(ShaderProgram* program, const SamplerPtr& immutable_sampler) {
  if (current_program_ == program && current_pipeline_layout_ &&
      current_pipeline_layout_->spec().immutable_sampler() == immutable_sampler) {
    return;
  }

  current_vk_pipeline_ = vk::Pipeline();
  current_program_ = program;
  impl_->KeepAlive(current_program_);

  // If we're in a render pass, the program must have a vertex stage.  If we're
  // not, the program must have a compute stage.
  FX_DCHECK(
      pipeline_state_.render_pass() && current_program_->GetModuleForStage(ShaderStage::kVertex) ||
      !pipeline_state_.render_pass() && current_program_->GetModuleForStage(ShaderStage::kCompute));

  SetDirty(kDirtyPipelineBit | kDirtyDynamicBits);

  auto old_pipeline_layout = current_pipeline_layout_;
  current_pipeline_layout_ =
      program->ObtainPipelineLayout(escher_->pipeline_layout_cache(), immutable_sampler);

  current_vk_pipeline_layout_ = current_pipeline_layout_->vk();
  impl_->KeepAlive(current_pipeline_layout_);

  if (!old_pipeline_layout) {
    dirty_descriptor_sets_ = ~0u;
    SetDirty(kDirtyPushConstantsBit);
  } else if (old_pipeline_layout != current_pipeline_layout_) {
    auto& new_spec = current_pipeline_layout_->spec();
    auto& old_spec = old_pipeline_layout->spec();

    // If the push constant layout changes, all descriptor sets
    // are invalidated.
    if (new_spec.push_constant_layout_hash() != old_spec.push_constant_layout_hash()) {
      dirty_descriptor_sets_ = ~0u;
      SetDirty(kDirtyPushConstantsBit);
    } else {
      // Find the first set whose descriptor set layout differs.  Set dirty bits
      // for that set and all subsequent sets.
      for (uint32_t set_index = 0; set_index < VulkanLimits::kNumDescriptorSets; ++set_index) {
        if (current_pipeline_layout_->GetDescriptorSetAllocator(set_index) !=
            old_pipeline_layout->GetDescriptorSetAllocator(set_index)) {
          SetBitsAtAndAboveIndex(&dirty_descriptor_sets_, set_index);
          break;
        }
      }
    }
  }
}

}  // namespace escher
