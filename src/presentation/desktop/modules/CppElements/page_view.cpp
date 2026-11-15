#include "page_view.hpp"
#include <QClipboard>
#include <QDebug>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <cmath>
#include <QDesktopServices>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSGSimpleTextureNode>
#include <QtWidgets/QApplication>
#include <limits>
#include "fz_utils.hpp"
#include "highlight.hpp"
#include "page_controller.hpp"
#include "touch_selection_session.hpp"

using adapters::controllers::BookController;
using adapters::controllers::PageController;
using application::utility::TouchSelectionSession;
using domain::entities::Highlight;
using namespace application::core;

namespace
{
TouchSelectionSession g_touchSession;
QPointer<cpp_elements::PageView> g_touchPage;
}  // namespace

namespace cpp_elements
{

cpp_elements::PageView::PageView()
{
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);

    m_tripleClickTimer.setInterval(400);
    m_tripleClickTimer.setSingleShot(true);
}

qreal PageView::currentDevicePixelRatio() const
{
    auto* win = window();
    return win != nullptr ? win->devicePixelRatio() : 1.0;
}

void PageView::setBookController(BookController* newBookController)
{
    m_bookController = newBookController;
    if(m_bookController == nullptr ||
       m_bookController->getFzDocument() == nullptr)
        return;

    m_pageController = std::make_unique<PageController>(
        m_bookController->getFzDocument(), m_pageNumber,
        currentDevicePixelRatio());
    m_pageController->setZoom(m_bookController->getZoom());

    // Setup connections to the BookController
    connect(m_bookController, &BookController::zoomChanged, this,
            &PageView::updateZoom);

    connect(m_bookController, &BookController::selectText, this,
            [this](int pageNumber, QPointF left, QPointF right)
            {
                if(pageNumber != m_pageNumber)
                    return;

                // The offsets are only valid once the page was rendered, make
                // sure that it is always rendered first.
                m_pageController->renderPage();

                auto xOffset = m_pageController->getXOffset();
                auto yOffset = m_pageController->getYOffset();
                left = QPoint(left.x() - xOffset, left.y() - yOffset);
                right = QPoint(right.x() - xOffset, right.y() - yOffset);

                // The points received from this signal are relative to a zoom
                // of 1, but all of the methods in this class handle points as
                // if they have the currentZoom applied, so we need to scale it.
                // We need to divide by the dpr since the lower level methods
                // expect the caller not to know anything about the dpr, but the
                // current zoom we get from the page controller is zoom * dpr.
                // Thus we need to reset it to the zoom without the dpr.
                auto zoom = m_pageController->getZoom();
                auto dpr = currentDevicePixelRatio();
                left = utils::scalePointToCurrentZoom(left, 1, zoom / dpr);
                right = utils::scalePointToCurrentZoom(right, 1, zoom / dpr);

                m_selectionStart = left;
                m_selectionEnd = right;

                createSelection();
            });
}

int PageView::getImplicitWidth() const
{
    if(m_pageController == nullptr)
        return 0;
    return m_pageController->getWidth();
}

int PageView::getImplicitHeight() const
{
    if(m_pageController == nullptr)
        return 0;
    return m_pageController->getHeight();
}

int PageView::getPageNumber() const
{
    return m_pageNumber;
}

void PageView::setPageNumber(int newCurrentPage)
{
    m_pageNumber = newCurrentPage;
}

void PageView::updateZoom(float newZoom)
{
    if(m_pageController == nullptr)
        return;

    auto oldZoom = m_pageController->getZoom();
    m_pageController->setZoom(newZoom);

    // Update selection positions to match new zoom
    if(!m_selectionStart.isNull() && !m_selectionEnd.isNull())
    {
        m_selectionStart =
            utils::scalePointToCurrentZoom(m_selectionStart, oldZoom, newZoom);
        m_selectionEnd =
            utils::scalePointToCurrentZoom(m_selectionEnd, oldZoom, newZoom);

        m_pageController->generateSelectionRects(m_selectionStart,
                                                 m_selectionEnd);
    }

    emit implicitWidthChanged();
    emit implicitHeightChanged();
}

void PageView::geometryChange(const QRectF& newGeometry,
                              const QRectF& oldGeometry)
{
    if(newGeometry.size().isEmpty())
        return;

    if(newGeometry.width() != oldGeometry.width() ||
       newGeometry.height() != newGeometry.height())
    {
        update();
    }

    QQuickItem::geometryChange(newGeometry, oldGeometry);
}

QSGNode* PageView::updatePaintNode(QSGNode* node, UpdatePaintNodeData* nodeData)
{
    Q_UNUSED(nodeData);
    if(m_pageController == nullptr)
        return node;
    QSGSimpleTextureNode* n = static_cast<QSGSimpleTextureNode*>(node);
    if(!n)
    {
        n = new QSGSimpleTextureNode();
        n->setOwnsTexture(true);
    }

    auto image = m_pageController->renderPage();
    QPainter painter(&image);

    paintSelectionOnPage(painter);
    paintHighlightsOnPage(painter);

    auto* win = window();
    if(win == nullptr)
        return n;

    n->setTexture(win->createTextureFromImage(image));
    n->setRect(boundingRect());
    return n;
}

bool PageView::isSynthesizedTouch(const QMouseEvent* event) const
{
    return event->source() == Qt::MouseEventSynthesizedBySystem ||
           event->source() == Qt::MouseEventSynthesizedByQt;
}

QPointF PageView::snapTouchToText(qreal x, qreal y) const
{
    if(m_pageController == nullptr)
        return QPointF(x, y);

    const int yOffsets[] = { 28, 40, 16, 52, 8, 0, 64, -8 };
    for(int dy : yOffsets)
    {
        QPointF candidate(x, y - dy);
        if(candidate.y() < 0)
            continue;
        if(m_pageController->pointIsAboveText(candidate))
            return candidate;
    }

    const int xOffsets[] = { -16, 16, -32, 32 };
    for(int dx : xOffsets)
    {
        QPointF candidate(x + dx, y - 28);
        if(candidate.y() < 0)
            continue;
        if(m_pageController->pointIsAboveText(candidate))
            return candidate;
    }

    return QPointF(x, qMax(0.0, y - 28.0));
}

void PageView::emitSelectionFinished()
{
    if(m_pageController == nullptr || m_bookController == nullptr)
        return;

    auto rects = m_pageController->getBufferedSelectionRects();
    if(rects.isEmpty())
        return;

    QList<QRectF> restoredRects;
    restoredRects.reserve(rects.size());
    for(auto rect : rects)
    {
        utils::restoreQRect(rect, m_pageController->getZoom());
        utils::scaleQRectFToZoom(rect, m_pageController->getZoom() /
                                           currentDevicePixelRatio());
        restoredRects.push_back(rect);
    }

    auto positions = getCenterXAndTopYFromRects(restoredRects);
    if(!std::isfinite(positions.first) || !std::isfinite(positions.second))
        return;

    emit m_bookController->textSelectionFinished(positions.first,
                                                 positions.second);
}

void PageView::beginTouchSelection(qreal x, qreal y)
{
    if(m_pageController == nullptr)
        return;

    if(g_touchPage != nullptr && g_touchPage != this)
        g_touchPage->removeSelection();

    g_touchPage = this;
    g_touchSession.longPress();
    m_touchSelecting = true;
    forceActiveFocus();
    auto point = snapTouchToText(x, y);
    m_touchAnchor = point;
    m_selectionStart = point;
    m_selectionEnd = point;
    selectSingleWord();
    grabMouse();
    QMetaObject::invokeMethod(
        this,
        [this]()
        {
            if(m_pageController == nullptr)
                return;
            if(!g_touchSession.menuVisible())
                return;
            emitSelectionFinished();
        },
        Qt::QueuedConnection);
}

void PageView::updateTouchSelection(qreal x, qreal y)
{
    if(!m_touchSelecting || m_pageController == nullptr)
        return;

    if(g_touchSession.menuVisible())
    {
        g_touchSession.dragStarted();
        if(m_bookController != nullptr)
            emit m_bookController->touchSelectionMenuHidden();
    }

    m_selectionStart = m_touchAnchor;
    m_selectionEnd = snapTouchToText(x, y);
    createSelection();
}

void PageView::finishTouchSelection()
{
    if(!m_touchSelecting)
        return;

    g_touchSession.released();
    m_touchSelecting = false;
    ungrabMouse();
    QMetaObject::invokeMethod(
        this,
        [this]()
        {
            if(m_pageController == nullptr)
                return;
            if(!g_touchSession.menuVisible())
                return;
            emitSelectionFinished();
        },
        Qt::QueuedConnection);
}

void PageView::clearTouchSelection()
{
    g_touchSession.tap();
    if(g_touchPage != nullptr)
    {
        g_touchPage->removeSelection();
        g_touchPage = nullptr;
    }
    m_touchSelecting = false;
    ungrabMouse();
    if(m_bookController != nullptr)
        emit m_bookController->touchSelectionMenuHidden();
}

void PageView::handleTouchTap(qreal x, qreal y)
{
    if(m_pageController == nullptr || m_bookController == nullptr)
        return;

    QPoint point(static_cast<int>(x), static_cast<int>(y));
    forceActiveFocus();

    auto highlight = m_bookController->getHighlightAtPoint(point, m_pageNumber);
    if(highlight != nullptr)
    {
        handleClickingOnHighlight(highlight);
        return;
    }

    if(m_pageController->pointIsAboveLink(point))
    {
        auto uri = m_pageController->getLinkUriAtPoint(point);
        m_bookController->followLink(uri);
        return;
    }

    clearTouchSelection();
}

void PageView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if(isSynthesizedTouch(event))
    {
        event->ignore();
        return;
    }

    if(event->button() == Qt::RightButton)
        return;

    if(m_startedMousePressOnHighlight)
        return;

    int mouseX = event->position().x();
    int mouseY = event->position().y();
    QPoint mousePoint(mouseX, mouseY);

    m_selectionStart = mousePoint;
    m_selectionEnd = mousePoint;
    selectSingleWord();

    m_tripleClickTimer.start();
    m_doubleClickHold = true;
}

void PageView::mousePressEvent(QMouseEvent* event)
{
    if(isSynthesizedTouch(event) && g_touchPage != this)
    {
        event->ignore();
        return;
    }

    if(event->button() == Qt::RightButton)
        return;

    if(m_pageController == nullptr || m_bookController == nullptr)
        return;

    int mouseX = event->position().x();
    int mouseY = event->position().y();
    QPoint point(mouseX, mouseY);

    forceActiveFocus();

    if(m_pageController->pointIsAboveLink(point))
        m_startedMousePressOnLink = true;

    auto highlight = m_bookController->getHighlightAtPoint(point, m_pageNumber);
    if(highlight != nullptr)
    {
        handleClickingOnHighlight(highlight);
        return;
    }
    m_startedMousePressOnHighlight = false;

    // Select line when left mouse button is pressed 3 times
    if(m_tripleClickTimer.isActive())
    {
        selectLine();
        return;
    }

    m_selectionStart = point;
}

void PageView::handleClickingOnHighlight(const Highlight* highlight)
{
    // Scale the highlight rects (initially zoom of 1) to the current zoom and
    // get the center x and top y position of the highlight.
    auto rects = highlight->getRects();
    QList<QRectF> qRects;
    qRects.reserve(rects.size());
    for(auto& rect : rects)
    {
        auto qRectF = rect.getQRect();
        utils::scaleQRectFToZoom(
            qRectF, m_pageController->getZoom() / currentDevicePixelRatio());
        qRects.append(qRectF);
    }
    auto positions = getCenterXAndTopYFromRects(qRects);

    auto uuidAsString = highlight->getUuid().toString(QUuid::WithoutBraces);
    m_bookController->highlightSelected(positions.first, positions.second,
                                        uuidAsString);
    m_startedMousePressOnHighlight = true;
}

void PageView::mouseReleaseEvent(QMouseEvent* event)
{
    if(isSynthesizedTouch(event))
    {
        if(g_touchPage == this && m_touchSelecting)
            finishTouchSelection();
        event->ignore();
        return;
    }

    int mouseX = event->position().x();
    int mouseY = event->position().y();
    QPoint mousePoint(mouseX, mouseY);

    if(m_startedMousePressOnLink &&
       m_pageController->pointIsAboveLink(mousePoint))
    {
        auto uri = m_pageController->getLinkUriAtPoint(mousePoint);
        m_bookController->followLink(uri);
    }
    m_startedMousePressOnLink = false;

    // This gets triggered when the user simply clicks on the page, without
    // dragging the mouse, so on a normal click. In this case we want to
    // reset the selection.
    if(m_selectionStart == QPointF(mouseX, mouseY) &&
       !m_tripleClickTimer.isActive())
    {
        removeSelection();

        // Restart it since some actions are checking if it is active to e.g.
        // prevent removing the line select on mouse release
        m_tripleClickTimer.start();
    }
    else if(!m_startedMousePressOnHighlight)
    {
        emitSelectionFinished();
    }

    m_doubleClickHold = false;
}

QPair<float, float> PageView::getCenterXAndTopYFromRects(
    const QList<QRectF>& rects)
{
    float mostLeftX = std::numeric_limits<float>::max();
    float mostRightX = 0;
    float topY = std::numeric_limits<float>::max();
    for(auto& rect : rects)
    {
        if(rect.x() < mostLeftX)
            mostLeftX = rect.x();

        if(rect.x() + rect.width() > mostRightX)
            mostRightX = rect.x() + rect.width();

        if(rect.top() < topY)
            topY = rect.top();
    }

    auto centerX = (mostLeftX + mostRightX) / 2;
    return { centerX, topY };
}

void PageView::mouseMoveEvent(QMouseEvent* event)
{
    if(isSynthesizedTouch(event))
    {
        if(g_touchPage == this && m_touchSelecting)
        {
            updateTouchSelection(event->position().x(), event->position().y());
            return;
        }
        event->ignore();
        return;
    }

    if(event->buttons() == Qt::RightButton)
        return;

    if(m_startedMousePressOnHighlight)
        return;

    int mouseX = event->position().x();
    int mouseY = event->position().y();

    // 'hoverMoveEvent' is not triggered when the left mouse button is pressed,
    // thus the cursor will not change correctly. Make sure to handle it here.
    setCorrectCursor(mouseX, mouseY);

    m_selectionEnd = QPointF(mouseX, mouseY);
    if(m_doubleClickHold)
        selectMultipleWords();
    else
        createSelection();
}

void PageView::hoverMoveEvent(QHoverEvent* event)
{
    int mouseX = event->position().x();
    int mouseY = event->position().y();
    setCorrectCursor(mouseX, mouseY);

    emit mouseHoverMoved();
}

void PageView::keyPressEvent(QKeyEvent* event)
{
    if(event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier)
    {
        copySelectedText();
    }
}

void PageView::hoverLeaveEvent(QHoverEvent* event)
{
    resetCursorToDefault();
}

void PageView::paintSelectionOnPage(QPainter& painter)
{
    auto& selectionRects = m_pageController->getBufferedSelectionRects();
    for(auto rect : selectionRects)
    {
        QColor selectionColor(134, 171, 175, 125);
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.fillRect(rect, selectionColor);
    }
}

void PageView::paintHighlightsOnPage(QPainter& painter)
{
    for(auto& highlight : m_bookController->getHighlights())
    {
        if(highlight.getPageNumber() != m_pageNumber)
            continue;

        for(auto rect : highlight.getRects())
        {
            // We store the highlights zoom independent, so we need to scale
            // them to the current zoom here.
            auto qRect = rect.getQRect();
            utils::scaleQRectFToZoom(qRect, m_pageController->getZoom());
            rect.setQRect(qRect);

            painter.setCompositionMode(QPainter::CompositionMode_Multiply);
            painter.fillRect(rect.getQRect(), highlight.getColor());
        }
    }
}

void PageView::removeConflictingHighlights(Highlight& highlight)
{
    bool existingHighlightRemoved = false;

    auto& highlights = m_bookController->getHighlights();
    for(int i = 0; i < highlights.size(); ++i)
    {
        auto& existingHighlight = highlights[i];
        if(existingHighlight.getPageNumber() != highlight.getPageNumber())
            continue;

        for(int u = 0; u < highlight.getRects().size(); ++u)
        {
            auto rect = highlight.getRects()[u].getQRect();

            for(int k = 0; k < existingHighlight.getRects().size(); ++k)
            {
                auto existingRect = existingHighlight.getRects()[k].getQRect();

                // New rect intersects with old rect
                if(rect.intersects(existingRect))
                {
                    // Make sure that the rects are on the same line. Depending
                    // on the line height, lines above eachother will overlap,
                    // but its only an intersect when they are on the same line.
                    bool onSameLine = rectsAreOnSameLine(existingRect, rect);
                    if(onSameLine)
                    {
                        auto uuid = highlights[i].getUuid();
                        m_bookController->removeHighlight(uuid);
                        --i;
                        existingHighlightRemoved = true;
                        break;
                    }
                }
            }

            if(existingHighlightRemoved)
            {
                existingHighlightRemoved = false;
                break;
            }
        }
    }
}

bool PageView::mouseAboveSelection(const QPointF mouse)
{
    auto& selectionRects = m_pageController->getBufferedSelectionRects();
    for(auto& rect : selectionRects)
    {
        if(rect.contains(mouse))
            return true;
    }

    return false;
}

QString PageView::createHighlightFromCurrentSelection(const QString& hex,
                                                      int alpha)
{
    auto bufferedSelectionRects = m_pageController->getBufferedSelectionRects();

    // Make sure to restore the rects to their original size, since we want to
    // store them, so they need to be zoom independent.
    for(auto& rect : bufferedSelectionRects)
    {
        utils::restoreQRect(rect, m_pageController->getZoom());
    }

    removeSelection();

    auto pageNumber = m_pageNumber;
    auto color = QColor(hex);
    color.setAlpha(alpha);
    auto rects = bufferedSelectionRects;
    Highlight highlight(pageNumber, color);
    highlight.setRects(rects);

    removeConflictingHighlights(highlight);
    m_bookController->addHighlight(highlight);

    update();
    return highlight.getUuid().toString(QUuid::WithoutBraces);
}

void PageView::removeHighlight(const QString& uuid)
{
    m_bookController->removeHighlight(QUuid(uuid));

    update();
}

void PageView::changeHighlightColor(const QString& uuid, const QString& color,
                                    int alpha)
{
    QColor newColor(color);
    newColor.setAlpha(alpha);

    m_bookController->changeHighlightColor(QUuid(uuid), newColor);

    update();
}

void PageView::copyHighlightedText(const QString& uuid)
{
    auto text = getHighlightedText(uuid);

    auto clipboard = QApplication::clipboard();
    clipboard->setText(text);
}

QString PageView::getSelectedText()
{
    return m_pageController->getTextFromSelection(m_selectionStart,
                                                  m_selectionEnd);
}

QString PageView::getHighlightedText(const QString& uuid)
{
    const Highlight* highlight = nullptr;
    for(auto& h : m_bookController->getHighlights())
    {
        if(h.getUuid() == QUuid(uuid))
        {
            highlight = &h;
            break;
        }
    }

    QPointF start(highlight->getRects().first().getQRect().left(),
                  highlight->getRects().first().getQRect().center().y());

    QPointF end(highlight->getRects().last().getQRect().right(),
                highlight->getRects().last().getQRect().center().y());

    start =
        utils::scalePointToCurrentZoom(start, 1, m_bookController->getZoom());
    end = utils::scalePointToCurrentZoom(end, 1, m_bookController->getZoom());

    return m_pageController->getTextFromSelection(start, end);
}

void PageView::setPointingCursor()
{
    if(QApplication::overrideCursor() == nullptr ||
       *QApplication::overrideCursor() != Qt::PointingHandCursor)
    {
        resetCursorToDefault();
        QApplication::setOverrideCursor(Qt::PointingHandCursor);
    }
}

void PageView::createSelection()
{
    m_pageController->generateSelectionRects(m_selectionStart, m_selectionEnd);
    update();
}

void PageView::removeSelection()
{
    if(m_pageController == nullptr)
        return;

    m_pageController->clearBufferedSelectionRects();
    update();

    m_selectionStart = QPointF(0, 0);
    m_selectionEnd = QPointF(0, 0);
}

void PageView::selectSingleWord()
{
    auto points = m_pageController->getPositionsForWordSelection(
        m_selectionStart, m_selectionEnd);

    m_selectionStart = points.first;
    m_selectionEnd = points.second;

    createSelection();
}

void PageView::selectMultipleWords()
{
    auto positions = m_pageController->getPositionsForWordSelection(
        m_selectionStart, m_selectionEnd);

    m_selectionStart = positions.first;
    m_selectionEnd = positions.second;

    createSelection();
}

void PageView::selectLine()
{
    auto positions =
        m_pageController->getPositionsForLineSelection(m_selectionStart);

    m_selectionStart = positions.first;
    m_selectionEnd = positions.second;

    createSelection();
}

void PageView::copySelectedText()
{
    QString text = getSelectedText();
    if(!m_includeNewLinesInCopiedText)
    {
        text.replace("\n", "");
        text.replace("\r", "");
    }

    auto clipboard = QApplication::clipboard();
    clipboard->setText(text);
}

void PageView::resetCursorToDefault()
{
    QApplication::restoreOverrideCursor();
}

void PageView::setCorrectCursor(int x, int y)
{
    if(m_pageController->pointIsAboveLink(QPoint(x, y)) ||
       m_bookController->getHighlightAtPoint(QPointF(x, y), m_pageNumber))
    {
        setPointingCursor();
    }
    else if(m_pageController->pointIsAboveText(QPoint(x, y)))
    {
        if(QApplication::overrideCursor() == nullptr ||
           *QApplication::overrideCursor() != Qt::IBeamCursor)
        {
            resetCursorToDefault();
            QApplication::setOverrideCursor(Qt::IBeamCursor);
        }
    }
    else
    {
        resetCursorToDefault();
    }
}

bool PageView::rectsAreOnSameLine(const QRectF& rect1, const QRectF& rect2)
{
    auto shorterRect = rect1.height() <= rect2.height() ? rect1 : rect2;
    auto intersectH = rect1.intersected(rect2).height();

    float offsetTolerance = 0.75;
    bool onSameLine = intersectH >= shorterRect.height() * offsetTolerance;

    return onSameLine;
}

void PageView::setColorInverted(bool newColorInverted)
{
    if(m_pageController == nullptr)
        return;

    // This method gets called on initialisation of the page item, but we don't
    // want to redraw it then, so we skip it if it's called for the first time.
    m_pageController->setInvertColor(newColorInverted);
    if(!m_firstTimeColorInverted)
        update();

    m_firstTimeColorInverted = false;
}

void PageView::setIncludeNewLinesInCopiedText(
    bool newIncludeNewLinesInCopiedText)
{
    m_includeNewLinesInCopiedText = newIncludeNewLinesInCopiedText;
}

float PageView::getYOffset() const
{
    if(m_pageController == nullptr)
        return 0;
    return m_pageController->getYOffset();
}

}  // namespace cpp_elements
