#include "RestructureDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

using namespace Restructure;

namespace {

enum { IdRole = Qt::UserRole + 1 };

// A tree that tells us after every drop.
class DraftTree : public QTreeWidget {
public:
    std::function<void()> beforeDrop;
    std::function<void()> afterDrop;

protected:
    void dropEvent(QDropEvent *e) override
    {
        if (beforeDrop) beforeDrop();
        QTreeWidget::dropEvent(e);
        if (afterDrop) afterDrop();
    }
};

} // namespace

RestructureDialog::RestructureDialog(const Model &model, QWidget *parent) : QDialog(parent), m_model(model)
{
    setWindowTitle(tr("Restructure — %1").arg(QFileInfo(model.master).fileName()));
    resize(1280, 820);

    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tr(
        "Drag chapters and section files to reorder them, move them to another part or chapter, or promote/demote "
        "them. Nothing changes on disk until you click <b>Apply Changes</b>; every step is listed on the right first, "
        "and <b>Tools › Undo Last Restructure</b> restores the book exactly. Greyed items are commented out; "
        "🔒 items are headings written inside another file and stay where they are for now."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *split = new QSplitter;
    lay->addWidget(split, 1);

    auto *left = new QWidget;
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    auto *filter = new QLineEdit;
    filter->setPlaceholderText(tr("Find a part, chapter or section…"));
    filter->setClearButtonEnabled(true);
    leftLay->addWidget(filter);
    auto *tree = new DraftTree;
    m_tree = tree;
    m_tree->setHeaderHidden(true);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_tree->setDropIndicatorShown(true);
    m_tree->setAnimated(true);
    leftLay->addWidget(m_tree, 1);
    split->addWidget(left);

    auto *right = new QWidget;
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->addWidget(new QLabel(tr("<b>What Apply Changes will do</b>")));
    m_planView = new QPlainTextEdit;
    m_planView->setReadOnly(true);
    m_planView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    rightLay->addWidget(m_planView, 1);
    m_figures = new QCheckBox(tr("Move figures that only the moved text uses, along with it"));
    m_figures->setChecked(true);
    rightLay->addWidget(m_figures);
    m_git = new QCheckBox(tr("Git: commit before and after"));
    m_git->setEnabled(isGitRepo(model.root));
    m_git->setChecked(isGitRepo(model.root));
    if (!isGitRepo(model.root)) m_git->setToolTip(tr("This book is not a Git repository; the restructure journal still allows Undo."));
    rightLay->addWidget(m_git);
    split->addWidget(right);
    split->setSizes({640, 640});

    auto *buttons = new QHBoxLayout;
    m_status = new QLabel;
    m_undoDrag = new QPushButton(tr("Undo Last Drag"));
    auto *reset = new QPushButton(tr("Reset"));
    auto *cancel = new QPushButton(tr("Cancel"));
    m_ok = new QPushButton(tr("Apply Changes"));
    m_ok->setDefault(true);
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_undoDrag);
    buttons->addWidget(reset);
    buttons->addWidget(cancel);
    buttons->addWidget(m_ok);
    lay->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(reset, &QPushButton::clicked, this, [this]() {
        m_history.clear();
        rebuild(originalTree(m_model));
        updatePlan();
    });
    connect(m_undoDrag, &QPushButton::clicked, this, [this]() {
        if (m_history.isEmpty()) return;
        rebuild(m_history.takeLast());
        updatePlan();
    });
    connect(m_figures, &QCheckBox::toggled, this, &RestructureDialog::updatePlan);
    tree->beforeDrop = [this]() { m_history << currentTree(); };
    tree->afterDrop = [this]() { updatePlan(); };
    connect(filter, &QLineEdit::textChanged, this, [this](const QString &t) {
        std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) -> bool {
            bool any = t.isEmpty() || it->text(0).contains(t, Qt::CaseInsensitive);
            for (int i = 0; i < it->childCount(); ++i) any = visit(it->child(i)) || any;
            it->setHidden(!any);
            if (!t.isEmpty() && any) it->setExpanded(true);
            return any;
        };
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) visit(m_tree->topLevelItem(i));
    });

    rebuild(originalTree(model));
    updatePlan();
}

bool RestructureDialog::gitCommit() const { return m_git->isChecked(); }

void RestructureDialog::addItems(QTreeWidgetItem *parentItem, int nodeId, const Tree &tree)
{
    for (int c : tree.value(nodeId)) {
        const Node &n = m_model.nodes[c];
        QString label;
        switch (n.kind) {
        case Kind::Heading: label = QString("🔒 %1 · %2").arg(kindName(n.level), n.title); break;
        case Kind::Unit: label = QString("%1 · %2").arg(kindName(n.level), n.title); break;
        case Kind::Container: label = QString("📄 %1").arg(n.title); break;
        case Kind::Plain: label = QString("%1  (no headings)").arg(n.title); break;
        case Kind::Missing: label = QString("%1  (file not found)").arg(n.title); break;
        default: label = n.title;
        }
        auto *it = new QTreeWidgetItem(parentItem, QStringList{label});
        it->setData(0, IdRole, c);
        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (isMovable(n)) f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
        else if (n.kind == Kind::Heading) f |= Qt::ItemIsDropEnabled;
        it->setFlags(f);
        QString tip = QDir(m_model.root).relativeFilePath(n.kind == Kind::Heading ? n.file : n.target);
        if (n.kind != Kind::Heading) tip += QString("\nincluded from %1, line %2").arg(QDir(m_model.root).relativeFilePath(n.file)).arg(n.line + 1);
        else tip += QString(", line %1").arg(n.line + 1);
        if (n.commented) tip += tr("\n(commented out — not compiled)");
        it->setToolTip(0, tip);
        if (n.commented) it->setForeground(0, palette().color(QPalette::PlaceholderText));
        if (n.kind == Kind::Missing) it->setForeground(0, QColor("#c0392b"));
        if (n.level <= 0.5 || n.kind == Kind::Container) {
            QFont fnt = it->font(0);
            fnt.setBold(true);
            it->setFont(0, fnt);
        }
        addItems(it, c, tree);
    }
}

void RestructureDialog::rebuild(const Tree &tree)
{
    // Keep what was expanded.
    QSet<int> expanded;
    bool first = m_tree->topLevelItemCount() == 0;
    std::function<void(QTreeWidgetItem *)> save = [&](QTreeWidgetItem *it) {
        if (it->isExpanded()) expanded.insert(it->data(0, IdRole).toInt());
        for (int i = 0; i < it->childCount(); ++i) save(it->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) save(m_tree->topLevelItem(i));

    m_tree->clear();
    auto *root = new QTreeWidgetItem(m_tree, QStringList{QString("📘 %1").arg(m_model.nodes[0].title)});
    root->setData(0, IdRole, 0);
    root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
    addItems(root, 0, tree);

    std::function<void(QTreeWidgetItem *)> restore = [&](QTreeWidgetItem *it) {
        const int id = it->data(0, IdRole).toInt();
        const Node &n = m_model.nodes[id];
        it->setExpanded(first ? (id == 0 || n.level < 1 || n.kind == Kind::Container) : expanded.contains(id));
        for (int i = 0; i < it->childCount(); ++i) restore(it->child(i));
    };
    restore(root);
}

Tree RestructureDialog::currentTree() const
{
    Tree t;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
        QVector<int> kids;
        for (int i = 0; i < it->childCount(); ++i) {
            kids << it->child(i)->data(0, IdRole).toInt();
            walk(it->child(i));
        }
        if (!kids.isEmpty()) t.insert(it->data(0, IdRole).toInt(), kids);
    };
    if (m_tree->topLevelItemCount() > 0) walk(m_tree->topLevelItem(0));
    return t;
}

void RestructureDialog::updatePlan()
{
    m_plan = makePlan(m_model, currentTree(), m_figures->isChecked());
    QStringList lines;
    for (const QString &e : m_plan.errors) lines << "✖ " + e;
    if (!m_plan.errors.isEmpty()) lines << QString();
    for (const QString &s : m_plan.steps) lines << (s.startsWith("    ") ? s : "• " + s);
    if (!m_plan.warnings.isEmpty()) lines << QString();
    for (const QString &w : m_plan.warnings) lines << "⚠ " + w;
    m_planView->setPlainText(lines.join('\n'));
    const bool can = m_plan.errors.isEmpty() && !m_plan.isEmpty();
    m_ok->setEnabled(can);
    m_undoDrag->setEnabled(!m_history.isEmpty());
    m_status->setText(!m_plan.errors.isEmpty() ? tr("Fix the problems marked ✖ (or Undo Last Drag) to continue.")
                      : m_plan.isEmpty() ? tr("No changes yet.")
                      : tr("%n file(s) will be edited, %1 item(s) moved.", nullptr, int(m_plan.newContents.size()))
                            .arg(m_plan.relocations.size()));
}
