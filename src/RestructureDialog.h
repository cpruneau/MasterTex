#pragma once
// RestructureDialog: drag-and-drop editing of the book's structure.
// The tree is a draft — nothing changes on disk until OK. The right-hand side
// always shows the full plan of what OK will do.

#include "Restructure.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class RestructureDialog : public QDialog {
    Q_OBJECT
public:
    RestructureDialog(const Restructure::Model &model, QWidget *parent = nullptr);

    const Restructure::Plan &plan() const { return m_plan; }
    bool gitCommit() const;

private:
    void rebuild(const Restructure::Tree &tree);
    Restructure::Tree currentTree() const;
    void updatePlan();
    void addItems(QTreeWidgetItem *parentItem, int nodeId, const Restructure::Tree &tree);

    const Restructure::Model &m_model;
    Restructure::Plan m_plan;
    QVector<Restructure::Tree> m_history;   // for "Undo Last Drag"
    QTreeWidget *m_tree;
    QPlainTextEdit *m_planView;
    QLabel *m_status;
    QCheckBox *m_figures;
    QCheckBox *m_git;
    QPushButton *m_ok;
    QPushButton *m_undoDrag;
};
