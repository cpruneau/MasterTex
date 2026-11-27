#pragma once
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QVector>
#include <functional>

class CodeEditor;
class FindBar;
class Project;
class StructurePanel;
class SymbolPanel;
class QAction;
class QDockWidget;
class QFileSystemModel;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProcess;
class QTabWidget;
class QTextDocument;
class QTreeView;
class QTreeWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // projectMaster may be empty (window without a project).
    explicit MainWindow(const QString &projectMaster = QString(), QWidget *parent = nullptr);
    ~MainWindow() override;

    void setProject(const QString &masterFile);
    Project *project() const { return m_project; }
    CodeEditor *openFile(const QString &path, int line = 0);

    static MainWindow *newWindow(const QString &projectMaster = QString());

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    // setup
    void createMenus();          // MainWindowMenus.cpp
    void createFileMenu();
    void createEditMenu();
    void createLatexMenu();
    void createMathMenu();
    void createToolsMenu();
    void createOtherMenus();
    void createToolBars();
    void createDocks();
    void updateTitle();
    void updateTabTitles();
    void updateRecentMenu();
    void updateIssues();

    // helpers
    CodeEditor *currentEditor() const;
    CodeEditor *editorAt(int index) const;
    QAction *act(const QString &text, const QKeySequence &key, std::function<void()> fn,
                 const QIcon &icon = QIcon());
    void insert(const QString &snippet);
    // Adds a submenu whose items insert snippets: {label, snippet}.
    QMenu *snippetMenu(QMenu *parent, const QString &title, const QList<QPair<QString, QString>> &items);
    bool closeTab(int index);
    bool maybeSave(const QList<QTextDocument *> &docs);
    bool saveDocument(QTextDocument *doc);
    bool saveDocumentAs(QTextDocument *doc);
    bool saveAll();
    void openLocationInNewWindow(const QString &file, int line);

    // commands
    void newFile();
    void openFileDialog();
    void openProjectDialog();
    void gotoLineDialog();
    void rememberPosition();
    void toggleBackForward();
    void insertRefToLabel(bool next);
    void insertFileCommand(const QString &command);
    void findRepetitions();
    void thesaurus();
    void openTerminal();
    void viewLog();
    void updateLogMarkers();
    void insertReference();
    void insertCitation();
    void runBuild(bool full);
    bool ensureProject();
    void runTool(const QString &program, const QStringList &args, const QString &label, bool latexRun);
    void runAuxTool(const QString &tool);   // "bibliography", "glossary", "index"
    void cleanBuild();
    void viewPdf();
    void showSettings();
    void about();
    void zoom(int delta);
    void wizardChapter();

    // build output
    void parseBuildLog();

    QPointer<Project> m_project;
    QTabWidget *m_tabs = nullptr;
    StructurePanel *m_structure = nullptr;
    SymbolPanel *m_symbols = nullptr;
    QTreeView *m_fileView = nullptr;
    QFileSystemModel *m_fsModel = nullptr;
    QTreeWidget *m_issues = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QDockWidget *m_messagesDock = nullptr;
    QTabWidget *m_messageTabs = nullptr;
    QProcess *m_build = nullptr;
    QLabel *m_status = nullptr;
    QMenu *m_recentMenu = nullptr;
    QMenu *m_viewMenu = nullptr;
    QMetaObject::Connection m_cursorConn;
    FindBar *m_findBar = nullptr;
    QTreeWidget *m_repetitions = nullptr;
    bool m_viewAfterBuild = false;
    bool m_latexRun = false;
    bool m_logMarkersOn = true;
    QString m_navFile;
    int m_navLine = 0;
    QString m_buildOutput;
    struct BuildIssue { int severity; QString message; QString file; int line; };
    QVector<BuildIssue> m_buildIssues;

    QHash<QString, QAction *> m_a;   // named actions shared by menus and toolbars
    QAction *m_runAct = nullptr;
    QAction *m_runAllAct = nullptr;
    QAction *m_stopAct = nullptr;
};
