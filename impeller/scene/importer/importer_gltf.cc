// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/scene/importer/importer.h"

#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include "flutter/fml/mapping.h"
#include "impeller/geometry/matrix.h"
#include "impeller/scene/importer/conversions.h"
#include "impeller/scene/importer/scene_flatbuffers.h"
#include "impeller/scene/importer/vertices_builder.h"
#include "third_party/tinygltf/tiny_gltf.h"

namespace impeller {
namespace scene {
namespace importer {

static const std::map<std::string, VerticesBuilder::AttributeType> kAttributes =
    {{"POSITION", VerticesBuilder::AttributeType::kPosition},
     {"NORMAL", VerticesBuilder::AttributeType::kNormal},
     {"TANGENT", VerticesBuilder::AttributeType::kTangent},
     {"TEXCOORD_0", VerticesBuilder::AttributeType::kTextureCoords},
     {"COLOR_0", VerticesBuilder::AttributeType::kColor},
     {"JOINTS_0", VerticesBuilder::AttributeType::kJoints},
     {"WEIGHTS_0", VerticesBuilder::AttributeType::kWeights}};

static bool WithinRange(int index, size_t size) {
  return index >= 0 && static_cast<size_t>(index) < size;
}

static bool MeshPrimitiveIsSkinned(const tinygltf::Primitive& primitive) {
  return primitive.attributes.find("JOINTS_0") != primitive.attributes.end() &&
         primitive.attributes.find("WEIGHTS_0") != primitive.attributes.end();
}

static void ProcessMaterial(const tinygltf::Model& gltf,
                            const tinygltf::Material& in_material,
                            fb::MaterialT& out_material) {
  out_material.type = fb::MaterialType::kUnlit;
  out_material.base_color_factor =
      ToFBColor(in_material.pbrMetallicRoughness.baseColorFactor);
  bool base_color_texture_valid =
      in_material.pbrMetallicRoughness.baseColorTexture.texCoord == 0 &&
      in_material.pbrMetallicRoughness.baseColorTexture.index >= 0 &&
      in_material.pbrMetallicRoughness.baseColorTexture.index <
          static_cast<int32_t>(gltf.textures.size());
  out_material.base_color_texture =
      base_color_texture_valid
          // This is safe because every GLTF input texture is mapped to a
          // `Scene->texture`.
          ? in_material.pbrMetallicRoughness.baseColorTexture.index
          : -1;
}

static bool ProcessMeshPrimitive(const tinygltf::Model& gltf,
                                 const tinygltf::Primitive& primitive,
                                 fb::MeshPrimitiveT& mesh_primitive) {
  //---------------------------------------------------------------------------
  /// Vertices.
  ///

  {
    bool is_skinned = MeshPrimitiveIsSkinned(primitive);
    std::unique_ptr<VerticesBuilder> builder =
        is_skinned ? VerticesBuilder::MakeSkinned()
                   : VerticesBuilder::MakeUnskinned();

    for (const auto& attribute : primitive.attributes) {
      auto attribute_type = kAttributes.find(attribute.first);
      if (attribute_type == kAttributes.end()) {
        std::cerr << "Vertex attribute \"" << attribute.first
                  << "\" not supported." << std::endl;
        continue;
      }
      if (!is_skinned &&
          (attribute_type->second == VerticesBuilder::AttributeType::kJoints ||
           attribute_type->second ==
               VerticesBuilder::AttributeType::kWeights)) {
        // If the primitive doesn't have enough information to be skinned, skip
        // skinning-related attributes.
        continue;
      }

      const auto accessor = gltf.accessors[attribute.second];
      const auto view = gltf.bufferViews[accessor.bufferView];

      const auto buffer = gltf.buffers[view.buffer];
      const unsigned char* source_start = &buffer.data[view.byteOffset];

      VerticesBuilder::ComponentType type;
      switch (accessor.componentType) {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
          type = VerticesBuilder::ComponentType::kSignedByte;
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
          type = VerticesBuilder::ComponentType::kUnsignedByte;
          break;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
          type = VerticesBuilder::ComponentType::kSignedShort;
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
          type = VerticesBuilder::ComponentType::kUnsignedShort;
          break;
        case TINYGLTF_COMPONENT_TYPE_INT:
          type = VerticesBuilder::ComponentType::kSignedInt;
          break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
          type = VerticesBuilder::ComponentType::kUnsignedInt;
          break;
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
          type = VerticesBuilder::ComponentType::kFloat;
          break;
        default:
          std::cerr << "Skipping attribute \"" << attribute.first
                    << "\" due to invalid component type." << std::endl;
          continue;
      }

      builder->SetAttributeFromBuffer(
          attribute_type->second,     // attribute
          type,                       // component_type
          source_start,               // buffer_start
          accessor.ByteStride(view),  // stride_bytes
          accessor.count);            // count
    }

    builder->WriteFBVertices(mesh_primitive);
  }

  //---------------------------------------------------------------------------
  /// Indices.
  ///

  {
    if (!WithinRange(primitive.indices, gltf.accessors.size())) {
      std::cerr << "Mesh primitive has no index buffer. Skipping." << std::endl;
      return false;
    }

    auto index_accessor = gltf.accessors[primitive.indices];
    auto index_view = gltf.bufferViews[index_accessor.bufferView];

    auto indices = std::make_unique<fb::IndicesT>();

    switch (index_accessor.componentType) {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        indices->type = fb::IndexType::k16Bit;
        break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        indices->type = fb::IndexType::k32Bit;
        break;
      default:
        std::cerr << "Mesh primitive has unsupported index type "
                  << index_accessor.componentType << ". Skipping.";
        return false;
    }
    indices->count = index_accessor.count;
    indices->data.resize(index_view.byteLength);
    const auto* index_buffer =
        &gltf.buffers[index_view.buffer].data[index_view.byteOffset];
    std::memcpy(indices->data.data(), index_buffer, indices->data.size());

    mesh_primitive.indices = std::move(indices);
  }

  //---------------------------------------------------------------------------
  /// Material.
  ///

  {
    auto material = std::make_unique<fb::MaterialT>();
    if (primitive.material >= 0 &&
        primitive.material < static_cast<int>(gltf.materials.size())) {
      ProcessMaterial(gltf, gltf.materials[primitive.material], *material);
    } else {
      material->type = fb::MaterialType::kUnlit;
    }
    mesh_primitive.material = std::move(material);
  }

  return true;
}

static void ProcessNode(const tinygltf::Model& gltf,
                        const tinygltf::Node& in_node,
                        fb::NodeT& out_node) {
  out_node.name = in_node.name;
  out_node.children = in_node.children;

  //---------------------------------------------------------------------------
  /// Transform.
  ///

  Matrix transform;
  if (in_node.translation.size() == 3) {
    transform = transform * Matrix::MakeTranslation(
                                {static_cast<Scalar>(in_node.translation[0]),
                                 static_cast<Scalar>(in_node.translation[0]),
                                 static_cast<Scalar>(in_node.translation[0])});
  }
  if (in_node.rotation.size() == 4) {
    transform = transform * Matrix::MakeRotation(Quaternion(
                                in_node.rotation[0], in_node.rotation[1],
                                in_node.rotation[2], in_node.rotation[3]));
  }
  if (in_node.scale.size() == 3) {
    transform =
        transform * Matrix::MakeScale({static_cast<Scalar>(in_node.scale[0]),
                                       static_cast<Scalar>(in_node.scale[1]),
                                       static_cast<Scalar>(in_node.scale[2])});
  }
  if (in_node.matrix.size() == 16) {
    if (!transform.IsIdentity()) {
      std::cerr << "The `matrix` attribute of node (name: " << in_node.name
                << ") is set in addition to one or more of the "
                   "`translation/rotation/scale` attributes. Using only the "
                   "`matrix` "
                   "attribute.";
    }
    transform = ToMatrix(in_node.matrix);
  }
  out_node.transform = ToFBMatrix(transform);

  //---------------------------------------------------------------------------
  /// Static meshes.
  ///

  if (WithinRange(in_node.mesh, gltf.meshes.size())) {
    auto& mesh = gltf.meshes[in_node.mesh];
    for (const auto& primitive : mesh.primitives) {
      auto mesh_primitive = std::make_unique<fb::MeshPrimitiveT>();
      if (!ProcessMeshPrimitive(gltf, primitive, *mesh_primitive)) {
        continue;
      }
      out_node.mesh_primitives.push_back(std::move(mesh_primitive));
    }
  }
}

static void ProcessTexture(const tinygltf::Model& gltf,
                           const tinygltf::Texture& in_texture,
                           fb::TextureT& out_texture) {
  if (in_texture.source < 0 ||
      in_texture.source >= static_cast<int>(gltf.images.size())) {
    return;
  }
  auto& image = gltf.images[in_texture.source];

  auto embedded = std::make_unique<fb::EmbeddedImageT>();
  embedded->bytes = image.image;
  size_t bytes_per_component = 0;
  switch (image.pixel_type) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
      embedded->component_type = fb::ComponentType::k8Bit;
      bytes_per_component = 1;
      break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
      embedded->component_type = fb::ComponentType::k16Bit;
      bytes_per_component = 2;
      break;
    default:
      std::cerr << "Texture component type " << image.pixel_type
                << " not supported." << std::endl;
      return;
  }
  if (image.image.size() !=
      bytes_per_component * image.component * image.width * image.height) {
    std::cerr << "Decompressed texture had unexpected buffer size. Skipping."
              << std::endl;
    return;
  }
  embedded->component_count = image.component;
  embedded->width = image.width;
  embedded->height = image.height;
  out_texture.embedded_image = std::move(embedded);
  out_texture.uri = image.uri;
}

bool ParseGLTF(const fml::Mapping& source_mapping, fb::SceneT& out_scene) {
  tinygltf::Model gltf;

  {
    tinygltf::TinyGLTF loader;
    std::string error;
    std::string warning;
    bool success = loader.LoadBinaryFromMemory(&gltf, &error, &warning,
                                               source_mapping.GetMapping(),
                                               source_mapping.GetSize());
    if (!warning.empty()) {
      std::cerr << "Warning while loading GLTF: " << warning << std::endl;
    }
    if (!error.empty()) {
      std::cerr << "Error while loading GLTF: " << error << std::endl;
    }
    if (!success) {
      return false;
    }
  }

  const tinygltf::Scene& scene = gltf.scenes[gltf.defaultScene];
  out_scene.children = scene.nodes;

  for (size_t texture_i = 0; texture_i < gltf.textures.size(); texture_i++) {
    auto texture = std::make_unique<fb::TextureT>();
    ProcessTexture(gltf, gltf.textures[texture_i], *texture);
    out_scene.textures.push_back(std::move(texture));
  }

  for (size_t node_i = 0; node_i < gltf.nodes.size(); node_i++) {
    auto node = std::make_unique<fb::NodeT>();
    ProcessNode(gltf, gltf.nodes[node_i], *node);
    out_scene.nodes.push_back(std::move(node));
  }

  return true;
}

}  // namespace importer
}  // namespace scene
}  // namespace impeller
