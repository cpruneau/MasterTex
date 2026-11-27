#include "CodeEditor.h"
#include "DocumentManager.h"

#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTextBlock>
#include <algorithm>

namespace {

const int kGutterMarks = 18;   // space on the left of line numbers for bookmarks/markers

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *e) : QWidget(e), m_editor(e) {}
    QSize sizeHint() const override { return {m_editor->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *ev) override { m_editor->paintLineNumbers(ev); }

private:
    CodeEditor *m_editor;
};

struct BookmarkData : QTextBlockUserData {
    int number = 0;
};

int bookmarkOf(const QTextBlock &b)
{
    auto *d = dynamic_cast<BookmarkData *>(b.userData());
    return d ? d->number : 0;
}

// Recent edit positions per document; cursors move with the text.
QHash<QTextDocument *, QList<QTextCursor>> &changeLog()
{
    static QHash<QTextDocument *, QList<QTextCursor>> log;
    return log;
}

void trackChanges(QTextDocument *doc)
{
    if (doc->property("mtTracked").toBool()) return;
    doc->setProperty("mtTracked", true);
    QObject::connect(doc, &QTextDocument::contentsChange, doc, [doc](int pos, int, int added) {
        auto &log = changeLog()[doc];
        QTextCursor c(doc);
        c.setPosition(qBound(0, pos + added, doc->characterCount() - 1));
        if (!log.isEmpty() && log.last().block() == c.block()) {
            log.last() = c;
            return;
        }
        log.append(c);
        if (log.size() > 200) log.removeFirst();
    });
    QObject::connect(doc, &QObject::destroyed, [doc]() { changeLog().remove(doc); });
}

QString rangeText(QTextDocument *doc, int a, int b)
{
    QTextCursor c(doc);
    c.setPosition(a);
    c.setPosition(b, QTextCursor::KeepAnchor);
    return c.selectedText().replace(QChar::ParagraphSeparator, '\n');
}

} // namespace

CodeEditor::CodeEditor(QTextDocument *doc, QWidget *parent) : QPlainTextEdit(parent)
{
    setDocument(doc);
    trackChanges(doc);
    m_lineNumbers = new LineNumberArea(this);

    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(QSettings().value("editor/fontSize", 13).toInt());
    setFont(f);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::refreshExtraSelections);
    updateLineNumberAreaWidth();
    refreshExtraSelections();
}

QString CodeEditor::filePath() const
{
    return DocumentManager::instance()->pathOf(document());
}

void CodeEditor::setEditorFontSize(int pt)
{
    QFont f = font();
    f.setPointSize(pt);
    setFont(f);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
    updateLineNumberAreaWidth();
}

// ---------------------------------------------------------------------------
// Gutter

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) { max /= 10; ++digits; }
    return kGutterMarks + 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * qMax(3, digits);
}

void CodeEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumbers->scroll(0, dy);
    else
        m_lineNumbers->update(0, rect.y(), m_lineNumbers->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void CodeEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    m_lineNumbers->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::focusInEvent(QFocusEvent *e)
{
    // The document may be shared with an editor of another width;
    // re-apply this editor's width so line wrapping is right here.
    QResizeEvent re(size(), size());
    resizeEvent(&re);
    QPlainTextEdit::focusInEvent(e);
}

void CodeEditor::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape && !m_matches.isEmpty()) {
        clearHighlights();
        return;
    }
    // Keep indentation on Enter
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        const QString line = textCursor().block().text();
        int k = 0;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        QPlainTextEdit::keyPressEvent(e);
        if (k > 0) insertPlainText(line.left(k));
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void CodeEditor::mousePressEvent(QMouseEvent *e)
{
    QPlainTextEdit::mousePressEvent(e);   // moves the cursor to the click
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
        emit syncToPdfRequested();
}

void CodeEditor::paintLineNumbers(QPaintEvent *event)
{
    QPainter painter(m_lineNumbers);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    painter.fillRect(event->rect(), pal.color(QPalette::AlternateBase));

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int h = fontMetrics().height();
    QFont small = painter.font();
    small.setPixelSize(qMax(8, h - 6));
    small.setBold(true);
    const QFont normal = painter.font();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int line = number + 1;
            if (m_logMarkers.contains(line)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(m_logMarkers.value(line) == 0 ? "#d9443a" : "#e0a800"));
                painter.drawEllipse(QRectF(4, top + h / 2.0 - 5, 10, 10));
            }
            if (int bm = bookmarkOf(block)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor("#2d6fd6"));
                QRectF r(1, top + 1, kGutterMarks - 3, h - 2);
                painter.drawRoundedRect(r, 3, 3);
                painter.setPen(Qt::white);
                painter.setFont(small);
                painter.drawText(r, Qt::AlignCenter, QString::number(bm));
                painter.setFont(normal);
            }
            painter.setPen(pal.color(QPalette::PlaceholderText));
            painter.drawText(kGutterMarks, top, m_lineNumbers->width() - kGutterMarks - 6, h, Qt::AlignRight,
                             QString::number(line));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeEditor::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> all;
    QTextEdit::ExtraSelection line;
    QColor c = palette().color(QPalette::Highlight);
    c.setAlpha(28);
    line.format.setBackground(c);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    all << line << m_matches;
    setExtraSelections(all);
}

// ---------------------------------------------------------------------------
// Basic navigation and insertion

void CodeEditor::gotoLine(int line)
{
    QTextBlock b = document()->findBlockByNumber(qMax(0, line - 1));
    if (!b.isValid()) return;
    QTextCursor c(b);
    setTextCursor(c);
    centerCursor();
    setFocus();
}

int CodeEditor::currentLine() const
{
    return textCursor().blockNumber() + 1;
}

void CodeEditor::insertSnippet(const QString &snippet)
{
    QTextCursor c = textCursor();
    const QString selected = c.selectedText().replace(QChar::ParagraphSeparator, '\n');
    QString text = snippet;
    int cursorOffset = -1;
    int idx = text.indexOf("%|");
    if (idx >= 0) {
        text.remove(idx, 2);
        if (!selected.isEmpty()) {
            text.insert(idx, selected);
            idx += selected.size();
        }
        cursorOffset = idx;
        text.remove("%|");
    }
    c.beginEditBlock();
    const int start = c.selectionStart();
    c.insertText(text);
    c.endEditBlock();
    if (cursorOffset >= 0) {
        c.setPosition(start + cursorOffset);
        setTextCursor(c);
    }
    setFocus();
}

void CodeEditor::toggleComment()
{
    QTextCursor c = textCursor();
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(c.selectionEnd());
    bool allCommented = true;
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        if (!b.text().trimmed().isEmpty() && !b.text().trimmed().startsWith('%')) allCommented = false;
        if (b == last) break;
    }
    c.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next()) {
        QTextCursor bc(b);
        if (allCommented) {
            const QString t = b.text();
            int k = t.indexOf('%');
            if (k >= 0) {
                bc.setPosition(b.position() + k);
                int len = (k + 1 < t.size() && t[k + 1] == ' ') ? 2 : 1;
                bc.setPosition(b.position() + k + len, QTextCursor::KeepAnchor);
                bc.removeSelectedText();
            }
        } else {
            bc.insertText("% ");
        }
        if (b == last) break;
    }
    c.endEditBlock();
}

// ---------------------------------------------------------------------------
// Line operations

// The lines touched by the cursor/selection, from start of first line to end
// of last line (without its newline).
QTextCursor CodeEditor::selectWholeLines() const
{
    QTextCursor c = textCursor();
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(c.selectionEnd());
    // A selection ending at the very start of a line doesn't include that line.
    if (c.hasSelection() && c.selectionEnd() == last.position() && last != first) last = last.previous();
    QTextCursor r(document());
    r.setPosition(first.position());
    r.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    return r;
}

void CodeEditor::deleteLines()
{
    QTextCursor r = selectWholeLines();
    int a = r.selectionStart(), b = r.selectionEnd();
    if (b < document()->characterCount() - 1) ++b;         // take the newline after
    else if (a > 0) --a;                                     // last line: take the newline before
    r.setPosition(a);
    r.setPosition(b, QTextCursor::KeepAnchor);
    r.removeSelectedText();
    setTextCursor(r);
}

void CodeEditor::cutLineOrSelection()
{
    if (textCursor().hasSelection()) { cut(); return; }
    QTextCursor r = selectWholeLines();
    int b = r.selectionEnd();
    if (b < document()->characterCount() - 1) ++b;
    r.setPosition(r.selectionStart());
    r.setPosition(b, QTextCursor::KeepAnchor);
    setTextCursor(r);
    cut();
}

void CodeEditor::gotoLineStart()
{
    QTextCursor c = textCursor();
    c.movePosition(QTextCursor::StartOfBlock);
    setTextCursor(c);
}

void CodeEditor::gotoLineEnd()
{
    QTextCursor c = textCursor();
    c.movePosition(QTextCursor::EndOfBlock);
    setTextCursor(c);
}

void CodeEditor::deleteToLineEnd()
{
    QTextCursor c = textCursor();
    c.clearSelection();
    if (c.atBlockEnd()) c.deleteChar();   // like Emacs ^K: at the end, join with the next line
    else {
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    setTextCursor(c);
}

void CodeEditor::deleteToLineStart()
{
    QTextCursor c = textCursor();
    c.clearSelection();
    c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setTextCursor(c);
}

void CodeEditor::moveLines(int direction)
{
    QTextCursor sel = textCursor();
    QTextCursor r = selectWholeLines();
    const int start = r.selectionStart(), end = r.selectionEnd();
    QTextBlock first = document()->findBlock(start);
    QTextBlock last = document()->findBlock(end);
    QTextBlock neighbour = direction < 0 ? first.previous() : last.next();
    if (!neighbour.isValid()) return;

    const QString lines = rangeText(document(), start, end);
    const QString other = neighbour.text();
    const int anchorOff = sel.anchor() - start, posOff = sel.position() - start;

    const int nPos = neighbour.position();
    const int nEnd = neighbour.position() + neighbour.length() - 1;
    QTextCursor e(document());
    int newStart;
    e.beginEditBlock();
    if (direction < 0) {
        e.setPosition(nPos);
        e.setPosition(end, QTextCursor::KeepAnchor);
        e.insertText(lines + "\n" + other);
        newStart = nPos;
    } else {
        e.setPosition(start);
        e.setPosition(nEnd, QTextCursor::KeepAnchor);
        e.insertText(other + "\n" + lines);
        newStart = start + other.size() + 1;
    }
    e.endEditBlock();
    QTextCursor c(document());
    c.setPosition(newStart + anchorOff);
    c.setPosition(newStart + posOff, QTextCursor::KeepAnchor);
    setTextCursor(c);
}

void CodeEditor::duplicateLines()
{
    QTextCursor r = selectWholeLines();
    const int start = r.selectionStart(), end = r.selectionEnd();
    const QString lines = rangeText(document(), start, end);
    const int offset = textCursor().position() - start;
    QTextCursor e(document());
    e.setPosition(end);
    e.insertText("\n" + lines);
    QTextCursor c(document());
    c.setPosition(end + 1 + offset);
    setTextCursor(c);
}

bool CodeEditor::sortLines()
{
    QTextCursor r = selectWholeLines();
    const QString text = rangeText(document(), r.selectionStart(), r.selectionEnd());
    QStringList lines = text.split('\n');
    if (lines.size() < 2) return false;
    // Natural order: case-insensitive, and numbers compare by value (item2 < item10).
    static const QRegularExpression chunk(R"(\d+|\D+)");
    auto chunks = [](const QString &s) {
        QStringList out;
        auto it = chunk.globalMatch(s);
        while (it.hasNext()) out << it.next().captured();
        return out;
    };
    std::stable_sort(lines.begin(), lines.end(), [&](const QString &a, const QString &b) {
        const QStringList ca = chunks(a), cb = chunks(b);
        for (int i = 0; i < qMin(ca.size(), cb.size()); ++i) {
            const bool da = ca[i][0].isDigit(), db = cb[i][0].isDigit();
            int c;
            if (da && db) {
                const qulonglong x = ca[i].toULongLong(), y = cb[i].toULongLong();
                c = x != y ? (x < y ? -1 : 1) : ca[i].compare(cb[i]);
            } else {
                c = ca[i].compare(cb[i], Qt::CaseInsensitive);
            }
            if (c != 0) return c < 0;
        }
        return ca.size() < cb.size();
    });
    const int start = r.selectionStart();
    r.insertText(lines.join('\n'));
    QTextCursor c(document());
    c.setPosition(start);
    c.setPosition(r.position(), QTextCursor::KeepAnchor);
    setTextCursor(c);
    return true;
}

// ---------------------------------------------------------------------------
// Text operations

static QString titleCase(const QString &text, bool smart)
{
    static const QSet<QString> small = {"a", "an", "the", "and", "but", "or", "nor", "for", "so", "yet",
                                        "as", "at", "by", "in", "of", "off", "on", "per", "to", "up",
                                        "via", "vs", "from", "into", "onto", "over", "with"};
    static const QRegularExpression wordRe(R"((\\?)([A-Za-z\x{00C0}-\x{024F}][\w'\x{00C0}-\x{024F}]*))");
    QString out;
    int last = 0, index = 0;
    QVector<QRegularExpressionMatch> matches;
    auto it = wordRe.globalMatch(text);
    while (it.hasNext()) matches << it.next();
    for (const auto &m : matches) {
        out += text.mid(last, m.capturedStart() - last);
        QString w = m.captured(2);
        const bool isCommand = !m.captured(1).isEmpty();
        const bool firstOrLast = index == 0 || index == matches.size() - 1;
        if (isCommand) {
            // leave LaTeX commands alone
        } else if (!smart) {
            w = w.left(1).toUpper() + w.mid(1).toLower();
        } else {
            const bool hasInnerCaps = std::any_of(w.begin() + 1, w.end(), [](QChar ch) { return ch.isUpper(); });
            if (hasInnerCaps) {
                // acronyms and names like QCD, LaTeX, McLerran stay as written
            } else if (!firstOrLast && small.contains(w.toLower())) {
                w = w.toLower();
            } else {
                w = w.left(1).toUpper() + w.mid(1);
            }
        }
        out += m.captured(1) + w;
        last = m.capturedEnd();
        ++index;
    }
    out += text.mid(last);
    return out;
}

void CodeEditor::changeCase(CaseOp op)
{
    QTextCursor c = textCursor();
    if (!c.hasSelection()) c.select(QTextCursor::WordUnderCursor);
    if (!c.hasSelection()) return;
    const int a = c.selectionStart();
    const QString text = c.selectedText().replace(QChar::ParagraphSeparator, '\n');
    QString out;
    switch (op) {
    case CaseOp::Lower: out = text.toLower(); break;
    case CaseOp::Upper: out = text.toUpper(); break;
    case CaseOp::TitleStrict: out = titleCase(text, false); break;
    case CaseOp::TitleSmart: out = titleCase(text, true); break;
    }
    c.insertText(out);
    c.setPosition(a);
    c.setPosition(a + out.size(), QTextCursor::KeepAnchor);
    setTextCursor(c);
}

// ---------------------------------------------------------------------------
// Occurrences

QString CodeEditor::wordOrSelection(bool selectWord)
{
    QTextCursor c = textCursor();
    if (!c.hasSelection()) {
        c.select(QTextCursor::WordUnderCursor);
        if (selectWord) setTextCursor(c);
    }
    return c.selectedText().replace(QChar::ParagraphSeparator, '\n');
}

bool CodeEditor::selectOccurrence(bool backward)
{
    if (!textCursor().hasSelection()) {
        // First press: select the word under the cursor.
        return !wordOrSelection(true).isEmpty();
    }
    const QString term = textCursor().selectedText();
    QTextDocument::FindFlags flags = QTextDocument::FindCaseSensitively;
    if (backward) flags |= QTextDocument::FindBackward;
    if (find(term, flags)) return true;
    QTextCursor c = textCursor();
    c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    setTextCursor(c);
    return find(term, flags);
}

int CodeEditor::highlightAll(const QRegularExpression &re)
{
    m_matches.clear();
    if (re.pattern().isEmpty() || !re.isValid()) { refreshExtraSelections(); return 0; }
    QTextCharFormat fmt;
    fmt.setBackground(QColor(255, 210, 0, 110));
    QTextCursor c(document());
    while (true) {
        c = document()->find(re, c);
        if (c.isNull()) break;
        if (c.selectionStart() == c.selectionEnd()) {   // empty match: avoid looping forever
            if (!c.movePosition(QTextCursor::NextCharacter)) break;
            continue;
        }
        QTextEdit::ExtraSelection s;
        s.cursor = c;
        s.format = fmt;
        m_matches << s;
        if (m_matches.size() > 20000) break;
    }
    refreshExtraSelections();
    return m_matches.size();
}

void CodeEditor::clearHighlights()
{
    m_matches.clear();
    refreshExtraSelections();
}

// ---------------------------------------------------------------------------
// Bookmarks

void CodeEditor::toggleBookmark(int n)
{
    QTextBlock current = textCursor().block();
    const bool had = bookmarkOf(current) == n;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (bookmarkOf(b) == n) b.setUserData(nullptr);
    if (!had) {
        auto *d = new BookmarkData;
        d->number = n;
        current.setUserData(d);
    }
    m_lineNumbers->update();
}

bool CodeEditor::gotoBookmark(int n)
{
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (bookmarkOf(b) == n) {
            gotoLine(b.blockNumber() + 1);
            return true;
        }
    return false;
}

void CodeEditor::clearBookmarks()
{
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        if (bookmarkOf(b)) b.setUserData(nullptr);
    m_lineNumbers->update();
}

// ---------------------------------------------------------------------------
// Changes and markers

bool CodeEditor::gotoChange(bool next)
{
    QList<int> lines;
    for (const QTextCursor &c : changeLog().value(document())) lines << c.blockNumber() + 1;
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    const int cur = currentLine();
    if (next) {
        for (int l : lines) if (l > cur) { gotoLine(l); return true; }
    } else {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) if (*it < cur) { gotoLine(*it); return true; }
    }
    return false;
}

void CodeEditor::setLogMarkers(const QHash<int, int> &markers)
{
    m_logMarkers = markers;
    m_lineNumbers->update();
}
