#pragma once
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

class LatexHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit LatexHighlighter(QTextDocument *doc);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule {
        QRegularExpression re;
        QTextCharFormat fmt;
        int group = 0;
    };
    QVector<Rule> m_rules;
    QTextCharFormat m_comment;
    QTextCharFormat m_math;
};
