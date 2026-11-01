/*
 * zlevelrenderer.cpp
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "zlevelrenderer.h"
#include "../../../shared/rastertilecache.h"

#include "customtilesize.h"
#include "map.h"
#include "mapobject.h"
#include "objectgroup.h"
#include "tile.h"
#include "tilelayer.h"
#include "tileset.h"
#include "imagelayer.h"
#include "ztilelayergroup.h"

#include <cmath>
#include <functional>

#include <QCoreApplication>
#include <QPaintEngine>
#include <QPainterPath>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

using namespace Tiled;

#define DISPLAY_TILE_WIDTH (map()->tileWidth() * (is2x() ? 2 : 1))
#define DISPLAY_TILE_HEIGHT (map()->tileHeight() * (is2x() ? 2 : 1))

namespace {
const int minimumParallelTilePositions = 12288;
const int minimumParallelBandHeight = 48;
const int bandsPerParallelThread = 2;

quint64 &frameRenderedTileCount()
{
    static thread_local quint64 count = 0;
    return count;
}
bool &renderedTileCountingEnabled()
{
    static thread_local bool enabled = false;
    return enabled;
}

class TileBandRenderTask : public QRunnable
{
public:
    explicit TileBandRenderTask(std::function<void()> work)
        : mWork(std::move(work))
    {
    }

    void run() override
    {
        mWork();
    }

private:
    std::function<void()> mWork;
};

QThreadPool *tileBandThreadPool()
{
    static QThreadPool *threadPool = [] {
        QThreadPool *pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(qMax(1, QThread::idealThreadCount()));
        return pool;
    }();
    return threadPool;
}

Tile *loadPlaceholderTile(const QString &tilesetName, const QString &imageFileName)
{
    Tileset *tileset = new Tileset(tilesetName, 64, 128);
    if (tileset->loadFromImage(QImage(imageFileName), imageFileName))
        return tileset->tileAt(0);
    delete tileset;
    return nullptr;
}

Tile *invisiblePlaceholderTile()
{
    static Tile *tile = loadPlaceholderTile(QStringLiteral("INVISIBLE"),
                                            QStringLiteral(":/images/invisible-tile.svg"));
    return tile;
}

Tile *missingPlaceholderTile()
{
    static Tile *tile = loadPlaceholderTile(QStringLiteral("MISSING"),
                                            QStringLiteral(":/images/missing-tile.svg"));
    return tile;
}

struct JUMBO
{
    QString tilesetName;
    bool bHasLeaves;
};

static JUMBO s_jumbo[] = {
    { QStringLiteral("e_americalholly"), false },
    { QStringLiteral("e_americanlinden"), true },
    { QStringLiteral("e_canadianhemlock"), false },
    { QStringLiteral("e_carolinasilverbell"), true },
    { QStringLiteral("e_cockspurhawthorn"), true },
    { QStringLiteral("e_dogwood"), true },
    { QStringLiteral("e_easternredbud"), false },
    { QStringLiteral("e_redmaple"), true },
    { QStringLiteral("e_riverbirch"), true },
    { QStringLiteral("e_virginiapine"), false },
    { QStringLiteral("e_yellowwood"), true },
};
} // namespace anonymous

void ZLevelRenderer::resetRenderedTileCount()
{
    frameRenderedTileCount() = 0;
}
void ZLevelRenderer::setRenderedTileCountingEnabled(bool enabled)
{
    renderedTileCountingEnabled() = enabled;
}
void ZLevelRenderer::addRenderedTileCount(quint64 count)
{
    frameRenderedTileCount() += count;
}
quint64 ZLevelRenderer::renderedTileCount()
{
    return frameRenderedTileCount();
}
QSize ZLevelRenderer::mapSize() const
{
    // Map width and height contribute equally in both directions
    const int side = map()->height() + map()->width();
    return QSize(side * DISPLAY_TILE_WIDTH / 2
                 + maxLevel() * map()->cellsPerLevel().x() * DISPLAY_TILE_WIDTH,
                 side * DISPLAY_TILE_HEIGHT / 2
                 + maxLevel() * map()->cellsPerLevel().y() * DISPLAY_TILE_HEIGHT);
}

QRect ZLevelRenderer::boundingRect(const QRect &rect, int level) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    const int originX = map()->height() * tileWidth / 2;
    const QPoint pos((rect.x() - (rect.y() + rect.height()))
                     * tileWidth / 2 + originX,
                     (rect.x() + rect.y()) * tileHeight / 2
                     + (maxLevel() - level) * map()->cellsPerLevel().y() * tileHeight);

    const int side = rect.height() + rect.width();
    const QSize size(side * tileWidth / 2,
                     side * tileHeight / 2);

   return QRect(pos, size);
}

QRectF ZLevelRenderer::boundingRect(const MapObject *object) const
{
    Layer *layer = object->objectGroup();
    int level = layer ? layer->level() : 0;
    if (object->tile()) {
        const QPointF bottomCenter = tileToPixelCoords(object->position(), level);
        const QImage &img = object->tile()->image();
        return QRectF(bottomCenter.x() - img.width() / 2,
                      bottomCenter.y() - img.height(),
                      img.width(),
                      img.height()).adjusted(-1, -1, 1, 1);
    } else if (!object->polygon().isEmpty()) {
        const QPointF &pos = object->position();
        const QPolygonF polygon = object->polygon().translated(pos);
        const QPolygonF screenPolygon = tileToPixelCoords(polygon, level);
        return screenPolygon.boundingRect().adjusted(-2, -2, 3, 3);
    } else {
        // Take the bounding rect of the projected object, and then add a few
        // pixels on all sides to correct for the line width.
        const QRectF base = tileRectToPolygon(object->bounds(), level).boundingRect();
        return base.adjusted(-2, -3, 2, 2);
    }
}

QPainterPath ZLevelRenderer::shape(const MapObject *object) const
{
    Layer *layer = object->objectGroup();
    int level = layer ? layer->level() : 0;
    /*
AddRemoveMapObject::removeObject
    mObjectGroup->removeObject(mMapObject) <<--------- why layer==0
    mMapDocument->emitObjectRemoved(mMapObject);
        MapDocument::deselectObjects
            emit selectedObjectsChanged()
                MapScene::updateSelectedObjectItems()
                    item->setEditable(...);
                    unsetCursor()
                        MapObjectItem::shape()
                            <this function>
    */
    if (layer == 0)
        return QPainterPath();

    QPainterPath path;
    if (object->tile()) {
        path.addRect(boundingRect(object));
    } else {
        switch (object->shape()) {
        case MapObject::Rectangle:
            path.addPolygon(tileRectToPolygon(object->bounds(), level));
            break;
        case MapObject::Polygon:
        case MapObject::Polyline: {
            const QPointF &pos = object->position();
            const QPolygonF polygon = object->polygon().translated(pos);
            const QPolygonF screenPolygon = tileToPixelCoords(polygon, level);
            if (object->shape() == MapObject::Polygon) {
                path.addPolygon(screenPolygon);
            } else {
                for (int i = 1; i < screenPolygon.size(); ++i) {
                    path.addPolygon(lineToPolygon(screenPolygon[i - 1],
                                                  screenPolygon[i]));
                }
                path.setFillRule(Qt::WindingFill);
            }
            break;
        }
        }
    }
    return path;
}

void ZLevelRenderer::drawGrid(QPainter *painter, const QRectF &rect,
                              QColor gridColor, int level,
                              const QRect &tileBounds) const
{
    QRect b = tileBounds;
    if (b.isEmpty())
        b = QRect(QPoint(0, 0), map()->size());

    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    QRect r = rect.toAlignedRect();
    r.adjust(-tileWidth / 2, -tileHeight / 2,
             tileWidth / 2, tileHeight / 2);

    const int startX = qMax(qreal(b.left()), pixelToTileCoords(r.topLeft(), level).x());
    const int startY = qMax(qreal(b.top()), pixelToTileCoords(r.topRight(), level).y());
    const int endX = qMin(qreal(b.right() + 1),
                          pixelToTileCoords(r.bottomRight(), level).x());
    const int endY = qMin(qreal(b.bottom() + 1),
                          pixelToTileCoords(r.bottomLeft(), level).y());

    gridColor.setAlpha(128);

    QPen pen;
    pen.setCosmetic(true);
    qreal scaleX = painter->transform().m11();
    pen.setWidthF(std::max(scaleX, 1.0));
    QBrush brush(gridColor, Qt::Dense4Pattern);
    brush.setTransform(QTransform::fromScale(1/painter->transform().m11(),
                                             1/painter->transform().m22()));
    pen.setBrush(brush);
    painter->setPen(pen);

    for (int y = startY; y <= endY; ++y) {
        const QPointF start = tileToPixelCoords(startX, (qreal)y, level);
        const QPointF end = tileToPixelCoords(endX, (qreal)y, level);
        painter->drawLine(start, end);
    }
    for (int x = startX; x <= endX; ++x) {
        const QPointF start = tileToPixelCoords(x, (qreal)startY, level);
        const QPointF end = tileToPixelCoords(x, (qreal)endY, level);
        painter->drawLine(start, end);
    }
}

void ZLevelRenderer::drawTileLayer(QPainter *painter,
                                      const TileLayer *layer,
                                      const QRectF &exposed) const
{
    int level = layer->level();

    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    if (tileWidth <= 0 || tileHeight <= 1)
        return;

    QRect rect = exposed.toAlignedRect();
    if (rect.isNull())
        rect = boundingRect(layer->bounds(), level);

    QMargins drawMargins = layer->drawMargins() * (is2x() ? 2 : 1);
    drawMargins.setTop(drawMargins.top() - tileHeight);
    drawMargins.setRight(drawMargins.right() - tileWidth);

    rect.adjust(-drawMargins.right(),
                -drawMargins.bottom(),
                drawMargins.left(),
                drawMargins.top());

    // Determine the tile and pixel coordinates to start at
    QPointF tilePos = pixelToTileCoords(rect.x(), rect.y(), level);
    QPoint rowItr = QPoint((int) std::floor(tilePos.x()),
                           (int) std::floor(tilePos.y()));
    QPointF startPos = tileToPixelCoords(rowItr, level);

    startPos.rx() -= tileWidth / 2;
    startPos.ry() += tileHeight;

    // Compensate for the layer position
    rowItr -= QPoint(layer->x(), layer->y());

    /* Determine in which half of the tile the top-left corner of the area we
     * need to draw is. If we're in the upper half, we need to start one row
     * up due to those tiles being visible as well. How we go up one row
     * depends on whether we're in the left or right half of the tile.
     */
    const bool inUpperHalf = startPos.y() - rect.y() > tileHeight / 2;
    const bool inLeftHalf = rect.x() - startPos.x() < tileWidth / 2;

    if (inUpperHalf) {
        if (inLeftHalf) {
            --rowItr.rx();
            startPos.rx() -= tileWidth / 2;
        } else {
            --rowItr.ry();
            startPos.rx() += tileWidth / 2;
        }
        startPos.ry() -= tileHeight / 2;
    }

    // Determine whether the current row is shifted half a tile to the right
    bool shifted = inUpperHalf ^ inLeftHalf;

    QTransform baseTransform = painter->transform();

    for (int y = startPos.y(); y - tileHeight < rect.bottom();
         y += tileHeight / 2)
    {
        QPoint columnItr = rowItr;

        for (int x = startPos.x(); x < rect.right(); x += tileWidth) {
            // Multi-threading
            if (mAbortDrawing && *mAbortDrawing) {
                painter->setTransform(baseTransform);
                return;
            }
            if (layer->contains(columnItr)) {
                const Cell &cell = layer->cellAt(columnItr);
                if (!cell.isEmpty()) {
                    QImage img = cell.tile->image();
                    const QPoint offset = cell.tile->tileset()->tileOffset() + cell.tile->offset();

                    qreal m11 = 1;      // Horizontal scaling factor
                    qreal m12 = 0;      // Vertical shearing factor
                    qreal m21 = 0;      // Horizontal shearing factor
                    qreal m22 = 1;      // Vertical scaling factor
                    qreal dx = offset.x() + x;
                    qreal dy = offset.y() + y - cell.tile->height();

                    if (cell.flippedAntiDiagonally) {
                        // Use shearing to swap the X/Y axis
                        m11 = 0;
                        m12 = 1;
                        m21 = 1;
                        m22 = 0;

                        // Compensate for the swap of image dimensions
                        dy += img.height() - img.width();
                    }
                    if (cell.flippedHorizontally) {
                        m11 = -m11;
                        m21 = -m21;
                        dx += cell.flippedAntiDiagonally ? img.height()
                                                         : img.width();
                    }
                    if (cell.flippedVertically) {
                        m12 = -m12;
                        m22 = -m22;
                        dy += cell.flippedAntiDiagonally ? img.width()
                                                         : img.height();
                    }

                    QString tilesetName = cell.tile->tileset()->name();
                    QSize customSize = CustomTileSize::forTileset(tilesetName);
                    bool bJUMBO = !customSize.isEmpty();
                    if (bJUMBO) {
                        dx -= (customSize.width() - 64) / 2; // tileWidth / 2;
                    } else if (tileWidth == cell.tile->width() * 2) {
                        m11 *= 2.0f;
                        m22 *= 2.0f;
                        dx += cell.tile->offset().x();
                        dy -= cell.tile->height() - cell.tile->offset().y();
                    } else if (tileWidth == cell.tile->width() / 2) {
                        float scale = 0.5f;
                        m11 *= scale;
                        m22 *= scale;
                        dy += cell.tile->height() / 2;
                    }

                    if (bJUMBO && !tilesetName.contains(QStringLiteral("JUMBOXL")) && !tilesetName.contains(QStringLiteral("JUMBOXXL"))) {
                        drawJumboTreeTile_Trunk(cell.tile, painter, baseTransform, x, y, m11, m12, m21, m22);
                    }

                    const QTransform transform(m11, m12, m21, m22, dx, dy);
                    painter->setTransform(transform * baseTransform);

                    if (!PZTools::drawCachedRasterTile(painter, img))
                        painter->drawImage(0, 0, img);

                    if (bJUMBO && !tilesetName.contains(QStringLiteral("JUMBOXL")) && !tilesetName.contains(QStringLiteral("JUMBOXXL"))) {
                        drawJumboTreeTile_Leaves(cell.tile, painter, baseTransform, x, y, m11, m12, m21, m22);
                    }
                }
            }

            // Advance to the next column
            ++columnItr.rx();
            --columnItr.ry();
        }

        // Advance to the next row
        if (!shifted) {
            ++rowItr.rx();
            startPos.rx() += tileWidth / 2;
            shifted = true;
        } else {
            ++rowItr.ry();
            startPos.rx() -= tileWidth / 2;
            shifted = false;
        }
    }

    painter->setTransform(baseTransform);
}

#ifdef ZOMBOID
void ZLevelRenderer::drawTileLayerGroup(QPainter *painter, ZTileLayerGroup *layerGroup,
                            const QRectF &exposed, ZTileLayerGroupRenderData *renderData) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    if (tileWidth <= 0 || tileHeight <= 1 || layerGroup->bounds().isEmpty())
        return;

    const QRect rect = tileLayerGroupDrawRect(layerGroup, exposed);
    layerGroup->prepareDrawing(this, rect);

    if (drawTileLayerGroupInParallel(painter, layerGroup, exposed, renderData))
        return;
    drawTileLayerGroupRect(painter, layerGroup, rect, renderData);
}

QRect ZLevelRenderer::tileLayerGroupDrawRect(const ZTileLayerGroup *layerGroup,
                                             const QRectF &exposed) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    QRect rect = exposed.toAlignedRect();
    if (rect.isNull())
        rect = layerGroup->boundingRect(this).toAlignedRect();

    QMargins drawMargins = layerGroup->drawMargins() * (is2x() ? 2 : 1);
    drawMargins.setTop(drawMargins.top() - tileHeight);
    drawMargins.setRight(drawMargins.right() - tileWidth);

    rect.adjust(-drawMargins.right(),
                -drawMargins.bottom(),
                drawMargins.left(),
                drawMargins.top());
    return rect;
}

bool ZLevelRenderer::drawTileLayerGroupInParallel(QPainter *painter, ZTileLayerGroup *layerGroup,
                                                  const QRectF &exposed,
                                                  ZTileLayerGroupRenderData *renderData) const
{
    if (mAbortDrawing != nullptr)
        return false;
    const QCoreApplication *application = QCoreApplication::instance();
    if (application == nullptr || QThread::currentThread() != application->thread())
        return false;
    QPaintEngine *paintEngine = painter->paintEngine();
    if (paintEngine == nullptr || paintEngine->type() != QPaintEngine::Raster
            || painter->compositionMode() != QPainter::CompositionMode_SourceOver)
        return false;
    const QPaintDevice *targetDevice = paintEngine->paintDevice();
    if (targetDevice == nullptr || targetDevice->devType() != QInternal::Image)
        return false;
    const QImage *target = static_cast<const QImage*>(targetDevice);

    const QTransform deviceTransform = painter->deviceTransform();
    if (deviceTransform.type() > QTransform::TxScale
            || deviceTransform.m11() <= 0 || deviceTransform.m22() <= 0)
        return false;

    const QRectF sceneRect = exposed.isNull() ? layerGroup->boundingRect(this) : exposed;
    const qreal tilePositionArea = DISPLAY_TILE_WIDTH * DISPLAY_TILE_HEIGHT / 2.0;
    if (sceneRect.width() * sceneRect.height() < minimumParallelTilePositions * tilePositionArea)
        return false;

    const QTransform baseDeviceTransform = painter->worldTransform().inverted() * deviceTransform;
    QRect deviceRect = deviceTransform.mapRect(sceneRect).toAlignedRect() & target->rect();
    if (painter->hasClipping())
        deviceRect &= deviceTransform.mapRect(painter->clipBoundingRect()).toAlignedRect();

    QThreadPool *threadPool = tileBandThreadPool();
    const int bandCount = qMin(threadPool->maxThreadCount() * bandsPerParallelThread,
                               deviceRect.height() / minimumParallelBandHeight);
    if (bandCount < 2)
        return false;

    struct TileBand
    {
        QRect deviceRect;
        QImage image;
        ZTileLayerGroupRenderData *renderData = nullptr;
        quint64 drawnTiles = 0;
    };
    QVector<TileBand> bands(bandCount);
    for (int bandIndex = 0; bandIndex < bandCount; ++bandIndex) {
        TileBand &band = bands[bandIndex];
        const int top = deviceRect.top() + deviceRect.height() * bandIndex / bandCount;
        const int bottom = deviceRect.top() + deviceRect.height() * (bandIndex + 1) / bandCount;
        band.deviceRect = QRect(deviceRect.left(), top, deviceRect.width(), bottom - top);
        if (renderData != nullptr) {
            band.renderData = layerGroup->createRenderData();
            if (band.renderData == nullptr) {
                for (const TileBand &createdBand : qAsConst(bands))
                    layerGroup->destroyRenderData(createdBand.renderData);
                return false;
            }
        }
    }

    invisiblePlaceholderTile();
    missingPlaceholderTile();

    const QTransform deviceToScene = deviceTransform.inverted();
    const QPainter::RenderHints renderHints = painter->renderHints();
    const qreal opacity = painter->opacity();
    auto drawBand = [&](TileBand &band) {
        band.image = target->copy(band.deviceRect);
        band.image.setDevicePixelRatio(1.0);
        QPainter bandPainter(&band.image);
        bandPainter.setRenderHints(renderHints);
        bandPainter.setOpacity(opacity);
        bandPainter.setTransform(deviceTransform
                                 * QTransform::fromTranslate(-band.deviceRect.x(),
                                                             -band.deviceRect.y()));
        const QRectF bandSceneRect = deviceToScene.mapRect(QRectF(band.deviceRect));
        band.drawnTiles = drawTileLayerGroupRect(&bandPainter, layerGroup,
                                                 tileLayerGroupDrawRect(layerGroup, bandSceneRect),
                                                 band.renderData);
    };
    for (TileBand &band : bands)
        threadPool->start(new TileBandRenderTask([&drawBand, &band] { drawBand(band); }));
    threadPool->waitForDone();

    quint64 drawnTiles = 0;
    painter->save();
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->setOpacity(1.0);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->setWorldTransform(baseDeviceTransform.inverted());
    for (const TileBand &band : qAsConst(bands)) {
        if (band.drawnTiles > 0)
            painter->drawImage(band.deviceRect.topLeft(), band.image);
        drawnTiles += band.drawnTiles;
        layerGroup->destroyRenderData(band.renderData);
    }
    painter->restore();

    if (renderedTileCountingEnabled())
        addRenderedTileCount(drawnTiles);
    return true;
}

quint64 ZLevelRenderer::drawTileLayerGroupRect(QPainter *painter, ZTileLayerGroup *layerGroup,
                                               const QRect &rect,
                                               ZTileLayerGroupRenderData *renderData) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    int level = layerGroup->level();

    // Determine the tile and pixel coordinates to start at
    QPointF tilePos = pixelToTileCoords(rect.x(), rect.y(), level);
    QPoint rowItr = QPoint((int) std::floor(tilePos.x()),
                           (int) std::floor(tilePos.y()));
    QPointF startPos = tileToPixelCoords(rowItr, level);
    startPos.rx() -= tileWidth / 2;
    startPos.ry() += tileHeight;

    /* Determine in which half of the tile the top-left corner of the area we
     * need to draw is. If we're in the upper half, we need to start one row
     * up due to those tiles being visible as well. How we go up one row
     * depends on whether we're in the left or right half of the tile.
     */
    const bool inUpperHalf = startPos.y() - rect.y() > tileHeight / 2;
    const bool inLeftHalf = rect.x() - startPos.x() < tileWidth / 2;

    if (inUpperHalf) {
        if (inLeftHalf) {
            --rowItr.rx();
            startPos.rx() -= tileWidth / 2;
        } else {
            --rowItr.ry();
            startPos.rx() += tileWidth / 2;
        }
        startPos.ry() -= tileHeight / 2;
    }

    // Determine whether the current row is shifted half a tile to the right
    bool shifted = inUpperHalf ^ inLeftHalf;

    QTransform baseTransform = painter->transform();

    /*static*/ QVector<const Cell*> cells(40); // or QVarLengthArray
    /*static*/ QVector<qreal> opacities(40); // or QVarLengthArray

    qreal opacity = painter->opacity();
    const bool countRenderedTiles = renderedTileCountingEnabled();
    quint64 drawnTiles = 0;

    for (int y = startPos.y(); y - tileHeight < rect.bottom();
         y += tileHeight / 2)
    {
        QPoint columnItr = rowItr;

        for (int x = startPos.x(); x < rect.right(); x += tileWidth) {
            cells.resize(0);
            if (layerGroup->orderedCellsAt(columnItr, cells, opacities, renderData)) {
                for (int i = 0; i < cells.size(); i++) {
                    // Multi-threading
                    if (mAbortDrawing && *mAbortDrawing) {
                        painter->setTransform(baseTransform);
                        return drawnTiles;
                    }
                    const Cell *cell = cells[i];
                    if (!cell->isEmpty()) {
                        Tile *tile = cell->tile;
                        const bool transparentTile = tile->image().isNull()
                                && tile->hasResolvedSource();
                        if (tile->properties().contains(QLatin1String("invisible"))
                                || transparentTile) {
                            if (isShowInvisibleTiles() == false)
                                continue;
                            if (Tile *invisibleTile = invisiblePlaceholderTile())
                                tile = invisibleTile;

                        }
                        if (tile->image().isNull() && !tile->hasResolvedSource()) {
                            if (Tile *missingTile = missingPlaceholderTile())
                                tile = missingTile;
                        }
                        const QImage &img = layerGroup->useImageBlack(columnItr.x(), columnItr.y()) ? tile->imageBlack() : tile->image();
                        const QPoint offset = tile->tileset()->tileOffset() + tile->offset();

                        qreal m11 = 1;      // Horizontal scaling factor
                        qreal m12 = 0;      // Vertical shearing factor
                        qreal m21 = 0;      // Horizontal shearing factor
                        qreal m22 = 1;      // Vertical scaling factor
                        qreal dx = offset.x() + x;
                        qreal dy = offset.y() + y - tile->height();

                        if (cell->flippedAntiDiagonally) {
                            // Use shearing to swap the X/Y axis
                            m11 = 0;
                            m12 = 1;
                            m21 = 1;
                            m22 = 0;

                            // Compensate for the swap of image dimensions
                            dy += img.height() - img.width();
                        }
                        if (cell->flippedHorizontally) {
                            m11 = -m11;
                            m21 = -m21;
                            dx += cell->flippedAntiDiagonally ? img.height()
                                                             : img.width();
                        }
                        if (cell->flippedVertically) {
                            m12 = -m12;
                            m22 = -m22;
                            dy += cell->flippedAntiDiagonally ? img.width()
                                                             : img.height();
                        }

                        const QString &tilesetName = cell->tile->tileset()->name();
                        QSize customSize = CustomTileSize::forTileset(tilesetName);
                        bool bJUMBO = !customSize.isEmpty();
                        if (bJUMBO) {
                            dx -= (customSize.width() - 64) / 2; // tileWidth / 2; // FIXME: Shouldn't Tiled::setZomboidTileOffset() take care of this? Possibly a TileScale=2 issue.
                        } else if (tileWidth == tile->width() * 2) {
                            m11 *= 2.0f;
                            m22 *= 2.0f;
                            dx += tile->offset().x();
                            dy -= tile->height() - tile->offset().y();
                        } else if (tileWidth == tile->width() / 2) {
                            float scale = 0.5f;
                            m11 *= scale;
                            m22 *= scale;
//                            dx += (tileWidth - img.width() * scale) / 2;
//                            dy += (tile->tileset()->tileHeight() - img.height() * scale);
//                            dy -= (tileHeight - tileHeight * scale) / 2;
                            dy += tile->height() / 2;
                        }

                        if (bJUMBO && !tilesetName.contains(QStringLiteral("JUMBOXL")) && !tilesetName.contains(QStringLiteral("JUMBOXXL"))) {
                            drawJumboTreeTile_Trunk(cell->tile, painter, baseTransform, x, y, m11, m12, m21, m22);
                        }

                        const QTransform transform(m11, m12, m21, m22, dx, dy);
                        painter->setTransform(transform * baseTransform);

                        painter->setOpacity(opacities[i] * opacity);

                        if (!PZTools::drawCachedRasterTile(painter, img))
                            painter->drawImage(0, 0, img);
                        ++drawnTiles;
                        if (countRenderedTiles)
                            addRenderedTileCount();

                        if (bJUMBO && !tilesetName.contains(QStringLiteral("JUMBOXL")) && !tilesetName.contains(QStringLiteral("JUMBOXXL"))) {
                            drawJumboTreeTile_Leaves(cell->tile, painter, baseTransform, x, y, m11, m12, m21, m22);
                        }
                    }
                }
            }

            // Advance to the next column
            ++columnItr.rx();
            --columnItr.ry();
        }

        // Advance to the next row
        if (!shifted) {
            ++rowItr.rx();
            startPos.rx() += tileWidth / 2;
            shifted = true;
        } else {
            ++rowItr.ry();
            startPos.rx() -= tileWidth / 2;
            shifted = false;
        }
    }

    painter->setTransform(baseTransform);
    return drawnTiles;
}
#endif // ZOMBOID

void ZLevelRenderer::drawTileSelection(QPainter *painter,
                                          const QRegion &region,
                                          const QColor &color,
                                          const QRectF &exposed,
                                          int level) const
{
    painter->setBrush(color);
    painter->setPen(Qt::NoPen);
    for (const QRect &r : region) {
        QPolygonF polygon = tileRectToPolygon(r, level);
        if (QRectF(polygon.boundingRect()).intersects(exposed))
            painter->drawConvexPolygon(polygon);
    }
}

void ZLevelRenderer::drawMapObject(QPainter *painter,
                                      const MapObject *object,
                                      const QColor &color) const
{
    Layer *layer = object->objectGroup();
    int level = layer->level();

    painter->save();

    QPen pen(Qt::black);

    if (object->tile()) {
        const QImage &img = object->tile()->image();
        QPointF paintOrigin(-img.width() / 2, -img.height());
        paintOrigin += tileToPixelCoords(object->position(), level).toPoint();
        painter->drawImage(paintOrigin, img);

        pen.setStyle(Qt::SolidLine);
        painter->setPen(pen);
        painter->drawRect(QRectF(paintOrigin, img.size()));
        pen.setStyle(Qt::DotLine);
        pen.setColor(color);
        painter->setPen(pen);
        painter->drawRect(QRectF(paintOrigin, img.size()));
    } else {
        QColor brushColor = color;
        if (color.alpha() != 255)
            brushColor.setAlpha(color.alpha());
        else
        brushColor.setAlpha(50);
        QBrush brush(brushColor);

        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        pen.setWidth(2);

        painter->setPen(pen);
        painter->setRenderHint(QPainter::Antialiasing);

        // TODO: Draw the object name
        // TODO: Do something sensible to make null-sized objects usable

        switch (object->shape()) {
        case MapObject::Rectangle: {
            QPolygonF polygon = tileRectToPolygon(object->bounds(), level);
            painter->drawPolygon(polygon);

            pen.setColor(color);
            painter->setPen(pen);
            painter->setBrush(brush);
            polygon.translate(0, -1);

            painter->drawPolygon(polygon);
            break;
        }
        case MapObject::Polygon: {
            const QPointF &pos = object->position();
            const QPolygonF polygon = object->polygon().translated(pos);
            QPolygonF screenPolygon = tileToPixelCoords(polygon, level);

            painter->drawPolygon(screenPolygon);

            pen.setColor(color);
            painter->setPen(pen);
            painter->setBrush(brush);
            screenPolygon.translate(0, -1);

            painter->drawPolygon(screenPolygon);
            break;
        }
        case MapObject::Polyline: {
            const QPointF &pos = object->position();
            const QPolygonF polygon = object->polygon().translated(pos);
            QPolygonF screenPolygon = tileToPixelCoords(polygon, level);

            painter->drawPolyline(screenPolygon);

            pen.setColor(color);
            painter->setPen(pen);
            screenPolygon.translate(0, -1);

            painter->drawPolyline(screenPolygon);
            break;
        }
        }
    }

    painter->restore();
}

void ZLevelRenderer::drawFancyRectangle(QPainter *painter,
                                        const QRectF &tileBounds,
                                        const QColor &color,
                                        int level) const
{
    painter->save();

    QPen pen(Qt::black);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    pen.setWidth(2);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->setRenderHint(QPainter::Antialiasing);
    QPolygonF polygon = tileRectToPolygon(tileBounds, level);
    painter->drawPolygon(polygon);

    pen.setColor(color);
    painter->setPen(pen);
    QColor brushColor = color;
    brushColor.setAlpha(50);
    QBrush brush(brushColor);
    painter->setBrush(brush);
    polygon.translate(0, -1);
    painter->drawPolygon(polygon);

    painter->restore();
}

void ZLevelRenderer::drawImageLayer(QPainter *painter,
                                       const ImageLayer *imageLayer,
                                       const QRectF &exposed) const
{
    Q_UNUSED(exposed)

    const QPixmap &img = imageLayer->image();
    QPointF paintOrigin(-img.width() / 2, -img.height());

    paintOrigin += tileToPixelCoords(imageLayer->x(), (qreal)imageLayer->y());

    painter->drawPixmap(paintOrigin, img);
}

QPointF ZLevelRenderer::pixelToTileCoords(qreal x, qreal y, int level) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;
    const qreal ratio = (qreal) tileWidth / tileHeight;

    x -= map()->height() * tileWidth / 2;
#ifdef ZOMBOID
    y -= map()->cellsPerLevel().y() * (maxLevel() - level) * tileHeight;
#endif
    const qreal mx = y + (x / ratio);
    const qreal my = y - (x / ratio);

    return QPointF(mx / tileHeight,
                   my / tileHeight);
}

QPointF ZLevelRenderer::tileToPixelCoords(qreal x, qreal y, int level) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;
#ifdef ZOMBOID
    const int originX = map()->height() * tileWidth / 2; // top-left corner
    const int originY = map()->cellsPerLevel().y() * (maxLevel() - level) * tileHeight;
    return QPointF((x - y) * tileWidth / 2 + originX,
                   (x + y) * tileHeight / 2 + originY);
#else
    const int originX = map()->height() * tileWidth / 2;
    return QPointF((x - y) * tileWidth / 2 + originX,
                   (x + y) * tileHeight / 2);
#endif
}

QPolygonF ZLevelRenderer::tileRectToPolygon(const QRect &rect, int level) const
{
    const int tileWidth = DISPLAY_TILE_WIDTH;
    const int tileHeight = DISPLAY_TILE_HEIGHT;

    const QPointF topRight = tileToPixelCoords(rect.topRight(), level);
    const QPointF bottomRight = tileToPixelCoords(rect.bottomRight(), level);
    const QPointF bottomLeft = tileToPixelCoords(rect.bottomLeft(), level);

    QPolygonF polygon;
    polygon << QPointF(tileToPixelCoords(rect.topLeft(), level));
    polygon << QPointF(topRight.x() + tileWidth / 2,
                       topRight.y() + tileHeight / 2);
    polygon << QPointF(bottomRight.x(), bottomRight.y() + tileHeight);
    polygon << QPointF(bottomLeft.x() - tileWidth / 2,
                       bottomLeft.y() + tileHeight / 2);
    return polygon;
}

QPolygonF ZLevelRenderer::tileRectToPolygon(const QRectF &rect, int level) const
{
    QPolygonF polygon;
    polygon << QPointF(tileToPixelCoords(rect.topLeft(), level));
    polygon << QPointF(tileToPixelCoords(rect.topRight(), level));
    polygon << QPointF(tileToPixelCoords(rect.bottomRight(), level));
    polygon << QPointF(tileToPixelCoords(rect.bottomLeft(), level));
    return polygon;
}

void ZLevelRenderer::drawJumboTreeTile_Trunk(Tile *tile, QPainter *painter, const QTransform &baseTransform, qreal x, qreal y, qreal m11, qreal m12, qreal m21, qreal m22) const
{
    if (!tile || !tile->tileset())
        return;
    Tileset *tileset = tile->tileset();
    const int columns = tileset->columnCount();
    if (columns <= 0)
        return;
    int row_trunk = 0;
    int row = tile->id() / columns;
    if (row < 2) {
        return;
    }
    const int tileWidth = DISPLAY_TILE_WIDTH;
    QString tilesetName = tileset->name();
    for (size_t i = 0; i < sizeof(s_jumbo) / sizeof(JUMBO); i++) {
        if (s_jumbo[i].bHasLeaves && tilesetName.startsWith(s_jumbo[i].tilesetName)) {
            Tile *tile2 = tileset->tileAt(columns * row_trunk + tile->id() % columns);
            if (!tile2)
                return;
            QImage img = tile2->image();
            QPoint offset = tileset->tileOffset() + tile2->offset();
            int dx = offset.x() + x;
            int dy = offset.y() + y - tile2->height();
            dx -= tileWidth / 2;
            QTransform transform(m11, m12, m21, m22, dx, dy);
            painter->setTransform(transform * baseTransform);
            if (!PZTools::drawCachedRasterTile(painter, img))
                painter->drawImage(0, 0, img);
            break;
        }
    }
}

void ZLevelRenderer::drawJumboTreeTile_Leaves(Tile *tile, QPainter *painter, const QTransform &baseTransform, qreal x, qreal y, qreal m11, qreal m12, qreal m21, qreal m22) const
{
    if (!tile || !tile->tileset())
        return;
    Tileset *tileset = tile->tileset();
    const int columns = tileset->columnCount();
    if (columns <= 0)
        return;
    int row_summer = 3;
    int row = tile->id() / columns;
    if (row >= 2) {
        return;
    }
    const int tileWidth = DISPLAY_TILE_WIDTH;
    QString tilesetName = tileset->name();
    for (size_t i = 0; i < sizeof(s_jumbo) / sizeof(JUMBO); i++) {
        if (s_jumbo[i].bHasLeaves && tilesetName.startsWith(s_jumbo[i].tilesetName)) {
            Tile *tile2 = tileset->tileAt(columns * row_summer + tile->id() % columns);
            if (!tile2)
                return;
            QImage img = tile2->image();
            QPoint offset = tileset->tileOffset() + tile2->offset();
            int dx = offset.x() + x;
            int dy = offset.y() + y - tile2->height();
            dx -= tileWidth / 2;
            QTransform transform(m11, m12, m21, m22, dx, dy);
            painter->setTransform(transform * baseTransform);
            if (!PZTools::drawCachedRasterTile(painter, img))
                painter->drawImage(0, 0, img);
            break;
        }
    }
}
