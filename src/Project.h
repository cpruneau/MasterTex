#pragma once
// Project: the linked model of a multi-file LaTeX document.
// Starting from the master file it follows \input/\include/\subfile/\import,
// and collects sectioning, floats (figures/tables with captions, labels and
// graphics), labels and references, citations and bibliography keys.
// The model is rebuilt from the live editor buffers, so it reflects unsaved
// edits, and every window that shows the project shares the same instance.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

struct SourceLoc {
    QString file;   // absolute path
    int line = 0;   // 1-based
};

enum class EntryKind { Part, Chapter, Section, Subsection, Subsubsection, Paragraph, Figure, Table };

struct OutlineEntry {
    EntryKind kind;
    int level = 0;           // 0 = part ... 5 = paragraph, 10 = float
    QString title;           // heading title or float caption
    QString label;           // first \label attached to it
    SourceLoc loc;
    QStringList graphics;    // as written in \includegraphics
};

struct IncludeNode {
    QString path;            // absolute path (may not exist)
    QString written;         // as written in the source
    SourceLoc from;          // where it was included
    bool exists = true;
    QVector<IncludeNode> children;
};

enum class Severity { Error, Warning, Info };

struct Issue {
    Severity severity;
    QString message;
    SourceLoc loc;
};

struct GraphicUse {
    QString written;
    QString resolved;        // empty if not found
    SourceLoc loc;
};

struct ProjectData {
    QVector<OutlineEntry> outline;
    IncludeNode includes;
    QStringList files;                              // all parsed .tex files, in order
    QHash<QString, QVector<SourceLoc>> labels;      // key -> definitions (>1 = duplicate)
    QHash<QString, QVector<SourceLoc>> refs;        // key -> uses
    QHash<QString, QVector<SourceLoc>> cites;       // key -> uses
    QHash<QString, SourceLoc> bibKeys;              // key -> entry in .bib / \bibitem
    QStringList bibFiles;                           // absolute paths
    QVector<GraphicUse> graphics;
    QStringList graphicsPath;                       // from \graphicspath, relative to root
    QVector<Issue> issues;
};

class Project : public QObject {
    Q_OBJECT
public:
    // Shared per master file: windows acquire/release the same instance.
    static Project *acquire(const QString &masterFile);
    static void release(Project *p);

    QString masterFile() const { return m_master; }
    QString rootDir() const;
    QString name() const;
    const ProjectData &data() const { return m_data; }
    bool containsFile(const QString &path) const;

    QString resolveGraphic(const QString &written) const;

    // Stand-alone parse, used for the command line --dump option and tests.
    static ProjectData parse(const QString &masterFile);

public slots:
    void reparse();
    void scheduleReparse();

signals:
    void changed();

private:
    explicit Project(const QString &masterFile);
    QString m_master;
    ProjectData m_data;
    QSet<QString> m_fileSet;
    QTimer m_reparseTimer;
    int m_refs = 0;
};
