#include "opengltilerenderer.h"

#include <QHash>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPoint>
#include <QSize>
#include <QTransform>
#include <QtDebug>

#include <cstddef>
#include <cstring>
#include <limits>

namespace Tiled {

namespace {

const int atlasSamplerCount = 16;
const int maximumAtlasPageSize = 4096;
const int atlasPadding = 1;
const int maximumShelfWaste = 32;
const int maximumClearedErrors = 16;
const quint8 unresolvedAtlasPage = 0xFF;
const float nearestTexelBias = -1.0f / 256.0f;

const char *tileVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 tilePosition;
layout(location = 1) in vec4 tileMatrix;
layout(location = 2) in vec4 tileAtlasRect;
layout(location = 3) in uint tilePage;
layout(location = 4) in float tileOpacity;
uniform mat3 sceneToClip;
uniform vec2 atlasTexelSize;
uniform float texelBias;
out vec2 texCoord;
flat out uint texPage;
flat out float texOpacity;
void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 imagePoint = corner * tileAtlasRect.zw;
    vec2 scenePoint = tilePosition + mat2(tileMatrix.xy, tileMatrix.zw) * imagePoint;
    gl_Position = vec4((sceneToClip * vec3(scenePoint, 1.0)).xy, 0.0, 1.0);
    texCoord = (tileAtlasRect.xy + imagePoint + vec2(texelBias)) * atlasTexelSize;
    texPage = tilePage;
    texOpacity = tileOpacity;
}
)";

const char *tileFragmentShader = R"(
#version 330 core
in vec2 texCoord;
flat in uint texPage;
flat in float texOpacity;
out vec4 fragColor;
uniform sampler2D pages[16];
vec4 pageTexel()
{
    switch (texPage) {
    case 0u: return textureLod(pages[0], texCoord, 0.0);
    case 1u: return textureLod(pages[1], texCoord, 0.0);
    case 2u: return textureLod(pages[2], texCoord, 0.0);
    case 3u: return textureLod(pages[3], texCoord, 0.0);
    case 4u: return textureLod(pages[4], texCoord, 0.0);
    case 5u: return textureLod(pages[5], texCoord, 0.0);
    case 6u: return textureLod(pages[6], texCoord, 0.0);
    case 7u: return textureLod(pages[7], texCoord, 0.0);
    case 8u: return textureLod(pages[8], texCoord, 0.0);
    case 9u: return textureLod(pages[9], texCoord, 0.0);
    case 10u: return textureLod(pages[10], texCoord, 0.0);
    case 11u: return textureLod(pages[11], texCoord, 0.0);
    case 12u: return textureLod(pages[12], texCoord, 0.0);
    case 13u: return textureLod(pages[13], texCoord, 0.0);
    case 14u: return textureLod(pages[14], texCoord, 0.0);
    case 15u: return textureLod(pages[15], texCoord, 0.0);
    }
    return vec4(0.0);
}
void main()
{
    fragColor = pageTexel() * texOpacity;
}
)";

quint8 opacityLevel(qreal opacity)
{
    return quint8(qRound(qBound<qreal>(0.0, opacity, 1.0) * 255.0));
}

void clearOpenGLErrors(QOpenGLFunctions_3_3_Core *functions)
{
    for (int attempt = 0; attempt < maximumClearedErrors; ++attempt) {
        if (functions->glGetError() == GL_NO_ERROR)
            return;
    }
}

QImage paddedAtlasImage(const QImage &image)
{
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const int width = source.width();
    const int height = source.height();
    QImage padded(width + 2 * atlasPadding, height + 2 * atlasPadding, source.format());
    for (int row = 0; row < height; ++row) {
        const quint32 *sourceLine = reinterpret_cast<const quint32 *>(source.constScanLine(row));
        quint32 *paddedLine = reinterpret_cast<quint32 *>(padded.scanLine(row + atlasPadding));
        std::memcpy(paddedLine + atlasPadding, sourceLine, size_t(width) * sizeof(quint32));
        for (int column = 0; column < atlasPadding; ++column) {
            paddedLine[column] = sourceLine[0];
            paddedLine[atlasPadding + width + column] = sourceLine[width - 1];
        }
    }
    const size_t lineBytes = size_t(padded.bytesPerLine());
    for (int row = 0; row < atlasPadding; ++row) {
        std::memcpy(padded.scanLine(row), padded.constScanLine(atlasPadding), lineBytes);
        std::memcpy(padded.scanLine(atlasPadding + height + row),
                    padded.constScanLine(atlasPadding + height - 1), lineBytes);
    }
    return padded;
}

QHash<QOpenGLContext *, OpenGLTileRenderer *> &renderersByContext()
{
    static QHash<QOpenGLContext *, OpenGLTileRenderer *> renderers;
    return renderers;
}

QHash<QOpenGLContextGroup *, OpenGLTileAtlas *> &atlasesByShareGroup()
{
    static QHash<QOpenGLContextGroup *, OpenGLTileAtlas *> atlases;
    return atlases;
}

} // namespace anonymous

class OpenGLTileAtlas
{
public:
    struct Placement
    {
        quint16 x;
        quint16 y;
        quint8 page;
    };

    explicit OpenGLTileAtlas(QOpenGLFunctions_3_3_Core *functions);

    quint64 generation() const { return mGeneration; }
    int pageSize() const { return mPageSize; }
    int pageCount() const { return mPages.size(); }
    GLuint pageTexture(int page) const { return mPages.at(page); }

    bool fits(const QSize &imageSize) const;
    const Placement *find(qint64 imageKey) const;
    bool insert(QOpenGLFunctions_3_3_Core *functions, const QImage &image, Placement &placement);
    void clear();
    void destroyTextures(QOpenGLFunctions_3_3_Core *functions);

    void addUser() { ++mUserCount; }
    bool removeUser() { return --mUserCount == 0; }

private:
    struct Shelf
    {
        int page;
        int x;
        int y;
        int height;
    };

    bool allocate(QOpenGLFunctions_3_3_Core *functions, const QSize &size, int &page, QPoint &position);
    int addShelf(QOpenGLFunctions_3_3_Core *functions, int height);
    bool addPage(QOpenGLFunctions_3_3_Core *functions);

    QVector<GLuint> mPages;
    QVector<int> mPageUsedHeights;
    QVector<Shelf> mShelves;
    QHash<qint64, Placement> mPlacements;
    quint64 mGeneration;
    int mPageSize;
    int mMaximumPageCount;
    int mUserCount;
};

OpenGLTileAtlas::OpenGLTileAtlas(QOpenGLFunctions_3_3_Core *functions)
    : mGeneration(1)
    , mPageSize(0)
    , mMaximumPageCount(0)
    , mUserCount(0)
{
    GLint maximumTextureSize = 0;
    GLint maximumTextureUnits = 0;
    functions->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    functions->glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maximumTextureUnits);
    mPageSize = qMin(maximumAtlasPageSize, int(maximumTextureSize));
    mMaximumPageCount = qMin(atlasSamplerCount, int(maximumTextureUnits));
}

bool OpenGLTileAtlas::fits(const QSize &imageSize) const
{
    return imageSize.width() + 2 * atlasPadding <= mPageSize
            && imageSize.height() + 2 * atlasPadding <= mPageSize;
}

const OpenGLTileAtlas::Placement *OpenGLTileAtlas::find(qint64 imageKey) const
{
    const auto placement = mPlacements.constFind(imageKey);
    return placement == mPlacements.constEnd() ? nullptr : &placement.value();
}

bool OpenGLTileAtlas::insert(QOpenGLFunctions_3_3_Core *functions, const QImage &image,
                             Placement &placement)
{
    const QImage padded = paddedAtlasImage(image);
    int page = 0;
    QPoint position;
    if (!allocate(functions, padded.size(), page, position))
        return false;

    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, mPages.at(page));
    functions->glTexSubImage2D(GL_TEXTURE_2D, 0, position.x(), position.y(),
                               padded.width(), padded.height(), GL_RGBA, GL_UNSIGNED_BYTE,
                               padded.constBits());

    placement.x = quint16(position.x() + atlasPadding);
    placement.y = quint16(position.y() + atlasPadding);
    placement.page = quint8(page);
    mPlacements.insert(image.cacheKey(), placement);
    return true;
}

void OpenGLTileAtlas::clear()
{
    mPlacements.clear();
    mShelves.clear();
    mPageUsedHeights.fill(0);
    ++mGeneration;
}

void OpenGLTileAtlas::destroyTextures(QOpenGLFunctions_3_3_Core *functions)
{
    if (!mPages.isEmpty())
        functions->glDeleteTextures(mPages.size(), mPages.constData());
    mPages.clear();
    mPageUsedHeights.clear();
    mShelves.clear();
    mPlacements.clear();
    ++mGeneration;
}

bool OpenGLTileAtlas::allocate(QOpenGLFunctions_3_3_Core *functions, const QSize &size,
                               int &page, QPoint &position)
{
    if (size.width() > mPageSize || size.height() > mPageSize)
        return false;

    int bestShelf = -1;
    int bestWaste = std::numeric_limits<int>::max();
    for (int shelfIndex = 0; shelfIndex < mShelves.size(); ++shelfIndex) {
        const Shelf &shelf = mShelves.at(shelfIndex);
        if (shelf.height < size.height() || shelf.x + size.width() > mPageSize)
            continue;
        const int waste = shelf.height - size.height();
        if (waste < bestWaste) {
            bestShelf = shelfIndex;
            bestWaste = waste;
        }
    }
    if (bestShelf < 0 || bestWaste > qMax(maximumShelfWaste, size.height() / 4)) {
        const int newShelf = addShelf(functions, size.height());
        if (newShelf >= 0)
            bestShelf = newShelf;
    }
    if (bestShelf < 0)
        return false;

    Shelf &shelf = mShelves[bestShelf];
    page = shelf.page;
    position = QPoint(shelf.x, shelf.y);
    shelf.x += size.width();
    return true;
}

int OpenGLTileAtlas::addShelf(QOpenGLFunctions_3_3_Core *functions, int height)
{
    for (int page = 0; page < mPages.size(); ++page) {
        if (mPageUsedHeights.at(page) + height <= mPageSize) {
            mShelves.append({page, 0, mPageUsedHeights.at(page), height});
            mPageUsedHeights[page] += height;
            return mShelves.size() - 1;
        }
    }
    if (!addPage(functions))
        return -1;
    mShelves.append({mPages.size() - 1, 0, 0, height});
    mPageUsedHeights.last() = height;
    return mShelves.size() - 1;
}

bool OpenGLTileAtlas::addPage(QOpenGLFunctions_3_3_Core *functions)
{
    if (mPages.size() >= mMaximumPageCount)
        return false;

    clearOpenGLErrors(functions);
    GLuint texture = 0;
    functions->glGenTextures(1, &texture);
    if (texture == 0)
        return false;
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, texture);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    functions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mPageSize, mPageSize, 0,
                            GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (functions->glGetError() != GL_NO_ERROR) {
        functions->glDeleteTextures(1, &texture);
        mMaximumPageCount = mPages.size();
        return false;
    }
    mPages.append(texture);
    mPageUsedHeights.append(0);
    return true;
}

OpenGLTileBatch::OpenGLTileBatch(const OpenGLTileAtlas *atlas, qreal opacity)
    : mAtlas(atlas)
    , mAtlasGeneration(atlas->generation())
    , mOpacity(opacityLevel(opacity))
    , mDrawable(true)
    , mHasUnresolvedTiles(false)
{
}

void OpenGLTileBatch::setOpacity(qreal opacity)
{
    mOpacity = opacityLevel(opacity);
}

void OpenGLTileBatch::drawTile(const QImage &image, const QTransform &transform)
{
    if (image.isNull())
        return;
    if (!mAtlas->fits(image.size())) {
        mDrawable = false;
        return;
    }

    Instance instance;
    instance.position[0] = float(transform.dx());
    instance.position[1] = float(transform.dy());
    instance.matrix[0] = qfloat16(float(transform.m11()));
    instance.matrix[1] = qfloat16(float(transform.m12()));
    instance.matrix[2] = qfloat16(float(transform.m21()));
    instance.matrix[3] = qfloat16(float(transform.m22()));
    instance.atlasRect[2] = quint16(image.width());
    instance.atlasRect[3] = quint16(image.height());
    instance.opacity = mOpacity;
    if (const OpenGLTileAtlas::Placement *placement = mAtlas->find(image.cacheKey())) {
        instance.atlasRect[0] = placement->x;
        instance.atlasRect[1] = placement->y;
        instance.page = placement->page;
    } else {
        instance.atlasRect[0] = 0;
        instance.atlasRect[1] = 0;
        instance.page = unresolvedAtlasPage;
        mHasUnresolvedTiles = true;
    }
    mInstances.append(instance);
    mImages.append(&image);
}

QSurfaceFormat OpenGLTileRenderer::surfaceFormat()
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    return format;
}

bool OpenGLTileRenderer::isAvailable()
{
    static const bool available = [] {
        QOpenGLContext *previousContext = QOpenGLContext::currentContext();
        QSurface *previousSurface = previousContext ? previousContext->surface() : nullptr;

        bool supported = false;
        QOpenGLContext context;
        context.setFormat(surfaceFormat());
        if (context.create() && !context.isOpenGLES()) {
            QOffscreenSurface surface;
            surface.setFormat(context.format());
            surface.create();
            if (surface.isValid() && context.makeCurrent(&surface)) {
                supported = context.format().version() >= qMakePair(3, 3);
                context.doneCurrent();
            }
        }
        if (previousContext != nullptr && previousSurface != nullptr)
            previousContext->makeCurrent(previousSurface);

        if (!supported)
            qInfo() << "OpenGL tile renderer: OpenGL 3.3 is unavailable, using the raster renderer";
        return supported;
    }();
    return available;
}

OpenGLTileRenderer *OpenGLTileRenderer::forCurrentContext()
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr || context->isOpenGLES()
            || context->format().version() < qMakePair(3, 3))
        return nullptr;

    QHash<QOpenGLContext *, OpenGLTileRenderer *> &renderers = renderersByContext();
    const auto existing = renderers.constFind(context);
    if (existing != renderers.constEnd())
        return existing.value()->mAtlas != nullptr ? existing.value() : nullptr;

    OpenGLTileRenderer *renderer = new OpenGLTileRenderer(context);
    renderers.insert(context, renderer);
    QObject::connect(context, &QOpenGLContext::aboutToBeDestroyed, context,
                     [context] { contextAboutToBeDestroyed(context); }, Qt::DirectConnection);
    if (!renderer->initialize()) {
        renderer->releaseResources(true);
        return nullptr;
    }
    return renderer;
}

OpenGLTileRenderer::OpenGLTileRenderer(QOpenGLContext *context)
    : mContext(context)
    , mShareGroup(context->shareGroup())
    , mAtlas(nullptr)
    , mVertexArray(0)
    , mInstanceBuffer(0)
    , mNearestSampler(0)
    , mLinearSampler(0)
    , mSceneToClipLocation(-1)
    , mAtlasTexelSizeLocation(-1)
    , mTexelBiasLocation(-1)
    , mBoundPageCount(0)
{
}

OpenGLTileRenderer::~OpenGLTileRenderer()
{
}

bool OpenGLTileRenderer::initialize()
{
    if (!initializeOpenGLFunctions()) {
        qWarning() << "OpenGL tile renderer: OpenGL 3.3 functions are unavailable"
                   << mContext->format();
        return false;
    }
    if (!mProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, tileVertexShader)
            || !mProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, tileFragmentShader)
            || !mProgram.link()) {
        qWarning() << "OpenGL tile renderer: failed to create the tile shader:" << mProgram.log();
        return false;
    }
    mSceneToClipLocation = mProgram.uniformLocation("sceneToClip");
    mAtlasTexelSizeLocation = mProgram.uniformLocation("atlasTexelSize");
    mTexelBiasLocation = mProgram.uniformLocation("texelBias");
    GLint samplerUnits[atlasSamplerCount];
    for (int unit = 0; unit < atlasSamplerCount; ++unit)
        samplerUnits[unit] = unit;
    mProgram.bind();
    mProgram.setUniformValueArray("pages", samplerUnits, atlasSamplerCount);
    mProgram.release();

    GLint previousVertexArray = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGenVertexArrays(1, &mVertexArray);
    glGenBuffers(1, &mInstanceBuffer);
    if (mVertexArray == 0 || mInstanceBuffer == 0) {
        qWarning() << "OpenGL tile renderer: failed to create the tile buffers";
        return false;
    }
    glBindVertexArray(mVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, mInstanceBuffer);
    for (GLuint attribute = 0; attribute < 5; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribDivisor(attribute, 1);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(GLuint(previousVertexArray));

    glGenSamplers(1, &mNearestSampler);
    glGenSamplers(1, &mLinearSampler);
    const GLuint samplers[] = {mNearestSampler, mLinearSampler};
    for (GLuint sampler : samplers) {
        const GLint filter = sampler == mLinearSampler ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    OpenGLTileAtlas *&atlas = atlasesByShareGroup()[mShareGroup];
    if (atlas == nullptr)
        atlas = new OpenGLTileAtlas(this);
    atlas->addUser();
    mAtlas = atlas;

    const GLubyte *vendor = glGetString(GL_VENDOR);
    const GLubyte *device = glGetString(GL_RENDERER);
    const GLubyte *version = glGetString(GL_VERSION);
    qInfo().noquote() << "OpenGL tile renderer:"
                      << (vendor ? QString::fromLatin1(reinterpret_cast<const char *>(vendor))
                                 : QStringLiteral("unknown vendor"))
                      << "|"
                      << (device ? QString::fromLatin1(reinterpret_cast<const char *>(device))
                                 : QStringLiteral("unknown renderer"))
                      << "| OpenGL"
                      << (version ? QString::fromLatin1(reinterpret_cast<const char *>(version))
                                  : QStringLiteral("unknown version"));
    return true;
}

void OpenGLTileRenderer::releaseResources(bool contextIsCurrent)
{
    if (contextIsCurrent) {
        if (mVertexArray != 0)
            glDeleteVertexArrays(1, &mVertexArray);
        if (mInstanceBuffer != 0)
            glDeleteBuffers(1, &mInstanceBuffer);
        if (mNearestSampler != 0)
            glDeleteSamplers(1, &mNearestSampler);
        if (mLinearSampler != 0)
            glDeleteSamplers(1, &mLinearSampler);
        mProgram.removeAllShaders();
    }
    mVertexArray = 0;
    mInstanceBuffer = 0;
    mNearestSampler = 0;
    mLinearSampler = 0;

    if (mAtlas != nullptr && mAtlas->removeUser()) {
        if (contextIsCurrent)
            mAtlas->destroyTextures(this);
        atlasesByShareGroup().remove(mShareGroup);
        delete mAtlas;
    }
    mAtlas = nullptr;
}

void OpenGLTileRenderer::contextAboutToBeDestroyed(QOpenGLContext *context)
{
    OpenGLTileRenderer *renderer = renderersByContext().take(context);
    if (renderer == nullptr)
        return;

    QOpenGLContext *previousContext = QOpenGLContext::currentContext();
    QSurface *previousSurface = previousContext ? previousContext->surface() : nullptr;
    QSurface *surface = context->surface();
    const bool contextIsCurrent = surface != nullptr && context->makeCurrent(surface)
            && QOpenGLContext::currentContext() == context;
    renderer->releaseResources(contextIsCurrent);
    delete renderer;
    if (previousContext != nullptr && previousContext != context && previousSurface != nullptr)
        previousContext->makeCurrent(previousSurface);
}

void OpenGLTileRenderer::draw(const QVector<OpenGLTileBatch *> &batches,
                              const QTransform &sceneToDevice, bool smoothTransform)
{
    using Instance = OpenGLTileBatch::Instance;

    int instanceCount = 0;
    for (const OpenGLTileBatch *batch : batches)
        instanceCount += batch->mInstances.size();
    if (instanceCount == 0)
        return;

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0)
        return;

    GLint previousVertexArray = 0;
    GLint previousUnpackAlignment = 0;
    GLint previousUnpackRowLength = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previousUnpackRowLength);

    const QTransform deviceToClip(2.0 / viewport[2], 0.0, 0.0, -2.0 / viewport[3], -1.0, 1.0);
    const QTransform sceneToClip = sceneToDevice * deviceToClip;
    const GLfloat sceneToClipMatrix[] = {
        GLfloat(sceneToClip.m11()), GLfloat(sceneToClip.m12()), GLfloat(sceneToClip.m13()),
        GLfloat(sceneToClip.m21()), GLfloat(sceneToClip.m22()), GLfloat(sceneToClip.m23()),
        GLfloat(sceneToClip.m31()), GLfloat(sceneToClip.m32()), GLfloat(sceneToClip.m33())
    };

    mProgram.bind();
    glUniformMatrix3fv(mSceneToClipLocation, 1, GL_FALSE, sceneToClipMatrix);
    glUniform2f(mAtlasTexelSizeLocation, 1.0f / mAtlas->pageSize(), 1.0f / mAtlas->pageSize());
    glUniform1f(mTexelBiasLocation, smoothTransform ? 0.0f : nearestTexelBias);
    glBindVertexArray(mVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, mInstanceBuffer);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(instanceCount) * GLsizeiptr(sizeof(Instance)),
                 nullptr, GL_STREAM_DRAW);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    mBoundPageCount = 0;

    int drawnInstances = 0;
    int writtenInstances = 0;
    for (OpenGLTileBatch *batch : batches) {
        Instance *instances = batch->mInstances.data();
        const int count = batch->mInstances.size();
        if (batch->mAtlasGeneration == mAtlas->generation() && !batch->mHasUnresolvedTiles) {
            uploadInstances(writtenInstances, instances, count);
            writtenInstances += count;
            continue;
        }

        int firstPending = 0;
        for (int index = 0; index < count; ++index) {
            Instance &instance = instances[index];
            if (batch->mAtlasGeneration == mAtlas->generation()
                    && instance.page != unresolvedAtlasPage)
                continue;
            const QImage &image = *batch->mImages.at(index);
            if (resolvePlacement(image, instance))
                continue;

            uploadInstances(writtenInstances, instances + firstPending, index - firstPending);
            writtenInstances += index - firstPending;
            firstPending = index;
            drawInstances(drawnInstances, writtenInstances - drawnInstances, smoothTransform);
            drawnInstances = writtenInstances;
            mAtlas->clear();
            if (!resolvePlacement(image, instance))
                instance.opacity = 0;
        }
        uploadInstances(writtenInstances, instances + firstPending, count - firstPending);
        writtenInstances += count - firstPending;
    }
    drawInstances(drawnInstances, writtenInstances - drawnInstances, smoothTransform);

    for (int unit = mBoundPageCount - 1; unit >= 0; --unit) {
        glActiveTexture(GLenum(GL_TEXTURE0 + unit));
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindSampler(GLuint(unit), 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(GLuint(previousVertexArray));
    glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, previousUnpackRowLength);
    mProgram.release();
}

bool OpenGLTileRenderer::resolvePlacement(const QImage &image, OpenGLTileBatch::Instance &instance)
{
    OpenGLTileAtlas::Placement placement;
    if (const OpenGLTileAtlas::Placement *existing = mAtlas->find(image.cacheKey()))
        placement = *existing;
    else if (!mAtlas->insert(this, image, placement))
        return false;
    instance.atlasRect[0] = placement.x;
    instance.atlasRect[1] = placement.y;
    instance.page = placement.page;
    return true;
}

void OpenGLTileRenderer::uploadInstances(int firstInstance,
                                         const OpenGLTileBatch::Instance *instances, int count)
{
    if (count <= 0)
        return;
    const GLsizeiptr instanceSize = GLsizeiptr(sizeof(OpenGLTileBatch::Instance));
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(firstInstance) * instanceSize,
                    GLsizeiptr(count) * instanceSize, instances);
}

void OpenGLTileRenderer::bindInstanceAttributes(int firstInstance)
{
    using Instance = OpenGLTileBatch::Instance;

    const GLsizei stride = GLsizei(sizeof(Instance));
    const size_t firstOffset = size_t(firstInstance) * sizeof(Instance);
    const auto fieldOffset = [firstOffset](size_t offset) {
        return reinterpret_cast<const void *>(firstOffset + offset);
    };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          fieldOffset(offsetof(Instance, position)));
    glVertexAttribPointer(1, 4, GL_HALF_FLOAT, GL_FALSE, stride,
                          fieldOffset(offsetof(Instance, matrix)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_SHORT, GL_FALSE, stride,
                          fieldOffset(offsetof(Instance, atlasRect)));
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, stride,
                           fieldOffset(offsetof(Instance, page)));
    glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                          fieldOffset(offsetof(Instance, opacity)));
}

void OpenGLTileRenderer::drawInstances(int firstInstance, int instanceCount, bool smoothTransform)
{
    if (instanceCount <= 0)
        return;
    const GLuint sampler = smoothTransform ? mLinearSampler : mNearestSampler;
    const int pageCount = mAtlas->pageCount();
    for (int page = 0; page < pageCount; ++page) {
        glActiveTexture(GLenum(GL_TEXTURE0 + page));
        glBindTexture(GL_TEXTURE_2D, mAtlas->pageTexture(page));
        glBindSampler(GLuint(page), sampler);
    }
    mBoundPageCount = qMax(mBoundPageCount, pageCount);
    bindInstanceAttributes(firstInstance);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instanceCount);
}

} // namespace Tiled
