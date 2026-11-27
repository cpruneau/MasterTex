#include "FindBar.h"
#include "CodeEditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QToolButton>
#include <functional>

FindBar::FindBar(QWidget *parent) : QWidget(parent)
{
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(6, 4, 6, 4);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);

    m_find = new QLineEdit;
    m_find->setPlaceholderText(tr("Find"));
    m_find->setClearButtonEnabled(true);
    m_replace = new QLineEdit;
    m_replace->setPlaceholderText(tr("Replace with"));
    m_case = new QCheckBox(tr("Match case"));
    m_word = new QCheckBox(tr("Whole word"));
    m_regex = new QCheckBox(tr("Regex"));
    m_info = new QLabel;
    m_info->setMinimumWidth(120);

    auto button = [this](const QString &text, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(text);
        b->setToolTip(tip);
        b->setAutoDefault(false);
        connect(b, &QPushButton::clicked, this, [fn]() { fn(); });
        return b;
    };

    auto *findRow = new QHBoxLayout;
    findRow->addWidget(m_find, 1);
    findRow->addWidget(button(tr("◀ Prev"), tr("Find previous"), [this]() { findNext(true); }));
    findRow->addWidget(button(tr("Next ▶"), tr("Find next"), [this]() { findNext(false); }));
    findRow->addWidget(button(tr("Count"), tr("Count matches"), [this]() { count(); }));
    findRow->addWidget(button(tr("Select All"), tr("Highlight all matches"), [this]() { selectAllMatches(); }));
    findRow->addWidget(m_case);
    findRow->addWidget(m_word);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_info);
    auto *close = new QToolButton;
    close->setText("✕");
    close->setAutoRaise(true);
    close->setToolTip(tr("Close (Esc)"));
    connect(close, &QToolButton::clicked, this, [this]() {
        hide();
        if (m_editor) { m_editor->clearHighlights(); m_editor->setFocus(); }
    });
    findRow->addWidget(close);
    grid->addLayout(findRow, 0, 0);

    m_replaceRow = new QWidget;
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->addWidget(m_replace, 1);
    replaceRow->addWidget(button(tr("Replace ◀"), tr("Replace and find previous"), [this]() { replaceNext(true); }));
    replaceRow->addWidget(button(tr("Replace ▶"), tr("Replace and find next"), [this]() { replaceNext(false); }));
    replaceRow->addWidget(button(tr("Replace All"), tr("Replace every match in this file"), [this]() { replaceAll(); }));
    replaceRow->addStretch();
    grid->addWidget(m_replaceRow, 1, 0);

    connect(m_find, &QLineEdit::returnPressed, this, [this]() {
        findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(m_replace, &QLineEdit::returnPressed, this, [this]() { replaceNext(false); });
    connect(m_find, &QLineEdit::textChanged, this, [this]() { m_info->clear(); });
    connect(this, &FindBar::message, m_info, &QLabel::setText);
}

void FindBar::setEditor(CodeEditor *editor)
{
    m_editor = editor;
    m_info->clear();
}

QString FindBar::term() const { return m_find->text(); }

void FindBar::showBar(bool withReplace)
{
    if (m_editor) {
        const QString sel = m_editor->textCursor().selectedText();
        if (!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator))
            m_find->setText(m_regex->isChecked() ? QRegularExpression::escape(sel) : sel);
    }
    m_replaceRow->setVisible(withReplace);
    show();
    m_find->setFocus();
    m_find->selectAll();
}

void FindBar::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        hide();
        if (m_editor) { m_editor->clearHighlights(); m_editor->setFocus(); }
        return;
    }
    QWidget::keyPressEvent(e);
}

QRegularExpression FindBar::regex(bool anchored) const
{
    QString pattern = m_regex->isChecked() ? m_find->text() : QRegularExpression::escape(m_find->text());
    if (m_word->isChecked()) pattern = "\\b" + pattern + "\\b";
    if (anchored) pattern = QRegularExpression::anchoredPattern(pattern);
    QRegularExpression::PatternOptions opts = QRegularExpression::UseUnicodePropertiesOption;
    if (!m_case->isChecked()) opts |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(pattern, opts);
}

bool FindBar::ensureTerm()
{
    if (!m_editor) return false;
    if (m_find->text().isEmpty()) {
        showBar(m_replaceRow->isVisible());
        return false;
    }
    if (!regex().isValid()) {
        emit message(tr("Invalid regular expression"));
        return false;
    }
    return true;
}

bool FindBar::findNext(bool backward)
{
    if (!ensureTerm()) return false;
    const QRegularExpression re = regex();
    QTextDocument::FindFlags flags;
    if (backward) flags |= QTextDocument::FindBackward;
    if (m_editor->find(re, flags)) { emit message(QString()); return true; }
    QTextCursor c = m_editor->textCursor();
    c.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    m_editor->setTextCursor(c);
    if (m_editor->find(re, flags)) {
        emit message(backward ? tr("Wrapped to the end") : tr("Wrapped to the top"));
        return true;
    }
    emit message(tr("Not found"));
    return false;
}

int FindBar::count()
{
    if (!ensureTerm()) return 0;
    const QRegularExpression re = regex();
    int n = 0;
    QTextCursor c(m_editor->document());
    while (true) {
        c = m_editor->document()->find(re, c);
        if (c.isNull()) break;
        if (c.selectionStart() == c.selectionEnd() && !c.movePosition(QTextCursor::NextCharacter)) break;
        ++n;
    }
    emit message(tr("%n match(es)", nullptr, n));
    return n;
}

int FindBar::selectAllMatches()
{
    if (!ensureTerm()) return 0;
    const int n = m_editor->highlightAll(regex());
    emit message(tr("%n match(es) highlighted", nullptr, n));
    return n;
}

bool FindBar::replaceNext(bool backward)
{
    if (!ensureTerm()) return false;
    QTextCursor c = m_editor->textCursor();
    const QString sel = c.selectedText();
    const QRegularExpression anchored = regex(true);
    if (c.hasSelection() && anchored.match(sel).hasMatch()) {
        QString replacement = m_replace->text();
        if (m_regex->isChecked()) {
            QString s = sel;
            replacement = s.replace(anchored, m_replace->text());
        }
        c.insertText(replacement);
        if (backward) {   // stand before the replaced text so the backward search moves on
            c.setPosition(c.position() - replacement.size());
            m_editor->setTextCursor(c);
        }
    }
    return findNext(backward);
}

int FindBar::replaceAll()
{
    if (!ensureTerm()) return 0;
    const QRegularExpression re = regex();
    const QRegularExpression anchored = regex(true);
    QTextDocument *doc = m_editor->document();
    QTextCursor edit(doc);
    edit.beginEditBlock();
    QTextCursor c(doc);
    int n = 0;
    while (true) {
        c = doc->find(re, c);
        if (c.isNull() || c.selectionStart() == c.selectionEnd()) break;
        QString replacement = m_replace->text();
        if (m_regex->isChecked()) {
            QString s = c.selectedText();
            replacement = s.replace(anchored, m_replace->text());
        }
        c.insertText(replacement);
        ++n;
    }
    edit.endEditBlock();
    emit message(tr("Replaced %n match(es)", nullptr, n));
    return n;
}
