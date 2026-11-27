// Menus of the main window: File, Edit, LaTeX, Math, Wizards, Bibliography,
// Macros, Tools, View, Options, Help (and Preferences in the macOS app menu).

#include "MainWindow.h"

#include "CodeEditor.h"
#include "Dialogs.h"
#include "FindBar.h"
#include "BookToolsUi.h"
#include "Project.h"
#include "SettingsDialog.h"
#include "SymbolPanel.h"
#include "Wizards.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QUrl>
#include <algorithm>

namespace {

// Build icons drawn in colour, so they stay visible on light and dark toolbars.
enum class BuildIcon { Run, RunAll, Stop, Pdf };
QIcon buildIcon(BuildIcon kind)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const QColor green("#2fa84f"), red("#d9443a");
    auto triangle = [&](qreal x0, qreal w) {
        QPolygonF t;
        t << QPointF(x0, 10) << QPointF(x0 + w, 32) << QPointF(x0, 54);
        p.drawPolygon(t);
    };
    switch (kind) {
    case BuildIcon::Run: p.setBrush(green); triangle(16, 38); break;
    case BuildIcon::RunAll: p.setBrush(green); triangle(6, 28); triangle(30, 28); break;
    case BuildIcon::Stop: p.setBrush(red); p.drawRoundedRect(QRectF(14, 14, 36, 36), 5, 5); break;
    case BuildIcon::Pdf: {
        p.setBrush(red);
        p.drawRoundedRect(QRectF(8, 4, 48, 56), 6, 6);
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(17);
        p.setFont(f);
        p.drawText(QRectF(8, 4, 48, 56), Qt::AlignCenter, "PDF");
        break;
    }
    }
    return QIcon(pm);
}

using Snippets = QList<QPair<QString, QString>>;

} // namespace

QMenu *MainWindow::snippetMenu(QMenu *parent, const QString &title, const Snippets &items)
{
    QMenu *m = parent->addMenu(title);
    for (const auto &it : items) {
        if (it.first.isEmpty()) { m->addSeparator(); continue; }
        const QString snippet = it.second;
        m->addAction(act(it.first, QKeySequence(), [this, snippet]() { insert(snippet); }));
    }
    return m;
}

void MainWindow::createMenus()
{
    createFileMenu();
    createEditMenu();
    createLatexMenu();
    createToolsMenu();
    createMathMenu();
    createOtherMenus();
}

// ---------------------------------------------------------------------------
// File

void MainWindow::createFileMenu()
{
    QStyle *st = style();
    QMenu *file = menuBar()->addMenu(tr("&File"));
    m_a["new"] = act(tr("&New File…"), QKeySequence::New, [this]() { newFile(); }, st->standardIcon(QStyle::SP_FileIcon));
    m_a["new"]->setToolTip(tr("New file"));
    m_a["open"] = act(tr("&Open File…"), QKeySequence::Open, [this]() { openFileDialog(); }, st->standardIcon(QStyle::SP_DialogOpenButton));
    m_a["open"]->setToolTip(tr("Open a file"));
    m_a["openProject"] = act(tr("Open &Project…"), QKeySequence("Ctrl+Shift+O"), [this]() { openProjectDialog(); }, st->standardIcon(QStyle::SP_DirOpenIcon));
    m_a["openProject"]->setToolTip(tr("Open a project — choose the master .tex file (Shift+Cmd+O)"));
    file->addAction(m_a["new"]);
    file->addAction(m_a["open"]);
    file->addAction(m_a["openProject"]);
    m_recentMenu = file->addMenu(tr("Recent Projects"));
    updateRecentMenu();
    file->addSeparator();
    file->addAction(act(tr("New &Window"), QKeySequence("Ctrl+Shift+N"), [this]() {
        newWindow(m_project ? m_project->masterFile() : QString());
    }));
    file->addAction(act(tr("Move Tab to New Window"), QKeySequence(), [this]() {
        CodeEditor *e = currentEditor();
        if (!e) return;
        const QString path = e->filePath();
        const int line = e->currentLine();
        MainWindow *w = newWindow(m_project ? m_project->masterFile() : QString());
        w->openFile(path, line);
        closeEditor(e);
    }));
    file->addSeparator();
    m_a["save"] = act(tr("&Save"), QKeySequence::Save, [this]() {
        if (CodeEditor *e = currentEditor()) saveDocument(e->document());
    }, st->standardIcon(QStyle::SP_DialogSaveButton));
    m_a["saveAs"] = act(tr("Save &As…"), QKeySequence("Ctrl+Shift+S"), [this]() {
        if (CodeEditor *e = currentEditor()) saveDocumentAs(e->document());
    }, st->standardIcon(QStyle::SP_DriveFDIcon));
    m_a["saveAll"] = act(tr("Save A&ll"), QKeySequence("Ctrl+Alt+S"), [this]() { saveAll(); });
    file->addAction(m_a["save"]);
    file->addAction(m_a["saveAs"]);
    file->addAction(m_a["saveAll"]);
    file->addSeparator();
    m_a["close"] = act(tr("&Close File"), QKeySequence::Close, [this]() { closeEditor(currentEditor()); },
                       st->standardIcon(QStyle::SP_DialogCloseButton));
    file->addAction(m_a["close"]);
    file->addAction(act(tr("Close Window"), QKeySequence("Ctrl+Shift+W"), [this]() { close(); }));
    file->addSeparator();
    QAction *quit = act(tr("&Quit"), QKeySequence::Quit, []() {
        qApp->closeAllWindows();
        const auto tops = QApplication::topLevelWidgets();
        if (std::none_of(tops.begin(), tops.end(),
                         [](QWidget *w) { return qobject_cast<MainWindow *>(w) && w->isVisible(); }))
            qApp->quit();
    });
    quit->setMenuRole(QAction::QuitRole);
    file->addAction(quit);
}

// ---------------------------------------------------------------------------
// Edit

void MainWindow::createEditMenu()
{
    auto ed = [this](std::function<void(CodeEditor *)> f) {
        return [this, f]() { if (CodeEditor *e = currentEditor()) f(e); };
    };
    auto withFind = [this](std::function<void(FindBar *)> f) {
        return [this, f]() {
            if (!currentEditor()) return;
            m_findBar->setEditor(currentEditor());
            f(m_findBar);
        };
    };

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(act(tr("&Undo"), QKeySequence::Undo, ed([](CodeEditor *e) { e->undo(); })));
    edit->addAction(act(tr("&Redo"), QKeySequence::Redo, ed([](CodeEditor *e) { e->redo(); })));
    edit->addSeparator();
    edit->addAction(act(tr("Cu&t"), QKeySequence::Cut, ed([](CodeEditor *e) { e->cut(); })));
    edit->addAction(act(tr("&Copy"), QKeySequence::Copy, ed([](CodeEditor *e) { e->copy(); })));
    edit->addAction(act(tr("&Paste"), QKeySequence::Paste, ed([](CodeEditor *e) { e->paste(); })));
    edit->addSeparator();

    // Selection
    QMenu *sel = edit->addMenu(tr("Selection"));
    sel->addAction(act(tr("Select All"), QKeySequence::SelectAll, ed([](CodeEditor *e) { e->selectAll(); })));
    sel->addAction(act(tr("Select All Occurrences"), QKeySequence("Ctrl+Shift+L"), ed([this](CodeEditor *e) {
        const QString w = e->wordOrSelection(true);
        if (w.isEmpty()) return;
        const int n = e->highlightAll(QRegularExpression(QRegularExpression::escape(w)));
        statusBar()->showMessage(tr("%n occurrence(s) of \"%1\" highlighted — Esc clears", nullptr, n).arg(w), 5000);
    })));
    sel->addAction(act(tr("Select Previous Occurrence"), QKeySequence("Ctrl+Shift+D"), ed([](CodeEditor *e) { e->selectOccurrence(true); })));
    sel->addAction(act(tr("Select Next Occurrence"), QKeySequence("Ctrl+D"), ed([](CodeEditor *e) { e->selectOccurrence(false); })));

    // Line operations (Control-key bindings as in Emacs / macOS text fields)
    QMenu *lines = edit->addMenu(tr("Line Operations"));
    lines->addAction(act(tr("Delete Line"), QKeySequence("Ctrl+Shift+K"), ed([](CodeEditor *e) { e->deleteLines(); })));
    lines->addAction(act(tr("Cut Line or Selection"), QKeySequence("Ctrl+Alt+X"), ed([](CodeEditor *e) { e->cutLineOrSelection(); })));
    lines->addSeparator();
    lines->addAction(act(tr("Go to Beginning of Line"), QKeySequence("Meta+A"), ed([](CodeEditor *e) { e->gotoLineStart(); })));
    lines->addAction(act(tr("Go to End of Line"), QKeySequence("Meta+E"), ed([](CodeEditor *e) { e->gotoLineEnd(); })));
    lines->addAction(act(tr("Delete to End of Line"), QKeySequence("Meta+K"), ed([](CodeEditor *e) { e->deleteToLineEnd(); })));
    lines->addAction(act(tr("Delete to Beginning of Line"), QKeySequence("Meta+U"), ed([](CodeEditor *e) { e->deleteToLineStart(); })));
    lines->addSeparator();
    lines->addAction(act(tr("Move Line Up"), QKeySequence("Alt+Up"), ed([](CodeEditor *e) { e->moveLines(-1); })));
    lines->addAction(act(tr("Move Line Down"), QKeySequence("Alt+Down"), ed([](CodeEditor *e) { e->moveLines(+1); })));
    lines->addAction(act(tr("Duplicate Line"), QKeySequence("Shift+Alt+Down"), ed([](CodeEditor *e) { e->duplicateLines(); })));
    lines->addAction(act(tr("Sort Lines"), QKeySequence(), ed([this](CodeEditor *e) {
        if (!e->sortLines()) statusBar()->showMessage(tr("Select two or more lines to sort."), 4000);
    })));

    // Text operations
    QMenu *textOps = edit->addMenu(tr("Text Operations"));
    textOps->addAction(act(tr("To Lowercase"), QKeySequence(), ed([](CodeEditor *e) { e->changeCase(CodeEditor::CaseOp::Lower); })));
    textOps->addAction(act(tr("To Uppercase"), QKeySequence(), ed([](CodeEditor *e) { e->changeCase(CodeEditor::CaseOp::Upper); })));
    textOps->addAction(act(tr("To Titlecase (Strict)"), QKeySequence(), ed([](CodeEditor *e) { e->changeCase(CodeEditor::CaseOp::TitleStrict); })));
    QAction *smart = act(tr("To Titlecase (Smart)"), QKeySequence(), ed([](CodeEditor *e) { e->changeCase(CodeEditor::CaseOp::TitleSmart); }));
    smart->setToolTip(tr("Keeps short words (of, the, and…) lowercase and leaves acronyms and LaTeX commands alone"));
    textOps->addAction(smart);
    edit->addSeparator();

    // Search
    QMenu *search = edit->addMenu(tr("Search"));
    search->addAction(act(tr("Find…"), QKeySequence::Find, withFind([](FindBar *f) { f->showBar(false); })));
    search->addAction(act(tr("Find Next"), QKeySequence::FindNext, withFind([](FindBar *f) { f->findNext(false); })));
    search->addAction(act(tr("Find Previous"), QKeySequence::FindPrevious, withFind([](FindBar *f) { f->findNext(true); })));
    search->addAction(act(tr("Continue Find"), QKeySequence("Ctrl+Alt+G"), withFind([](FindBar *f) {
        f->show();
        f->findNext(false);
    })));
    search->addAction(act(tr("Count"), QKeySequence(), withFind([](FindBar *f) { f->show(); f->count(); })));
    search->addAction(act(tr("Select All Matches"), QKeySequence(), withFind([](FindBar *f) { f->show(); f->selectAllMatches(); })));

    // Replace
    QMenu *replace = edit->addMenu(tr("Replace"));
    replace->addAction(act(tr("Replace…"), QKeySequence("Ctrl+Alt+F"), withFind([](FindBar *f) { f->showBar(true); })));
    replace->addAction(act(tr("Replace Next"), QKeySequence(), withFind([](FindBar *f) { f->showBar(true); f->replaceNext(false); })));
    replace->addAction(act(tr("Replace Previous"), QKeySequence(), withFind([](FindBar *f) { f->showBar(true); f->replaceNext(true); })));
    replace->addAction(act(tr("Replace All"), QKeySequence(), withFind([](FindBar *f) { f->showBar(true); f->replaceAll(); })));
    replace->addSeparator();
    replace->addAction(act(tr("Replace in All Files…"), QKeySequence("Ctrl+Shift+F"), [this]() { replaceInAllFiles(); }));
    edit->addAction(act(tr("Replace in All Files…"), QKeySequence(), [this]() { replaceInAllFiles(); }));
    QMenu *rebase = edit->addMenu(tr("Rebase Labels"));
    rebase->addAction(act(tr("Rebase (Current File)…"), QKeySequence(), [this]() { rebaseLabels(true); }));
    rebase->addAction(act(tr("Rebase All Files…"), QKeySequence(), [this]() { rebaseLabels(false); }));

    // Go to
    QMenu *go = edit->addMenu(tr("Go To"));
    go->addAction(act(tr("Line…"), QKeySequence("Ctrl+L"), [this]() { gotoLineDialog(); }));
    go->addAction(act(tr("Previous Change"), QKeySequence("Meta+Alt+Up"), ed([this](CodeEditor *e) {
        if (!e->gotoChange(false)) statusBar()->showMessage(tr("No earlier change in this file."), 3000);
    })));
    go->addAction(act(tr("Next Change"), QKeySequence("Meta+Alt+Down"), ed([this](CodeEditor *e) {
        if (!e->gotoChange(true)) statusBar()->showMessage(tr("No later change in this file."), 3000);
    })));
    go->addAction(act(tr("Go Back / Go Forward (Toggle)"), QKeySequence("Meta+-"), [this]() { toggleBackForward(); }));

    // Bookmarks
    QMenu *gotoBm = edit->addMenu(tr("Go to Bookmark"));
    QMenu *toggleBm = edit->addMenu(tr("Toggle Bookmark"));
    for (int n = 1; n <= 9; ++n) {
        gotoBm->addAction(act(tr("Bookmark %1").arg(n), QKeySequence(QString("Ctrl+%1").arg(n)), ed([this, n](CodeEditor *e) {
            rememberPosition();
            if (!e->gotoBookmark(n)) statusBar()->showMessage(tr("Bookmark %1 is not set in this file.").arg(n), 3000);
        })));
        toggleBm->addAction(act(tr("Bookmark %1").arg(n), QKeySequence(QString("Ctrl+Alt+%1").arg(n)),
                                ed([n](CodeEditor *e) { e->toggleBookmark(n); })));
    }
    toggleBm->addSeparator();
    toggleBm->addAction(act(tr("Clear All Bookmarks"), QKeySequence(), ed([](CodeEditor *e) { e->clearBookmarks(); })));
    edit->addSeparator();
    edit->addAction(act(tr("Toggle Comment"), QKeySequence("Ctrl+/"), ed([](CodeEditor *e) { e->toggleComment(); })));
    edit->addAction(act(tr("Insert Unicode Character…"), QKeySequence(), [this]() { insert(Dialogs::unicodeCharacter(this)); }));
    // macOS adds AutoFill, Start Dictation… and Emoji & Symbols to this menu itself.
}

// ---------------------------------------------------------------------------
// Tools

void MainWindow::createToolsMenu()
{
    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    tools->addAction(act(tr("Build && View"), QKeySequence("Ctrl+Shift+B"), [this]() {
        m_viewAfterBuild = true;
        runBuild(false);
    }));
    m_runAct = act(tr("&Compile"), QKeySequence("Ctrl+R"), [this]() { runBuild(false); }, buildIcon(BuildIcon::Run));
    m_runAllAct = act(tr("Compile &All (full rebuild)"), QKeySequence("Ctrl+Shift+R"), [this]() { runBuild(true); }, buildIcon(BuildIcon::RunAll));
    m_stopAct = act(tr("&Stop Compile"), QKeySequence("Ctrl+."), [this]() { m_build->kill(); }, buildIcon(BuildIcon::Stop));
    m_stopAct->setEnabled(false);
    m_a["viewPdf"] = act(tr("&View"), QKeySequence("Ctrl+Shift+V"), [this]() { viewPdf(); }, buildIcon(BuildIcon::Pdf));
    m_a["viewPdf"]->setToolTip(tr("View PDF"));
    tools->addAction(m_runAct);
    tools->addAction(m_runAllAct);
    tools->addAction(m_stopAct);
    tools->addAction(m_a["viewPdf"]);
    tools->addAction(act(tr("Show Cursor Position in PDF"), QKeySequence("Ctrl+Alt+P"), [this]() {
        syncToPdf(currentEditor());
    }));
    tools->addAction(act(tr("View PDF in External Viewer"), QKeySequence(), [this]() { viewPdfExternal(); }));
    tools->addSeparator();
    tools->addAction(act(tr("&Bibliography"), QKeySequence(), [this]() { runAuxTool("bibliography"); }));
    tools->addAction(act(tr("&Glossary"), QKeySequence(), [this]() { runAuxTool("glossary"); }));
    tools->addAction(act(tr("&Index"), QKeySequence(), [this]() { runAuxTool("index"); }));

    QMenu *commands = tools->addMenu(tr("Commands"));
    auto single = [this](const QString &engine) {
        return [this, engine]() {
            if (!ensureProject()) return;
            runTool(engine, {"-synctex=1", "-interaction=nonstopmode", "-file-line-error",
                             QFileInfo(m_project->masterFile()).fileName()},
                    tr("Running %1…").arg(engine), true);
        };
    };
    commands->addAction(act(tr("latexmk (Compile)"), QKeySequence(), [this]() { runBuild(false); }));
    commands->addAction(act(tr("pdfLaTeX (one run)"), QKeySequence(), single("pdflatex")));
    commands->addAction(act(tr("XeLaTeX (one run)"), QKeySequence(), single("xelatex")));
    commands->addAction(act(tr("LuaLaTeX (one run)"), QKeySequence(), single("lualatex")));
    commands->addSeparator();
    commands->addAction(act(tr("BibTeX"), QKeySequence(), [this]() {
        if (ensureProject()) runTool("bibtex", {m_project->name()}, tr("Running bibtex…"), false);
    }));
    commands->addAction(act(tr("Biber"), QKeySequence(), [this]() {
        if (ensureProject()) runTool("biber", {m_project->name()}, tr("Running biber…"), false);
    }));
    commands->addAction(act(tr("makeindex"), QKeySequence(), [this]() { runAuxTool("index"); }));
    commands->addAction(act(tr("makeglossaries"), QKeySequence(), [this]() { runAuxTool("glossary"); }));

    QMenu *user = tools->addMenu(tr("User"));
    user->addAction(tr("User commands — coming in a later milestone"))->setEnabled(false);

    tools->addSeparator();
    tools->addAction(act(tr("Restructure Document…"), QKeySequence("Ctrl+Alt+T"), [this]() { restructure(); }));
    tools->addAction(act(tr("Undo Last Book-Wide Change…"), QKeySequence(), [this]() { undoRestructure(); }));
    tools->addSeparator();
    QMenu *checks = tools->addMenu(tr("Checks"));
    checks->addAction(act(tr("Check Eqs"), QKeySequence(), [this]() { showChecks(); m_checks->runCheck(BookTools::CheckKind::Equations); }));
    checks->addAction(act(tr("Check Tabs"), QKeySequence(), [this]() { showChecks(); m_checks->runCheck(BookTools::CheckKind::Tables); }));
    checks->addAction(act(tr("Check Figs"), QKeySequence(), [this]() { showChecks(); m_checks->runCheck(BookTools::CheckKind::Figures); }));
    checks->addSeparator();
    checks->addAction(act(tr("Check / Fix Figure Files…"), QKeySequence(), [this]() { showChecks(); m_checks->runFigureFiles(); }));
    tools->addSeparator();
    tools->addAction(act(tr("Clean Auxiliary Files"), QKeySequence(), [this]() { cleanBuild(); }));
    tools->addAction(act(tr("Open External Terminal"), QKeySequence(), [this]() { openTerminal(); }));
    tools->addSeparator();
    tools->addAction(act(tr("View Log"), QKeySequence(), [this]() { viewLog(); }));
    tools->addAction(act(tr("Clear Log"), QKeySequence(), [this]() {
        m_log->clear();
        m_buildOutput.clear();
        m_buildIssues.clear();
        updateIssues();
        updateLogMarkers();
    }));
    QAction *markers = act(tr("View Log Markers"), QKeySequence(), []() {});
    markers->setCheckable(true);
    markers->setChecked(m_logMarkersOn);
    markers->setToolTip(tr("Show build errors and warnings as dots beside the line numbers"));
    connect(markers, &QAction::toggled, this, [this](bool on) {
        m_logMarkersOn = on;
        updateLogMarkers();
    });
    tools->addAction(markers);
    tools->addSeparator();
    tools->addAction(act(tr("Generate Random Text (Lorem Ipsum)…"), QKeySequence(), [this]() { insert(Dialogs::loremIpsum(this)); }));
    tools->addAction(tr("Check Spelling — coming in a later milestone"))->setEnabled(false);
    tools->addAction(act(tr("Thesaurus"), QKeySequence(), [this]() { thesaurus(); }));
    tools->addAction(act(tr("Find Repetitions"), QKeySequence(), [this]() { findRepetitions(); }));
}

// ---------------------------------------------------------------------------
// LaTeX

void MainWindow::createLatexMenu()
{
    QMenu *latex = menuBar()->addMenu(tr("&LaTeX"));
    latex->addAction(act(tr("\\documentclass…"), QKeySequence(), [this]() { insert(Dialogs::documentClass(this)); }));
    latex->addAction(act(tr("\\usepackage…"), QKeySequence(), [this]() { insert(Dialogs::usePackage(this)); }));
    latex->addAction(act(tr("AMS Packages"), QKeySequence(), [this]() {
        insert("\\usepackage{amsmath}\n\\usepackage{amssymb}\n\\usepackage{amsthm}\n%|");
    }));
    latex->addSeparator();
    latex->addAction(act(tr("\\begin{document} … \\end{document}"), QKeySequence(), [this]() {
        insert("\\begin{document}\n\n%|\n\n\\end{document}\n");
    }));
    latex->addAction(act("\\title{}", QKeySequence(), [this]() { insert("\\title{%|}\n"); }));
    latex->addAction(act("\\author{}", QKeySequence(), [this]() { insert("\\author{%|}\n"); }));
    latex->addAction(act("\\date{}", QKeySequence(), [this]() { insert("\\date{%|\\today}\n"); }));
    latex->addAction(act("\\maketitle", QKeySequence(), [this]() { insert("\\maketitle\n%|"); }));
    latex->addAction(act("\\tableofcontents", QKeySequence(), [this]() { insert("\\tableofcontents\n%|"); }));
    latex->addSeparator();

    // Sectioning (asks for a title and adds a matching \label)
    QMenu *sect = latex->addMenu(tr("Sectioning"));
    const QList<QPair<QString, QString>> sections = {
        {"\\part", "part"}, {"\\chapter", "ch"}, {"\\section", "sec"},
        {"\\subsection", "sec"}, {"\\subsubsection", "sec"}, {"\\paragraph", ""}, {"\\subparagraph", ""}};
    for (const auto &s : sections) {
        QAction *a = act(s.first, QKeySequence(), [this, s]() {
            const QString title = QInputDialog::getText(this, tr("Insert %1").arg(s.first), tr("Title:"));
            if (title.isEmpty()) return;
            QString snippet = QString("%1{%2}\n").arg(s.first, title);
            if (!s.second.isEmpty()) snippet += QString("\\label{%1}\n").arg(Wizards::makeLabel(s.second, title));
            insert(snippet + "%|");
        });
        sect->addAction(a);
        if (s.first == "\\section") m_a["section"] = a;
    }

    auto env = [](const QString &name, const QString &arg = QString(), const QString &body = "  %|\n") {
        return QString("\\begin{%1}%2\n%3\\end{%1}\n").arg(name, arg, body);
    };
    snippetMenu(latex, tr("Environments"), {
        {"center", env("center")}, {"flushleft", env("flushleft")}, {"flushright", env("flushright")},
        {"quote", env("quote")}, {"quotation", env("quotation")}, {"verse", env("verse")},
        {"abstract", env("abstract")}, {"minipage", env("minipage", "{0.45\\linewidth}")},
        {"verbatim", env("verbatim")}, {"figure", env("figure", "[htbp]", "  \\centering\n  %|\n  \\caption{}\n  \\label{fig:}\n")},
        {"table", env("table", "[htbp]", "  \\centering\n  %|\n  \\caption{}\n  \\label{tab:}\n")}});

    QMenu *lists = latex->addMenu(tr("List Environments"));
    m_a["env-itemize"] = act("itemize", QKeySequence(), [this, env]() { insert(env("itemize", QString(), "  \\item %|\n")); });
    m_a["env-enumerate"] = act("enumerate", QKeySequence(), [this, env]() { insert(env("enumerate", QString(), "  \\item %|\n")); });
    lists->addAction(m_a["env-itemize"]);
    lists->addAction(m_a["env-enumerate"]);
    lists->addAction(act("description", QKeySequence(), [this, env]() { insert(env("description", QString(), "  \\item[%|] \n")); }));
    lists->addAction(act("\\item", QKeySequence("Ctrl+Shift+I"), [this]() { insert("\\item %|"); }));

    snippetMenu(latex, tr("Boxes"), {
        {"\\mbox{}", "\\mbox{%|}"}, {"\\fbox{}", "\\fbox{%|}"}, {"\\framebox[width][pos]{}", "\\framebox[%|\\width][c]{}"},
        {"\\makebox[width][pos]{}", "\\makebox[%|\\width][c]{}"}, {"\\parbox{width}{}", "\\parbox{0.5\\linewidth}{%|}"},
        {"\\raisebox{lift}{}", "\\raisebox{%|1ex}{}"}, {"minipage", env("minipage", "{0.45\\linewidth}")}});

    QMenu *font = latex->addMenu(tr("Font"));
    QMenu *styles = font->addMenu(tr("Styles"));
    m_a["bold"] = act(tr("Bold  \\textbf"), QKeySequence::Bold, [this]() { insert("\\textbf{%|}"); });
    m_a["italic"] = act(tr("Italic  \\textit"), QKeySequence::Italic, [this]() { insert("\\textit{%|}"); });
    m_a["emph"] = act(tr("Emphasis  \\emph"), QKeySequence("Ctrl+E"), [this]() { insert("\\emph{%|}"); });
    styles->addAction(m_a["bold"]);
    styles->addAction(m_a["italic"]);
    styles->addAction(m_a["emph"]);
    for (const auto &p : Snippets{{tr("Slanted  \\textsl"), "\\textsl{%|}"}, {tr("Small Caps  \\textsc"), "\\textsc{%|}"},
                                  {tr("Typewriter  \\texttt"), "\\texttt{%|}"}, {tr("Sans Serif  \\textsf"), "\\textsf{%|}"},
                                  {tr("Roman  \\textrm"), "\\textrm{%|}"}, {tr("Upright  \\textup"), "\\textup{%|}"},
                                  {tr("Medium  \\textmd"), "\\textmd{%|}"}, {tr("Underline  \\underline"), "\\underline{%|}"}}) {
        const QString snip = p.second;
        styles->addAction(act(p.first, QKeySequence(), [this, snip]() { insert(snip); }));
    }
    QList<QPair<QString, QString>> sizes;
    for (const char *s : {"tiny", "scriptsize", "footnotesize", "small", "normalsize", "large", "Large", "LARGE", "huge", "Huge"})
        sizes << qMakePair(QString("\\%1").arg(s), QString("{\\%1 %|}").arg(s));
    snippetMenu(font, tr("Sizes"), sizes);

    QMenu *tab = latex->addMenu(tr("Tabular Environment"));
    tab->addAction(act(tr("Table Wizard…"), QKeySequence(), [this]() { insert(Wizards::table(this)); }));
    tab->addSeparator();
    for (const auto &p : Snippets{{"tabular", env("tabular", "{%|lcr}", "  a & b & c \\\\\n")},
                                  {"\\hline", "\\hline\n%|"}, {"\\cline{i-j}", "\\cline{%|}"},
                                  {"\\toprule / \\midrule / \\bottomrule", "\\toprule\n%|\n\\midrule\n\n\\bottomrule\n"},
                                  {"\\multicolumn{n}{align}{text}", "\\multicolumn{%|2}{c}{}"},
                                  {"\\multirow{n}{width}{text}", "\\multirow{%|2}{*}{}"},
                                  {tr("New Row  \\\\"), " \\\\\n%|"}}) {
        const QString snip = p.second;
        tab->addAction(act(p.first, QKeySequence(), [this, snip]() { insert(snip); }));
    }

    snippetMenu(latex, tr("Horizontal Spacing"), {
        {"\\,  thin", "\\,"}, {"\\:  medium", "\\:"}, {"\\;  thick", "\\;"}, {"\\enspace", "\\enspace "},
        {"\\quad", "\\quad "}, {"\\qquad", "\\qquad "}, {"\\hspace{}", "\\hspace{%|}"}, {"\\hfill", "\\hfill "},
        {"~  non-breaking space", "~"}});
    snippetMenu(latex, tr("Vertical Spacing"), {
        {"\\smallskip", "\\smallskip\n%|"}, {"\\medskip", "\\medskip\n%|"}, {"\\bigskip", "\\bigskip\n%|"},
        {"\\vspace{}", "\\vspace{%|}\n"}, {"\\vfill", "\\vfill\n%|"}, {"\\newline", "\\newline\n%|"},
        {"\\newpage", "\\newpage\n%|"}, {"\\clearpage", "\\clearpage\n%|"}});
    snippetMenu(latex, tr("International Accents"), {
        {"é  \\'{e}", "\\'{%|}"}, {"è  \\`{e}", "\\`{%|}"}, {"ê  \\^{e}", "\\^{%|}"}, {"ë  \\\"{e}", "\\\"{%|}"},
        {"ñ  \\~{n}", "\\~{%|}"}, {"ā  \\={a}", "\\={%|}"}, {"ż  \\.{z}", "\\.{%|}"}, {"ă  \\u{a}", "\\u{%|}"},
        {"š  \\v{s}", "\\v{%|}"}, {"ő  \\H{o}", "\\H{%|}"}, {"ç  \\c{c}", "\\c{%|}"}, {"ą  \\k{a}", "\\k{%|}"},
        {"å  \\r{a}", "\\r{%|}"}, {"", ""}, {"å  \\aa", "\\aa{}"}, {"Å  \\AA", "\\AA{}"}, {"ø  \\o", "\\o{}"},
        {"Ø  \\O", "\\O{}"}, {"ß  \\ss", "\\ss{}"}, {"æ  \\ae", "\\ae{}"}, {"œ  \\oe", "\\oe{}"}, {"ł  \\l", "\\l{}"},
        {"¿  ?`", "?`"}, {"¡  !`", "!`"}});

    QMenu *files = latex->addMenu(tr("Input / Include Files"));
    files->addAction(act(tr("\\input{file}…"), QKeySequence(), [this]() { insertFileCommand("\\input"); }));
    files->addAction(act(tr("\\include{file}…"), QKeySequence(), [this]() { insertFileCommand("\\include"); }));
    files->addAction(act(tr("\\includeonly{}"), QKeySequence(), [this]() { insert("\\includeonly{%|}\n"); }));
    files->addAction(act(tr("New Chapter File…"), QKeySequence(), [this]() { wizardChapter(); }));

    QMenu *xref = latex->addMenu(tr("Cross References"));
    m_a["label"] = act(tr("\\label{}"), QKeySequence(), [this]() { insert("\\label{%|}"); });
    m_a["ref"] = act(tr("\\ref{} (choose label)…"), QKeySequence("Ctrl+Alt+R"), [this]() { insertReference(); });
    m_a["cite"] = act(tr("\\cite{} (choose entry)…"), QKeySequence("Ctrl+Alt+C"), [this]() { insertCitation(); });
    xref->addAction(m_a["label"]);
    xref->addAction(m_a["ref"]);
    xref->addAction(act(tr("\\eqref{}"), QKeySequence(), [this]() { insert("\\eqref{%|}"); }));
    xref->addAction(act(tr("\\pageref{}"), QKeySequence(), [this]() { insert("\\pageref{%|}"); }));
    xref->addAction(m_a["cite"]);
    xref->addAction(act(tr("\\footnote{}"), QKeySequence(), [this]() { insert("\\footnote{%|}"); }));
    xref->addAction(act(tr("\\url{}"), QKeySequence(), [this]() { insert("\\url{%|}"); }));
    xref->addAction(act(tr("\\href{url}{text}"), QKeySequence(), [this]() { insert("\\href{%|}{}"); }));

    snippetMenu(latex, tr("Bibliography"), {
        {"\\bibliographystyle{}", "\\bibliographystyle{%|plain}\n"}, {"\\bibliography{}", "\\bibliography{%|}\n"},
        {"", ""}, {"\\usepackage{biblatex}", "\\usepackage[%|backend=biber]{biblatex}\n"},
        {"\\addbibresource{}", "\\addbibresource{%|.bib}\n"}, {"\\printbibliography", "\\printbibliography\n%|"}});
    latex->addSeparator();
    latex->addAction(act(tr("Insert \\ref to Next Label"), QKeySequence("Ctrl+Alt+]"), [this]() { insertRefToLabel(true); }));
    latex->addAction(act(tr("Insert \\ref to Previous Label"), QKeySequence("Ctrl+Alt+["), [this]() { insertRefToLabel(false); }));
    latex->addSeparator();
    latex->addAction(act(tr("Add / Remove Comment"), QKeySequence(), [this]() {
        if (CodeEditor *e = currentEditor()) e->toggleComment();
    }));
}

// ---------------------------------------------------------------------------
// Math

void MainWindow::createMathMenu()
{
    QMenu *math = menuBar()->addMenu(tr("&Math"));
    m_a["inlineMath"] = act(tr("Inline Math  $…$"), QKeySequence("Ctrl+Alt+M"), [this]() { insert("$%|$"); });
    math->addAction(m_a["inlineMath"]);
    math->addAction(act(tr("Display Math  \\[…\\]"), QKeySequence("Ctrl+Alt+Shift+M"), [this]() { insert("\\[\n  %|\n\\]\n"); }));
    math->addAction(act(tr("Subscript  _{}"), QKeySequence(), [this]() { insert("_{%|}"); }));
    math->addAction(act(tr("Superscript  ^{}"), QKeySequence(), [this]() { insert("^{%|}"); }));
    math->addAction(act(tr("Fraction  \\frac{}{}"), QKeySequence(), [this]() { insert("\\frac{%|}{}"); }));
    math->addAction(act(tr("Fraction  \\dfrac{}{}"), QKeySequence(), [this]() { insert("\\dfrac{%|}{}"); }));
    math->addAction(act(tr("Square Root  \\sqrt{}"), QKeySequence(), [this]() { insert("\\sqrt{%|}"); }));
    snippetMenu(math, tr("\\left … \\right"), {
        {"\\left( \\right)", "\\left( %| \\right)"}, {"\\left[ \\right]", "\\left[ %| \\right]"},
        {"\\left\\{ \\right\\}", "\\left\\{ %| \\right\\}"}, {"\\left| \\right|", "\\left| %| \\right|"},
        {"\\left\\langle \\right\\rangle", "\\left\\langle %| \\right\\rangle"},
        {"\\left. \\right|", "\\left. %| \\right|"}, {"\\left\\| \\right\\|", "\\left\\| %| \\right\\|"}});
    math->addAction(act(tr("\\begin{array} … \\end{array}"), QKeySequence(), [this]() {
        insert("\\begin{array}{%|cc}\n   &  \\\\\n   & \n\\end{array}");
    }));
    math->addSeparator();

    QMenu *eqs = math->addMenu(tr("Math Equations"));
    m_a["equation"] = act(tr("equation"), QKeySequence("Ctrl+Alt+E"), [this]() {
        insert("\\begin{equation}\n  %|\n  \\label{eq:}\n\\end{equation}\n");
    });
    eqs->addAction(m_a["equation"]);
    for (const auto &p : Snippets{
             {"equation*", "\\begin{equation*}\n  %|\n\\end{equation*}\n"},
             {"align", "\\begin{align}\n  %| &= \\\\\n    &= \n\\end{align}\n"},
             {"align*", "\\begin{align*}\n  %| &= \\\\\n    &= \n\\end{align*}\n"},
             {"gather", "\\begin{gather}\n  %| \\\\\n  \n\\end{gather}\n"},
             {"multline", "\\begin{multline}\n  %| \\\\\n  \n\\end{multline}\n"},
             {"split (inside equation)", "\\begin{split}\n  %| &= \\\\\n    &= \n\\end{split}"},
             {"cases", "\\begin{cases}\n  %| & \\text{if } \\\\\n   & \\text{otherwise}\n\\end{cases}"},
             {"subequations", "\\begin{subequations}\n  %|\n\\end{subequations}\n"}}) {
        const QString snip = p.second;
        eqs->addAction(act(p.first, QKeySequence(), [this, snip]() { insert(snip); }));
    }

    QList<QPair<QString, QString>> funcs;
    for (const char *f : {"sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
                          "tanh", "coth", "exp", "log", "ln", "lg", "lim", "liminf", "limsup", "max", "min",
                          "sup", "inf", "det", "dim", "ker", "arg", "deg", "gcd", "Pr", "hom"})
        funcs << qMakePair(QString("\\%1").arg(f), QString("\\%1").arg(f) +
                           (QString(f).startsWith("lim") || QString(f) == "max" || QString(f) == "min" ||
                            QString(f) == "sup" || QString(f) == "inf" ? "_{%|}" : " %|"));
    snippetMenu(math, tr("Math Functions"), funcs);

    snippetMenu(math, tr("Math Definitions"), {
        {"\\newtheorem{theorem}{Theorem}", "\\newtheorem{theorem}{Theorem}[chapter]\n%|"},
        {"\\DeclareMathOperator{}{}", "\\DeclareMathOperator{\\%|}{}\n"},
        {"\\newcommand{}{}", "\\newcommand{\\%|}{}\n"},
        {"", ""},
        {"theorem", "\\begin{theorem}\n  %|\n\\end{theorem}\n"},
        {"lemma", "\\begin{lemma}\n  %|\n\\end{lemma}\n"},
        {"definition", "\\begin{definition}\n  %|\n\\end{definition}\n"},
        {"proof", "\\begin{proof}\n  %|\n\\end{proof}\n"}});

    snippetMenu(math, tr("Math Font Styles"), {
        {"\\mathrm{}  roman", "\\mathrm{%|}"}, {"\\mathbf{}  bold", "\\mathbf{%|}"},
        {"\\boldsymbol{}  bold symbol", "\\boldsymbol{%|}"}, {"\\mathit{}  italic", "\\mathit{%|}"},
        {"\\mathsf{}  sans serif", "\\mathsf{%|}"}, {"\\mathtt{}  typewriter", "\\mathtt{%|}"},
        {"\\mathcal{}  calligraphic", "\\mathcal{%|}"}, {"\\mathbb{}  blackboard", "\\mathbb{%|}"},
        {"\\mathfrak{}  fraktur", "\\mathfrak{%|}"}, {"\\text{}  text", "\\text{%|}"}});

    snippetMenu(math, tr("Math Stacking Symbols"), {
        {"\\overset{above}{}", "\\overset{%|}{}"}, {"\\underset{below}{}", "\\underset{%|}{}"},
        {"\\stackrel{above}{}", "\\stackrel{%|}{}"}, {"\\overline{}", "\\overline{%|}"},
        {"\\underline{}", "\\underline{%|}"}, {"\\overbrace{}^{}", "\\overbrace{%|}^{}"},
        {"\\underbrace{}_{}", "\\underbrace{%|}_{}"}, {"\\overrightarrow{}", "\\overrightarrow{%|}"},
        {"\\overleftarrow{}", "\\overleftarrow{%|}"}, {"\\binom{n}{k}", "\\binom{%|}{}"},
        {"\\substack{}", "\\substack{%| \\\\ }"}});

    snippetMenu(math, tr("Math Accents"), {
        {"\\hat{}  â", "\\hat{%|}"}, {"\\widehat{}", "\\widehat{%|}"}, {"\\check{}  ǎ", "\\check{%|}"},
        {"\\tilde{}  ã", "\\tilde{%|}"}, {"\\widetilde{}", "\\widetilde{%|}"}, {"\\acute{}  á", "\\acute{%|}"},
        {"\\grave{}  à", "\\grave{%|}"}, {"\\dot{}  ȧ", "\\dot{%|}"}, {"\\ddot{}  ä", "\\ddot{%|}"},
        {"\\breve{}  ă", "\\breve{%|}"}, {"\\bar{}  ā", "\\bar{%|}"}, {"\\vec{}  a⃗", "\\vec{%|}"}});

    snippetMenu(math, tr("Math Spaces"), {
        {"\\,  thin", "\\,"}, {"\\:  medium", "\\:"}, {"\\;  thick", "\\;"}, {"\\!  negative thin", "\\!"},
        {"\\quad", "\\quad "}, {"\\qquad", "\\qquad "}});
    math->addSeparator();

    for (const SymbolPanel::Group &g : SymbolPanel::groups()) {
        QMenu *m = math->addMenu(tr("Symbols: %1").arg(tr(g.name)));
        for (const SymbolPanel::Symbol &s : g.symbols) {
            const QString latex = QString::fromUtf8(s.latex);
            QString shown = latex;
            shown.remove("%|");
            m->addAction(act(QString("%1    %2").arg(QString::fromUtf8(s.glyph), shown), QKeySequence(),
                             [this, latex]() { insert(latex); }));
        }
    }
    math->addAction(act(tr("Matrix…"), QKeySequence(), [this]() { insert(Wizards::matrix(this)); }));
    math->addAction(act(tr("Show Symbol Panes"), QKeySequence("Ctrl+Alt+Y"), [this]() {
        if (auto *d = findChild<QDockWidget *>("SymbolsDock")) { d->show(); d->raise(); }
    }));
}

// ---------------------------------------------------------------------------
// Wizards, Bibliography, Macros, View, Options, Help

void MainWindow::createOtherMenus()
{
    QMenu *wiz = menuBar()->addMenu(tr("&Wizards"));
    m_a["wizFigure"] = act(tr("&Figure…"), QKeySequence(), [this]() {
        insert(Wizards::figure(this, m_project ? m_project->rootDir() : QDir::homePath()));
    });
    m_a["wizTable"] = act(tr("&Table…"), QKeySequence(), [this]() { insert(Wizards::table(this)); });
    wiz->addAction(m_a["wizFigure"]);
    wiz->addAction(m_a["wizTable"]);
    wiz->addAction(act(tr("&List…"), QKeySequence(), [this]() { insert(Wizards::list(this)); }));
    wiz->addAction(act(tr("&Matrix…"), QKeySequence(), [this]() { insert(Wizards::matrix(this)); }));
    wiz->addSeparator();
    wiz->addAction(act(tr("New &Chapter File…"), QKeySequence(), [this]() { wizardChapter(); }));
    wiz->addAction(act(tr("New &Bibliography Entry…"), QKeySequence(), [this]() { insert(Wizards::bibEntry(this)); }));

    QMenu *bib = menuBar()->addMenu(tr("&Bibliography"));
    bib->addAction(m_a["cite"]);
    bib->addAction(act(tr("&Open Bibliography File"), QKeySequence(), [this]() {
        if (!m_project || m_project->data().bibFiles.isEmpty()) {
            statusBar()->showMessage(tr("This project has no bibliography file."), 5000);
            return;
        }
        const QStringList bibs = m_project->data().bibFiles;
        QString pick = bibs.first();
        if (bibs.size() > 1) {
            bool ok = false;
            pick = QInputDialog::getItem(this, tr("Bibliography"), tr("File:"), bibs, 0, false, &ok);
            if (!ok) return;
        }
        openFile(pick);
    }));
    bib->addAction(act(tr("&New Entry…"), QKeySequence(), [this]() { insert(Wizards::bibEntry(this)); }));
    bib->addSeparator();
    bib->addAction(act(tr("Run BibTeX / Biber"), QKeySequence(), [this]() { runAuxTool("bibliography"); }));

    QMenu *macros = menuBar()->addMenu(tr("Ma&cros"));
    macros->addAction(tr("User macros — coming in a later milestone"))->setEnabled(false);

    m_viewMenu = menuBar()->addMenu(tr("&View"));
    for (QDockWidget *d : findChildren<QDockWidget *>()) m_viewMenu->addAction(d->toggleViewAction());
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(act(tr("Undock Editor"), QKeySequence("Ctrl+Shift+U"), [this]() {
        CodeEditor *e = currentEditor();
        if (!e) return;
        if (isUndocked(e)) statusBar()->showMessage(tr("This editor is already undocked."), 3000);
        else undockEditor(e);
    }));
    m_viewMenu->addAction(act(tr("Dock Editor Back"), QKeySequence("Ctrl+Shift+J"), [this]() {
        if (CodeEditor *e = currentEditor()) dockBack(e);
    }));
    m_viewMenu->addAction(act(tr("Dock All Editors Back"), QKeySequence(), [this]() { dockAllBack(); }));
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(act(tr("Zoom &In"), QKeySequence::ZoomIn, [this]() { zoom(+1); }));
    m_viewMenu->addAction(act(tr("Zoom &Out"), QKeySequence::ZoomOut, [this]() { zoom(-1); }));
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(act(tr("&Full Screen"), QKeySequence::FullScreen, [this]() {
        isFullScreen() ? showNormal() : showFullScreen();
    }));

    QMenu *options = menuBar()->addMenu(tr("&Options"));
    QAction *settings = act(tr("&Settings…"), QKeySequence(), [this]() { showSettings(); });
    settings->setMenuRole(QAction::NoRole);   // stays in the Options menu on macOS
    options->addAction(settings);
    // On macOS, Qt moves this item to the MasterTex application menu.
    QAction *prefs = act(tr("Preferences…"), QKeySequence::Preferences, [this]() {
        PreferencesDialog dlg(this);
        dlg.exec();
    });
    prefs->setMenuRole(QAction::PreferencesRole);
    options->addAction(prefs);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAct = act(tr("&About MasterTex"), QKeySequence(), [this]() { about(); });
    aboutAct->setMenuRole(QAction::AboutRole);
    help->addAction(aboutAct);
    help->addAction(act(tr("LaTeX Wikibook"), QKeySequence(), []() {
        QDesktopServices::openUrl(QUrl("https://en.wikibooks.org/wiki/LaTeX"));
    }));
    help->addAction(act(tr("Search CTAN Packages"), QKeySequence(), []() {
        QDesktopServices::openUrl(QUrl("https://ctan.org/search"));
    }));
}
