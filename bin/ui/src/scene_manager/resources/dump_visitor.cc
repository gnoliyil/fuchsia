// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/mozart/src/scene_manager/resources/dump_visitor.h"

#include <ostream>

#include "apps/mozart/src/scene_manager/resources/camera.h"
#include "apps/mozart/src/scene_manager/resources/compositor/display_compositor.h"
#include "apps/mozart/src/scene_manager/resources/compositor/layer.h"
#include "apps/mozart/src/scene_manager/resources/compositor/layer_stack.h"
#include "apps/mozart/src/scene_manager/resources/gpu_memory.h"
#include "apps/mozart/src/scene_manager/resources/host_memory.h"
#include "apps/mozart/src/scene_manager/resources/image.h"
#include "apps/mozart/src/scene_manager/resources/image_pipe.h"
#include "apps/mozart/src/scene_manager/resources/import.h"
#include "apps/mozart/src/scene_manager/resources/lights/directional_light.h"
#include "apps/mozart/src/scene_manager/resources/material.h"
#include "apps/mozart/src/scene_manager/resources/nodes/entity_node.h"
#include "apps/mozart/src/scene_manager/resources/nodes/scene.h"
#include "apps/mozart/src/scene_manager/resources/nodes/shape_node.h"
#include "apps/mozart/src/scene_manager/resources/renderers/renderer.h"
#include "apps/mozart/src/scene_manager/resources/shapes/circle_shape.h"
#include "apps/mozart/src/scene_manager/resources/shapes/rectangle_shape.h"
#include "apps/mozart/src/scene_manager/resources/shapes/rounded_rectangle_shape.h"
#include "lib/ftl/logging.h"

namespace scene_manager {

DumpVisitor::DumpVisitor(std::ostream& output) : output_(output) {}

DumpVisitor::~DumpVisitor() = default;

void DumpVisitor::Visit(GpuMemory* r) {
  // To prevent address space layout leakage, we don't print the pointers.
  BeginItem("GpuMemory", r);
  WriteProperty("size") << r->escher_gpu_mem()->size();
  WriteProperty("offset") << r->escher_gpu_mem()->offset();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(HostMemory* r) {
  // To prevent address space layout leakage, we don't print the pointers.
  BeginItem("HostMemory", r);
  WriteProperty("size") << r->size();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::VisitEscherImage(escher::Image* i) {
  WriteProperty("width") << i->width();
  WriteProperty("height") << i->height();
  WriteProperty("format") << static_cast<int>(i->format());
  WriteProperty("has_depth") << i->has_depth();
  WriteProperty("has_stencil") << i->has_stencil();
}

void DumpVisitor::Visit(Image* r) {
  BeginItem("Image", r);
  VisitEscherImage(r->GetEscherImage().get());
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(ImagePipe* r) {
  BeginItem("ImagePipe", r);
  if (r->GetEscherImage()) {
    BeginSection("currently presented image");
    VisitEscherImage(r->GetEscherImage().get());
    EndSection();
  }
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(EntityNode* r) {
  BeginItem("EntityNode", r);
  VisitNode(r);
  EndItem();
}

void DumpVisitor::Visit(ShapeNode* r) {
  BeginItem("ShapeNode", r);
  VisitNode(r);
  if (r->shape()) {
    BeginSection("shape");
    r->shape()->Accept(this);
    EndSection();
  }
  if (r->material()) {
    BeginSection("material");
    r->material()->Accept(this);
    EndSection();
  }
  EndItem();
}

void DumpVisitor::Visit(Scene* r) {
  BeginItem("Scene", r);
  VisitNode(r);
  EndItem();
}

void DumpVisitor::VisitNode(Node* r) {
  if (r->tag_value()) {
    WriteProperty("tag_value") << r->tag_value();
  }
  if (r->hit_test_behavior() != mozart2::HitTestBehavior::kDefault) {
    WriteProperty("hit_test_behavior")
        << static_cast<int>(r->hit_test_behavior());
  }
  if (r->clip_to_self()) {
    WriteProperty("clip_to_self") << r->clip_to_self();
  }
  if (!r->transform().IsIdentity()) {
    WriteProperty("transform") << r->transform();
  }
  if (!r->parts().empty()) {
    BeginSection("parts");
    for (auto& part : r->parts()) {
      part->Accept(this);
    }
    EndSection();
  }
  if (!r->children().empty()) {
    BeginSection("children");
    for (auto& child : r->children()) {
      child->Accept(this);
    }
    EndSection();
  }
  VisitResource(r);
}

void DumpVisitor::Visit(CircleShape* r) {
  BeginItem("CircleShape", r);
  WriteProperty("radius") << r->radius();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(RectangleShape* r) {
  BeginItem("RectangleShape", r);
  WriteProperty("width") << r->width();
  WriteProperty("height") << r->height();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(RoundedRectangleShape* r) {
  BeginItem("RoundedRectangleShape", r);
  WriteProperty("width") << r->width();
  WriteProperty("height") << r->height();
  WriteProperty("top_left_radius") << r->top_left_radius();
  WriteProperty("top_right_radius") << r->top_right_radius();
  WriteProperty("bottom_right_radius") << r->bottom_right_radius();
  WriteProperty("bottom_left_radius") << r->bottom_left_radius();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(Material* r) {
  BeginItem("Material", r);
  WriteProperty("red") << r->red();
  WriteProperty("green") << r->green();
  WriteProperty("blue") << r->blue();
  if (r->escher_material()->texture()) {
    WriteProperty("texture.width") << r->escher_material()->texture()->width();
    WriteProperty("texture.height")
        << r->escher_material()->texture()->height();
  } else if (r->texture_image()) {
    WriteProperty("texture.pending") << "true";
  }
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(DisplayCompositor* r) {
  BeginItem("DisplayCompositor", r);
  if (r->layer_stack()) {
    BeginSection("stack");
    r->layer_stack()->Accept(this);
    EndSection();
  }
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(LayerStack* r) {
  BeginItem("LayerStack", r);
  if (!r->layers().empty()) {
    BeginSection("layers");
    for (auto& layer : r->layers()) {
      layer->Accept(this);
    }
    EndSection();
  }
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(Layer* r) {
  BeginItem("Layer", r);
  if (r->renderer()) {
    VisitResource(r->renderer().get());
  } else {
    // TODO(MZ-249): Texture or ImagePipe or whatever.
  }
  EndItem();
}

void DumpVisitor::Visit(Camera* r) {
  using escher::operator<<;
  BeginItem("Camera", r);
  WriteProperty("position") << r->eye_position();
  WriteProperty("look_at") << r->eye_look_at();
  WriteProperty("up") << r->eye_up();
  BeginSection("scene");
  r->scene()->Accept(this);
  EndSection();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(Renderer* r) {
  BeginItem("Renderer", r);
  if (r->camera()) {
    BeginSection("camera");
    r->camera()->Accept(this);
    EndSection();
  }
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(DirectionalLight* r) {
  BeginItem("DirectionalLight", r);
  escher::operator<<(WriteProperty("direction"), r->direction());
  WriteProperty("intensity") << r->intensity();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::Visit(Import* r) {
  BeginItem("Import", r);
  WriteProperty("import_spec") << r->import_spec();
  WriteProperty("is_bound") << r->is_bound();
  BeginSection("delegate");
  r->delegate()->Accept(this);
  EndSection();
  VisitResource(r);
  EndItem();
}

void DumpVisitor::VisitResource(Resource* r) {
  if (r->event_mask()) {
    WriteProperty("event_mask") << r->event_mask();
  }
  if (!r->imports().empty()) {
    BeginSection("imports");
    for (auto& import : r->imports()) {
      import->Accept(this);
    }
    EndSection();
  }
}

void DumpVisitor::BeginItem(const char* type, Resource* r) {
  BeginLine();
  if (r) {
    output_ << r->id();
    if (!r->label().empty())
      output_ << ":\"" << r->label() << "\"";
    output_ << "> ";
  }
  output_ << type;
  indentation_ += 2;
}

std::ostream& DumpVisitor::WriteProperty(const char* label) {
  property_count_++;
  if (partial_line_) {
    if (property_count_ == 1u)
      output_ << ": ";
    else
      output_ << ", ";
  } else {
    BeginLine();
  }
  output_ << label << "=";
  return output_;
}

void DumpVisitor::EndItem() {
  EndLine();
  indentation_ -= 2;
}

void DumpVisitor::BeginSection(const char* label) {
  BeginLine();
  output_ << label << "...";
  EndLine();
  indentation_ += 2;
}

void DumpVisitor::EndSection() {
  FTL_DCHECK(!partial_line_);
  indentation_ -= 2;
}

void DumpVisitor::BeginLine() {
  EndLine();
  output_ << std::string(indentation_, ' ');
  partial_line_ = true;
}

void DumpVisitor::EndLine() {
  if (!partial_line_)
    return;
  output_ << std::endl;
  partial_line_ = false;
  property_count_ = 0u;
}

}  // namespace scene_manager
