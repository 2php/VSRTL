#include "vsrtl_valuelabel.h"

#include "vsrtl_graphics_util.h"

#include <QFontMetrics>
#include <QGraphicsSceneContextMenuEvent>
#include <QPainter>

namespace vsrtl {

static QRectF getTextRect(const QString& text) {
    QFontMetrics metric = QFontMetrics(QFont());
    return metric.boundingRect(text);
}

ValueLabel::ValueLabel(DisplayType& type, unsigned int maxBitWidth, QGraphicsItem* parent)
    : m_type(type), m_maxBitWidth(maxBitWidth), QGraphicsItem(parent) {
    setFlags(ItemIsSelectable | ItemIsMovable);
}

QRectF ValueLabel::boundingRect() const {
    auto textRect = getTextRect(m_text);
    textRect.adjust(-10, -10, 10, 10);

    return textRect;
}

void ValueLabel::paint(QPainter* painter, const QStyleOptionGraphicsItem* item, QWidget*) {
    painter->save();
    QRectF textRect = getTextRect(m_text);
    textRect.adjust(-5, 0, 10, 5);  // adjust for pen width
    painter->fillRect(textRect, Qt::white);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(Qt::black, 2));
    painter->drawRect(textRect);
    painter->setFont(QFont());
    // Calculate drawing position
    QPointF textPos = textRect.topLeft();
    textPos.rx() += 5;
    textPos.ry() += 16;
    painter->drawText(textPos, m_text);

    painter->restore();
}

void ValueLabel::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
    QMenu menu;
    QAction* showLabel = new QAction("Show value");
    showLabel->setCheckable(true);
    showLabel->setChecked(isVisible());
    QObject::connect(showLabel, &QAction::triggered, [this](bool checked) { setVisible(checked); });
    menu.addAction(showLabel);
    menu.addMenu(createDisplayTypeMenu(m_type));
    menu.exec(event->screenPos());

    // Schedule an update of the label to register any change in the display type
    updateText();
}

void ValueLabel::updateText() {
    m_text = encodeDisplayValue(m_value, m_maxBitWidth, m_type);
    update();
}

void ValueLabel::setValue(VSRTL_VT_U v) {
    m_value = v;
    updateText();
}

}  // namespace vsrtl
