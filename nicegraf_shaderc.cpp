/**
 * Copyright (c) 2019 nicegraf contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy 
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#define _CRT_SECURE_NO_WARNINGS

#include "file_utils.h"
#include "header_file_writer.h"
#include "linear_dict.h"
#include "pipeline_layout.h"
#include "pipeline_metadata_file.h"
#include "separate_to_combined_map.h"
#include "shader_defines.h"
#include "shader_includer.h"
#include "target.h"
#include "technique_parser.h"
#include "shaderc/shaderc.hpp"
#include "spirv_glsl.hpp"
#include "spirv_msl.hpp"
#include "spirv_reflect.hpp"

#include <ctype.h>
#include <memory>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

const char *USAGE = R"RAW(
Usage: ngf_shaderc <input file name> [options]

Compiles HLSL shaders for multiple different targets.

Options:

  -O <path> - Folder to store output files in. Default is the current working
    directory.
  
  -t <target> - Generate shaders for the given target.  Accepted values are:
      * gl430;
      * gles310, gles300;
      * msl10, msl11, msl12, msl20;
      * msl10ios, msl11ios, msl12ios, msl20ios;
      * spv 
    If the option is encountered multiple times, shaders for all of the
    mentioned targets will be generated.

  -h <path> - Path (relative to the output folder) for the generated
      header file with descriptor binding and set IDs. If not specified, no
      header file will be generated.

  -n <identifier> - Namespace for the generated shader file. If not specified,
     global namespace is used.
)RAW";

// Create an instance of SPIRV-Cross compiler for a given target.
std::unique_ptr<spirv_cross::Compiler> create_cross_compiler(
    const uint32_t *spv_data, uint32_t spv_data_size, const target_info &ti) {
  switch(ti.api) {
  case target_api::GL: {
    auto spv_cross = std::make_unique<spirv_cross::CompilerGLSL>(
       spv_data, spv_data_size);
    spirv_cross::CompilerGLSL::Options opts;
    opts.version = ti.version_maj * 100u + ti.version_min * 10u;
    opts.separate_shader_objects = true;
    opts.es = (ti.platform == target_platform_class::MOBILE);
    spv_cross->set_common_options(opts);
    spv_cross->build_dummy_sampler_for_combined_images();
    spv_cross->build_combined_image_samplers();
    return spv_cross;
    break;
  }
  case target_api::VULKAN: {
    auto spv_cross =
        std::make_unique<spirv_cross::CompilerReflection>(spv_data,
                                                          spv_data_size);
    return spv_cross;
    break;
  }
  case target_api::METAL: {
    auto spv_cross = std::make_unique<spirv_cross::CompilerMSL>(spv_data,
                                                                spv_data_size);
    spirv_cross::CompilerMSL::Options opts;
    opts.set_msl_version(ti.version_maj, ti.version_min);
    const bool ios = ti.platform == target_platform_class::MOBILE;
    opts.platform = ios ? spirv_cross::CompilerMSL::Options::iOS
                        : spirv_cross::CompilerMSL::Options::macOS;
    spv_cross->set_msl_options(opts);
    return spv_cross;
    break;
  }
  default: assert(false);
  }
  return nullptr;
}

int main(int argc, const char *argv[]) {
  if (argc <= 1) { // Display help if invoked with no arguments.
    printf("%s\n", USAGE);
    exit(0);
  }

  // Process command line arguments.
  const std::string input_file_path { argv[1] }; // input file name.
  std::string out_folder = ".";
  std::string header_path = "";
  std::string header_namespace = "";
  std::vector<const target_info*> targets;
  for (uint32_t o = 2u; o < (uint32_t)argc; o += 2u) { // process options.
    const std::string option_name { argv[o] };
    if (o + 1u >= (uint32_t)argc) {
      fprintf(stderr, "Expected an option value after %s\n", argv[o]);
      exit(1);
    }
    const std::string option_value { argv[o + 1u] };
    if ("-t" == option_name) { // Target to generate code for.
      const auto *t = std::find_if(TARGET_MAP, TARGET_MAP + TARGET_COUNT,
                                   [&option_value](const named_target_info &x) {
                                     return option_value == x.name;
                                   });
      if (t == TARGET_MAP + TARGET_COUNT) {
        fprintf(stderr, "Unknown target \"%s\"\n", option_value.c_str());
        exit(1);
      }
      targets.push_back(&(t->target));
    } else if ("-O" == option_name) { // Output folder.
      out_folder = option_value;
    } else if ("-h" == option_name) {
      header_path = option_value;
    } else if ("-n" == option_name) {
      header_namespace = option_value;
    } else {
      fprintf(stderr, "Unknown option: \"%s\"\n", option_name.c_str());
      exit(1);
    }
  }
  // Do a sanity check - no point in running with no targets.
  if (targets.empty()) {
    fprintf(stderr, "No target shader flavors specified!"
                    " Use -t to specify a target.\n");
    exit(1);
  }

  // Make sure targets are always processed in the same order, no matter
  // what order they're specified in.
  std::sort(targets.begin(), targets.end(), [](const target_info *t1,
                                               const target_info *t2) { 
                                              return t1->api < t2->api;
                                            });

  // Load the input file.
  std::string input_source = read_file(input_file_path.c_str());
  input_source.push_back('\n');

  // Look for and parse technique directives in the code.
  std::vector<technique> techniques;
  parse_techniques(input_source, techniques);
  if (techniques.size() == 0u) {
    fprintf(stderr, "Input file does not appear to define any techniques. "
                    "Define techniques with a special comment (`//T:').\n");
    exit(1);
  }

  // Obtain SPIR-V.
  std::vector<shaderc::SpvCompilationResult> spv_results;
  const std::string kForceColumnMajorName = "force_column_major";
  const std::string kForceColumnMajorValue = "row_major";
  shaderc::Compiler compiler;
  for (const technique &tech : techniques) {
    for (const technique::entry_point ep : tech.entry_points) {
      // Set compile options.
      shaderc::CompileOptions shaderc_opts;
      add_defines_from_container(shaderc_opts, tech.defines);
      shaderc_opts.AddMacroDefinition(kForceColumnMajorName.c_str(),
                                      kForceColumnMajorName.size(),
                                      kForceColumnMajorValue.c_str(),
                                      kForceColumnMajorValue.size());
      shaderc_opts.SetAutoBindUniforms(true);
      shaderc_opts.SetAutoMapLocations(true);
      shaderc_opts.SetSourceLanguage(shaderc_source_language_hlsl);
      shaderc_opts.SetIncluder(std::make_unique<includer>());
      shaderc_opts.SetWarningsAsErrors();
      // Produce SPIR-V.
      spv_results.emplace_back(
        compiler.CompileGlslToSpv(input_source,
                                  ep.kind,
                                  input_file_path.c_str(),
                                  ep.name.c_str(),
                                  shaderc_opts));

      if (spv_results.back().GetCompilationStatus() !=
          shaderc_compilation_status_success) {
        fprintf(stderr, "%s", spv_results.back().GetErrorMessage().c_str());
        exit(1);
      } 
    }
  }

  // Attempt to open header file.
  const bool generate_header = !header_path.empty();
  header_file_writer header_writer(out_folder, header_path, header_namespace);
  if (!header_path.empty() && !header_writer.is_open()) {
    fprintf(stderr, "Failed to open output file %s\n", header_writer.path());
    exit(1);
  }

  // Generate output.
  bool generate_pipeline_metadata = true;
  for (const target_info *target_info : targets) {
    uint32_t spv_idx = 0u;
    for (const technique &tech : techniques) {
      pipeline_layout res_layout;
      separate_to_combined_map images_to_cis, samplers_to_cis;
      for (const technique::entry_point ep : tech.entry_points) {
        const shaderc::SpvCompilationResult &spv_result =
            spv_results[spv_idx++];
        std::string out;
        std::string out_file_path =
            out_folder + PATH_SEPARATOR + tech.name + 
            (ep.kind == shaderc_vertex_shader ? ".vs." : ".ps.")
            + target_info->file_ext;
        std::unique_ptr<spirv_cross::Compiler> compiler =
            create_cross_compiler(
                spv_result.cbegin(),
                (uint32_t)(spv_result.cend() - spv_result.cbegin()),
                *target_info);
        spirv_cross::ShaderResources resources =
            compiler->get_shader_resources();
        const std::vector<spirv_cross::CombinedImageSampler> &cis =
            compiler->get_combined_image_samplers();
        for (uint32_t cis_idx = 0u; cis_idx < cis.size(); ++cis_idx) {
          const spirv_cross::CombinedImageSampler &remap = cis[cis_idx];
          compiler->set_name(remap.combined_id,
                             compiler->get_name(remap.image_id) + "_" +
                             compiler->get_name(remap.sampler_id));
          compiler->set_decoration(remap.combined_id, spv::DecorationBinding,
                                   cis_idx);
          compiler->set_decoration(remap.combined_id,
                                   spv::DecorationDescriptorSet,
                                   AUTOGEN_CIS_DESCRIPTOR_SET);
        }
        const bool do_remapping = target_info->api == target_api::GL
                                  || target_info->api == target_api::METAL;
        if (do_remapping || generate_pipeline_metadata) {
          for (const spirv_cross::CombinedImageSampler &cis:
                   compiler->get_combined_image_samplers()) {
            images_to_cis.add_resource(cis.image_id, cis.combined_id,
                                       *compiler);
            samplers_to_cis.add_resource(cis.sampler_id, cis.combined_id,
                                         *compiler);
          }
          const stage_mask_bit smb =
              ep.kind == shaderc_vertex_shader
                           ? STAGE_MASK_VERTEX
                           : STAGE_MASK_FRAGMENT;
          auto process_resources =
            [smb, do_remapping, &compiler, &res_layout](
              const std::vector<spirv_cross::Resource> &resources,
              descriptor_type dtype) {
              res_layout.process_resources(resources, dtype, smb,
                                           do_remapping, *compiler);
            };
          process_resources(resources.uniform_buffers,
                            descriptor_type::UNIFORM_BUFFER);
          process_resources(resources.storage_buffers,
                            descriptor_type::STORAGE_BUFFER);
          process_resources(resources.separate_samplers,
                            descriptor_type::SAMPLER);
          process_resources(resources.separate_images,
                            descriptor_type::TEXTURE);
        }
        FILE *out_file = fopen(out_file_path.c_str(), "wb");
        if (out_file == nullptr) {
          fprintf(stderr, "Failed to open output file %s\n",
                  out_file_path.c_str());
          exit(1);
        }
        if (target_info->api != target_api::VULKAN) {
          out = compiler->compile();
          fwrite(&out[0], sizeof(uint8_t), out.length(), out_file);
        } else {
          fwrite(spv_result.cbegin(), sizeof(uint32_t),
                 spv_result.cend() - spv_result.cbegin(), out_file);
        }
        fclose(out_file);
      }

      // Write out the .pipeline file for the current technique.
      if (generate_pipeline_metadata) {
        header_writer.begin_technique(tech.name);
        std::string metadata_file_path =
            out_folder + PATH_SEPARATOR + tech.name + ".pipeline";
        pipeline_metadata_file metadata_file(metadata_file_path.c_str());

        // Write out the pipeline layout record.
        metadata_file.start_new_record();
        metadata_file.write_field(res_layout.set_count());
        for (uint32_t set = 0u; set < res_layout.set_count(); ++set) {
          const descriptor_set_layout &ds = res_layout.set(set);
          metadata_file.write_field((uint32_t)ds.size());
          for (const auto &d : ds) {
            metadata_file.write_field(d.second.slot);
            metadata_file.write_field((uint32_t)d.second.type);
            metadata_file.write_field(d.second.stage_mask);
            header_writer.write_descriptor(d.second, set);
          }
        }
        header_writer.end_technique();

        // Write out separate-to-combined map records.
        metadata_file.start_new_record();
        images_to_cis.serialize(metadata_file);
        metadata_file.start_new_record();
        samplers_to_cis.serialize(metadata_file);

        // Write out user metadata record.
        metadata_file.start_new_record();
        metadata_file.write_field((uint32_t)tech.additional_metadata.size());
        for (const auto &nameval : tech.additional_metadata) {
          metadata_file.write_raw_bytes(nameval.first.c_str(),
                                        nameval.first.size() + 1u);
          metadata_file.write_raw_bytes(nameval.second.c_str(),
                                        nameval.second.size() + 1u);
        }
        metadata_file.finalize();
      }
    }
    generate_pipeline_metadata = false;
  }

  return 0;
}
