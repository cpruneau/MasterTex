#include "PdfViewer.h"
#include "DocumentManager.h"
#include "SettingsDialog.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfDocument>
#include <QProcess>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
const int kMargin = 12;    // around the pages, pixels
const int kSpacing = 12;   // between pages, pixels
}

// ---------------------------------------------------------------------------
// Canvas: lays out all pages vertically, paints only the visible ones.

class PdfCanvas : public QWidget {
public:
    explicit PdfCanvas(PdfViewer *viewer) : QWidget(viewer), m_viewer(viewer)
    {
        setMouseTracking(false);
        setCursor(Qt::ArrowCursor);
    }

    QVector<QRectF> pageRects;   // pixel rectangles in canvas coordinates
    QHash<int, QImage> cache;    // page -> rendered image at the current zoom
    int highlightPage = -1;
    QRectF highlightRect;        // in PDF points
    QTimer highlightTimer;

    void layoutPages(QPdfDocument *doc, qreal zoom, int viewportWidth)
    {
        pageRects.clear();
        qreal maxW = 0;
        for (int i = 0; i < doc->pageCount(); ++i) maxW = qMax(maxW, doc->pagePointSize(i).width() * zoom);
        const int width = qMax<int>(viewportWidth, int(std::ceil(maxW)) + 2 * kMargin);
        qreal y = kMargin;
        for (int i = 0; i < doc->pageCount(); ++i) {
            const QSizeF s = doc->pagePointSize(i) * zoom;
            pageRects << QRectF((width - s.width()) / 2.0, y, s.width(), s.height());
            y += s.height() + kSpacing;
        }
        resize(width, int(y) + kMargin);
        cache.clear();
        update();
    }

    int pageAt(const QPointF &p) const
    {
        for (int i = 0; i < pageRects.size(); ++i)
            if (pageRects[i].contains(p)) return i;
        return -1;
    }

protected:
    void paintEvent(QPaintEvent *ev) override
    {
        QPainter p(this);
        p.fillRect(ev->rect(), palette().color(QPalette::Mid));
        QPdfDocument *doc = m_viewer->m_doc;
        if (doc->status() != QPdfDocument::Status::Ready) return;
        const qreal dpr = devicePixelRatioF();
        for (int i = 0; i < pageRects.size(); ++i) {
            const QRectF r = pageRects[i];
            if (!r.intersects(ev->rect())) continue;
            if (!cache.contains(i)) {
                QImage img = doc->render(i, (r.size() * dpr).toSize());
                img.setDevicePixelRatio(dpr);
                cache.insert(i, img);
                trimCache(i);
            }
            p.fillRect(r, Qt::white);
            p.drawImage(r.topLeft(), cache.value(i));
            if (i == highlightPage && !highlightRect.isNull()) {
                const qreal z = m_viewer->m_zoom;
                QRectF h(r.left() + highlightRect.left() * z, r.top() + highlightRect.top() * z,
                         highlightRect.width() * z, highlightRect.height() * z);
                p.fillRect(h.adjusted(-3, -3, 3, 3), QColor(255, 200, 0, 90));
                p.setPen(QPen(QColor(230, 120, 0), 1.5));
                p.drawRect(h.adjusted(-3, -3, 3, 3));
            }
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        // Cmd-click on macOS (Ctrl elsewhere) jumps to the source.
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)) {
            jump(e->position());
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) jump(e->position());
    }

private:
    void jump(const QPointF &pos)
    {
        const int page = pageAt(pos);
        if (page < 0) return;
        const QPointF pt = (pos - pageRects[page].topLeft()) / m_viewer->m_zoom;
        m_viewer->inverseSearch(page, pt);
    }

    void trimCache(int around)
    {
        if (cache.size() <= 24) return;
        QList<int> keys = cache.keys();
        std::sort(keys.begin(), keys.end(), [around](int a, int b) { return std::abs(a - around) > std::abs(b - around); });
        for (int k = 0; k < keys.size() && cache.size() > 16; ++k) cache.remove(keys[k]);
    }

    PdfViewer *m_viewer;
};

// ---------------------------------------------------------------------------

PdfViewer::PdfViewer(QWidget *parent) : QWidget(parent)
{
    m_doc = new QPdfDocument(this);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(6, 3, 6, 3);
    auto button = [this, bar](const QString &text, const QString &tip, std::function<void()> fn) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, [fn]() { fn(); });
        bar->addWidget(b);
        return b;
    };
    m_pageBox = new QSpinBox;
    m_pageBox->setRange(1, 1);
    m_pageBox->setKeyboardTracking(false);
    m_pageCount = new QLabel("/ 0");
    m_zoomLabel = new QLabel;
    m_zoomLabel->setMinimumWidth(48);
    bar->addWidget(new QLabel(tr("Page")));
    bar->addWidget(m_pageBox);
    bar->addWidget(m_pageCount);
    bar->addSpacing(12);
    button("−", tr("Zoom out"), [this]() { setZoom(m_zoom / 1.2, false); });
    bar->addWidget(m_zoomLabel);
    button("+", tr("Zoom in"), [this]() { setZoom(m_zoom * 1.2, false); });
    button(tr("Fit Width"), tr("Fit the page width to the pane"), [this]() { setZoom(m_zoom, true); });
    bar->addStretch();
    button("↻", tr("Reload the PDF"), [this]() { reload(); });
    auto *hint = new QLabel(tr("Cmd-click or double-click → source"));
    hint->setStyleSheet("color: gray;");
    bar->addWidget(hint);
    lay->addLayout(bar);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_canvas = new PdfCanvas(this);
    m_canvas->setObjectName("pdfCanvas");
    m_scroll->setWidget(m_canvas);
    m_scroll->viewport()->installEventFilter(this);
    lay->addWidget(m_scroll, 1);

    m_canvas->highlightTimer.setSingleShot(true);
    m_canvas->highlightTimer.setInterval(3000);
    connect(&m_canvas->highlightTimer, &QTimer::timeout, this, [this]() {
        m_canvas->highlightPage = -1;
        m_canvas->update();
    });
    connect(m_pageBox, &QSpinBox::valueChanged, this, [this](int v) {
        if (v - 1 != currentPage()) scrollToPage(v - 1);
    });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewer::updatePageLabel);
    connect(m_doc, &QPdfDocument::statusChanged, this, [this](QPdfDocument::Status s) {
        if (s == QPdfDocument::Status::Ready) relayout();
    });
}

PdfViewer::~PdfViewer() = default;

bool PdfViewer::isLoaded() const
{
    return m_doc->status() == QPdfDocument::Status::Ready;
}

bool PdfViewer::load(const QString &pdfPath)
{
    QFile f(pdfPath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit message(tr("Cannot open %1").arg(pdfPath));
        return false;
    }
    QByteArray bytes = f.readAll();
    if (bytes.isEmpty()) return false;

    // Remember where we were reading.
    const bool samePdf = (pdfPath == m_path) && isLoaded();
    const int page = samePdf ? currentPage() : 0;
    qreal offsetInPage = 0;
    if (samePdf && page >= 0 && page < m_canvas->pageRects.size())
        offsetInPage = (m_scroll->verticalScrollBar()->value() - m_canvas->pageRects[page].top()) / m_zoom;
    const int hScroll = m_scroll->horizontalScrollBar()->value();

    // Work from a private copy so a rebuild in progress can't disturb the view.
    m_doc->close();
    delete m_buffer;
    m_bytes = bytes;
    m_buffer = new QBuffer(&m_bytes, this);
    m_buffer->open(QIODevice::ReadOnly);
    m_doc->load(m_buffer);
    m_path = pdfPath;
    if (!isLoaded()) {
        emit message(tr("Could not read %1").arg(QFileInfo(pdfPath).fileName()));
        return false;
    }
    relayout();
    if (samePdf) {
        scrollToPage(qMin(page, m_doc->pageCount() - 1), offsetInPage);
        m_scroll->horizontalScrollBar()->setValue(hScroll);
    } else {
        scrollToPage(0);
    }
    return true;
}

void PdfViewer::reload()
{
    if (!m_path.isEmpty()) load(m_path);
}

bool PdfViewer::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_scroll->viewport() && ev->type() == QEvent::Resize && m_fitWidth && isLoaded()) {
        const int page = currentPage();
        relayout();
        scrollToPage(page);
    }
    return QWidget::eventFilter(obj, ev);
}

void PdfViewer::relayout()
{
    if (!isLoaded()) return;
    if (m_fitWidth) {
        qreal maxW = 1;
        for (int i = 0; i < m_doc->pageCount(); ++i) maxW = qMax(maxW, m_doc->pagePointSize(i).width());
        m_zoom = qMax<qreal>(0.1, (m_scroll->viewport()->width() - 2 * kMargin) / maxW);
    }
    m_canvas->layoutPages(m_doc, m_zoom, m_scroll->viewport()->width());
    m_pageBox->setRange(1, qMax(1, m_doc->pageCount()));
    m_pageCount->setText(QString("/ %1").arg(m_doc->pageCount()));
    m_zoomLabel->setText(QString("%1%").arg(qRound(m_zoom * 100)));
    updatePageLabel();
}

void PdfViewer::setZoom(qreal pixelsPerPoint, bool fitWidth)
{
    const int page = currentPage();
    m_fitWidth = fitWidth;
    m_zoom = qBound<qreal>(0.15, pixelsPerPoint, 8.0);
    relayout();
    scrollToPage(page);
}

int PdfViewer::currentPage() const
{
    const int y = m_scroll->verticalScrollBar()->value() + m_scroll->viewport()->height() / 3;
    for (int i = 0; i < m_canvas->pageRects.size(); ++i)
        if (m_canvas->pageRects[i].bottom() + kSpacing >= y) return i;
    return qMax(0, int(m_canvas->pageRects.size()) - 1);
}

void PdfViewer::scrollToPage(int page, qreal yPoints)
{
    if (page < 0 || page >= m_canvas->pageRects.size()) return;
    const QRectF r = m_canvas->pageRects[page];
    m_scroll->verticalScrollBar()->setValue(int(r.top() + yPoints * m_zoom) - (yPoints > 0 ? 0 : kMargin));
    updatePageLabel();
}

void PdfViewer::updatePageLabel()
{
    QSignalBlocker block(m_pageBox);
    m_pageBox->setValue(currentPage() + 1);
}

// ---------------------------------------------------------------------------
// SyncTeX

static QHash<QString, QString> parseSynctex(const QString &out)
{
    // Takes the first record of "Key:value" lines.
    QHash<QString, QString> rec;
    for (const QString &line : out.split('\n')) {
        const int c = line.indexOf(':');
        if (c <= 0) continue;
        const QString key = line.left(c).trimmed();
        if (!rec.contains(key)) rec.insert(key, line.mid(c + 1).trimmed());
    }
    return rec;
}

static QString runSynctex(const QStringList &args, QString *error)
{
    const QString synctex = SettingsDialog::findProgram("synctex");
    if (synctex.isEmpty()) {
        *error = QObject::tr("The synctex program was not found (it comes with MacTeX). Check Options > Settings.");
        return {};
    }
    QProcess p;
    p.setProcessEnvironment(SettingsDialog::environment());
    p.start(synctex, args);
    if (!p.waitForFinished(8000)) {
        *error = QObject::tr("synctex did not answer.");
        return {};
    }
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}

void PdfViewer::inverseSearch(int page, const QPointF &pt)
{
    QString error;
    const QString out = runSynctex({"edit", "-o", QString("%1:%2:%3:%4").arg(page + 1)
                                                     .arg(pt.x(), 0, 'f', 2).arg(pt.y(), 0, 'f', 2).arg(m_path)},
                                   &error);
    if (!error.isEmpty()) { emit message(error); return; }
    const auto rec = parseSynctex(out);
    QString input = rec.value("Input");
    const int line = rec.value("Line").toInt();
    if (input.isEmpty() || line <= 0) {
        emit message(tr("No source position here. Was the PDF compiled with -synctex=1 (the default in MasterTex)?"));
        return;
    }
    if (QFileInfo(input).isRelative()) input = QFileInfo(m_path).absoluteDir().absoluteFilePath(input);
    input = DocumentManager::normalize(QDir::cleanPath(input));
    emit sourceRequested(input, line);
}

bool PdfViewer::forwardSearch(const QString &texFile, int line, int column)
{
    if (!isLoaded()) return false;
    QString error;
    const QString out = runSynctex({"view", "-i", QString("%1:%2:%3").arg(line).arg(qMax(0, column)).arg(texFile),
                                    "-o", m_path, "-d", QFileInfo(m_path).absolutePath()},
                                   &error);
    if (!error.isEmpty()) { emit message(error); return false; }
    const auto rec = parseSynctex(out);
    const int page = rec.value("Page").toInt() - 1;
    if (page < 0 || page >= m_doc->pageCount()) {
        emit message(tr("This line has no position in the PDF (recompile if you just changed it)."));
        return false;
    }
    const qreal h = rec.value("h").toDouble(), v = rec.value("v").toDouble();
    const qreal W = rec.value("W").toDouble(), H = rec.value("H").toDouble();
    QRectF box;
    if (W > 0 && H > 0) {
        box = QRectF(h, v - H, W, H);
    } else {
        const qreal y = rec.value("y").toDouble();
        box = QRectF(0, y - 10, m_doc->pagePointSize(page).width(), 13);
    }
    m_canvas->highlightPage = page;
    m_canvas->highlightRect = box;
    m_canvas->highlightTimer.start();
    // Put the spot about a third of the way down the pane.
    const QRectF r = m_canvas->pageRects[page];
    m_scroll->verticalScrollBar()->setValue(int(r.top() + box.top() * m_zoom - m_scroll->viewport()->height() / 3));
    m_scroll->ensureVisible(int(r.left() + box.center().x() * m_zoom), int(r.top() + box.center().y() * m_zoom), 40, 40);
    m_canvas->update();
    updatePageLabel();
    return true;
}
