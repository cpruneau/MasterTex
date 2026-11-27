#pragma once
// PdfViewer: the built-in PDF pane.
// Renders pages lazily (only what is on screen), keeps the scroll position
// when the PDF is rebuilt, and talks to the `synctex` tool so that
//  - Cmd-click (or double-click) in the PDF opens the matching source line, and
//  - "Show in PDF" from the editor scrolls here and highlights the spot.

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QPdfDocument;
class QScrollArea;
class QLabel;
class QSpinBox;
class QBuffer;

class PdfCanvas;

class PdfViewer : public QWidget {
    Q_OBJECT
public:
    explicit PdfViewer(QWidget *parent = nullptr);
    ~PdfViewer() override;

    // Loads (or reloads) a PDF; keeps the reading position when it is the same file.
    bool load(const QString &pdfPath);
    void reload();
    QString pdfPath() const { return m_path; }
    bool isLoaded() const;

    // Source -> PDF: scrolls to and highlights the place for that line.
    bool forwardSearch(const QString &texFile, int line, int column);

signals:
    void sourceRequested(const QString &file, int line);
    void message(const QString &text);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    friend class PdfCanvas;
    void inverseSearch(int page, const QPointF &pointInPage);   // page is 0-based, point in PDF points
    void relayout();
    void setZoom(qreal pixelsPerPoint, bool fitWidth);
    void scrollToPage(int page, qreal yPoints = 0);
    int currentPage() const;
    void updatePageLabel();

    QPdfDocument *m_doc;
    QBuffer *m_buffer = nullptr;
    QByteArray m_bytes;
    QString m_path;
    QScrollArea *m_scroll;
    PdfCanvas *m_canvas;
    QSpinBox *m_pageBox;
    QLabel *m_pageCount;
    QLabel *m_zoomLabel;
    bool m_fitWidth = true;
    qreal m_zoom = 1.0;   // pixels per PDF point
};
