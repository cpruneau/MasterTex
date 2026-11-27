#include "MainWindow.h"

#include "CodeEditor.h"
#include "DocumentManager.h"
#include "LatexHighlighter.h"
#include "Project.h"
#include "SettingsDialog.h"
#include "StructurePanel.h"
#include "SymbolPanel.h"
#include "Wizards.h"
#include "FindBar.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

enum { FileRole = Qt::UserRole + 1, LineRole };

static const QSet<QString> &textExtensions()
{
    static const QSet<QString> s = {"tex", "bib", "sty", "cls", "txt", "md", "cfg", "bst", "bbx", "cbx",
                                    "def", "ltx", "dtx", "ins", "log", "latexmkrc", "csv", "dat"};
    return s;
}


// A floating window holding an undocked editor. Its close button docks the
// editor back into the tabs instead of closing the file.
class EditorDock : public QDockWidget {
public:
    using QDockWidget::QDockWidget;
    std::function<void()> onClose;

protected:
    void closeEvent(QCloseEvent *e) override
    {
        e->ignore();
        if (onClose) onClose();
    }
};

// The master file for a .tex file: a "% !TEX root = ..." line wins; otherwise the
// file itself if it contains \documentclass; otherwise empty.
static QString masterFileFor(const QString &path, const QString &text)
{
    if (!path.endsWith(".tex", Qt::CaseInsensitive)) return QString();
    static const QRegularExpression magic(R"(^\s*%\s*!\s*TE?X\s+root\s*=\s*(.+?)\s*$)",
        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    auto m = magic.match(text.left(4000));
    if (m.hasMatch()) {
        QFileInfo fi(QFileInfo(path).absoluteDir(), m.captured(1));
        if (fi.exists()) return fi.absoluteFilePath();
    }
    static const QRegularExpression docclass(R"(^\s*\\documentclass)", QRegularExpression::MultilineOption);
    if (docclass.match(text).hasMatch()) return path;
    return QString();
}

// ---------------------------------------------------------------------------

MainWindow *MainWindow::newWindow(const QString &projectMaster)
{
    auto *w = new MainWindow(projectMaster);
    w->show();
    return w;
}

MainWindow::MainWindow(const QString &projectMaster, QWidget *parent) : QMainWindow(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_findBar = new FindBar;
    m_findBar->hide();
    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_tabs, 1);
    centralLayout->addWidget(m_findBar);
    setCentralWidget(central);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this]() { setActiveEditor(editorAt(m_tabs->currentIndex())); });
    // Double-click a tab to undock it; right-click for more.
    connect(m_tabs, &QTabWidget::tabBarDoubleClicked, this, [this](int i) {
        if (CodeEditor *e = editorAt(i)) undockEditor(e);
    });
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        CodeEditor *e = editorAt(m_tabs->tabBar()->tabAt(p));
        if (!e) return;
        QMenu menu;
        menu.addAction(tr("Undock into Its Own Window"), this, [this, e]() { undockEditor(e); });
        menu.addAction(tr("Move to New MasterTex Window"), this, [this, e]() {
            const QString path = e->filePath();
            const int line = e->currentLine();
            MainWindow *w = newWindow(m_project ? m_project->masterFile() : QString());
            w->openFile(path, line);
            closeEditor(e);
        });
        menu.addSeparator();
        menu.addAction(tr("Close"), this, [this, e]() { closeEditor(e); });
        menu.exec(m_tabs->tabBar()->mapToGlobal(p));
    });
    // The editor you are typing in — tabbed or undocked — is the one menus act on.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (QWidget *w = now; w; w = w->parentWidget())
            if (auto *e = qobject_cast<CodeEditor *>(w)) {
                if (allEditors().contains(e) && e != m_activeEditor) setActiveEditor(e);
                return;
            }
    });

    m_build = new QProcess(this);
    m_build->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_build, &QProcess::readyRead, this, [this]() {
        const QString chunk = QString::fromLocal8Bit(m_build->readAll());
        m_buildOutput += chunk;
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(chunk);
        m_log->moveCursor(QTextCursor::End);
    });
    connect(m_build, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        if (m_latexRun) parseBuildLog();
        const bool ok = st == QProcess::NormalExit && code == 0;
        statusBar()->showMessage(ok ? tr("Finished.") : tr("Failed (exit code %1).").arg(code), 8000);
        m_runAct->setEnabled(true);
        m_runAllAct->setEnabled(true);
        m_stopAct->setEnabled(false);
        if (!ok && m_latexRun) m_messageTabs->setCurrentIndex(0);
        if (ok && m_viewAfterBuild) viewPdf();
        m_viewAfterBuild = false;
    });
    connect(m_build, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            m_log->appendPlainText(tr("Could not start the build program. Check Options > Settings."));
            m_runAct->setEnabled(true);
            m_runAllAct->setEnabled(true);
            m_stopAct->setEnabled(false);
        }
    });

    auto *docs = DocumentManager::instance();
    connect(docs, &DocumentManager::documentRenamed, this, [this]() { updateTabTitles(); updateTitle(); });

    m_status = new QLabel(this);
    statusBar()->addPermanentWidget(m_status);

    createDocks();
    createMenus();
    createToolBars();

    resize(1400, 900);
    restoreState(QSettings().value("window/state").toByteArray());

    if (!projectMaster.isEmpty())
        setProject(projectMaster);
    updateTitle();
}

MainWindow::~MainWindow()
{
    // Stop reacting to focus and tab changes while child widgets are torn down.
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(m_tabs, nullptr, this, nullptr);
    auto *docs = DocumentManager::instance();
    for (CodeEditor *ed : allEditors()) docs->release(ed->document());
    if (m_project) Project::release(m_project);
}

// ---------------------------------------------------------------------------
// Setup

QAction *MainWindow::act(const QString &text, const QKeySequence &key, std::function<void()> fn, const QIcon &icon)
{
    auto *a = new QAction(icon, text, this);
    if (!key.isEmpty()) a->setShortcut(key);
    connect(a, &QAction::triggered, this, [fn]() { fn(); });
    return a;
}

void MainWindow::createDocks()
{
    // File browser
    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setReadOnly(true);
    m_fileView = new QTreeView;
    m_fileView->setModel(m_fsModel);
    for (int c = 1; c < 4; ++c) m_fileView->hideColumn(c);
    m_fileView->setHeaderHidden(true);
    connect(m_fileView, &QTreeView::doubleClicked, this, [this](const QModelIndex &idx) {
        QFileInfo fi = m_fsModel->fileInfo(idx);
        if (fi.isDir()) return;
        if (textExtensions().contains(fi.suffix().toLower()) || fi.suffix().isEmpty())
            openFile(fi.absoluteFilePath());
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath()));
    });
    m_fileView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_fileView, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        QModelIndex idx = m_fileView->indexAt(p);
        if (!idx.isValid()) return;
        QFileInfo fi = m_fsModel->fileInfo(idx);
        QMenu menu;
        if (fi.isFile()) {
            menu.addAction(tr("Open"), this, [this, fi]() { openFile(fi.absoluteFilePath()); });
            menu.addAction(tr("Open in New Window"), this, [this, fi]() { openLocationInNewWindow(fi.absoluteFilePath(), 0); });
            if (fi.suffix() == "tex")
                menu.addAction(tr("Open as Project (master file)"), this, [this, fi]() {
                    if (m_project) newWindow(fi.absoluteFilePath()); else setProject(fi.absoluteFilePath());
                });
        }
        menu.addAction(tr("Open with Default App"), this, [fi]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absoluteFilePath()));
        });
        menu.exec(m_fileView->viewport()->mapToGlobal(p));
    });
    auto *filesDock = new QDockWidget(tr("Files"), this);
    filesDock->setObjectName("FilesDock");
    filesDock->setWidget(m_fileView);
    addDockWidget(Qt::LeftDockWidgetArea, filesDock);

    // Structure browser
    m_structure = new StructurePanel;
    connect(m_structure, &StructurePanel::openLocation, this, [this](const QString &f, int l) { openFile(f, l); });
    connect(m_structure, &StructurePanel::openLocationInNewWindow, this, &MainWindow::openLocationInNewWindow);
    auto *structDock = new QDockWidget(tr("Structure"), this);
    structDock->setObjectName("StructureDock");
    structDock->setWidget(m_structure);
    addDockWidget(Qt::LeftDockWidgetArea, structDock);
    splitDockWidget(filesDock, structDock, Qt::Vertical);
    resizeDocks({filesDock, structDock}, {250, 550}, Qt::Vertical);
    resizeDocks({structDock}, {340}, Qt::Horizontal);

    // Symbol panes
    m_symbols = new SymbolPanel;
    connect(m_symbols, &SymbolPanel::insertRequested, this, &MainWindow::insert);
    auto *symDock = new QDockWidget(tr("Symbols"), this);
    symDock->setObjectName("SymbolsDock");
    symDock->setWidget(m_symbols);
    addDockWidget(Qt::RightDockWidgetArea, symDock);
    symDock->hide();

    // Messages: issues + build log
    m_issues = new QTreeWidget;
    m_issues->setColumnCount(2);
    m_issues->setHeaderLabels({tr("Message"), tr("Location")});
    m_issues->setRootIsDecorated(false);
    m_issues->header()->setStretchLastSection(false);
    m_issues->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_issues->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(m_issues, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        const QString f = it->data(0, FileRole).toString();
        if (!f.isEmpty()) openFile(f, it->data(0, LineRole).toInt());
    });
    connect(m_issues, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        const QString f = it->data(0, FileRole).toString();
        if (!f.isEmpty()) openFile(f, it->data(0, LineRole).toInt());
    });

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMaximumBlockCount(20000);

    m_messageTabs = new QTabWidget;
    m_messageTabs->setDocumentMode(true);
    m_messageTabs->addTab(m_issues, tr("Issues"));
    m_messageTabs->addTab(m_log, tr("Build Log"));
    m_repetitions = new QTreeWidget;
    m_repetitions->setColumnCount(2);
    m_repetitions->setHeaderLabels({tr("Repeated word"), tr("Location")});
    m_repetitions->setRootIsDecorated(false);
    m_repetitions->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(m_repetitions, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        openFile(it->data(0, FileRole).toString(), it->data(0, LineRole).toInt());
    });
    connect(m_repetitions, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        openFile(it->data(0, FileRole).toString(), it->data(0, LineRole).toInt());
    });
    m_messageTabs->addTab(m_repetitions, tr("Repetitions"));
    m_messagesDock = new QDockWidget(tr("Messages"), this);
    m_messagesDock->setObjectName("MessagesDock");
    m_messagesDock->setWidget(m_messageTabs);
    addDockWidget(Qt::BottomDockWidgetArea, m_messagesDock);
    resizeDocks({m_messagesDock}, {180}, Qt::Vertical);
}

void MainWindow::createToolBars()
{
    QToolBar *main = addToolBar(tr("Main"));
    main->setObjectName("MainToolBar");
    main->addAction(m_a["new"]);
    main->addAction(m_a["open"]);
    main->addAction(m_a["openProject"]);
    main->addAction(m_a["save"]);
    main->addAction(m_a["saveAs"]);
    main->addAction(m_a["close"]);
    main->addSeparator();
    for (QAction *a : {m_runAct, m_runAllAct, m_stopAct, m_a["viewPdf"]}) {
        main->addAction(a);
        if (auto *b = qobject_cast<QToolButton *>(main->widgetForAction(a))) {
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            QString label = a->text();
            label.remove('&');
            b->setText(label.section(" (", 0, 0));
            b->setToolTip(a->text().remove('&') + "  " + a->shortcut().toString(QKeySequence::NativeText));
        }
    }

    QToolBar *tex = addToolBar(tr("LaTeX"));
    tex->setObjectName("LatexToolBar");
    tex->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto add = [tex](QAction *a, const QString &label) {
        tex->addAction(a);
        if (auto *w = tex->widgetForAction(a)) {
            if (auto *b = qobject_cast<QToolButton *>(w)) { b->setText(label); b->setToolTip(a->text()); }
        }
    };
    add(m_a["section"], "§");
    add(m_a["bold"], "B");
    add(m_a["italic"], "I");
    add(m_a["emph"], "emph");
    tex->addSeparator();
    add(m_a["env-itemize"], "• list");
    add(m_a["env-enumerate"], "1. list");
    add(m_a["inlineMath"], "$x$");
    add(m_a["equation"], "eq");
    tex->addSeparator();
    add(m_a["wizFigure"], "Figure");
    add(m_a["wizTable"], "Table");
    tex->addSeparator();
    add(m_a["label"], "label");
    add(m_a["ref"], "ref");
    add(m_a["cite"], "cite");
}

// ---------------------------------------------------------------------------
// Project and files

void MainWindow::setProject(const QString &masterFile)
{
    const QString key = DocumentManager::normalize(masterFile);
    if (m_project && m_project->masterFile() == key) return;
    if (!QFileInfo::exists(key)) {
        QMessageBox::warning(this, tr("Open Project"), tr("File not found:\n%1").arg(key));
        return;
    }
    if (m_project) {
        disconnect(m_project, nullptr, this, nullptr);
        Project::release(m_project);
    }
    m_project = Project::acquire(key);
    connect(m_project, &Project::changed, this, &MainWindow::updateIssues);
    m_structure->setProject(m_project);

    const QString root = m_project->rootDir();
    m_fsModel->setRootPath(root);
    m_fileView->setRootIndex(m_fsModel->index(root));

    QSettings s;
    QStringList recent = s.value("recentProjects").toStringList();
    recent.removeAll(key);
    recent.prepend(key);
    while (recent.size() > 10) recent.removeLast();
    s.setValue("recentProjects", recent);
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *mw = qobject_cast<MainWindow *>(w)) mw->updateRecentMenu();

    if (allEditors().isEmpty()) openFile(key);
    updateIssues();
    updateTitle();
}

void MainWindow::updateRecentMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    const QStringList recent = QSettings().value("recentProjects").toStringList();
    for (const QString &p : recent) {
        QAction *a = m_recentMenu->addAction(QFileInfo(p).fileName() + "  —  " + QFileInfo(p).absolutePath());
        connect(a, &QAction::triggered, this, [this, p]() {
            if (m_project && m_project->masterFile() != p) newWindow(p);
            else setProject(p);
        });
    }
    if (recent.isEmpty()) m_recentMenu->addAction(tr("(none)"))->setEnabled(false);
}

CodeEditor *MainWindow::editorAt(int index) const
{
    return qobject_cast<CodeEditor *>(m_tabs->widget(index));
}

CodeEditor *MainWindow::currentEditor() const
{
    if (m_activeEditor && (m_undocked.contains(m_activeEditor) || m_tabs->indexOf(m_activeEditor) >= 0))
        return m_activeEditor;
    return qobject_cast<CodeEditor *>(m_tabs->currentWidget());
}

QList<CodeEditor *> MainWindow::allEditors() const
{
    QList<CodeEditor *> out;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (CodeEditor *e = editorAt(i)) out << e;
    for (CodeEditor *e : m_undocked.keys()) out << e;
    return out;
}

void MainWindow::setActiveEditor(CodeEditor *ed)
{
    m_activeEditor = ed;
    updateTitle();
    m_findBar->setEditor(currentEditor());
    disconnect(m_cursorConn);
    if (ed) {
        auto update = [this, ed]() {
            QTextCursor c = ed->textCursor();
            m_status->setText(tr("Line %1, Col %2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1));
        };
        m_cursorConn = connect(ed, &QPlainTextEdit::cursorPositionChanged, this, update);
        update();
    } else {
        m_status->clear();
    }
}

void MainWindow::undockEditor(CodeEditor *e)
{
    const int idx = m_tabs->indexOf(e);
    if (!e || idx < 0) return;
    m_tabs->removeTab(idx);

    auto *dock = new EditorDock(QFileInfo(e->filePath()).fileName(), this);
    dock->setObjectName(QString("EditorDock-%1").arg(++m_dockCounter));
    dock->setAttribute(Qt::WA_DeleteOnClose, false);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    dock->setWidget(e);
    dock->onClose = [this, e]() { dockBack(e); };
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->setFloating(true);
    const int offset = 30 * (m_undocked.size() % 8);
    dock->setGeometry(QRect(geometry().topLeft() + QPoint(120 + offset, 80 + offset), QSize(820, 900)));
    m_undocked.insert(e, dock);
    dock->show();
    dock->raise();
    dock->activateWindow();
    e->setFocus();
    setActiveEditor(e);
    updateTabTitles();
    statusBar()->showMessage(tr("Undocked %1 — close its window (or View > Dock Editor Back) to return it to the tabs; "
                                "drag it onto an edge of this window to place it side by side.")
                                 .arg(QFileInfo(e->filePath()).fileName()), 8000);
}

void MainWindow::dockBack(CodeEditor *e)
{
    QDockWidget *dock = m_undocked.take(e);
    if (!dock) return;
    e->setParent(nullptr);
    const int idx = m_tabs->addTab(e, QFileInfo(e->filePath()).fileName());
    m_tabs->setCurrentIndex(idx);
    dock->deleteLater();
    updateTabTitles();
    e->setFocus();
    setActiveEditor(e);
}

void MainWindow::dockAllBack()
{
    for (CodeEditor *e : m_undocked.keys()) dockBack(e);
}

CodeEditor *MainWindow::openFile(const QString &path, int line)
{
    auto *docs = DocumentManager::instance();
    const QString key = DocumentManager::normalize(path);
    for (CodeEditor *e : allEditors()) {
        if (e->filePath() != key) continue;
        if (line > 0) rememberPosition();
        if (QDockWidget *dock = m_undocked.value(e)) {
            dock->show();
            dock->raise();
            dock->activateWindow();
        } else {
            m_tabs->setCurrentIndex(m_tabs->indexOf(e));
        }
        setActiveEditor(e);
        if (line > 0) e->gotoLine(line);
        else e->setFocus();
        return e;
    }

    QString err;
    QTextDocument *doc = docs->acquire(key, &err);
    if (!doc) {
        QMessageBox::warning(this, tr("Open File"), tr("Could not open %1:\n%2").arg(key, err));
        return nullptr;
    }
    if (docs->refCount(doc) == 1 && docs->hasNewerAutosave(key)) {
        auto answer = QMessageBox::question(this, tr("Recover unsaved changes?"),
            tr("MasterTex has unsaved changes for\n%1\nfrom a previous session that did not close normally.\n\n"
               "Recover them? (You can still choose not to save afterwards.)").arg(key),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) docs->recoverAutosave(doc);
        else docs->discardAutosave(key);
    }
    if (!doc->findChild<LatexHighlighter *>()) new LatexHighlighter(doc);

    if (line > 0) rememberPosition();
    auto *editor = new CodeEditor(doc);
    const int idx = m_tabs->addTab(editor, QFileInfo(key).fileName());
    m_tabs->setTabToolTip(idx, key);
    connect(doc, &QTextDocument::modificationChanged, this, &MainWindow::updateTabTitles, Qt::UniqueConnection);
    m_tabs->setCurrentIndex(idx);
    updateTabTitles();
    updateLogMarkers();
    if (line > 0) editor->gotoLine(line);
    else editor->setFocus();

    if (!m_project) {
        const QString master = masterFileFor(key, doc->toPlainText());
        if (!master.isEmpty()) {
            setProject(master);
            statusBar()->showMessage(tr("Project: %1").arg(QFileInfo(master).fileName()), 5000);
        }
    }
    return editor;
}

void MainWindow::openLocationInNewWindow(const QString &file, int line)
{
    MainWindow *w = newWindow(m_project ? m_project->masterFile() : QString());
    w->openFile(file, line);
}

void MainWindow::updateTabTitles()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        CodeEditor *e = editorAt(i);
        if (!e) continue;
        const QString path = e->filePath();
        m_tabs->setTabText(i, QFileInfo(path).fileName() + (e->document()->isModified() ? " •" : ""));
        m_tabs->setTabToolTip(i, path);
    }
    for (auto it = m_undocked.cbegin(); it != m_undocked.cend(); ++it)
        it.value()->setWindowTitle(QFileInfo(it.key()->filePath()).fileName()
                                   + (it.key()->document()->isModified() ? " •" : ""));
    updateTitle();
}

void MainWindow::updateTitle()
{
    QString t = "MasterTex";
    if (m_project) t += " — " + m_project->name();
    if (CodeEditor *e = currentEditor()) {
        t += " — " + QFileInfo(e->filePath()).fileName();
        setWindowFilePath(e->filePath());
        setWindowModified(e->document()->isModified());
        t += "[*]";
    } else {
        setWindowModified(false);
    }
    setWindowTitle(t);
}

void MainWindow::updateIssues()
{
    m_issues->clear();
    QStyle *st = style();
    const QIcon icons[] = {st->standardIcon(QStyle::SP_MessageBoxCritical),
                           st->standardIcon(QStyle::SP_MessageBoxWarning),
                           st->standardIcon(QStyle::SP_MessageBoxInformation)};
    const QString root = m_project ? m_project->rootDir() : QString();
    auto add = [&](int sev, const QString &msg, const QString &file, int line, const QString &prefix) {
        auto *it = new QTreeWidgetItem(QStringList{prefix + msg,
            file.isEmpty() ? QString() : QString("%1:%2").arg(QDir(root).relativeFilePath(file)).arg(line)});
        it->setIcon(0, icons[qBound(0, sev, 2)]);
        it->setData(0, FileRole, file);
        it->setData(0, LineRole, line);
        m_issues->addTopLevelItem(it);
    };
    int errors = 0, warnings = 0;
    for (const BuildIssue &b : m_buildIssues) {
        add(b.severity, b.message, b.file, b.line, tr("[build] "));
        (b.severity == 0 ? errors : warnings)++;
    }
    if (m_project)
        for (const Issue &i : m_project->data().issues) {
            add(int(i.severity), i.message, i.loc.file, i.loc.line, QString());
            if (i.severity == Severity::Error) ++errors;
            else if (i.severity == Severity::Warning) ++warnings;
        }
    m_messageTabs->setTabText(0, tr("Issues (%1 errors, %2 warnings)").arg(errors).arg(warnings));
}

// ---------------------------------------------------------------------------
// Saving and closing

bool MainWindow::saveDocument(QTextDocument *doc)
{
    QString err;
    if (!DocumentManager::instance()->save(doc, &err)) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2")
                                                   .arg(DocumentManager::instance()->pathOf(doc), err));
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(DocumentManager::instance()->pathOf(doc)).fileName()), 3000);
    return true;
}

bool MainWindow::saveDocumentAs(QTextDocument *doc)
{
    auto *docs = DocumentManager::instance();
    const QString path = QFileDialog::getSaveFileName(this, tr("Save As"), docs->pathOf(doc));
    if (path.isEmpty()) return false;
    QString err;
    if (!docs->saveAs(doc, path, &err)) {
        QMessageBox::warning(this, tr("Save As"), tr("Could not save %1:\n%2").arg(path, err));
        return false;
    }
    return true;
}

bool MainWindow::saveAll()
{
    for (QTextDocument *d : DocumentManager::instance()->modifiedDocuments())
        if (!saveDocument(d)) return false;
    return true;
}

bool MainWindow::maybeSave(const QList<QTextDocument *> &docs)
{
    QList<QTextDocument *> modified;
    for (QTextDocument *d : docs)
        if (d->isModified() && !modified.contains(d)) modified << d;
    if (modified.isEmpty()) return true;

    QStringList names;
    for (QTextDocument *d : modified) names << QFileInfo(DocumentManager::instance()->pathOf(d)).fileName();
    auto answer = QMessageBox::warning(this, tr("Unsaved changes"),
        tr("Save changes to:\n\n%1").arg(names.join("\n")),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    for (QTextDocument *d : modified) {
        if (answer == QMessageBox::Save) {
            if (!saveDocument(d)) return false;
        } else {
            DocumentManager::instance()->discardAutosave(DocumentManager::instance()->pathOf(d));
            d->setModified(false);
        }
    }
    return true;
}

bool MainWindow::closeTab(int index)
{
    return closeEditor(editorAt(index));
}

bool MainWindow::closeEditor(CodeEditor *e)
{
    if (!e) return false;
    QTextDocument *doc = e->document();
    auto *docs = DocumentManager::instance();
    if (docs->refCount(doc) == 1 && !maybeSave({doc})) return false;
    if (QDockWidget *dock = m_undocked.take(e)) {
        e->setParent(nullptr);
        dock->deleteLater();
    } else {
        m_tabs->removeTab(m_tabs->indexOf(e));
    }
    if (m_activeEditor == e) m_activeEditor = nullptr;
    e->deleteLater();
    docs->release(doc);
    setActiveEditor(editorAt(m_tabs->currentIndex()));
    return true;
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    // Only ask about documents that no other window still shows.
    auto *docs = DocumentManager::instance();
    QList<QTextDocument *> onlyHere;
    for (CodeEditor *e : allEditors())
        if (docs->refCount(e->document()) == 1) onlyHere << e->document();
    if (!maybeSave(onlyHere)) {
        ev->ignore();
        return;
    }
    if (m_build->state() != QProcess::NotRunning) m_build->kill();
    QSettings().setValue("window/state", saveState());
    ev->accept();
}

// ---------------------------------------------------------------------------
// Commands

void MainWindow::insert(const QString &snippet)
{
    if (snippet.isEmpty()) return;
    if (CodeEditor *e = currentEditor()) e->insertSnippet(snippet);
    else statusBar()->showMessage(tr("Open a file first."), 4000);
}

void MainWindow::newFile()
{
    const QString start = m_project ? m_project->rootDir() : QDir::homePath();
    const QString path = QFileDialog::getSaveFileName(this, tr("New File"), start + "/untitled.tex",
                                                      tr("LaTeX (*.tex);;BibTeX (*.bib);;All files (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!QFileInfo::exists(path)) {
        if (!f.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, tr("New File"), f.errorString());
            return;
        }
        f.close();
    }
    openFile(path);
}

void MainWindow::openFileDialog()
{
    const QString start = m_project ? m_project->rootDir() : QDir::homePath();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open File"), start,
        tr("TeX files (*.tex *.bib *.sty *.cls);;All files (*)"));
    for (const QString &f : files) openFile(f);
}

void MainWindow::openProjectDialog()
{
    const QString start = m_project ? m_project->rootDir() : QDir::homePath();
    const QString f = QFileDialog::getOpenFileName(this, tr("Open Project — choose the master .tex file"),
                                                   start, tr("LaTeX (*.tex)"));
    if (f.isEmpty()) return;
    if (m_project && m_project->masterFile() != DocumentManager::normalize(f)) newWindow(f);
    else setProject(f);
}

void MainWindow::gotoLineDialog()
{
    CodeEditor *e = currentEditor();
    if (!e) return;
    bool ok = false;
    int line = QInputDialog::getInt(this, tr("Go to Line"), tr("Line:"), e->currentLine(), 1,
                                    e->document()->blockCount(), 1, &ok);
    if (ok) {
        rememberPosition();
        e->gotoLine(line);
    }
}

void MainWindow::insertReference()
{
    QStringList keys = m_project ? m_project->data().labels.keys() : QStringList();
    if (keys.isEmpty()) { insert("\\ref{%|}"); return; }
    std::sort(keys.begin(), keys.end());
    bool ok = false;
    const QString key = QInputDialog::getItem(this, tr("Insert Reference"), tr("Label:"), keys, 0, true, &ok);
    if (!ok || key.isEmpty()) return;
    const QString cmd = key.startsWith("eq:") ? "\\eqref" : "\\ref";
    insert(QString("%1{%2}%|").arg(cmd, key));
}

void MainWindow::insertCitation()
{
    QStringList keys = m_project ? m_project->data().bibKeys.keys() : QStringList();
    if (keys.isEmpty()) { insert("\\cite{%|}"); return; }
    std::sort(keys.begin(), keys.end());
    bool ok = false;
    const QString key = QInputDialog::getItem(this, tr("Insert Citation"), tr("Bibliography key:"), keys, 0, true, &ok);
    if (!ok || key.isEmpty()) return;
    insert(QString("\\cite{%1}%|").arg(key));
}

void MainWindow::wizardChapter()
{
    if (!m_project) { statusBar()->showMessage(tr("Open a project first."), 4000); return; }
    QString created;
    const QString snippet = Wizards::chapterFile(this, m_project->rootDir(), &created);
    if (snippet.isEmpty()) return;
    insert(snippet);
    if (!created.isEmpty()) openFile(created, 3);
}

bool MainWindow::ensureProject()
{
    if (m_project) return true;
    CodeEditor *e = currentEditor();
    const QString master = e ? masterFileFor(e->filePath(), e->document()->toPlainText()) : QString();
    if (master.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to compile yet"),
            tr("MasterTex compiles a project, starting from its master file (the one with "
               "\\documentclass).\n\nUse File > Open Project… (Shift+Cmd+O) and choose your "
               "book's master .tex file.\n\nTip: a chapter file can name its master with a first line like\n"
               "% !TEX root = ../main.tex"));
        return false;
    }
    setProject(master);
    return m_project != nullptr;
}

void MainWindow::runTool(const QString &programName, const QStringList &args, const QString &label, bool latexRun)
{
    if (!ensureProject()) return;
    if (m_build->state() != QProcess::NotRunning) {
        statusBar()->showMessage(tr("Something is already running — wait or press Stop."), 5000);
        return;
    }
    if (!saveAll()) return;

    const QString program = SettingsDialog::findProgram(programName);
    m_messagesDock->show();
    m_messageTabs->setCurrentIndex(1);
    m_log->clear();
    m_buildOutput.clear();
    if (latexRun) m_buildIssues.clear();
    if (program.isEmpty()) {
        m_log->appendPlainText(tr("Could not find \"%1\". Install MacTeX, or set the program and search path "
                                  "in Options > Settings.").arg(programName));
        m_viewAfterBuild = false;
        return;
    }
    m_latexRun = latexRun;
    m_build->setProcessEnvironment(SettingsDialog::environment());
    m_build->setWorkingDirectory(m_project->rootDir());
    m_log->appendPlainText(QString("$ %1 %2\n").arg(QFileInfo(program).fileName(), args.join(' ')));
    m_runAct->setEnabled(false);
    m_runAllAct->setEnabled(false);
    m_stopAct->setEnabled(true);
    statusBar()->showMessage(label);
    m_build->start(program, args);
}

void MainWindow::runBuild(bool full)
{
    if (!ensureProject()) return;
    QStringList args = SettingsDialog::latexmkArguments();
    if (full) args << "-g";
    args << QFileInfo(m_project->masterFile()).fileName();
    runTool(SettingsDialog::latexmkProgram(), args, full ? tr("Rebuilding everything…") : tr("Compiling…"), true);
}

void MainWindow::runAuxTool(const QString &tool)
{
    if (!ensureProject()) return;
    const QString base = m_project->name();
    if (tool == "bibliography") {
        // biblatex projects use biber, others bibtex.
        bool biblatex = false;
        for (const QString &f : m_project->data().files) {
            const QString text = DocumentManager::instance()->textFor(f);
            if (text.contains("{biblatex}") || text.contains("\\addbibresource")) { biblatex = true; break; }
        }
        runTool(biblatex ? "biber" : "bibtex", {base}, tr("Running %1…").arg(biblatex ? "biber" : "bibtex"), false);
    } else if (tool == "glossary") {
        runTool("makeglossaries", {base}, tr("Running makeglossaries…"), false);
    } else if (tool == "index") {
        runTool("makeindex", {base + ".idx"}, tr("Running makeindex…"), false);
    }
}

void MainWindow::cleanBuild()
{
    if (!ensureProject()) return;
    runTool(SettingsDialog::latexmkProgram(), {"-c", QFileInfo(m_project->masterFile()).fileName()},
            tr("Cleaning auxiliary files…"), false);
}

void MainWindow::parseBuildLog()
{
    const QString root = m_project ? m_project->rootDir() : QString();
    QSet<QString> seen;
    static const QRegularExpression fileLineError(R"(^(.+?\.(?:tex|sty|cls|bib|ltx)):(\d+): (.+)$)");
    static const QRegularExpression latexWarning(R"((?:LaTeX|Package \w+) Warning: (.+?)(?: on input line (\d+))?\.?$)");
    const QStringList lines = m_buildOutput.split('\n');
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        auto m = fileLineError.match(line);
        if (m.hasMatch()) {
            QString f = m.captured(1);
            if (f.startsWith("./")) f = f.mid(2);
            const QString abs = DocumentManager::normalize(QDir(root).absoluteFilePath(f));
            const QString key = abs + m.captured(2) + m.captured(3);
            if (seen.contains(key)) continue;
            seen.insert(key);
            m_buildIssues.push_back({0, m.captured(3), abs, m.captured(2).toInt()});
            continue;
        }
        auto w = latexWarning.match(line);
        if (w.hasMatch()) {
            const QString key = w.captured(0);
            if (seen.contains(key)) continue;
            seen.insert(key);
            // LaTeX does not say which file the line belongs to; leave the file empty.
            m_buildIssues.push_back({1, w.captured(1) + (w.captured(2).isEmpty() ? QString() : tr(" (input line %1)").arg(w.captured(2))), QString(), 0});
        }
    }
    updateIssues();
    updateLogMarkers();
}

void MainWindow::updateLogMarkers()
{
    for (CodeEditor *e : allEditors()) {
        QHash<int, int> markers;
        if (m_logMarkersOn) {
            const QString path = e->filePath();
            for (const BuildIssue &b : m_buildIssues)
                if (b.file == path && b.line > 0 && (!markers.contains(b.line) || b.severity < markers.value(b.line)))
                    markers.insert(b.line, b.severity);
        }
        e->setLogMarkers(markers);
    }
}

void MainWindow::viewPdf()
{
    if (!ensureProject()) return;
    const QString pdf = m_project->rootDir() + "/" + m_project->name() + ".pdf";
    if (!QFileInfo::exists(pdf)) {
        statusBar()->showMessage(tr("No PDF yet — run the build first (Tools > Compile)."), 5000);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(pdf));
}

void MainWindow::viewLog()
{
    if (!ensureProject()) return;
    const QString log = m_project->rootDir() + "/" + m_project->name() + ".log";
    if (!QFileInfo::exists(log)) {
        statusBar()->showMessage(tr("No log file yet — compile first."), 5000);
        return;
    }
    openFile(log);
}

void MainWindow::openTerminal()
{
    const QString dir = m_project ? m_project->rootDir()
                      : (currentEditor() ? QFileInfo(currentEditor()->filePath()).absolutePath() : QDir::homePath());
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {"-a", "Terminal", dir});
#else
    QProcess::startDetached("x-terminal-emulator", {}, dir);
#endif
}

// ---------------------------------------------------------------------------
// Navigation helpers

void MainWindow::rememberPosition()
{
    if (CodeEditor *e = currentEditor()) {
        m_navFile = e->filePath();
        m_navLine = e->currentLine();
    }
}

void MainWindow::toggleBackForward()
{
    if (m_navFile.isEmpty()) {
        statusBar()->showMessage(tr("No previous position yet."), 3000);
        return;
    }
    const QString file = m_navFile;
    const int line = m_navLine;
    rememberPosition();          // so the next toggle comes back here
    const QString backFile = m_navFile;
    const int backLine = m_navLine;
    openFile(file, line);        // records again; restore the toggle target
    m_navFile = backFile;
    m_navLine = backLine;
}

void MainWindow::insertRefToLabel(bool next)
{
    CodeEditor *e = currentEditor();
    if (!e) return;
    static const QRegularExpression label(R"(\\label\{([^}]+)\})");
    const QString text = e->document()->toPlainText();
    const int pos = e->textCursor().position();
    QString key;
    if (next) {
        auto m = label.match(text, pos);
        if (m.hasMatch()) key = m.captured(1);
    } else {
        auto it = label.globalMatch(text.left(pos));
        while (it.hasNext()) key = it.next().captured(1);
    }
    if (key.isEmpty()) {
        statusBar()->showMessage(next ? tr("No \\label after the cursor in this file.")
                                      : tr("No \\label before the cursor in this file."), 4000);
        return;
    }
    insert(QString("%1{%2}%|").arg(key.startsWith("eq:") ? "\\eqref" : "\\ref", key));
}

void MainWindow::insertFileCommand(const QString &command)
{
    const QString root = m_project ? m_project->rootDir() : QDir::homePath();
    const QString f = QFileDialog::getOpenFileName(this, tr("Choose file for %1").arg(command), root,
                                                   tr("LaTeX (*.tex);;All files (*)"));
    if (f.isEmpty()) return;
    QString rel = QDir(root).relativeFilePath(f);
    if (rel.endsWith(".tex")) rel.chop(4);
    insert(QString("%1{%2}\n%|").arg(command, rel));
}

void MainWindow::findRepetitions()
{
    CodeEditor *e = currentEditor();
    if (!e) return;
    // A word (5+ letters) that comes back within the next 12 words.
    const int window = 12;
    QString text = e->document()->toPlainText();
    static const QRegularExpression comment(R"((?<!\\)%[^\n]*)");
    static const QRegularExpression command(R"(\\[A-Za-z@]+\*?)");
    static const QRegularExpression word(R"([A-Za-z\x{00C0}-\x{024F}]{5,})");
    text.replace(comment, QString());
    // Keep offsets: blank out commands rather than removing them.
    auto cm = command.globalMatch(text);
    QString masked = text;
    while (cm.hasNext()) {
        auto m = cm.next();
        masked.replace(m.capturedStart(), m.capturedLength(), QString(m.capturedLength(), ' '));
    }
    struct W { QString w; int pos; };
    QVector<W> words;
    auto it = word.globalMatch(masked);
    while (it.hasNext()) {
        auto m = it.next();
        words.push_back({m.captured().toLower(), int(m.capturedStart())});
    }
    m_repetitions->clear();
    const QString path = e->filePath();
    int found = 0;
    for (int i = 0; i < words.size(); ++i)
        for (int j = i + 1; j < words.size() && j <= i + window; ++j)
            if (words[i].w == words[j].w) {
                const int line = text.left(words[j].pos).count('\n') + 1;
                auto *item = new QTreeWidgetItem(QStringList{
                    tr("\"%1\" repeated within %2 words").arg(words[i].w).arg(j - i),
                    QString("%1:%2").arg(QFileInfo(path).fileName()).arg(line)});
                item->setData(0, FileRole, path);
                item->setData(0, LineRole, line);
                m_repetitions->addTopLevelItem(item);
                ++found;
                break;
            }
    m_messagesDock->show();
    m_messageTabs->setCurrentWidget(m_repetitions);
    m_messageTabs->setTabText(m_messageTabs->indexOf(m_repetitions), tr("Repetitions (%1)").arg(found));
}

void MainWindow::thesaurus()
{
    CodeEditor *e = currentEditor();
    const QString w = e ? e->wordOrSelection(false).trimmed() : QString();
    if (w.isEmpty()) {
        statusBar()->showMessage(tr("Put the cursor on a word first."), 4000);
        return;
    }
#ifdef Q_OS_MACOS
    // The macOS Dictionary app includes a thesaurus.
    QDesktopServices::openUrl(QUrl("dict://" + QUrl::toPercentEncoding(w)));
#else
    QDesktopServices::openUrl(QUrl("https://www.thesaurus.com/browse/" + QUrl::toPercentEncoding(w)));
#endif
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    const int pt = QSettings().value("editor/fontSize", 13).toInt();
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *mw = qobject_cast<MainWindow *>(w))
            for (CodeEditor *e : mw->allEditors()) e->setEditorFontSize(pt);
}

void MainWindow::zoom(int delta)
{
    const int pt = qBound(8, QSettings().value("editor/fontSize", 13).toInt() + delta, 32);
    QSettings().setValue("editor/fontSize", pt);
    for (CodeEditor *e : allEditors()) e->setEditorFontSize(pt);
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About MasterTex"),
        tr("<h3>MasterTex %1</h3>"
           "<p>A LaTeX editor for large multi-file projects.</p>"
           "<p>Built with Qt %2.</p>").arg(QApplication::applicationVersion(), qVersion()));
}
