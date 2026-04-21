#pragma once

#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#include "tiny_gltf.h"
#include "stb_image.h"

#ifdef PLATFORM_EMSCRIPTEN
  #include <GLES3/gl3.h>
#else
  #include <glad/glad.h>
#endif
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <iostream>

struct GLTFModel {
    GLuint vao=0;
    GLuint vbo=0;
    GLuint ebo=0;
    GLsizei indexCount=0;
    GLuint texture = 0;
};

inline bool loadSimpleGLTF(const std::string &path, GLTFModel &out) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err; std::string warn;
    bool ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty()) std::cerr << "gltf warn: " << warn << std::endl;
    if (!err.empty()) std::cerr << "gltf err: " << err << std::endl;
    if (!ret) return false;
    if (model.meshes.empty()) return false;
    const tinygltf::Mesh &mesh = model.meshes[0];
    if (mesh.primitives.empty()) return false;
    const tinygltf::Primitive &prim = mesh.primitives[0];
    // Verify required attributes exist
    if (prim.attributes.find("POSITION") == prim.attributes.end() ||
        prim.attributes.find("NORMAL") == prim.attributes.end()) {
        std::cerr << "gltf: missing POSITION or NORMAL attribute" << std::endl;
        return false;
    }
    if (prim.indices < 0) {
        std::cerr << "gltf: no index accessor" << std::endl;
        return false;
    }

    // Safely resolve an accessor to a raw data pointer, validating all indices
    // and computing the effective per-element stride.  Returns nullptr on error.
    auto resolveAccessor = [&](int accIdx_, size_t requiredElemBytes, size_t& strideOut)
        -> const unsigned char* {
        if (accIdx_ < 0 || static_cast<size_t>(accIdx_) >= model.accessors.size())
            return nullptr;
        const auto& acc = model.accessors[static_cast<size_t>(accIdx_)];
        if (acc.bufferView < 0 || static_cast<size_t>(acc.bufferView) >= model.bufferViews.size())
            return nullptr;
        const auto& bv = model.bufferViews[static_cast<size_t>(acc.bufferView)];
        if (bv.buffer < 0 || static_cast<size_t>(bv.buffer) >= model.buffers.size())
            return nullptr;
        const auto& buf = model.buffers[static_cast<size_t>(bv.buffer)];
        strideOut = (bv.byteStride != 0) ? static_cast<size_t>(bv.byteStride) : requiredElemBytes;
        if (strideOut < requiredElemBytes) return nullptr;
        size_t dataOffset = static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(acc.byteOffset);
        size_t lastElemStart = dataOffset + strideOut * (acc.count > 0 ? acc.count - 1 : 0);
        if (lastElemStart + requiredElemBytes > buf.data.size()) return nullptr;
        return buf.data.data() + dataOffset;
    };

    size_t posStride = 0, normStride = 0, uvStride = 0;
    const unsigned char* posData = resolveAccessor(
        prim.attributes.at("POSITION"), 3 * sizeof(float), posStride);
    if (!posData) { std::cerr << "gltf: invalid POSITION accessor" << std::endl; return false; }

    const unsigned char* normData = resolveAccessor(
        prim.attributes.at("NORMAL"), 3 * sizeof(float), normStride);
    if (!normData) { std::cerr << "gltf: invalid NORMAL accessor" << std::endl; return false; }

    const unsigned char* uvData = nullptr;
    if (prim.attributes.find("TEXCOORD_0") != prim.attributes.end()) {
        uvData = resolveAccessor(prim.attributes.at("TEXCOORD_0"), 2 * sizeof(float), uvStride);
        if (!uvData) uvStride = 0; // UV is optional; fall back gracefully
    }

    size_t idxAccessorStride = 0;
    const unsigned char* idxData = resolveAccessor(
        prim.indices, sizeof(unsigned short), idxAccessorStride);
    if (!idxData) { std::cerr << "gltf: invalid index accessor" << std::endl; return false; }

    const tinygltf::Accessor& accPos = model.accessors[static_cast<size_t>(prim.attributes.at("POSITION"))];
    const tinygltf::Accessor& accIdx = model.accessors[static_cast<size_t>(prim.indices)];
    size_t idxCount = accIdx.count;

    // interleave pos/norm/uv — use per-element stride for each attribute
    struct Vert { float px,py,pz; float nx,ny,nz; float u,v; };
    std::vector<Vert> verts(accPos.count);
    for (size_t i = 0; i < accPos.count; ++i) {
        const auto* pf = reinterpret_cast<const float*>(posData  + i * posStride);
        const auto* nf = reinterpret_cast<const float*>(normData + i * normStride);
        verts[i].px = pf[0]; verts[i].py = pf[1]; verts[i].pz = pf[2];
        verts[i].nx = nf[0]; verts[i].ny = nf[1]; verts[i].nz = nf[2];
        if (uvData) {
            const auto* uvf = reinterpret_cast<const float*>(uvData + i * uvStride);
            verts[i].u = uvf[0]; verts[i].v = uvf[1];
        } else {
            verts[i].u = verts[i].v = 0.0f;
        }
    }
    std::vector<unsigned int> indices(idxCount);
    if (accIdx.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        for (size_t i = 0; i < idxCount; ++i)
            indices[i] = *reinterpret_cast<const unsigned short*>(idxData + i * idxAccessorStride);
    } else if (accIdx.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        for (size_t i = 0; i < idxCount; ++i)
            indices[i] = *reinterpret_cast<const unsigned int*>(idxData + i * idxAccessorStride);
    } else if (accIdx.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        for (size_t i = 0; i < idxCount; ++i)
            indices[i] = *reinterpret_cast<const unsigned char*>(idxData + i * idxAccessorStride);
    }

    // create GL buffers
    glGenVertexArrays(1, &out.vao);
    glGenBuffers(1, &out.vbo);
    glGenBuffers(1, &out.ebo);
    glBindVertexArray(out.vao);
    glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(Vert), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    // positions -> location 0
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vert),(void*)offsetof(Vert,px));
    // normals -> location 1
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Vert),(void*)offsetof(Vert,nx));
    // shader expects aCol at location 2; glTF doesn't provide color so set a default constant color here
    glEnableVertexAttribArray(2); glVertexAttrib3f(2, 1.0f, 1.0f, 1.0f);
    // texcoords -> shader expects aUV at location 3
    glEnableVertexAttribArray(3); glVertexAttribPointer(3,2,GL_FLOAT,GL_FALSE,sizeof(Vert),(void*)offsetof(Vert,u));
    glBindVertexArray(0);
    out.indexCount = (GLsizei)indices.size();

    // load first image as texture if present; otherwise create a small procedural fallback texture
    out.texture = 0;
    if (!model.images.empty()) {
        const tinygltf::Image &img = model.images[0];
        if (!img.image.empty() && img.width > 0 && img.height > 0) {
            int w = img.width; int h = img.height; int comp = img.component;
            GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            GLenum format = GL_RGBA;
            if (comp == 3) format = GL_RGB;
            glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, GL_UNSIGNED_BYTE, img.image.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            out.texture = tex;
        }
    }
    if (out.texture == 0) {
        // create a small 4x4 checker fallback texture (RGB)
        const int CX = 4, CY = 4;
        unsigned char pixels[CX * CY * 3];
        for (int y = 0; y < CY; ++y) {
            for (int x = 0; x < CX; ++x) {
                int idx = (y * CX + x) * 3;
                bool on = ((x / 2) % 2) ^ ((y / 2) % 2);
                unsigned char c = on ? 200 : 80;
                pixels[idx+0] = c; pixels[idx+1] = (unsigned char)(c * 0.9f); pixels[idx+2] = (unsigned char)(c * 0.8f);
            }
        }
        GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, CX, CY, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        out.texture = tex;
    }

    return true;
}
