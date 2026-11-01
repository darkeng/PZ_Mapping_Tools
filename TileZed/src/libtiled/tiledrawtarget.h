#ifndef TILEDRAWTARGET_H
#define TILEDRAWTARGET_H

#include <QtGlobal>

class QImage;
class QTransform;

namespace Tiled {

class TileDrawTarget
{
public:
    virtual ~TileDrawTarget() = default;

    virtual void setOpacity(qreal opacity) = 0;
    virtual void drawTile(const QImage &image, const QTransform &transform) = 0;
};

} // namespace Tiled

#endif // TILEDRAWTARGET_H
