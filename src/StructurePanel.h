#pragma once
// StructurePanel: the document structure browser.
//  - Outline:    parts/chapters/sections across all files, with figures and
//                tables (caption, label, images) nested under their section.
//  - Files:      the \input/\include tree, plus bibliography files.
//  - References: every label with its definition and all uses, every
//                citation key with its uses, every image with its uses.
// Every item knows its file and line; activating it opens the place.

#include <QWidget>

class Project;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;

class StructurePanel : public QWidget {
    Q_OBJECT
public:
    explicit StructurePanel(QWidget *parent = nullptr);
    ~StructurePanel() override;
    void setProject(Project *p);
    void refresh();

signals:
    void openLocation(const QString &file, int line);
    void openLocationInNewWindow(const QString &file, int line);

private:
    QTreeWidget *makeTree();
    void buildOutline();
    void buildFiles();
    void buildReferences();
    void applyFilter(const QString &text);
    void showContextMenu(QTreeWidget *tree, const QPoint &pos);

    struct TreeState;
    TreeState *m_outlineState;
    TreeState *m_filesState;
    TreeState *m_refsState;
    Project *m_project = nullptr;
    QTabWidget *m_tabs;
    QLineEdit *m_filter;
    QTreeWidget *m_outline;
    QTreeWidget *m_files;
    QTreeWidget *m_refs;
};
