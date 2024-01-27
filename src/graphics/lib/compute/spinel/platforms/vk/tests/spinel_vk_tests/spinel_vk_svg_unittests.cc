// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fxt_spinel_vk_render.h"
#include "spinel/ext/svg2spinel/svg2spinel.h"
#include "spinel/ext/transform_stack/transform_stack.h"
#include "svg/svg.h"

//
//
//

namespace spinel::vk::test {

//
// SVG tests
//

struct test_spinel_vk_svg : test_spinel_vk_render
{
  char const * svg_string;
  struct svg * svg;

  spinel_path_t *   paths;
  spinel_raster_t * rasters;

  test_spinel_vk_svg(char const * svg_string) : svg_string(svg_string)
  {
    ;
  }

  void
  create()
  {
    svg = svg_parse(svg_string, false);

    ASSERT_NE(svg, nullptr);
  }

  void
  dispose()
  {
    svg_dispose(svg);
  }

  uint32_t
  layer_count()
  {
    return svg_layer_count(svg);
  }

  void
  paths_create(spinel_path_builder_t pb)
  {
    paths = spinel_svg_paths_decode(svg, pb);
  }

  void
  rasters_create(spinel_raster_builder_t rb, struct spinel_transform_stack * const ts)
  {
    rasters = spinel_svg_rasters_decode(svg, rb, paths, ts);
  }

  void
  layers_create(spinel_composition_t composition, spinel_styling_t styling, bool is_srgb)
  {
    spinel_svg_layers_decode(svg, rasters, composition, styling, is_srgb);
  }

  void
  paths_dispose(spinel_context_t context)
  {
    spinel_svg_paths_release(svg, context, paths);
  }

  void
  rasters_dispose(spinel_context_t context)
  {
    spinel_svg_rasters_release(svg, context, rasters);
  }
};

//
// alias for test output aesthetics
//
using spinel_vk_svg = fxt_spinel_vk_render;
using param         = param_spinel_vk_render;
using test          = test_spinel_vk_svg;

//
//
//
TEST_P(spinel_vk_svg, svg_tests)
{
  ;
}

//
// Each test is a name, surface size, a snippet of SVG and a device-specific checksum
//
param const params[] = {
  {
    .name      = "black_square_2x2",
    .surface   = { 1024, 1024 },
    .checksums = {
      { 0xFBF00004, {} }
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g style = \"fill: black\">\n"
      "    <polyline points = \"2,2 4,2 4,4 2,4 2,2\"/>\n"
      "  </g>\n"
      "</svg>")
  },
  {
    .name      = "red_square_2x2",
    .surface   = { 1024, 1024 },
    .checksums = {
      { 0xFBF00400, {} }
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g style = \"fill: red\">\n"
      "    <polyline points = \"2,2 4,2 4,4 2,4 2,2\"/>\n"
      "  </g>\n"
      "</svg>")
  },
  {
    // NOTE: checksum varies due to differing fp32 and imageStore()
    // implementations
    .name      = "rasters_prefix_fix", // bug:39620
    .surface   = { 1024, 300 },
    .checksums = {
      { 0xFE013607, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xFD0B4012, {
          { param::AMD,    {} },
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, {} },
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g fill=\"black\"\n"
      "     transform=\"translate(-900,-950)\n"
      "                 scale(0.03125)\n"
      "                 matrix(-63986.14, -1331.7272, 1331.7272, -63986.14, 48960.0, 33920.0)\">\n"
      "    <polyline points =\n"
      "              \"-0.08,-0.02 0.28,-0.02 0.28,-0.02 0.28,0.02\n"
      "               0.28,0.02 -0.08,0.02 -0.08,0.02 -0.08,-0.02\"/>\n"
      "  </g>\n"
      "</svg>\n")
  },
  {
    .name      = "evenodd", // bug:42114
    .surface   = { 256, 256 },
    .checksums = {
      { 0x8FFF0070, {} }
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <path fill-rule=\"nonzero\" d=\"M24,8  h8 v8 h-8 z\n"
      "                                  M26,10 h4 v4 h-4 z\"/>\n"
      "  <path fill-rule=\"evenodd\" d=\"M8,8   h8 v8 h-8 z\n"
      "                                  M10,10 h4 v4 h-4 z\"/>\n"
      "</svg>\n")
  },
  {
    .name             = "composition_clip", // bug:25525
    .surface          = { 256, 256 },
    .clip             = { .composition = { 0, 0, 128, 128 } },
    .checksums        = {
      { 0xBFFF3840, {} }
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <path fill-rule=\"nonzero\" d=\"M8,8 h240 v240 h-240 z\"/>\n"
      "</svg>\n")
  },
  {
    .name        = "render_clip", // bug:25525
    .surface     = { 256, 256 },
    .clip        = { .render = { 0, 0, 128, 128 } },
    .checksums   = {
      { 0xBFFF3840, {} }
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <path fill-rule=\"nonzero\" d=\"M8,8 h240 v240 h-240 z\"/>\n"
      "</svg>\n")
  },
  {
    .name        = "bezier_quads",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0xF0F96717, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xEE9E0BBE, {
          { param::AMD,    {} },
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, {} },
        }
      },
      { 0xEEA613C6, {
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <!-- collinear quads -->\n"
      "  <path d= \"M450,200\n"
      "            Q500,200 550,200\n"
      "            Q550,500 550,800\n"
      "            Q500,800 450,800\n"
      "            Q450,500 450,200\"/>\n"
      "  <!-- W3C SVG Paths: Quads -->\n"
      "  <path d=\"M100,200 Q250,100 400,200\"/>\n"
      "  <path d=\"M600,200 Q825,100 900,200\"/>\n"
      "  <path d=\"M600,800 Q675,700 750,800 T900,800\"/>\n"
      "</svg>\n")
  },
  {
    .name        = "bezier_cubics",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0xC34EC099, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xBEFA6C49, {
          { param::AMD,    {} },
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, {} },
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <!-- collinear cubics -->\n"
      "  <path d= \"M450,200\n"
      "            C500,200 500,200 550,200\n"
      "            C550,500 550,500 550,800\n"
      "            C500,800 500,800 450,800\n"
      "            C450,500 450,500 450,200\"/>\n"
      "  <!-- W3C SVG Paths: Cubics -->\n"
      "  <path d=\"M100,200 C100,100 400,100 400,200\"/>\n"
      "  <path d=\"M100,500 C 25,400 475,400 400,500\"/>\n"
      "  <path d=\"M100,800 C175,700 325,700 400,800\"/>\n"
      "  <path d=\"M600,200 C675,100 975,100 900,200\"/>\n"
      "  <path d=\"M600,500 C600,350 900,650 900,500\"/>\n"
      "  <path d=\"M600,800 C625,700 725,700 750,800 S875,900 900,800\"/>\n"
      "</svg>\n")
  },
  {
    .name        = "rational_quads",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0xF9364B98, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xF994CF80, {
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, { { param::NVIDIA_VOLTA, UINT32_MAX } } },
        }
      },
      { 0xF994BD80, {
          { param::AMD,    {} },
          { param::NVIDIA, { { 0, param::NVIDIA_PASCAL } } },
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g transform=\"translate(16,16)\">\n"
      "    <g>\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "      <g fill=\"blue\" transform=\"translate(0,24)\">\n"
      "        <path d= \"r64,64 128,0 +3.0 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"r64,64 128,0 +1.0 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"r64,64 128,0 +0.3 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"r64,64 128,0 +0.0 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.506757,-0.281532,0,1,0,200,-0.00112613,0)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "      <g fill=\"red\" transform=\"translate(0,24)\">\n"
      "        <path d= \"r64,64 128,0 +3.0 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"r64,64 128,0 +1.0 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"r64,64 128,0 +0.3 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"r64,64 128,0 +0.0 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.675676,0,-3.12,0,312,400,0,-0.006)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "      <g fill=\"yellow\" transform=\"translate(0,24)\">\n"
      "        <path d= \"r64,64 128,0 +3.0 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"r64,64 128,0 +1.0 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"r64,64 128,0 +0.3 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"r64,64 128,0 +0.0 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "  </g>\n"
      "</svg>\n")
  },
  {
    .name        = "proj_quads",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0xC4D0DB79, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xC5127E22, {
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, { { param::NVIDIA_VOLTA, UINT32_MAX } } },
        }
      },
      { 0xC5127B22, {
          { param::AMD,    {} },
          { param::NVIDIA, { { 0, param::NVIDIA_PASCAL } } },
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g transform=\"translate(16,16)\">\n"
      "    <g>\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "      <g fill=\"blue\" transform=\"translate(0,24)\">\n"
      "        <path d= \"q64,64 128,0 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.506757,-0.281532,0,1,0,200,-0.00112613,0)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "      <g fill=\"red\" transform=\"translate(0,24)\">\n"
      "        <path d= \"q64,64 128,0 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.675676,0,-3.12,0,312,400,0,-0.006)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "      <g fill=\"yellow\" transform=\"translate(0,24)\">\n"
      "        <path d= \"q64,64 128,0 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "  </g>\n"
      "</svg>\n")
  },
  {
    .name        = "rational_cubics",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0xB783614E, {
          { param::AMD,    {} }
        }
      },
      { 0xB90FF0FC, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xB7841DF8, {
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
        }
      },
      { 0xB783FDD8, {
          { param::INTEL,  {} }
        }
      },
      { 0xB783B9A6, {
          { param::NVIDIA, { { 0, param::NVIDIA_PASCAL } } }
        }
      },
      { 0xB783CBA6, {
          { param::NVIDIA, { { param::NVIDIA_VOLTA, UINT32_MAX } } }
        }
      },
      { 0xB783C0B0, {
          { param::MESA,   {} }
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g transform=\"translate(16,16)\">\n"
      "    <g>\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "      <g fill=\"blue\" transform=\"translate(0,24)\">\n"
      "        <path d= \"d32,68 96,68 128,0 +2.0 +2.0 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +1.0 +1.0 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +0.5 +0.5 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +0.0 +0.0 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.506757,-0.281532,0,1,0,200,-0.00112613,0)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "      <g fill=\"red\" transform=\"translate(0,24)\">\n"
      "        <path d= \"d32,68 96,68 128,0 +2.0 +2.0 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +1.0 +1.0 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +0.5 +0.5 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +0.0 +0.0 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.675676,0,-3.12,0,312,400,0,-0.006)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "      <g fill=\"yellow\" transform=\"translate(0,24)\">\n"
      "        <path d= \"d32,68 96,68 128,0 +2.0 +2.0 z\" transform=\"translate( 16)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +1.0 +1.0 z\" transform=\"translate(160)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +0.5 +0.5 z\" transform=\"translate(304)\"/>\n"
      "        <path d= \"d32,68 96,68 128,0 +0.0 +0.0 z\" transform=\"translate(448)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "  </g>\n"
      "</svg>\n")
  },
  {
    .name        = "proj_cubics",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0x13804D7A, {
          { param::AMD,    {} },
        }
      },
      { 0x1502A167, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0x138096C0, {
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, { { param::NVIDIA_VOLTA, UINT32_MAX } } },
        }
      },
      { 0x138093C0, {
          { param::NVIDIA, { { 0, param::NVIDIA_PASCAL } } },
        }
      },
      { 0x138093C3, {
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <g transform=\"translate(16,16)\">\n"
      "    <g>\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "      <g fill=\"blue\" transform=\"translate(0,24)\">\n"
      "        <path d= \"c32,68 96,68 128,0 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.506757,-0.281532,0,1,0,200,-0.00112613,0)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "      <g fill=\"red\" transform=\"translate(0,24)\">\n"
      "        <path d= \"c32,68 96,68 128,0 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "    <g transform=\"project(0.675676,0,-3.12,0,312,400,0,-0.006)\">\n"
      "      <rect width=\"592\" height=\"100\"/>\n"
      "      <g fill=\"green\" transform=\"translate(0,8)\">\n"
      "        <path d= \"v16 h128 v-16 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "      <g fill=\"yellow\" transform=\"translate(0,24)\">\n"
      "        <path d= \"c32,68 96,68 128,0 z\" transform=\"translate(160)\"/>\n"
      "      </g>\n"
      "    </g>\n"
      "  </g>\n"
      "</svg>\n")
  },
  {
    .name        = "circles",
    .surface     = { 1024, 1024 },
    .checksums = {
      { 0xE7E21D06, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xE86BA68F, {
          { param::AMD,    {} },
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, {} },
        }
      },
      { 0xE866A18A, {
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <circle cx=\"16\"  cy=\"512\" r=\"16\"/>\n"
      "  <circle cx=\"64\"  cy=\"512\" r=\"32\"/>\n"
      "  <circle cx=\"160\" cy=\"512\" r=\"64\"/>\n"
      "  <circle cx=\"352\" cy=\"512\" r=\"128\"/>\n"
      "  <circle cx=\"736\" cy=\"512\" r=\"256\"/>\n"
      "</svg>\n")
  },
  {
    .name        = "ellipses",
    .surface     = { 1024, 1024 },

    .checksums = {
      { 0xCB32986F, {
          { param::AMD,    {} },
        }
      },
      { 0xCC46AC82, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xCB49AF86, {
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
        }
      },
      { 0xCB21875E, {
          { param::NVIDIA, {} },
        }
      },
      { 0xCB379D74, {
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <ellipse cx=\"16\"  cy=\"512\" rx=\"16\"  ry=\"32\" />\n"
      "  <ellipse cx=\"64\"  cy=\"512\" rx=\"32\"  ry=\"64\" />\n"
      "  <ellipse cx=\"160\" cy=\"512\" rx=\"64\"  ry=\"128\"/>\n"
      "  <ellipse cx=\"352\" cy=\"512\" rx=\"128\" ry=\"256\"/>\n"
      "  <ellipse cx=\"736\" cy=\"512\" rx=\"256\" ry=\"512\"/>\n"
      "</svg>\n")
  },
  {
    .name        = "arcs",
    .surface     = { 1024, 512 },
    .checksums = {
      //
      // Fuchsia's `ulib/musl/.../math` trig functions produce a slightly
      // different set of rational quad control points than the trig functions
      // in the gLinux host environment. See fxbug.dev/115402 for more info.
      //
      { 0xC337DEDF, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xC2E4C4A9, {
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
        }
      },
      { 0xC2E4C2A9, {
          { param::INTEL,  { { 0x9A49, 0x9A49 } } }, // Xe TGL GT2 - Host: fxbug.dev/115402
        }
      },
      { 0xC2E4C3A9, {
          { param::INTEL,  {} }, // Xe TGL GT2 - Fuchsia: fxbug.dev/115402
          { param::NVIDIA, { { param::NVIDIA_VOLTA, UINT32_MAX } } },
        }
      },
      { 0xC2E7CDA9, {
          { param::AMD,    {} },
          { param::NVIDIA, { { 0, param::NVIDIA_PASCAL } } },
        }
      },
      { 0xC2F1D7B3, {
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
      "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <!-- four cases -->\n"
      "  <g transform=\"translate(0,0)\">\n"
      "    <ellipse cx=\"125\" cy=\"125\" rx=\"100\" ry=\"50\" fill=\"green\"/>\n"
      "    <ellipse cx=\"225\" cy=\"75\" rx=\"100\" ry=\"50\" fill=\"blue\"/>\n"
      "    <path d=\"M 125,75 a100,50 0 0,0 100,50\" fill=\"red\"/>\n"
      "  </g>\n"
      "  <g transform=\"translate(300,0)\">\n"
      "    <ellipse cx=\"225\" cy=\"75\" rx=\"100\" ry=\"50\" fill=\"blue\"/>\n"
      "    <ellipse cx=\"125\" cy=\"125\" rx=\"100\" ry=\"50\" fill=\"green\"/>\n"
      "    <path d=\"M 125,75 a100,50 0 0,1 100,50\" fill=\"red\"/>\n"
      "  </g>\n"
      "  <g transform=\"translate(0,250)\">\n"
      "    <ellipse cx=\"225\" cy=\"75\" rx=\"100\" ry=\"50\" fill=\"blue\"/>\n"
      "    <ellipse cx=\"125\" cy=\"125\" rx=\"100\" ry=\"50\" fill=\"green\"/>\n"
      "    <path d=\"M 125,75 a100,50 0 1,0 100,50\" fill=\"red\"/>\n"
      "  </g>\n"
      "  <g transform=\"translate(300,250)\">\n"
      "    <ellipse cx=\"125\" cy=\"125\" rx=\"100\" ry=\"50\" fill=\"green\"/>\n"
      "    <ellipse cx=\"225\" cy=\"75\" rx=\"100\" ry=\"50\" fill=\"blue\"/>\n"
      "    <path d=\"M 125,75 a100,50 0 1,1 100,50\" fill=\"red\"/>\n"
      "  </g>\n"
      "  <!-- simple -->\n"
      "  <g transform=\"translate(640,0)\">\n"
      "    <path d=\"M80 80\n"
      "             A 45 45, 0, 0, 0, 125 125\n"
      "             L 125 80 Z\" fill=\"green\"/>\n"
      "    <path d=\"M230 80\n"
      "             A 45 45, 0, 1, 0, 275 125\n"
      "             L 275 80 Z\" fill=\"red\"/>\n"
      "    <path d=\"M80 230\n"
      "             A 45 45, 0, 0, 1, 125 275\n"
      "             L 125 230 Z\" fill=\"purple\"/>\n"
      "    <path d=\"M230 230\n"
      "             A 45 45, 0, 1, 1, 275 275\n"
      "             L 275 230 Z\" fill=\"blue\"/>\n"
      "  </g>\n"
      "  <!-- angled -->\n"
      "  <g transform=\"translate(675,225)\">\n"
      "    <path d=\"M 110 215\n"
      "             A 30 50 0 0 1 162.55 162.45 z\n"
      "             M 172.55 152.45\n"
      "             A 30 50 -45 0 1 215.1 109.9 z\"/>\n"
      "  </g>\n"
      "</svg>")
  },
  {
    .name        = "bifrost4",
    .surface     = { 600, 1024 },
    .checksums = {
      { 0xD4E08B15, {
          { param::ARM,    { { param::ARM_MALI_G31, param::ARM_MALI_G31 } } },
        }
      },
      { 0xD526D15B, {
          { param::AMD,    {} },
          { param::ARM,    { { param::ARM_MALI_G52, param::ARM_MALI_G52 } } },
          { param::INTEL,  {} },
          { param::NVIDIA, {} },
          { param::MESA,   {} },
        }
      },
    },
    .test = std::make_shared<test>(
        "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
        "  <g transform=\"rotate(11,308,284) scale(1.0) translate(200,200)\">\n"
        "    <path d=\"M -16.81300000000002, 342.93499999999995\n"
        "             C -16.17700000000002, 346.405,             -14.10100000000002,   353.47799999999995, -7.31000000000002, 358.7919999999999\n"
        "             L  -6.47700000000002, 359.4439999999999\n"
        "             L  -6.5,              358.39\n"
        "             C  -6.741,            348.18,              -5.998,               331.775,            -2.976,            331.217\n"
        "             C  -2.231,            331.079,             -0.04599999999999982, 332.027,             4.128,            343.769\n"
        "             L   8.546,            361.894\n"
        "             Z\"\n"
        "          />\n"
        "  </g>\n"
        "</svg>\n")
  },
};

//
//
//
INSTANTIATE_TEST_SUITE_P(spinel_vk_svg_tests,  //
                         spinel_vk_svg,        //
                         ::testing::ValuesIn(params),
                         spinel_vk_svg::param_name);

}  // namespace spinel::vk::test

//
//
//
