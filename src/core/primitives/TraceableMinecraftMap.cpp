#include "TraceableMinecraftMap.hpp"

#include "primitives/TriangleMesh.hpp"

#include "materials/ConstantTexture.hpp"

#include "mc-loader/MapLoader.hpp"

#include "cameras/PinholeCamera.hpp"

#include "bsdfs/TransparencyBsdf.hpp"
#include "bsdfs/LambertBsdf.hpp"

#include "math/Mat4f.hpp"

#include "io/ImageIO.hpp"
#include "io/MeshIO.hpp"
#include "io/Scene.hpp"

#include <tinyformat/tinyformat.hpp>
#include <unordered_set>

namespace Tungsten {

TraceableMinecraftMap::TraceableMinecraftMap()
: _missingBsdf(std::make_shared<LambertBsdf>())
{
    _missingBsdf->setAlbedo(std::make_shared<ConstantTexture>(0.2f));
}

TraceableMinecraftMap::TraceableMinecraftMap(const TraceableMinecraftMap &o)
: Primitive(o)
{
    _mapPath = o._mapPath;
    _packPath = o._packPath;

    _missingBsdf = o._missingBsdf;
    _bsdfCache = o._bsdfCache;
    _models = o._models;
}

void TraceableMinecraftMap::saveTestScene()
{
    int w = int(std::sqrt(_models.size()));

    for (size_t i = 0; i < _models.size(); ++i) {
        const TriangleMesh &m = *static_cast<const TriangleMesh *>(_models[i].get());
        MeshIO::save(tfm::format("mctest/%s.wo3", m.name()), m.verts(), m.tris());

        float x = (i % w)*2.0f;
        float z = (i / w)*2.0f;

        Mat4f offset = Mat4f::translate(Vec3f(x, 0.0f, z));
        _models[i]->setTransform(offset*_models[i]->transform());
    }

    Scene::save("mctest/test.json", Scene(
        ".",
        _models,
        std::vector<std::shared_ptr<Bsdf>>(),
        std::make_shared<TextureCache>(),
        std::make_shared<PinholeCamera>()
    ));
}

void TraceableMinecraftMap::getTexProperties(const std::string &path, int w, int h,
        int &tileW, int &tileH, bool &clamp, bool &linear)
{
    tileW = w;
    tileH = w;
    linear = false;
    clamp = false;

    File meta(path + ".mcmeta");
    if (!meta.exists())
        return;

    std::string json = FileUtils::loadText(meta.path().c_str());
    if (json.empty())
        return;

    rapidjson::Document document;
    document.Parse<0>(json.c_str());
    if (document.HasParseError() || !document.IsObject())
        return;

    const rapidjson::Value::Member *animation = document.FindMember("animation");
    const rapidjson::Value::Member *texture   = document.FindMember("texture");

    if (animation) {
        int numTilesX, numTilesY;
        if (JsonUtils::fromJson(animation->value, "width", numTilesX))
            tileW = w/numTilesX;
        if (JsonUtils::fromJson(animation->value, "height", numTilesY))
            tileH = h/numTilesY;
    }

    if (texture) {
        JsonUtils::fromJson(texture->value, "blur", linear);
        JsonUtils::fromJson(texture->value, "clamp", clamp);
    }
}

void TraceableMinecraftMap::loadTexture(ResourcePackLoader &pack, const std::string &name)
{
    if (_bsdfCache.count(name))
        return;

    std::string path = pack.textureBasePath() + name + ".png";

    int w, h;
    std::unique_ptr<uint8[]> img = ImageIO::loadLdr(path, TexelConversion::REQUEST_RGB, w, h);

    if (!img)
        return;

    int tileW, tileH;
    bool linear, clamp;
    getTexProperties(path, w, h, tileW, tileH, clamp, linear);

    bool opaque = true;
    std::unique_ptr<uint8[]> tile(new uint8[tileW*tileH*4]), alpha;
    for (int y = 0; y < tileH; ++y) {
        for (int x = 0; x < tileW; ++x) {
            for (int i = 0; i < 4; ++i)
                tile[i + 4*(x + y*tileW)] = img[i + 4*(x + y*w)];
            opaque = opaque && (tile[3 + 4*(x + y*tileW)] == 0xFF);
        }
    }
    if (!opaque) {
        alpha.reset(new uint8[tileH*tileW]);
        for (int i = 0; i < tileH*tileW; ++i)
            alpha[i] = tile[i*4 + 3];
    }

    std::shared_ptr<Texture> texture(std::make_shared<BitmapTexture>(name + ".png", tile.release(),
            tileW, tileH, BitmapTexture::TexelType::RGB_LDR, linear, clamp));
    std::shared_ptr<Bsdf> bsdf(std::make_shared<LambertBsdf>());
    bsdf->setAlbedo(texture);

    if (!opaque)
        bsdf = std::make_shared<TransparencyBsdf>(std::make_shared<BitmapTexture>(name + ".png", alpha.release(),
                tileW, tileH, BitmapTexture::TexelType::SCALAR_LDR, linear, clamp), bsdf);

    _bsdfCache.insert(std::make_pair(name, bsdf));
}

void TraceableMinecraftMap::loadTextures(ResourcePackLoader &pack)
{
    for (const BlockDescriptor &desc : pack.blockDescriptors())
        for (const BlockVariant &var : desc.variants())
            for (const ModelRef &model : var.models())
                for (const TexturedQuad &quad : *model.builtModel())
                    loadTexture(pack, quad.texture);
}

void TraceableMinecraftMap::buildModel(const ModelRef &model)
{
    if (!model.builtModel())
        return;

    Mat4f tform =
         Mat4f::translate(Vec3f(0.5f))
        *Mat4f::rotXYZ(Vec3f(float(model.xRot()), 0.0f, 0.0f))
        *Mat4f::rotXYZ(Vec3f(0.0f, float(model.yRot()), 0.0f))
        *Mat4f::rotXYZ(Vec3f(0.0f, 0.0f, float(model.zRot())))
        *Mat4f::scale(Vec3f(1.0f/16.0f))
        *Mat4f::translate(Vec3f(-8.0f));

    std::vector<Vertex> verts;
    std::vector<TriangleI> tris;

    std::vector<std::shared_ptr<Bsdf>> bsdfs;
    std::unordered_map<std::string, int> mapping;
    std::unordered_set<Vec<float, 12>> duplicates;

    for (const TexturedQuad &quad : *model.builtModel()) {
        if (!mapping.count(quad.texture)) {
            mapping.insert(std::make_pair(quad.texture, int(bsdfs.size())));
            if (_bsdfCache.count(quad.texture))
                bsdfs.push_back(_bsdfCache[quad.texture]);
            else
                bsdfs.push_back(_missingBsdf);
        }
        int material = mapping[quad.texture];

        Vec3f p0 = tform*quad.p0;
        Vec3f p1 = tform*quad.p1;
        Vec3f p2 = tform*quad.p2;
        Vec3f p3 = tform*quad.p3;
        Vec<float, 12> key(
            p0.x(), p0.y(), p0.z(),
            p1.x(), p1.y(), p1.z(),
            p2.x(), p2.y(), p2.z(),
            p3.x(), p3.y(), p3.z()
        );
        if (duplicates.count(key))
            continue;
        duplicates.insert(key);

        Vec2f uv0(quad.uv0.x(), 1.0f - quad.uv0.y());
        Vec2f uv1(quad.uv1.x(), 1.0f - quad.uv1.y());
        Vec2f uv2(quad.uv2.x(), 1.0f - quad.uv2.y());
        Vec2f uv3(quad.uv3.x(), 1.0f - quad.uv3.y());

        verts.emplace_back(tform*quad.p0, uv0);
        verts.emplace_back(tform*quad.p1, uv1);
        verts.emplace_back(tform*quad.p2, uv2);
        verts.emplace_back(tform*quad.p0, uv0);
        verts.emplace_back(tform*quad.p2, uv2);
        verts.emplace_back(tform*quad.p3, uv3);

        tris.emplace_back(verts.size() - 6, verts.size() - 4, verts.size() - 5, material);
        tris.emplace_back(verts.size() - 3, verts.size() - 1, verts.size() - 2, material);
    }

    _modelToPrimitive.insert(std::make_pair(&model, _models.size()));

    _models.emplace_back(std::make_shared<TriangleMesh>(
        std::move(verts),
        std::move(tris),
        std::move(bsdfs),
        tfm::format("%s-%04d", model.modelPath(), _models.size()),
        false,
        true)
    );
}

void TraceableMinecraftMap::buildModels(ResourcePackLoader &pack)
{
    for (const BlockDescriptor &desc : pack.blockDescriptors())
        for (const BlockVariant &var : desc.variants())
            for (const ModelRef &model : var.models())
                buildModel(model);
}

void TraceableMinecraftMap::fromJson(const rapidjson::Value &v, const Scene &scene)
{
    Primitive::fromJson(v, scene);

    JsonUtils::fromJson(v, "map_path", _mapPath);
    JsonUtils::fromJson(v, "resource_path", _packPath);

    Bvh::PrimVector prims;

    _bounds = Box3f();

    ResourcePackLoader pack(_packPath);
    loadTextures(pack);
    buildModels(pack);

    MapLoader<ElementType> loader(_mapPath);
    loader.loadRegions([&](int x, int z, int height, ElementType *data, uint8 *biomes) {
        Box3f bounds(Vec3f(x*256.0f, 0.0f, z*256.0f), Vec3f((x + 1)*256.0f, float(height), (z + 1)*256.0f));
        Vec3f centroid((x + 0.5f)*256.0f, height*0.5f, (z + 0.5f)*256.0f);

        _bounds.grow(bounds);

        prims.emplace_back(bounds, centroid, _grids.size());

        for (int i = 0; i < 256*256*256; ++i) {
            const ModelRef *map = pack.mapBlock(data[i], i);
            if (map)
                data[i] = _modelToPrimitive[map] + 1;
            else
                data[i] = 0;
        }

        std::cout << "Building grid " << _grids.size() + 1 << std::endl;

        _grids.emplace_back(new HierarchicalGrid(bounds.min(), data));
    });

    for (auto &m : _models)
        m->prepareForRender();

    _chunkBvh.reset(new Bvh::BinaryBvh(std::move(prims), 1));
}

rapidjson::Value TraceableMinecraftMap::toJson(Allocator &allocator) const
{
    rapidjson::Value v = Primitive::toJson(allocator);
    v.AddMember("type", "minecraft_map", allocator);
    v.AddMember("map_path", _mapPath.c_str(), allocator);
    v.AddMember("resource_path", _packPath.c_str(), allocator);
    return std::move(v);
}

bool TraceableMinecraftMap::intersect(Ray &ray, IntersectionTemporary &data) const
{
    bool hit = false;
    _chunkBvh->trace(ray, [&](Ray &ray, uint32 id) {
        _grids[id]->trace(ray, [&](uint32 idx, const Vec3f &offset) {
            Vec3f oldPos = ray.pos();
            ray.setPos(oldPos - offset);

            hit = _models[idx]->intersect(ray, data);
            ray.setPos(oldPos);
            return hit;
        });
    });

    return hit;
}

bool TraceableMinecraftMap::occluded(const Ray &ray) const
{
    IntersectionTemporary data;
    Ray temp(ray);
    return intersect(temp, data);
}

bool TraceableMinecraftMap::hitBackside(const IntersectionTemporary &/*data*/) const
{
    return false;
}

void TraceableMinecraftMap::intersectionInfo(const IntersectionTemporary &/*data*/, IntersectionInfo &/*info*/) const
{
}

bool TraceableMinecraftMap::tangentSpace(const IntersectionTemporary &/*data*/, const IntersectionInfo &/*info*/,
        Vec3f &/*T*/, Vec3f &/*B*/) const
{
    return false;
}

bool TraceableMinecraftMap::isSamplable() const
{
    return false;
}

void TraceableMinecraftMap::makeSamplable()
{
}

float TraceableMinecraftMap::inboundPdf(const IntersectionTemporary &/*data*/, const IntersectionInfo &/*info*/,
        const Vec3f &/*p*/, const Vec3f &/*d*/) const
{
    return 0.0f;
}

bool TraceableMinecraftMap::sampleInboundDirection(LightSample &/*sample*/) const
{
    return false;
}

bool TraceableMinecraftMap::sampleOutboundDirection(LightSample &/*sample*/) const
{
    return false;
}

bool TraceableMinecraftMap::invertParametrization(Vec2f /*uv*/, Vec3f &/*pos*/) const
{
    return false;
}

bool TraceableMinecraftMap::isDelta() const
{
    return false;
}

bool TraceableMinecraftMap::isInfinite() const
{
    return false;
}

float TraceableMinecraftMap::approximateRadiance(const Vec3f &/*p*/) const
{
    return -1.0f;
}

Box3f TraceableMinecraftMap::bounds() const
{
    return Box3f(Vec3f(0.0f), Vec3f(256.0f));
}

const TriangleMesh &TraceableMinecraftMap::asTriangleMesh()
{
    if (!_proxy) {
        _proxy.reset(new TriangleMesh());
        _proxy->makeCube();
    }
    return *_proxy;
}

int TraceableMinecraftMap::numBsdfs() const
{
    return 1;
}

std::shared_ptr<Bsdf> &TraceableMinecraftMap::bsdf(int /*index*/)
{
    return _missingBsdf;
}

void TraceableMinecraftMap::prepareForRender()
{

}

void TraceableMinecraftMap::cleanupAfterRender()
{
    /*_grids.clear();
    _chunkBvh.reset();

    for (auto &m : _models)
        m->cleanupAfterRender();*/
}

Primitive *TraceableMinecraftMap::clone()
{
    return new TraceableMinecraftMap(*this);
}

}