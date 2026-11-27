#pragma once
// SymbolPanel: clickable symbol panes (Greek, operators, relations, arrows,
// physics). Clicking a symbol emits its LaTeX code for insertion.

#include <QVector>
#include <QWidget>

class SymbolPanel : public QWidget {
    Q_OBJECT
public:
    explicit SymbolPanel(QWidget *parent = nullptr);

    struct Symbol { const char *glyph; const char *latex; };
    struct Group { const char *name; QVector<Symbol> symbols; };
    static const QVector<Group> &groups();

signals:
    void insertRequested(const QString &latex);
};
