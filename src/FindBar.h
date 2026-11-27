#pragma once
// FindBar: find / replace panel under the editor tabs.
// Plain text, whole-word, case-sensitive or regular-expression search;
// replacement may use \1, \2 … when regular expressions are on.

#include <QPointer>
#include <QRegularExpression>
#include <QWidget>

class CodeEditor;
class QCheckBox;
class QLineEdit;
class QLabel;

class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget *parent = nullptr);

    void setEditor(CodeEditor *editor);
    void showBar(bool withReplace);   // opens the bar, pre-filled with the selection
    QString term() const;

    bool findNext(bool backward = false);
    int count();
    int selectAllMatches();
    bool replaceNext(bool backward = false);
    int replaceAll();

signals:
    void message(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    QRegularExpression regex(bool anchored = false) const;
    bool ensureTerm();

    QPointer<CodeEditor> m_editor;
    QLineEdit *m_find;
    QLineEdit *m_replace;
    QWidget *m_replaceRow;
    QCheckBox *m_case;
    QCheckBox *m_word;
    QCheckBox *m_regex;
    QLabel *m_info;
};
