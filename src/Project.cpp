#include "Project.h"
#include "DocumentManager.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

// ---------------------------------------------------------------------------
// Registry: one Project per master file, shared by all windows.

static QHash<QString, Project *> &registry()
{
    static QHash<QString, Project *> r;
    return r;
}

Project *Project::acquire(const QString &masterFile)
{
    const QString key = DocumentManager::normalize(masterFile);
    Project *p = registry().value(key);
    if (!p) {
        p = new Project(key);
        registry().insert(key, p);
        p->reparse();
    }
    p->m_refs++;
    return p;
}

void Project::release(Project *p)
{
    if (!p || --p->m_refs > 0)
        return;
    registry().remove(p->m_master);
    p->deleteLater();
}

Project::Project(const QString &masterFile) : m_master(masterFile)
{
    m_reparseTimer.setSingleShot(true);
    m_reparseTimer.setInterval(1200);
    connect(&m_reparseTimer, &QTimer::timeout, this, &Project::reparse);

    auto *docs = DocumentManager::instance();
    connect(docs, &DocumentManager::documentTextChanged, this, [this](const QString &path) {
        if (containsFile(path) || path.endsWith(".bib"))
            scheduleReparse();
    });
    connect(docs, &DocumentManager::documentSaved, this, [this](const QString &) { scheduleReparse(); });
}

QString Project::rootDir() const { return QFileInfo(m_master).absolutePath(); }
QString Project::name() const { return QFileInfo(m_master).completeBaseName(); }
bool Project::containsFile(const QString &path) const { return m_fileSet.contains(path); }
void Project::scheduleReparse() { m_reparseTimer.start(); }

void Project::reparse()
{
    m_data = parse(m_master);
    m_fileSet = QSet<QString>(m_data.files.begin(), m_data.files.end());
    for (const QString &b : m_data.bibFiles)
        m_fileSet.insert(b);
    emit changed();
}

static QString resolveGraphicIn(const QString &root, const QStringList &gpath, const QString &written)
{
    static const QStringList exts = {"", ".pdf", ".png", ".jpg", ".jpeg", ".eps", ".svg"};
    QStringList dirs = gpath;
    dirs.prepend(QString());
    for (const QString &d : dirs) {
        for (const QString &e : exts) {
            QFileInfo fi(QDir(root), d + written + e);
            if (fi.exists() && fi.isFile())
                return DocumentManager::normalize(fi.absoluteFilePath());
        }
    }
    return QString();
}

QString Project::resolveGraphic(const QString &written) const
{
    return resolveGraphicIn(rootDir(), m_data.graphicsPath, written);
}

// ---------------------------------------------------------------------------
// Parser

namespace {

struct SectionCmd { EntryKind kind; int level; };

const QHash<QString, SectionCmd> &sectionCommands()
{
    static const QHash<QString, SectionCmd> m = {
        {"part", {EntryKind::Part, 0}},
        {"chapter", {EntryKind::Chapter, 1}},
        {"section", {EntryKind::Section, 2}},
        {"subsection", {EntryKind::Subsection, 3}},
        {"subsubsection", {EntryKind::Subsubsection, 4}},
        {"paragraph", {EntryKind::Paragraph, 5}},
    };
    return m;
}

const QSet<QString> &refCommands()
{
    static const QSet<QString> s = {"ref", "eqref", "pageref", "autoref", "nameref", "cref", "Cref",
                                    "cpageref", "Cpageref", "vref", "Vref", "labelcref"};
    return s;
}

const QSet<QString> &citeCommands()
{
    static const QSet<QString> s = {"cite", "citep", "citet", "citealp", "citealt", "citeauthor",
                                    "citeyear", "citenum", "parencite", "Parencite", "textcite",
                                    "Textcite", "autocite", "Autocite", "footcite", "smartcite",
                                    "supercite", "fullcite", "nocite", "Cite", "cites", "parencites"};
    return s;
}

const QSet<QString> &floatEnvs()
{
    static const QSet<QString> s = {"figure", "figure*", "table", "table*", "wrapfigure",
                                    "wraptable", "SCfigure", "sidewaysfigure", "sidewaystable"};
    return s;
}

// Replace comments with spaces, keeping offsets and newlines intact.
QString stripComments(const QString &s)
{
    QString out = s;
    const int n = out.size();
    for (int i = 0; i < n; ++i) {
        if (out[i] == '\\') { ++i; continue; }
        if (out[i] == '%') {
            while (i < n && out[i] != '\n') { out[i] = ' '; ++i; }
        }
    }
    return out;
}

QString simplifyTitle(QString t)
{
    // Readable title for the browser: drop common font switches and grouping braces.
    static const QRegularExpression fontCmd(R"(\\(?:em|it|bf|sc|tt|rm|sf|emph|textbf|textit|textsc|texttt|textrm|textsf|mathrm|mathbf|boldsymbol)\b\s*)");
    t.remove(fontCmd);
    t.remove('{');
    t.remove('}');
    t.replace('\n', ' ');
    t.replace('~', ' ');
    return t.simplified();
}

class Parser {
public:
    Parser(const QString &root, ProjectData &d) : m_root(root), d(d) {}

    void parseFile(const QString &path, IncludeNode &node, int depth)
    {
        if (depth > 40 || m_visited.contains(path))
            return;
        m_visited.insert(path);
        bool exists = false;
        const QString raw = DocumentManager::instance()->textFor(path, &exists);
        node.exists = exists;
        if (!exists) {
            d.issues.push_back({Severity::Error, QString("Included file not found: %1").arg(node.written), node.from});
            return;
        }
        d.files << path;

        const QString text = stripComments(raw);
        QVector<int> lineStarts{0};
        for (int i = 0; i < text.size(); ++i)
            if (text[i] == '\n') lineStarts << i + 1;
        auto lineAt = [&](int offset) {
            return int(std::upper_bound(lineStarts.begin(), lineStarts.end(), offset) - lineStarts.begin());
        };

        const int n = text.size();
        int i = 0;
        while ((i = text.indexOf('\\', i)) >= 0) {
            int j = i + 1;
            while (j < n && (text[j].isLetter() || text[j] == '@')) ++j;
            if (j == i + 1) { i = j + 1; continue; }
            const QString cmd = text.mid(i + 1, j - i - 1);
            int pos = j;
            if (pos < n && text[pos] == '*') ++pos;
            const SourceLoc loc{path, lineAt(i)};
            handle(cmd, text, pos, loc, path, node, depth);
            i = j;
        }
    }

private:
    void skipSpace(const QString &t, int &pos)
    {
        while (pos < t.size() && t[pos].isSpace()) ++pos;
    }

    bool readDelimited(const QString &t, int &pos, QChar open, QChar close, QString *out)
    {
        int p = pos;
        skipSpace(t, p);
        const int n = t.size();
        if (p >= n || t[p] != open) return false;
        const int start = p + 1;
        int braces = 0;
        for (p = start; p < n; ++p) {
            const QChar c = t[p];
            if (c == '\\') { ++p; continue; }
            if (c == '{') { ++braces; continue; }
            if (c == '}') {
                if (braces == 0 && close == '}') {
                    if (out) *out = t.mid(start, p - start);
                    pos = p + 1;
                    return true;
                }
                if (braces > 0) --braces;
                continue;
            }
            if (c == close && braces == 0) {
                if (out) *out = t.mid(start, p - start);
                pos = p + 1;
                return true;
            }
        }
        return false;
    }

    bool readGroup(const QString &t, int &pos, QString *out) { return readDelimited(t, pos, '{', '}', out); }
    bool readOptional(const QString &t, int &pos, QString *out) { return readDelimited(t, pos, '[', ']', out); }

    QString resolveTex(const QString &written, const QString &baseDir)
    {
        QString w = written.trimmed();
        QFileInfo fi(QDir(baseDir), w);
        if (!fi.exists() && fi.suffix() != "tex")
            fi = QFileInfo(QDir(baseDir), w + ".tex");
        return DocumentManager::normalize(fi.absoluteFilePath());
    }

    void include(const QString &written, const QString &baseDir, const SourceLoc &loc, IncludeNode &parent, int depth)
    {
        IncludeNode child;
        child.written = written.trimmed();
        child.path = resolveTex(written, baseDir);
        child.from = loc;
        parseFile(child.path, child, depth + 1);
        parent.children.push_back(child);
    }

    static QStringList splitKeys(const QString &s)
    {
        QStringList out;
        for (const QString &k : s.split(',')) {
            QString t = k.trimmed();
            if (!t.isEmpty()) out << t;
        }
        return out;
    }

    void handle(const QString &cmd, const QString &t, int pos, const SourceLoc &loc,
                const QString &path, IncludeNode &node, int depth)
    {
        QString arg, opt;

        if (sectionCommands().contains(cmd)) {
            readOptional(t, pos, &opt);
            if (!readGroup(t, pos, &arg)) return;
            const SectionCmd sc = sectionCommands().value(cmd);
            OutlineEntry e;
            e.kind = sc.kind;
            e.level = sc.level;
            e.title = simplifyTitle(arg);
            e.loc = loc;
            d.outline.push_back(e);
            m_lastHeading = d.outline.size() - 1;
            m_headingFresh = true;
            return;
        }

        if (cmd == "label") {
            if (!readGroup(t, pos, &arg)) return;
            const QString key = arg.trimmed();
            d.labels[key].push_back(loc);
            if (m_openFloat >= 0) {
                if (d.outline[m_openFloat].label.isEmpty()) d.outline[m_openFloat].label = key;
            } else if (m_headingFresh && m_lastHeading >= 0 && d.outline[m_lastHeading].label.isEmpty()) {
                d.outline[m_lastHeading].label = key;
            }
            m_headingFresh = false;
            return;
        }

        if (refCommands().contains(cmd)) {
            if (!readGroup(t, pos, &arg)) return;
            for (const QString &k : splitKeys(arg)) d.refs[k].push_back(loc);
            return;
        }

        if (citeCommands().contains(cmd)) {
            readOptional(t, pos, &opt);
            readOptional(t, pos, &opt);
            if (!readGroup(t, pos, &arg)) return;
            for (const QString &k : splitKeys(arg))
                if (k != "*") d.cites[k].push_back(loc);
            return;
        }

        if (cmd == "input" || cmd == "include" || cmd == "subfile" || cmd == "InputIfFileExists") {
            if (!readGroup(t, pos, &arg)) return;
            if (arg.trimmed().isEmpty()) return;
            include(arg, m_root, loc, node, depth);
            m_headingFresh = false;
            return;
        }

        if (cmd == "import" || cmd == "subimport" || cmd == "inputfrom" || cmd == "subinputfrom"
            || cmd == "includefrom" || cmd == "subincludefrom") {
            QString dir;
            if (!readGroup(t, pos, &dir) || !readGroup(t, pos, &arg)) return;
            const bool relative = cmd.startsWith("sub");
            const QString base = relative ? QFileInfo(path).absolutePath() : m_root;
            include(QDir::cleanPath(dir.trimmed() + "/" + arg.trimmed()), base, loc, node, depth);
            return;
        }

        if (cmd == "includegraphics") {
            readOptional(t, pos, &opt);
            readOptional(t, pos, &opt);
            if (!readGroup(t, pos, &arg)) return;
            GraphicUse g;
            g.written = arg.trimmed();
            g.loc = loc;
            d.graphics.push_back(g);   // resolved after parsing (graphicspath may come later)
            if (m_openFloat >= 0) d.outline[m_openFloat].graphics << g.written;
            return;
        }

        if (cmd == "graphicspath") {
            if (!readGroup(t, pos, &arg)) return;
            int p = 0;
            QString dir;
            while (readGroup(arg, p, &dir)) d.graphicsPath << dir.trimmed();
            return;
        }

        if (cmd == "begin") {
            if (!readGroup(t, pos, &arg)) return;
            const QString env = arg.trimmed();
            if (floatEnvs().contains(env) && m_openFloat < 0) {
                OutlineEntry e;
                e.kind = env.contains("table") ? EntryKind::Table : EntryKind::Figure;
                e.level = 10;
                e.loc = loc;
                d.outline.push_back(e);
                m_openFloat = d.outline.size() - 1;
                m_openFloatEnv = env;
            }
            m_headingFresh = false;
            return;
        }

        if (cmd == "end") {
            if (!readGroup(t, pos, &arg)) return;
            if (m_openFloat >= 0 && arg.trimmed() == m_openFloatEnv) m_openFloat = -1;
            return;
        }

        if (cmd == "caption" && m_openFloat >= 0) {
            readOptional(t, pos, &opt);
            if (!readGroup(t, pos, &arg)) return;
            if (d.outline[m_openFloat].title.isEmpty()) d.outline[m_openFloat].title = simplifyTitle(arg);
            return;
        }

        if (cmd == "bibliography") {
            if (!readGroup(t, pos, &arg)) return;
            for (QString b : splitKeys(arg)) {
                if (!b.endsWith(".bib")) b += ".bib";
                addBib(b, loc);
            }
            return;
        }

        if (cmd == "addbibresource" || cmd == "addglobalbib") {
            readOptional(t, pos, &opt);
            if (!readGroup(t, pos, &arg)) return;
            addBib(arg.trimmed(), loc);
            return;
        }

        if (cmd == "bibitem") {
            readOptional(t, pos, &opt);
            if (!readGroup(t, pos, &arg)) return;
            d.bibKeys.insert(arg.trimmed(), loc);
            return;
        }
    }

    void addBib(const QString &written, const SourceLoc &loc)
    {
        QFileInfo fi(QDir(m_root), written);
        const QString path = DocumentManager::normalize(fi.absoluteFilePath());
        if (d.bibFiles.contains(path)) return;
        d.bibFiles << path;
        bool exists = false;
        const QString text = DocumentManager::instance()->textFor(path, &exists);
        if (!exists) {
            d.issues.push_back({Severity::Error, QString("Bibliography file not found: %1").arg(written), loc});
            return;
        }
        static const QRegularExpression re(R"(@\s*(\w+)\s*[{(]\s*([^,\s{}()]+)\s*,)");
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            const QString type = m.captured(1).toLower();
            if (type == "string" || type == "comment" || type == "preamble") continue;
            const int line = text.left(m.capturedStart()).count('\n') + 1;
            d.bibKeys.insert(m.captured(2), SourceLoc{path, line});
        }
    }

    QString m_root;
    ProjectData &d;
    QSet<QString> m_visited;
    int m_lastHeading = -1;
    bool m_headingFresh = false;
    int m_openFloat = -1;
    QString m_openFloatEnv;
};

} // namespace

ProjectData Project::parse(const QString &masterFile)
{
    ProjectData d;
    const QString master = DocumentManager::normalize(masterFile);
    const QString root = QFileInfo(master).absolutePath();
    d.includes.path = master;
    d.includes.written = QFileInfo(master).fileName();

    Parser parser(root, d);
    parser.parseFile(master, d.includes, 0);

    // Resolve graphics now that \graphicspath is known.
    for (GraphicUse &g : d.graphics) {
        g.resolved = resolveGraphicIn(root, d.graphicsPath, g.written);
        if (g.resolved.isEmpty())
            d.issues.push_back({Severity::Error, QString("Image not found: %1").arg(g.written), g.loc});
    }

    for (auto it = d.labels.cbegin(); it != d.labels.cend(); ++it) {
        if (it->size() > 1)
            for (const SourceLoc &l : *it)
                d.issues.push_back({Severity::Error, QString("Duplicate label: %1").arg(it.key()), l});
        if (!d.refs.contains(it.key()))
            d.issues.push_back({Severity::Info, QString("Label never referenced: %1").arg(it.key()), it->first()});
    }
    for (auto it = d.refs.cbegin(); it != d.refs.cend(); ++it)
        if (!d.labels.contains(it.key()))
            for (const SourceLoc &l : *it)
                d.issues.push_back({Severity::Warning, QString("Undefined reference: %1").arg(it.key()), l});

    const bool haveBib = !d.bibFiles.isEmpty() || !d.bibKeys.isEmpty();
    for (auto it = d.cites.cbegin(); it != d.cites.cend(); ++it)
        if (haveBib && !d.bibKeys.contains(it.key()))
            for (const SourceLoc &l : *it)
                d.issues.push_back({Severity::Warning, QString("Citation not in bibliography: %1").arg(it.key()), l});

    std::stable_sort(d.issues.begin(), d.issues.end(), [](const Issue &a, const Issue &b) {
        return int(a.severity) < int(b.severity);
    });
    return d;
}
