#include "vsrtl_componentgraphic.h"

#include "vsrtl_componentbutton.h"
#include "vsrtl_graphics_defines.h"
#include "vsrtl_graphics_util.h"
#include "vsrtl_label.h"
#include "vsrtl_multiplexergraphic.h"
#include "vsrtl_placeroute.h"
#include "vsrtl_portgraphic.h"
#include "vsrtl_registergraphic.h"
#include "vsrtl_wiregraphic.h"

#include <cereal/archives/json.hpp>

#include <qmath.h>
#include <deque>
#include <fstream>

#include <QApplication>
#include <QFileDialog>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QMatrix>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionGraphicsItem>

namespace vsrtl {

namespace {

static inline qreal snapToGrid(qreal v) {
    return round(v / GRID_SIZE) * GRID_SIZE;
}

static inline QRectF gridToScene(const QRect& gridRect) {
    // Scales a rectangle in grid coordinates to scene coordinates
    QRectF sceneGridRect;
    sceneGridRect.setWidth(gridRect.width() * GRID_SIZE);
    sceneGridRect.setHeight(gridRect.height() * GRID_SIZE);
    return sceneGridRect;
}

static inline QRect sceneToGrid(QRectF sceneRect) {
    // Scales a rectangle in scene coordinates to grid coordinates
    sceneRect.setWidth(sceneRect.width() / GRID_SIZE);
    sceneRect.setHeight(sceneRect.height() / GRID_SIZE);

    // When converting to integer-based grid rect, round up to ensure all components are inside
    return QRect(QPoint(0, 0), QSize(std::ceil(sceneRect.width()), std::ceil(sceneRect.height())));
}

}  // namespace

static constexpr qreal c_resizeMargin = GRID_SIZE;
static constexpr qreal c_collapsedSideMargin = 15;

ComponentGraphic::ComponentGraphic(SimComponent* c, QGraphicsItem* parent)
    : m_component(c), m_minGridRect(ShapeRegister::getComponentMinGridRect(c->getGraphicsID())), GraphicsBase(parent) {
    c->changed.Connect(this, &ComponentGraphic::updateSlot);
    c->registerGraphic(this);
    verifySpecialSignals();
}

void ComponentGraphic::verifySpecialSignals() const {
    // Ensure that all special signals required by the graphics type of this component has been set by the simulator
    // component
    auto* type = m_component->getGraphicsType();
    for (const auto& typeID : type->specialPortIDs()) {
        if (m_component->getSpecialPort(typeID) == nullptr) {
            m_component->throwError(
                "Special port: '" + std::string(typeID) +
                "' not assigned. A special port of this ID should be registered through SimComponent::setSpecialPort");
        }
    }
}

bool ComponentGraphic::hasSubcomponents() const {
    return m_component->getSubComponents().size() != 0;
}

void ComponentGraphic::initialize() {
    Q_ASSERT(scene() != nullptr);

    setFlags(ItemIsSelectable | ItemSendsScenePositionChanges);
    setAcceptHoverEvents(true);
    setMoveable();

    m_label = new Label(QString::fromStdString(m_component->getDisplayName()), this);

    // Create IO ports of Component
    for (const auto& c : m_component->getPorts<SimPort::Direction::in, SimPort>()) {
        m_inputPorts[c] = new PortGraphic(c, PortType::in, this);
    }
    for (const auto& c : m_component->getPorts<SimPort::Direction::out, SimPort>()) {
        m_outputPorts[c] = new PortGraphic(c, PortType::out, this);
    }

    m_restrictSubcomponentPositioning = false;
    if (hasSubcomponents()) {
        // Setup expand button
        m_expandButton = new ComponentButton(this);
        connect(m_expandButton, &ComponentButton::toggled, [this](bool expanded) { setExpanded(expanded); });

        createSubcomponents();
        placeAndRouteSubcomponents();
    }

    // By default, a component is collapsed. This has no effect if a component does not have any subcomponents
    setExpanded(false);
    m_restrictSubcomponentPositioning = true;
}

/**
 * @brief ComponentGraphic::createSubcomponents
 * In charge of hide()ing subcomponents if the parent component (this) is not expanded
 */
void ComponentGraphic::createSubcomponents() {
    for (const auto& c : m_component->getSubComponents()) {
        ComponentGraphic* nc;
        auto typeId = c->getGraphicsID();
        if (typeId == GraphicsIDFor(Multiplexer)) {
            nc = new MultiplexerGraphic(c, this);
        } else if (typeId == GraphicsIDFor(Register)) {
            nc = new RegisterGraphic(c, this);
        } else if (typeId == GraphicsIDFor(Constant)) {
            // Don't create a distinct ComponentGraphic for constants - these will be drawn next to the port connecting
            // to it
            continue;
        } else {
            nc = new ComponentGraphic(c, this);
        }
        nc->initialize();
        nc->setParentItem(this);
        nc->m_parentComponentGraphic = this;
        m_subcomponents.push_back(nc);
        if (!m_isExpanded) {
            nc->hide();
        }
    }
}

void ComponentGraphic::placeAndRouteSubcomponents() {
    const auto& placements = PlaceRoute::get()->placeAndRoute(m_subcomponents);
    for (const auto& p : placements) {
        p.first->setPos(p.second);
    }
}

void ComponentGraphic::resetWires() {
    const QString text =
        "Reset wires?\nThis will remove all interconnecting points for all wires within this subcomponent";

    if (QMessageBox::Yes == QMessageBox::question(QApplication::activeWindow(), "Reset wires", text)) {
        // Clear subcomponent wires
        for (const auto& c : m_subcomponents) {
            for (const auto& p : c->outputPorts()) {
                p->getOutputWire()->clearWirePoints();
            }
        }
        // Clear wires from this components input ports
        for (const auto& p : m_inputPorts) {
            p->getOutputWire()->clearWirePoints();
        }
    }
}

void ComponentGraphic::loadLayout() {
    QString fileName = QFileDialog::getOpenFileName(QApplication::activeWindow(),
                                                    "Save Layout " + QString::fromStdString(m_component->getName()),
                                                    QString(), tr("JSON (*.json)"));

    if (fileName.isEmpty())
        return;

    std::ifstream file(fileName.toStdString());
    cereal::JSONInputArchive archive(file);
    m_isTopLevelSerializedComponent = true;
    archive(cereal::make_nvp("ComponentGraphic", *this));
    m_isTopLevelSerializedComponent = false;
}

void ComponentGraphic::saveLayout() {
    QString fileName = QFileDialog::getSaveFileName(QApplication::activeWindow(),
                                                    "Save Layout " + QString::fromStdString(m_component->getName()),
                                                    QString(), tr("JSON (*.json)"));

    if (fileName.isEmpty())
        return;
    if (!fileName.endsWith(".json"))
        fileName += ".json";
    std::ofstream file(fileName.toStdString());
    cereal::JSONOutputArchive archive(file);

    /// @todo: Is it more applicable to do a typeid(getComponent()).name() ? this would not work accross separate
    /// compilers, but would directly indicate the underlying type of which this layout is compatible with...
    m_isTopLevelSerializedComponent = true;
    archive(cereal::make_nvp("ComponentGraphic", *this));
    m_isTopLevelSerializedComponent = false;
}

void ComponentGraphic::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
    if (isLocked())
        return;

    QMenu menu;

    if (hasSubcomponents()) {
        // ======================== Layout menu ============================ //
        auto* layoutMenu = menu.addMenu("Layout");
        auto* loadAction = layoutMenu->addAction("Load layout");
        auto* saveAction = layoutMenu->addAction("Save layout");
        auto* resetWiresAction = layoutMenu->addAction("Reset wires");

        connect(saveAction, &QAction::triggered, this, &ComponentGraphic::saveLayout);
        connect(loadAction, &QAction::triggered, this, &ComponentGraphic::loadLayout);
        connect(resetWiresAction, &QAction::triggered, this, &ComponentGraphic::resetWires);
    }

    if (m_outputPorts.size() > 0) {
        // ======================== Ports menu ============================ //
        auto* portMenu = menu.addMenu("Ports");
        auto* showOutputsAction = portMenu->addAction("Show output values");
        auto* hideOutputsAction = portMenu->addAction("Hide output values");
        connect(showOutputsAction, &QAction::triggered, [=] {
            for (auto& c : m_outputPorts)
                c->setLabelVisible(true);
        });
        connect(hideOutputsAction, &QAction::triggered, [=] {
            for (auto& c : m_outputPorts)
                c->setLabelVisible(false);
        });
    }

    auto* hideAction = menu.addAction("Hide");
    connect(hideAction, &QAction::triggered, [=] { this->hide(); });

    menu.exec(event->screenPos());
}

void ComponentGraphic::setExpanded(bool state) {
    m_isExpanded = state;
    GeometryChange changeReason = GeometryChange::None;

    if (m_expandButton != nullptr) {
        m_expandButton->setChecked(m_isExpanded);
        changeReason = m_isExpanded ? GeometryChange::Expand : GeometryChange::Collapse;
        for (const auto& c : m_subcomponents) {
            c->setVisible(m_isExpanded);
        }
        // We are not hiding the input ports of a component, because these should always be drawn. However, a input port
        // of an expandable component has wires drawin inside the component, which must be hidden aswell, such that they
        // do not accept mouse events nor are drawn.
        for (const auto& p : m_inputPorts) {
            p->setOutwireVisible(m_isExpanded);
        }
    }

    // Recalculate geometry based on now showing child components
    updateGeometry(QRect(), changeReason);
}

ComponentGraphic* ComponentGraphic::getParent() const {
    Q_ASSERT(parentItem() != nullptr);
    return static_cast<ComponentGraphic*>(parentItem());
}

QRect ComponentGraphic::subcomponentBoundingGridRect() const {
    // Calculate bounding rectangle of subcomponents in scene coordinates, convert to grid coordinates, and
    // apply as grid rectangle
    auto sceneBoundingRect = QRectF();
    for (const auto& c : m_subcomponents) {
        sceneBoundingRect =
            boundingRectOfRects<QRectF>(sceneBoundingRect, mapFromItem(c, c->boundingRect()).boundingRect());
    }

    return sceneToGrid(sceneBoundingRect);
}

QRect ComponentGraphic::adjustedMinGridRect(bool includePorts) const {
    // Returns the minimum grid rect of the current component with ports taken into account

    // Add height to component based on the largest number of input or output ports. There should always be a
    // margin of 1 on top- and bottom of component
    QRect adjustedRect = m_minGridRect;
    const int largestPortSize = m_inputPorts.size() > m_outputPorts.size() ? m_inputPorts.size() : m_outputPorts.size();
    const int heightToAdd = (largestPortSize + 2) - adjustedRect.height();
    if (heightToAdd > 0) {
        adjustedRect.adjust(0, 0, 0, heightToAdd);
    }

    if (includePorts) {
        // To the view of the place/route algorithms, ports reside on the edge of a component - however, this is not how
        // components are drawn. Ports are defined as 1 grid tick wide, add this o each side if there are input/output
        // ports
        if (m_inputPorts.size() > 0)
            adjustedRect.adjust(0, 0, 1, 0);
        if (m_outputPorts.size() > 0)
            adjustedRect.adjust(0, 0, 1, 0);
    }

    return adjustedRect;
}

void ComponentGraphic::updateGeometry(QRect newGridRect, GeometryChange flag) {
    // Rect will change when expanding, so notify canvas that the rect of the current component will be dirty
    prepareGeometryChange();

    // Assert that components without subcomponents cannot be requested to expand or collapse
    Q_ASSERT(!((flag == GeometryChange::Expand || flag == GeometryChange::Collapse) && !hasSubcomponents()));

    // ================= Grid rect sizing ========================= //
    // move operation has already resized base rect to a valid size

    switch (flag) {
        case GeometryChange::Collapse:
        case GeometryChange::None: {
            m_gridRect = adjustedMinGridRect(false);

            // Add width to the component based on the name of the component - we define that 2 characters is equal to 1
            // grid spacing : todo; this ratio should be configurable
            m_gridRect.adjust(0, 0, m_component->getName().length() / GRID_SIZE, 0);
            break;
        }
        case GeometryChange::Resize: {
            if (snapToMinGridRect(newGridRect)) {
                m_gridRect = newGridRect;
            } else {
                return;
            }
            break;
        }
        case GeometryChange::Expand:
        case GeometryChange::ChildJustCollapsed:
        case GeometryChange::ChildJustExpanded: {
            m_gridRect = subcomponentBoundingGridRect();
            break;
        }
    }

    // =========================== Scene item positioning ====================== //
    const QRectF sceneRect = sceneGridRect();

    // 1. Set Input/Output port positions
    if (/*m_isExpanded*/ false) {
        // Some fancy logic for positioning IO positions in the best way to facilitate nice signal lines between
        // components
    } else {
        // Component is unexpanded - IO should be positionen in even positions.
        // +2 to ensure a 1 grid margin at the top and bottom of the component
        int i = 0;
        const qreal in_seg_y = sceneRect.height() / (m_inputPorts.size());
        // Iterate using the sorted ports of the Component
        for (const auto& p : m_component->getPorts<SimPort::Direction::in, SimPort>()) {
            auto& graphicsPort = m_inputPorts[p];
            int gridIndex = roundUp((i * in_seg_y + in_seg_y / 2), GRID_SIZE) / GRID_SIZE;
            graphicsPort->setGridIndex(gridIndex);
            const qreal y = gridIndex * GRID_SIZE;
            graphicsPort->setPos(QPointF(sceneRect.left() - GRID_SIZE * PortGraphic::portGridWidth(), y));
            i++;
        }
        i = 0;
        const qreal out_seg_y = sceneRect.height() / (m_outputPorts.size());
        // Iterate using the sorted ports of the Component
        for (const auto& p : m_component->getPorts<SimPort::Direction::out, SimPort>()) {
            auto& graphicsPort = m_outputPorts[p];
            const int gridIndex = roundUp((i * out_seg_y + out_seg_y / 2), GRID_SIZE) / GRID_SIZE;
            graphicsPort->setGridIndex(gridIndex);
            const qreal y = gridIndex * GRID_SIZE;
            graphicsPort->setPos(QPointF(sceneRect.right(), y));
            i++;
        }
    }

    // 2. Set label position
    m_label->setPos(sceneRect.width() / 2, 0);

    // 3 .Update the draw shape, scaling it to the current scene size of the component
    QTransform t;
    t.scale(sceneRect.width(), sceneRect.height());
    m_shape = ShapeRegister::getComponentShape(m_component->getGraphicsID(), t);

    // 5. Position the expand-button
    if (hasSubcomponents()) {
        if (m_isExpanded) {
            m_expandButton->setPos(QPointF(0, 0));
        } else {
            // Center
            const qreal x = sceneRect.width() / 2 - m_expandButton->boundingRect().width() / 2;
            const qreal y = sceneRect.height() / 2 - m_expandButton->boundingRect().height() / 2;
            m_expandButton->setPos(QPointF(x, y));
        }
    }

    // 6. If we have a parent, it should now update its geometry based on new size of its subcomponent(s)
    if (parentItem() && (flag == GeometryChange::Expand || flag == GeometryChange::Collapse)) {
        getParent()->updateGeometry(QRect(), flag == GeometryChange::Expand ? GeometryChange::ChildJustExpanded
                                                                            : GeometryChange::ChildJustCollapsed);
    }

    // 7. Update the grid points within this component, if it has subcomponents
    if (hasSubcomponents() && m_isExpanded) {
        // Grid should only be drawing inside the component, so remove 1 gridsize from each edge of the
        // component rect
        auto rect = m_shape.boundingRect();
        QPoint gridTopLeft = (rect.topLeft() / GRID_SIZE).toPoint() * GRID_SIZE;
        gridTopLeft += QPoint(GRID_SIZE, GRID_SIZE);
        QPoint gridBotRight = (rect.bottomRight() / GRID_SIZE).toPoint() * GRID_SIZE;
        gridBotRight -= QPoint(GRID_SIZE, GRID_SIZE);

        m_gridPoints.clear();
        for (int x = gridTopLeft.x(); x <= gridBotRight.x(); x += GRID_SIZE)
            for (int y = gridTopLeft.y(); y <= gridBotRight.y(); y += GRID_SIZE)
                m_gridPoints << QPoint(x, y);
    }
}

void ComponentGraphic::setLocked(bool locked) {
    // No longer give the option of expand the component if the scene is locked
    if (m_expandButton)
        m_expandButton->setVisible(!locked);

    GraphicsBase::setLocked(locked);
}

QVariant ComponentGraphic::itemChange(GraphicsItemChange change, const QVariant& value) {
    // @todo implement snapping inside parent component
    if (change == ItemPositionChange && scene()) {
        // Output port wires are implicitely redrawn given that the wire is a child of $this. We need to manually signal
        // the wires going to the input ports of this component, to redraw
        if (m_initialized) {
            for (const auto& inputPort : m_inputPorts) {
                if (!inputPort->getPort()->isConstant())
                    inputPort->getPort()->getInputPort()->getGraphic<PortGraphic>()->updateWireGeometry();
            }
        }

        // Snap to grid
        QPointF newPos = value.toPointF();
        qreal x = snapToGrid(newPos.x());
        qreal y = snapToGrid(newPos.y());
        if (m_parentComponentGraphic == nullptr || !m_parentComponentGraphic->restrictSubcomponentPositioning()) {
            // Parent has just expanded, and is expecting its children to position themselves, before it calculates its
            // size. Do not restrict movement
            return QPointF(x, y);
        } else {
            // Restrict position changes to inside parent item
            const auto& parentRect = getParent()->sceneGridRect();
            const auto& thisRect = boundingRect();
            if (parentRect.contains(thisRect.translated(newPos))) {
                return QPointF(x, y);
            } else {
                // Keep the item inside the scene rect.
                newPos.setX(
                    snapToGrid(qMin(parentRect.right() - thisRect.width(), qMax(newPos.x(), parentRect.left()))));
                newPos.setY(
                    snapToGrid(qMin(parentRect.bottom() - thisRect.height(), qMax(newPos.y(), parentRect.top()))));
                return newPos;
            }
        }
    }

    return QGraphicsItem::itemChange(change, value);
}

void ComponentGraphic::setShape(const QPainterPath& shape) {
    m_shape = shape;
}

namespace {
qreal largestPortWidth(const QMap<SimPort*, PortGraphic*>& ports) {
    qreal width = 0;
    for (const auto& port : ports) {
        width = port->boundingRect().width() > width ? port->boundingRect().width() : width;
    }
    return width;
}
}  // namespace

void ComponentGraphic::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* w) {
    QColor color = hasSubcomponents() ? QColor("#ecf0f1") : QColor(Qt::white);
    QColor fillColor = (option->state & QStyle::State_Selected) ? color.dark(150) : color;
    if (option->state & QStyle::State_MouseOver)
        fillColor = fillColor.light(125);

    const qreal lod = option->levelOfDetailFromTransform(painter->worldTransform());

    // Draw component outline
    QPen oldPen = painter->pen();
    QPen pen = oldPen;
    int width = COMPONENT_BORDER_WIDTH;
    if (option->state & QStyle::State_Selected)
        width += 1;

    pen.setWidth(width);
    painter->setBrush(QBrush(fillColor.dark(option->state & QStyle::State_Sunken ? 120 : 100)));
    painter->setPen(pen);
    painter->drawPath(m_shape);

    painter->setPen(oldPen);

    if (hasSubcomponents()) {
        if (lod >= 0.35) {
            // Determine whether expand button should be shown. If we are in locked state, do not interfere with the
            // view state of the expand button
            if (!isLocked()) {
                m_expandButton->show();
            } else {
                m_expandButton->hide();
            }

            if (m_isExpanded) {
                // Draw grid
                painter->save();
                painter->setPen(QPen(Qt::lightGray, 1));
                painter->drawPoints(m_gridPoints);
                painter->restore();
            }
        }
    }
out:

    paintOverlay(painter, option, w);

#ifdef VSRTL_DEBUG_DRAW
    painter->save();
    painter->setPen(Qt::green);
    painter->drawRect(sceneGridRect());
    painter->restore();
    DRAW_BOUNDING_RECT(painter)
#endif
}

bool ComponentGraphic::rectContainsAllSubcomponents(const QRectF& r) const {
    bool allInside = true;
    for (const auto& rect : m_subcomponents) {
        auto r2 = mapFromItem(rect, rect->boundingRect()).boundingRect();
        allInside &= r.contains(r2);
    }
    return allInside;
}

/**
 * @brief Adjust bounds of r to snap on the boundaries of the minimum grid rectangle, or if the component is currently
 * expanded, a rect which contains the subcomponents
 */
bool ComponentGraphic::snapToMinGridRect(QRect& r) const {
    bool snap_r, snap_b;
    snap_r = false;
    snap_b = false;

    const auto& cmpRect =
        hasSubcomponents() && isExpanded() ? subcomponentBoundingGridRect() : adjustedMinGridRect(true);

    if (r.right() < cmpRect.right()) {
        r.setRight(cmpRect.right());
        snap_r = true;
    }
    if (r.bottom() < cmpRect.bottom()) {
        r.setBottom(cmpRect.bottom());
        snap_b = true;
    }

    return !(snap_r & snap_b);
}

QRectF ComponentGraphic::sceneGridRect() const {
    return gridToScene(m_gridRect);
}

QRectF ComponentGraphic::boundingRect() const {
    QRectF boundingRect = sceneGridRect();

    // Adjust slightly for stuff such as shadows, pen sizes etc.
    boundingRect.adjust(-SIDE_MARGIN, -SIDE_MARGIN, SIDE_MARGIN, SIDE_MARGIN);

    return boundingRect;
}

void ComponentGraphic::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (flags().testFlag(ItemIsMovable) && event->button() == Qt::LeftButton && m_inResizeDragZone) {
        // start resize drag
        setFlags(flags() & ~ItemIsMovable);
        m_resizeDragging = true;
    }

    QGraphicsItem::mousePressEvent(event);
}

void ComponentGraphic::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (m_resizeDragging) {
        QPoint gridPos = (event->pos() / GRID_SIZE).toPoint();
        auto newGridRect = m_gridRect;
        newGridRect.setBottomRight(gridPos);

        updateGeometry(newGridRect, GeometryChange::Resize);
    }

    QGraphicsItem::mouseMoveEvent(event);
}

void ComponentGraphic::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (m_resizeDragging) {
        setFlags(flags() | ItemIsMovable);
        m_resizeDragging = false;
    }

    QGraphicsItem::mouseReleaseEvent(event);
}

void ComponentGraphic::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    if (!isLocked()) {
        const auto& sceneRect = sceneGridRect();
        if (sceneRect.width() - event->pos().x() <= c_resizeMargin &&
            sceneRect.height() - event->pos().y() <= c_resizeMargin) {
            this->setCursor(Qt::SizeFDiagCursor);
            m_inResizeDragZone = true;
        } else {
            this->setCursor(Qt::ArrowCursor);
            m_inResizeDragZone = false;
        }
    }
}
}  // namespace vsrtl
