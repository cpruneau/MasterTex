#include "BookToolsUi.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

enum { FileRole = Qt::UserRole + 1, LineRole, IndexRole };

static QString relTo(const QString &master, const QString &p)
{
    return QDir(QFileInfo(master).absolutePath()).relativeFilePath(p);
}

static void connectJump(QTreeWidget *t, QObject *owner, std::function<void(const QString &, int)> jump)
{
    auto go = [jump](QTreeWidgetItem *it) {
        const QString f = it->data(0, FileRole).toString();
        if (!f.isEmpty()) jump(f, it->data(0, LineRole).toInt());
    };
    QObject::connect(t, &QTreeWidget::itemActivated, owner, go);
    QObject::connect(t, &QTreeWidget::itemDoubleClicked, owner, go);
}

// ---------------------------------------------------------------------------
// Replace in All Files

ReplaceAllDialog::ReplaceAllDialog(const QString &master, PlanApplier apply, QWidget *parent)
    : QDialog(parent), m_master(master), m_apply(std::move(apply))
{
    setWindowTitle(tr("Replace in All Files — %1").arg(QFileInfo(master).fileName()));
    resize(1000, 700);
    auto *lay = new QVBoxLayout(this);
    auto *row1 = new QHBoxLayout;
    m_find = new QLineEdit;
    m_find->setPlaceholderText(tr("Find"));
    m_replace = new QLineEdit;
    m_replace->setPlaceholderText(tr("Replace with (\\1, \\2 … when Regex is on)"));
    row1->addWidget(m_find, 1);
    row1->addWidget(m_replace, 1);
    lay->addLayout(row1);
    auto *row2 = new QHBoxLayout;
    m_case = new QCheckBox(tr("Match case"));
    m_word = new QCheckBox(tr("Whole word"));
    m_regex = new QCheckBox(tr("Regex"));
    m_all = new QCheckBox(tr("Also other .tex files in the folder (drafts, copies)"));
    auto *searchBtn = new QPushButton(tr("Search"));
    searchBtn->setDefault(true);
    row2->addWidget(m_case);
    row2->addWidget(m_word);
    row2->addWidget(m_regex);
    row2->addWidget(m_all);
    row2->addStretch();
    row2->addWidget(searchBtn);
    lay->addLayout(row2);

    m_results = new QTreeWidget;
    m_results->setColumnCount(3);
    m_results->setHeaderLabels({tr("Match"), tr("Line"), tr("Becomes")});
    m_results->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    lay->addWidget(m_results, 1);
    connectJump(m_results, this, [this](const QString &f, int l) { emit openLocation(f, l); });

    auto *row3 = new QHBoxLayout;
    m_status = new QLabel(tr("Uncheck any match you want to keep. All replacements can be undone together "
                             "(Tools › Undo Last Book-Wide Change)."));
    m_status->setWordWrap(true);
    m_replaceBtn = new QPushButton(tr("Replace Checked"));
    m_replaceBtn->setEnabled(false);
    auto *close = new QPushButton(tr("Close"));
    row3->addWidget(m_status, 1);
    row3->addWidget(m_replaceBtn);
    row3->addWidget(close);
    lay->addLayout(row3);

    connect(searchBtn, &QPushButton::clicked, this, &ReplaceAllDialog::runSearch);
    connect(m_find, &QLineEdit::returnPressed, this, &ReplaceAllDialog::runSearch);
    connect(m_replace, &QLineEdit::textChanged, this, [this]() { if (!m_matches.isEmpty()) runSearch(); });
    connect(m_replaceBtn, &QPushButton::clicked, this, &ReplaceAllDialog::replaceChecked);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}

void ReplaceAllDialog::setFindText(const QString &t)
{
    if (!t.isEmpty()) m_find->setText(t);
    m_find->setFocus();
    m_find->selectAll();
}

QRegularExpression ReplaceAllDialog::regex() const
{
    QString p = m_regex->isChecked() ? m_find->text() : QRegularExpression::escape(m_find->text());
    if (m_word->isChecked()) p = "\\b" + p + "\\b";
    QRegularExpression::PatternOptions o = QRegularExpression::MultilineOption;
    if (!m_case->isChecked()) o |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(p, o);
}

void ReplaceAllDialog::runSearch()
{
    m_results->clear();
    m_matches.clear();
    if (m_find->text().isEmpty()) return;
    const QRegularExpression re = regex();
    if (!re.isValid()) { m_status->setText(tr("Invalid regular expression: %1").arg(re.errorString())); return; }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QStringList files = BookTools::bookFiles(m_master, m_all->isChecked());
    m_matches = BookTools::search(files, re, m_replace->text(), m_regex->isChecked());
    QApplication::restoreOverrideCursor();

    QHash<QString, QTreeWidgetItem *> fileItems;
    for (int i = 0; i < m_matches.size(); ++i) {
        const BookTools::Match &m = m_matches[i];
        QTreeWidgetItem *fi = fileItems.value(m.file);
        if (!fi) {
            fi = new QTreeWidgetItem(m_results, QStringList{relTo(m_master, m.file)});
            fi->setFlags(fi->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
            fi->setCheckState(0, Qt::Checked);
            QFont f = fi->font(0);
            f.setBold(true);
            fi->setFont(0, f);
            fileItems.insert(m.file, fi);
        }
        auto *it = new QTreeWidgetItem(fi, QStringList{m.lineText.trimmed().left(200), QString::number(m.line), m.replacement});
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(0, Qt::Checked);
        it->setData(0, FileRole, m.file);
        it->setData(0, LineRole, m.line);
        it->setData(0, IndexRole, i);
    }
    m_results->expandAll();
    m_status->setText(tr("%n match(es)", nullptr, int(m_matches.size())) + tr(" in %n file(s)", nullptr, int(fileItems.size())));
    m_replaceBtn->setEnabled(!m_matches.isEmpty());
}

void ReplaceAllDialog::replaceChecked()
{
    QVector<BookTools::Match> chosen;
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem *fi = m_results->topLevelItem(i);
        for (int k = 0; k < fi->childCount(); ++k)
            if (fi->child(k)->checkState(0) == Qt::Checked) chosen << m_matches[fi->child(k)->data(0, IndexRole).toInt()];
    }
    if (chosen.isEmpty()) return;
    const auto plan = BookTools::replacePlan(chosen, QString("Replace “%1” in all files").arg(m_find->text().left(40)));
    if (m_apply && m_apply(plan)) {
        m_status->setText(tr("Replaced %n match(es). Tools › Undo Last Book-Wide Change undoes them all.", nullptr, int(chosen.size())));
        m_results->clear();
        m_matches.clear();
        m_replaceBtn->setEnabled(false);
    }
}

// ---------------------------------------------------------------------------
// Rebase

RebaseDialog::RebaseDialog(const QString &master, const QString &onlyFile, QWidget *parent)
    : QDialog(parent), m_master(master), m_onlyFile(onlyFile)
{
    setWindowTitle(onlyFile.isEmpty() ? tr("Rebase Labels — all files") : tr("Rebase Labels — %1").arg(QFileInfo(onlyFile).fileName()));
    resize(1200, 760);
    auto *lay = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Labels are rebuilt as <b>type:Chapter:Section:Subsection:Name</b>, each level a short name "
                                "from its heading title (its acronym, or its first word); <i>Name</i> is the last part of the "
                                "current label. Every \\ref, \\eqref, \\cref … in every "
                                ".tex file of the folder is updated. Clashes get -2, -3 … and are listed for review."));
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *opts = new QHBoxLayout;
    m_part = new QCheckBox(tr("Start with the Part"));
    m_part->setChecked(false);
    m_depth = new QComboBox;
    m_depth->addItem(tr("down to Section"), 2);
    m_depth->addItem(tr("down to Subsection"), 3);
    m_depth->addItem(tr("down to Subsubsection"), 4);
    m_depth->setCurrentIndex(1);
    m_prefix = new QCheckBox(tr("One prefix per kind (eq, sec, ch, fig, tab, prob, thm …)"));
    m_prefix->setChecked(true);
    m_all = new QCheckBox(tr("Also labels in other .tex files (drafts, copies)"));
    m_all->setVisible(onlyFile.isEmpty());
    m_onlyReview = new QCheckBox(tr("Show only items to review"));
    opts->addWidget(m_part);
    opts->addWidget(m_depth);
    opts->addWidget(m_prefix);
    opts->addWidget(m_all);
    opts->addStretch();
    opts->addWidget(m_onlyReview);
    lay->addLayout(opts);

    m_table = new QTreeWidget;
    m_table->setRootIsDecorated(false);
    m_table->setColumnCount(4);
    m_table->setHeaderLabels({tr("Current label"), tr("New label"), tr("Where"), tr("References")});
    m_table->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 330);
    lay->addWidget(m_table, 1);
    connectJump(m_table, this, [this](const QString &f, int l) { emit openLocation(f, l); });

    auto *row = new QHBoxLayout;
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_ok = new QPushButton(tr("Rebase"));
    auto *cancel = new QPushButton(tr("Cancel"));
    row->addWidget(m_summary, 1);
    row->addWidget(cancel);
    row->addWidget(m_ok);
    lay->addLayout(row);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);
    for (QCheckBox *c : {m_part, m_prefix, m_all, m_onlyReview}) connect(c, &QCheckBox::toggled, this, &RebaseDialog::recompute);
    connect(m_depth, &QComboBox::currentIndexChanged, this, &RebaseDialog::recompute);
    recompute();
}

void RebaseDialog::recompute()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    BookTools::RebaseOptions o;
    o.includePart = m_part->isChecked();
    o.normalizePrefixes = m_prefix->isChecked();
    o.onlyFile = m_onlyFile;
    o.deepestLevel = m_depth->currentData().toInt();
    m_result = BookTools::rebase(m_master, BookTools::bookFiles(m_master, m_all->isChecked()), o);
    QApplication::restoreOverrideCursor();

    m_table->clear();
    const QColor warn("#b9770e");
    int shown = 0;
    for (const BookTools::LabelChange &c : m_result.changes) {
        const bool review = c.clash || c.duplicateOld;
        if (m_onlyReview->isChecked() && !review) continue;
        if (++shown > 5000) break;
        auto *it = new QTreeWidgetItem(m_table, QStringList{c.oldKey, c.newKey,
            QString("%1:%2").arg(relTo(m_master, c.file)).arg(c.line), QString::number(c.refs)});
        it->setData(0, FileRole, c.file);
        it->setData(0, LineRole, c.line);
        if (review) {
            for (int k = 0; k < 4; ++k) it->setForeground(k, warn);
            it->setToolTip(1, c.clash ? tr("Another label got the same name, so this one is numbered")
                                      : tr("The current label is defined more than once"));
        }
    }
    QStringList s = m_result.plan.steps;
    s << m_result.plan.warnings;
    m_summary->setText(s.join("  ·  "));
    m_ok->setEnabled(!m_result.changes.isEmpty());
}

// ---------------------------------------------------------------------------
// Checks pane

ChecksPanel::ChecksPanel(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto *bar = new QHBoxLayout;
    auto button = [this, bar](const QString &text, std::function<void()> fn) {
        auto *b = new QPushButton(text);
        connect(b, &QPushButton::clicked, this, [fn]() { fn(); });
        bar->addWidget(b);
    };
    button(tr("Check Eqs"), [this]() { runCheck(BookTools::CheckKind::Equations); });
    button(tr("Check Tabs"), [this]() { runCheck(BookTools::CheckKind::Tables); });
    button(tr("Check Figs"), [this]() { runCheck(BookTools::CheckKind::Figures); });
    button(tr("Figure Files"), [this]() { runFigureFiles(); });
    m_commented = new QCheckBox(tr("Include commented-out chapters"));
    bar->addWidget(m_commented);
    bar->addStretch();
    m_fix = new QPushButton(tr("Fix Checked"));
    m_fix->setVisible(false);
    bar->addWidget(m_fix);
    lay->addLayout(bar);
    m_title = new QLabel;
    lay->addWidget(m_title);
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Finding"), tr("Location")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    lay->addWidget(m_tree, 1);
    connectJump(m_tree, this, [this](const QString &f, int l) { emit openLocation(f, l); });
    connect(m_fix, &QPushButton::clicked, this, &ChecksPanel::fixChecked);
}

void ChecksPanel::runCheck(BookTools::CheckKind kind)
{
    if (m_master.isEmpty()) { m_title->setText(tr("Open a project first.")); return; }
    m_fix->setVisible(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto findings = BookTools::check(m_master, {}, kind, m_commented->isChecked());
    QApplication::restoreOverrideCursor();
    const QString what = kind == BookTools::CheckKind::Equations ? tr("Equations")
                       : kind == BookTools::CheckKind::Tables ? tr("Tables") : tr("Figures");
    m_tree->clear();
    QStyle *st = style();
    const QIcon icons[] = {st->standardIcon(QStyle::SP_MessageBoxCritical), st->standardIcon(QStyle::SP_MessageBoxWarning),
                           st->standardIcon(QStyle::SP_MessageBoxInformation)};
    QHash<QString, QTreeWidgetItem *> groups;
    for (const BookTools::Finding &f : findings) {
        QTreeWidgetItem *g = groups.value(f.category);
        if (!g) {
            g = new QTreeWidgetItem(m_tree, QStringList{f.category});
            g->setIcon(0, icons[qBound(0, f.severity, 2)]);
            groups.insert(f.category, g);
        }
        auto *it = new QTreeWidgetItem(g, QStringList{f.message, QString("%1:%2").arg(relTo(m_master, f.file)).arg(f.line)});
        it->setData(0, FileRole, f.file);
        it->setData(0, LineRole, f.line);
    }
    for (QTreeWidgetItem *g : groups) {
        g->setText(0, QString("%1 (%2)").arg(g->text(0)).arg(g->childCount()));
        g->setExpanded(g->childCount() <= 30);
    }
    m_title->setText(findings.isEmpty() ? tr("%1: nothing to report.").arg(what)
                                        : tr("%1: %n finding(s). Double-click to go there.", nullptr, int(findings.size())).arg(what));
}

void ChecksPanel::runFigureFiles()
{
    if (m_master.isEmpty()) { m_title->setText(tr("Open a project first.")); return; }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_figIssues = BookTools::checkFigureFiles(m_master, {}, m_commented->isChecked());
    QApplication::restoreOverrideCursor();
    m_tree->clear();
    const QHash<QString, QString> names = {{"missing", tr("Missing image files")}, {"misplaced", tr("Images in another chapter's folder")},
                                           {"shared", tr("Images shared by several chapters")}, {"unused", tr("Images not used anywhere")}};
    QHash<QString, QTreeWidgetItem *> groups;
    int fixable = 0;
    for (int i = 0; i < m_figIssues.size(); ++i) {
        const BookTools::FigureIssue &fi = m_figIssues[i];
        QTreeWidgetItem *g = groups.value(fi.kind);
        if (!g) {
            g = new QTreeWidgetItem(m_tree, QStringList{names.value(fi.kind, fi.kind)});
            g->setFlags(g->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
            groups.insert(fi.kind, g);
        }
        auto *it = new QTreeWidgetItem(g, QStringList{fi.message, fi.line ? QString("%1:%2").arg(relTo(m_master, fi.file)).arg(fi.line) : relTo(m_master, fi.file)});
        it->setData(0, FileRole, fi.line ? fi.file : QString());
        it->setData(0, LineRole, fi.line);
        it->setData(0, IndexRole, i);
        if (fi.fixable) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, Qt::Checked);
            ++fixable;
        }
    }
    for (QTreeWidgetItem *g : groups) {
        g->setText(0, QString("%1 (%2)").arg(g->text(0)).arg(g->childCount()));
        g->setExpanded(g->childCount() <= 30);
    }
    m_fix->setVisible(fixable > 0);
    m_title->setText(tr("Figure files: %n issue(s)", nullptr, int(m_figIssues.size())) +
                     (fixable ? tr("; %n can be fixed — uncheck any you want to leave, then Fix Checked.", nullptr, fixable) : QString()));
}

void ChecksPanel::fixChecked()
{
    QVector<BookTools::FigureIssue> chosen;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *gi = m_tree->topLevelItem(g);
        for (int k = 0; k < gi->childCount(); ++k) {
            QTreeWidgetItem *it = gi->child(k);
            if ((it->flags() & Qt::ItemIsUserCheckable) && it->checkState(0) == Qt::Checked)
                chosen << m_figIssues[it->data(0, IndexRole).toInt()];
        }
    }
    if (chosen.isEmpty()) return;
    const auto plan = BookTools::fixFigureFiles(m_master, {}, chosen);
    if (QMessageBox::question(this, tr("Fix figure files"),
            tr("%1\n\nThis can be undone with Tools › Undo Last Book-Wide Change.").arg(plan.steps.join("\n").left(3000))) != QMessageBox::Yes)
        return;
    if (m_apply && m_apply(plan)) runFigureFiles();
}

void ChecksPanel::showRebaseReview(const QVector<BookTools::LabelChange> &changes, const QStringList &skipped)
{
    m_fix->setVisible(false);
    m_tree->clear();
    auto *clash = new QTreeWidgetItem(m_tree, QStringList{tr("Numbered because two labels got the same name")});
    auto *dup = new QTreeWidgetItem(m_tree, QStringList{tr("Label was defined more than once before rebasing")});
    for (const BookTools::LabelChange &c : changes) {
        if (!c.clash && !c.duplicateOld) continue;
        auto *it = new QTreeWidgetItem(c.clash ? clash : dup, QStringList{QString("%1  →  %2").arg(c.oldKey, c.newKey),
                                                                          QString("%1:%2").arg(relTo(m_master, c.file)).arg(c.line)});
        it->setData(0, FileRole, c.file);
        it->setData(0, LineRole, c.line);
    }
    auto *sk = new QTreeWidgetItem(m_tree, QStringList{tr("Left unchanged")});
    for (const QString &s : skipped) new QTreeWidgetItem(sk, QStringList{s});
    for (QTreeWidgetItem *g : {clash, dup, sk}) g->setText(0, QString("%1 (%2)").arg(g->text(0)).arg(g->childCount()));
    clash->setExpanded(true);
    dup->setExpanded(true);
    m_title->setText(tr("Rebase review: labels to look at. Note that line numbers can have moved; the label text is shown."));
}
