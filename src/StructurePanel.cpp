#include "StructurePanel.h"
#include "Project.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QSet>
#include <QStyle>
#include <QTabWidget>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMap>
#include <QScrollBar>
#include <algorithm>
#include <functional>

enum Roles { FileRole = Qt::UserRole + 1, LineRole };

static QString keyPath(QTreeWidgetItem *it)
{
    QString k;
    for (; it; it = it->parent()) k.prepend(it->text(0) + "/");
    return k;
}

// Keep expansion and scroll position across rebuilds.
struct StructurePanel::TreeState {
    QSet<QString> expanded;
    int scroll = 0;
    bool first = true;
    void save(QTreeWidget *t)
    {
        first = t->topLevelItemCount() == 0;
        expanded.clear();
        QTreeWidgetItemIterator it(t);
        while (*it) {
            if ((*it)->isExpanded()) expanded.insert(keyPath(*it));
            ++it;
        }
        scroll = t->verticalScrollBar()->value();
    }
    void restore(QTreeWidget *t, int defaultDepth)
    {
        QTreeWidgetItemIterator it(t);
        while (*it) {
            int depth = 0;
            for (auto *p = (*it)->parent(); p; p = p->parent()) ++depth;
            (*it)->setExpanded(first ? depth < defaultDepth : expanded.contains(keyPath(*it)));
            ++it;
        }
        t->verticalScrollBar()->setValue(scroll);
    }
};

StructurePanel::StructurePanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);

    m_outlineState = new TreeState;
    m_filesState = new TreeState;
    m_refsState = new TreeState;

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter (title, label, key, file)…"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &StructurePanel::applyFilter);
    auto *restructure = new QToolButton(this);
    restructure->setText(tr("Restructure…"));
    restructure->setToolTip(tr("Rearrange parts, chapters and sections by dragging — files, folders, headings and paths follow"));
    connect(restructure, &QToolButton::clicked, this, &StructurePanel::restructureRequested);
    top->addWidget(m_filter, 1);
    top->addWidget(restructure);
    lay->addLayout(top);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_outline = makeTree();
    m_files = makeTree();
    m_refs = makeTree();
    m_tabs->addTab(m_outline, tr("Outline"));
    m_tabs->addTab(m_files, tr("Files"));
    m_tabs->addTab(m_refs, tr("References"));
    lay->addWidget(m_tabs);
}

StructurePanel::~StructurePanel()
{
    delete m_outlineState;
    delete m_filesState;
    delete m_refsState;
}

QTreeWidget *StructurePanel::makeTree()
{
    auto *t = new QTreeWidget(this);
    t->setHeaderHidden(true);
    t->setColumnCount(1);
    t->setUniformRowHeights(true);
    t->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(t, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        const QString f = it->data(0, FileRole).toString();
        if (!f.isEmpty()) emit openLocation(f, it->data(0, LineRole).toInt());
    });
    connect(t, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        const QString f = it->data(0, FileRole).toString();
        if (!f.isEmpty()) emit openLocation(f, it->data(0, LineRole).toInt());
    });
    connect(t, &QWidget::customContextMenuRequested, this, [this, t](const QPoint &p) { showContextMenu(t, p); });
    return t;
}

void StructurePanel::showContextMenu(QTreeWidget *tree, const QPoint &pos)
{
    QTreeWidgetItem *it = tree->itemAt(pos);
    if (!it) return;
    const QString f = it->data(0, FileRole).toString();
    const int line = it->data(0, LineRole).toInt();
    QMenu menu;
    if (!f.isEmpty()) {
        menu.addAction(tr("Open"), this, [=]() { emit openLocation(f, line); });
        menu.addAction(tr("Open in New Window"), this, [=]() { emit openLocationInNewWindow(f, line); });
        menu.addSeparator();
    }
    menu.addAction(tr("Expand All"), tree, &QTreeWidget::expandAll);
    menu.addAction(tr("Collapse All"), tree, &QTreeWidget::collapseAll);
    menu.exec(tree->viewport()->mapToGlobal(pos));
}

void StructurePanel::setProject(Project *p)
{
    if (m_project) disconnect(m_project, nullptr, this, nullptr);
    m_project = p;
    if (p) connect(p, &Project::changed, this, &StructurePanel::refresh);
    refresh();
}

static QTreeWidgetItem *makeItem(const QString &text, const SourceLoc &loc, const QIcon &icon = {})
{
    auto *it = new QTreeWidgetItem(QStringList{text});
    it->setData(0, FileRole, loc.file);
    it->setData(0, LineRole, loc.line);
    if (!icon.isNull()) it->setIcon(0, icon);
    it->setToolTip(0, QString("%1:%2").arg(QFileInfo(loc.file).fileName()).arg(loc.line));
    return it;
}

void StructurePanel::refresh()
{
    buildOutline();
    buildFiles();
    buildReferences();
    applyFilter(m_filter->text());
}

void StructurePanel::buildOutline()
{
    TreeState &state = *m_outlineState;
    state.save(m_outline);
    m_outline->clear();
    if (!m_project) return;

    const auto &d = m_project->data();
    QVector<QTreeWidgetItem *> stack;   // index = level
    QVector<int> levels;
    QFont bold = m_outline->font();
    bold.setBold(true);
    const QIcon fileIcon = qApp->style()->standardIcon(QStyle::SP_FileIcon);

    for (const OutlineEntry &e : d.outline) {
        QString text;
        switch (e.kind) {
        case EntryKind::Figure: text = tr("Figure: %1").arg(e.title.isEmpty() ? tr("(no caption)") : e.title); break;
        case EntryKind::Table: text = tr("Table: %1").arg(e.title.isEmpty() ? tr("(no caption)") : e.title); break;
        default: text = e.title;
        }
        if (!e.label.isEmpty()) text += QString("  [%1]").arg(e.label);
        auto *item = makeItem(text, e.loc);
        if (e.level <= 1) item->setFont(0, bold);
        if (e.level == 10) {
            item->setForeground(0, QColor(e.kind == EntryKind::Figure ? "#2a7ab0" : "#8a6d00"));
            for (const QString &g : e.graphics) {
                const QString resolved = m_project->resolveGraphic(g);
                auto *gi = makeItem(g, e.loc, fileIcon);
                if (resolved.isEmpty()) {
                    gi->setForeground(0, Qt::red);
                    gi->setToolTip(0, tr("Image not found"));
                } else {
                    gi->setToolTip(0, resolved);
                }
                item->addChild(gi);
            }
        }

        while (!levels.isEmpty() && levels.last() >= e.level) {
            levels.removeLast();
            stack.removeLast();
        }
        if (stack.isEmpty()) m_outline->addTopLevelItem(item);
        else stack.last()->addChild(item);
        if (e.level < 10) {
            stack << item;
            levels << e.level;
        }
    }
    state.restore(m_outline, 2);
}

static void addIncludeNode(QTreeWidgetItem *parent, const IncludeNode &n, const QString &root, const QIcon &icon)
{
    QString shown = QDir(root).relativeFilePath(n.path);
    auto *it = makeItem(shown, n.from.file.isEmpty() ? SourceLoc{n.path, 1} : SourceLoc{n.path, 1}, icon);
    if (!n.exists) {
        it->setForeground(0, Qt::red);
        it->setToolTip(0, QObject::tr("Missing — included from %1:%2")
                              .arg(QFileInfo(n.from.file).fileName()).arg(n.from.line));
        it->setData(0, FileRole, n.from.file);
        it->setData(0, LineRole, n.from.line);
    }
    parent->addChild(it);
    for (const IncludeNode &c : n.children) addIncludeNode(it, c, root, icon);
}

void StructurePanel::buildFiles()
{
    TreeState &state = *m_filesState;
    state.save(m_files);
    m_files->clear();
    if (!m_project) return;
    const auto &d = m_project->data();
    const QString root = m_project->rootDir();
    const QIcon fileIcon = qApp->style()->standardIcon(QStyle::SP_FileIcon);
    const QIcon dirIcon = qApp->style()->standardIcon(QStyle::SP_DirIcon);

    auto *docs = new QTreeWidgetItem(QStringList{tr("Document")});
    docs->setIcon(0, dirIcon);
    m_files->addTopLevelItem(docs);
    addIncludeNode(docs, d.includes, root, fileIcon);

    auto *bibs = new QTreeWidgetItem(QStringList{tr("Bibliography (%1)").arg(d.bibFiles.size())});
    bibs->setIcon(0, dirIcon);
    m_files->addTopLevelItem(bibs);
    for (const QString &b : d.bibFiles) {
        auto *it = makeItem(QDir(root).relativeFilePath(b), SourceLoc{b, 1}, fileIcon);
        if (!QFileInfo::exists(b)) it->setForeground(0, Qt::red);
        bibs->addChild(it);
    }
    state.restore(m_files, 10);
}

void StructurePanel::buildReferences()
{
    TreeState &state = *m_refsState;
    state.save(m_refs);
    m_refs->clear();
    if (!m_project) return;
    const auto &d = m_project->data();
    const QString root = m_project->rootDir();
    auto where = [&](const SourceLoc &l) {
        return QString("%1:%2").arg(QDir(root).relativeFilePath(l.file)).arg(l.line);
    };
    const QColor grey = qApp->palette().color(QPalette::PlaceholderText);

    // Labels
    QStringList labelKeys = d.labels.keys();
    std::sort(labelKeys.begin(), labelKeys.end());
    auto *labels = new QTreeWidgetItem(QStringList{tr("Labels (%1)").arg(labelKeys.size())});
    m_refs->addTopLevelItem(labels);
    for (const QString &k : labelKeys) {
        const auto defs = d.labels.value(k);
        const auto uses = d.refs.value(k);
        auto *it = makeItem(QString("%1   — %2 use(s)").arg(k).arg(uses.size()), defs.first());
        if (defs.size() > 1) it->setForeground(0, Qt::red);
        else if (uses.isEmpty()) it->setForeground(0, grey);
        for (const SourceLoc &l : defs) it->addChild(makeItem(tr("defined at %1").arg(where(l)), l));
        for (const SourceLoc &l : uses) it->addChild(makeItem(tr("used at %1").arg(where(l)), l));
        labels->addChild(it);
    }

    // Undefined references
    QStringList undefined;
    for (auto it = d.refs.cbegin(); it != d.refs.cend(); ++it)
        if (!d.labels.contains(it.key())) undefined << it.key();
    if (!undefined.isEmpty()) {
        std::sort(undefined.begin(), undefined.end());
        auto *und = new QTreeWidgetItem(QStringList{tr("Undefined references (%1)").arg(undefined.size())});
        und->setForeground(0, Qt::red);
        m_refs->addTopLevelItem(und);
        for (const QString &k : undefined) {
            const auto uses = d.refs.value(k);
            auto *it = makeItem(k, uses.first());
            it->setForeground(0, Qt::red);
            for (const SourceLoc &l : uses) it->addChild(makeItem(tr("used at %1").arg(where(l)), l));
            und->addChild(it);
        }
    }

    // Citations
    QStringList citeKeys = d.cites.keys();
    std::sort(citeKeys.begin(), citeKeys.end());
    const bool haveBib = !d.bibFiles.isEmpty() || !d.bibKeys.isEmpty();
    auto *cites = new QTreeWidgetItem(QStringList{tr("Citations (%1 cited, %2 in bibliography)")
                                                      .arg(citeKeys.size()).arg(d.bibKeys.size())});
    m_refs->addTopLevelItem(cites);
    for (const QString &k : citeKeys) {
        const auto uses = d.cites.value(k);
        const bool known = d.bibKeys.contains(k);
        auto *it = makeItem(QString("%1   — %2 use(s)").arg(k).arg(uses.size()),
                            known ? d.bibKeys.value(k) : uses.first());
        if (haveBib && !known) it->setForeground(0, Qt::red);
        if (known) it->addChild(makeItem(tr("entry at %1").arg(where(d.bibKeys.value(k))), d.bibKeys.value(k)));
        for (const SourceLoc &l : uses) it->addChild(makeItem(tr("cited at %1").arg(where(l)), l));
        cites->addChild(it);
    }

    // Uncited bibliography entries
    QStringList uncited;
    for (auto it = d.bibKeys.cbegin(); it != d.bibKeys.cend(); ++it)
        if (!d.cites.contains(it.key())) uncited << it.key();
    if (!uncited.isEmpty()) {
        std::sort(uncited.begin(), uncited.end());
        auto *un = new QTreeWidgetItem(QStringList{tr("Bibliography entries never cited (%1)").arg(uncited.size())});
        un->setForeground(0, grey);
        m_refs->addTopLevelItem(un);
        for (const QString &k : uncited) un->addChild(makeItem(k, d.bibKeys.value(k)));
    }

    // Images
    QMap<QString, QVector<const GraphicUse *>> byImage;
    for (const GraphicUse &g : d.graphics) byImage[g.written].push_back(&g);
    auto *imgs = new QTreeWidgetItem(QStringList{tr("Images (%1)").arg(byImage.size())});
    m_refs->addTopLevelItem(imgs);
    for (auto it = byImage.cbegin(); it != byImage.cend(); ++it) {
        const GraphicUse *first = it->first();
        auto *gi = makeItem(QString("%1   — %2 use(s)").arg(it.key()).arg(it->size()), first->loc);
        if (first->resolved.isEmpty()) gi->setForeground(0, Qt::red);
        else gi->setToolTip(0, first->resolved);
        for (const GraphicUse *g : *it) gi->addChild(makeItem(tr("used at %1").arg(where(g->loc)), g->loc));
        imgs->addChild(gi);
    }

    state.restore(m_refs, 1);
}

void StructurePanel::applyFilter(const QString &text)
{
    for (QTreeWidget *t : {m_outline, m_files, m_refs}) {
        // An item is visible if it or any descendant matches.
        std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) -> bool {
            bool any = text.isEmpty() || it->text(0).contains(text, Qt::CaseInsensitive);
            for (int i = 0; i < it->childCount(); ++i)
                any = visit(it->child(i)) || any;
            it->setHidden(!any);
            if (!text.isEmpty() && any) it->setExpanded(true);
            return any;
        };
        for (int i = 0; i < t->topLevelItemCount(); ++i) visit(t->topLevelItem(i));
    }
}
