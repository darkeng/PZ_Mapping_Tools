#ifndef OPENGLTILERENDERER_H
#define OPENGLTILERENDERER_H

#include "tiled_global.h"
#include "tiledrawtarget.h"

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <QVector>
#include <qfloat16.h>

class QOpenGLContext;
class QOpenGLContextGroup;
class QTransform;

namespace Tiled {

class OpenGLTileAtlas;

class OpenGLTileBatch : public TileDrawTarget
{
public:
    OpenGLTileBatch(const OpenGLTileAtlas *atlas, qreal opacity);

    void setOpacity(qreal opacity) override;
    void drawTile(const QImage &image, const QTransform &transform) override;

    bool isDrawable() const { return mDrawable; }

private:
    friend class OpenGLTileRenderer;

    struct Instance
    {
        float position[2];
        qfloat16 matrix[4];
        quint16 atlasRect[4];
        quint8 page;
        quint8 opacity;
    };

    const OpenGLTileAtlas *mAtlas;
    quint64 mAtlasGeneration;
    quint8 mOpacity;
    bool mDrawable;
    bool mHasUnresolvedTiles;
    QVector<Instance> mInstances;
    QVector<const QImage *> mImages;
};

class OpenGLTileRenderer : protected QOpenGLFunctions_3_3_Core
{
public:
    TILEDSHARED_EXPORT static QSurfaceFormat surfaceFormat();
    TILEDSHARED_EXPORT static bool isAvailable();
    static OpenGLTileRenderer *forCurrentContext();

    const OpenGLTileAtlas *atlas() const { return mAtlas; }

    void draw(const QVector<OpenGLTileBatch *> &batches, const QTransform &sceneToDevice,
              bool smoothTransform);

private:
    explicit OpenGLTileRenderer(QOpenGLContext *context);
    ~OpenGLTileRenderer();

    bool initialize();
    void releaseResources(bool contextIsCurrent);
    bool resolvePlacement(const QImage &image, OpenGLTileBatch::Instance &instance);
    void uploadInstances(int firstInstance, const OpenGLTileBatch::Instance *instances, int count);
    void bindInstanceAttributes(int firstInstance);
    void drawInstances(int firstInstance, int instanceCount, bool smoothTransform);

    static void contextAboutToBeDestroyed(QOpenGLContext *context);

    QOpenGLContext *mContext;
    QOpenGLContextGroup *mShareGroup;
    OpenGLTileAtlas *mAtlas;
    QOpenGLShaderProgram mProgram;
    GLuint mVertexArray;
    GLuint mInstanceBuffer;
    GLuint mNearestSampler;
    GLuint mLinearSampler;
    int mSceneToClipLocation;
    int mAtlasTexelSizeLocation;
    int mTexelBiasLocation;
    int mBoundPageCount;
};

} // namespace Tiled

#endif // OPENGLTILERENDERER_H
