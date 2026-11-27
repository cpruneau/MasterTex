#include "LatexHighlighter.h"

#include <QApplication>
#include <QPalette>

static bool darkMode()
{
    return qApp->palette().color(QPalette::Base).lightness() < 128;
}

LatexHighlighter::LatexHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc)
{
    const bool dark = darkMode();
    auto color = [dark](const char *light, const char *darkc) { return QColor(dark ? darkc : light); };

    QTextCharFormat command;
    command.setForeground(color("#1f4fbf", "#7aa2ff"));

    QTextCharFormat sectioning;
    sectioning.setForeground(color("#8a1c7c", "#e08ad4"));
    sectioning.setFontWeight(QFont::Bold);

    QTextCharFormat environment;
    environment.setForeground(color("#7a4a00", "#e8b45a"));

    QTextCharFormat keyArg;
    keyArg.setForeground(color("#00756a", "#5fd3c4"));

    m_math.setForeground(color("#1a7f1a", "#7fd67f"));
    m_comment.setForeground(color("#888888", "#7f7f7f"));
    m_comment.setFontItalic(true);

    m_rules.push_back({QRegularExpression(R"(\\[A-Za-z@]+\*?|\\[^A-Za-z\s])"), command});
    m_rules.push_back({QRegularExpression(
        R"(\\(?:part|chapter|section|subsection|subsubsection|paragraph)\*?(?:\[[^\]]*\])?\{[^}]*\})"),
        sectioning});
    m_rules.push_back({QRegularExpression(R"(\\(?:begin|end)\{([^}]*)\})"), environment, 1});
    m_rules.push_back({QRegularExpression(
        R"(\\(?:label|ref|eqref|pageref|autoref|cref|Cref|nameref|cite\w*|parencite|textcite|autocite|footcite|nocite)(?:\[[^\]]*\]){0,2}\{([^}]*)\})"),
        keyArg, 1});
}

void LatexHighlighter::highlightBlock(const QString &text)
{
    for (const Rule &r : m_rules) {
        auto it = r.re.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            setFormat(m.capturedStart(r.group), m.capturedLength(r.group), r.fmt);
        }
    }

    // Inline math $...$ and \( ... \) on a single line
    static const QRegularExpression math(R"((?<!\\)\$[^$]+(?<!\\)\$|\\\(.*?\\\))");
    auto it = math.globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        setFormat(m.capturedStart(), m.capturedLength(), m_math);
    }

    // Comments last so they win
    static const QRegularExpression comment(R"((?<!\\)%.*$)");
    auto c = comment.match(text);
    if (c.hasMatch())
        setFormat(c.capturedStart(), c.capturedLength(), m_comment);
}
