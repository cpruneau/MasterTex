#pragma once
#include <QHash>
#include <QPlainTextEdit>
#include <QTextDocument>

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QTextDocument *doc, QWidget *parent = nullptr);

    QString filePath() const;
    void gotoLine(int line);
    int currentLine() const;

    // Inserts a snippet. "%|" marks where the cursor goes; if text is
    // selected, the selection replaces the first "%|".
    void insertSnippet(const QString &snippet);
    void toggleComment();
    void setEditorFontSize(int pt);

    // Line operations
    void deleteLines();
    void cutLineOrSelection();
    void gotoLineStart();
    void gotoLineEnd();
    void deleteToLineEnd();
    void deleteToLineStart();
    void moveLines(int direction);   // -1 up, +1 down
    void duplicateLines();
    bool sortLines();

    // Text operations
    enum class CaseOp { Lower, Upper, TitleStrict, TitleSmart };
    void changeCase(CaseOp op);

    // Occurrences
    QString wordOrSelection(bool selectWord = true);
    bool selectOccurrence(bool backward);
    int highlightAll(const QRegularExpression &re);
    void clearHighlights();

    // Bookmarks 1..9 (stored on the text block, so they move with the text)
    void toggleBookmark(int n);
    bool gotoBookmark(int n);
    void clearBookmarks();

    // Navigation through recent edits
    bool gotoChange(bool next);

    // Build markers in the gutter: line -> 0 error, 1 warning
    void setLogMarkers(const QHash<int, int> &markers);

    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent *event);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void refreshExtraSelections();
    QTextCursor selectWholeLines() const;

    QWidget *m_lineNumbers;
    QList<QTextEdit::ExtraSelection> m_matches;
    QHash<int, int> m_logMarkers;
};
