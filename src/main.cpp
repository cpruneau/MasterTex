#include "MainWindow.h"
#include "Project.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

// Prints the parsed project model; used for testing the parser without the GUI.
static int dumpProject(const QString &master)
{
    QTextStream out(stdout);
    const ProjectData d = Project::parse(master);
    const QString root = QFileInfo(master).absolutePath();
    auto rel = [&](const QString &p) { return QDir(root).relativeFilePath(p); };

    out << "FILES (" << d.files.size() << ")\n";
    for (const QString &f : d.files) out << "  " << rel(f) << "\n";

    out << "OUTLINE\n";
    for (const OutlineEntry &e : d.outline) {
        out << QString(e.level == 10 ? 8 : e.level * 2 + 2, ' ');
        if (e.kind == EntryKind::Figure) out << "[Figure] ";
        if (e.kind == EntryKind::Table) out << "[Table] ";
        out << e.title;
        if (!e.label.isEmpty()) out << "  <" << e.label << ">";
        if (!e.graphics.isEmpty()) out << "  {" << e.graphics.join(", ") << "}";
        out << "   @" << rel(e.loc.file) << ":" << e.loc.line << "\n";
    }

    out << "LABELS " << d.labels.size() << "  REFS " << d.refs.size()
        << "  CITES " << d.cites.size() << "  BIBKEYS " << d.bibKeys.size()
        << "  IMAGES " << d.graphics.size() << "\n";

    out << "ISSUES (" << d.issues.size() << ")\n";
    const char *sev[] = {"ERROR", "WARN ", "INFO "};
    for (const Issue &i : d.issues)
        out << "  " << sev[int(i.severity)] << " " << i.message << "   @" << rel(i.loc.file) << ":" << i.loc.line << "\n";
    return 0;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("MasterTex");
    QApplication::setOrganizationName("MasterTex");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("MasterTex — a LaTeX editor for large multi-file projects");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption dump("dump", "Print the parsed structure of a master .tex file and exit.", "master.tex");
    parser.addOption(dump);
    parser.addPositionalArgument("master", "Master .tex file of the project to open.");
    parser.process(app);

    if (parser.isSet(dump))
        return dumpProject(parser.value(dump));

    const QStringList args = parser.positionalArguments();
    MainWindow::newWindow(args.isEmpty() ? QString() : QFileInfo(args.first()).absoluteFilePath());
    return app.exec();
}
