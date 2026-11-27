#include "Restructure.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <functional>

namespace Restructure {

// ---------------------------------------------------------------------------
// Helpers

static QString clean(const QString &p) { return QDir::cleanPath(p); }
static QString dirOf(const QString &p) { return clean(QFileInfo(p).absolutePath()); }
static bool isInside(const QString &p, const QString &dir) { return p == dir || p.startsWith(dir + "/"); }

static QString readText(const QString &path, bool *ok = nullptr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok) *ok = false;
        return {};
    }
    const QByteArray bytes = f.readAll();
    auto dec = QStringDecoder(QStringDecoder::Utf8);
    QString s = dec(bytes);
    if (dec.hasError()) s = QString::fromLatin1(bytes);
    if (ok) *ok = true;
    return s;
}

static bool writeText(const QString &path, const QString &text, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = f.errorString(); return false; }
    f.write(text.toUtf8());
    if (!f.commit()) { if (error) *error = f.errorString(); return false; }
    return true;
}

// Index of the first unescaped % (start of a comment), or -1.
static int commentPos(const QString &line)
{
    for (int i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++i; continue; }
        if (line[i] == '%') return i;
    }
    return -1;
}

static const QStringList &headingNames()
{
    static const QStringList n = {"part", "chapter", "section", "subsection", "subsubsection", "paragraph", "subparagraph"};
    return n;
}

QString kindName(double level)
{
    static const QStringList names = {"Part", "Chapter", "Section", "Subsection", "Subsubsection", "Paragraph", "Subparagraph"};
    const int l = int(std::floor(level + 0.5));
    return (l >= 0 && l < names.size()) ? names[l] : QString("Item");
}

struct HeadingHit { int level = -1; QString title; };

// A heading in the code part (before any comment) of a line.
static HeadingHit headingIn(const QString &line)
{
    static const QRegularExpression re(
        R"(\\(part|chapter|section|subsection|subsubsection|paragraph)\*?\s*(?:\[[^\]]*\])?\s*\{)");
    const int cp = commentPos(line);
    const QString code = cp >= 0 ? line.left(cp) : line;
    auto m = re.match(code);
    if (!m.hasMatch()) return {};
    HeadingHit h;
    h.level = int(headingNames().indexOf(m.captured(1)));
    // Title: brace-matched to the end of the line.
    int depth = 1;
    QString t;
    for (int i = m.capturedEnd(); i < code.size(); ++i) {
        const QChar c = code[i];
        if (c == '\\' && i + 1 < code.size()) { t += c; t += code[++i]; continue; }
        if (c == '{') ++depth;
        if (c == '}' && --depth == 0) break;
        t += c;
    }
    static const QRegularExpression fontCmd(R"(\\(?:em|it|bf|emph|textbf|textit|mathrm|mathbf)\b\s*)");
    t.remove(fontCmd);
    t.remove('{');
    t.remove('}');
    h.title = t.simplified();
    return h;
}

struct IncludeHit { bool found = false; bool commented = false; bool atStart = true; QString cmd, written; };

static IncludeHit includeIn(const QString &line)
{
    static const QRegularExpression start(R"(^\s*(%+\s*)?\\(input|include|subfile)\s*\{([^}]+)\})");
    static const QRegularExpression anywhere(R"(\\(input|include|subfile)\s*\{([^}]+)\})");
    IncludeHit h;
    auto m = start.match(line);
    if (m.hasMatch()) {
        h.found = true;
        h.commented = !m.captured(1).isEmpty();
        h.cmd = m.captured(2);
        h.written = m.captured(3).trimmed();
        return h;
    }
    const int cp = commentPos(line);
    auto a = anywhere.match(cp >= 0 ? line.left(cp) : line);
    if (a.hasMatch()) {
        h.found = true;
        h.atStart = false;
        h.cmd = a.captured(1);
        h.written = a.captured(2).trimmed();
    }
    return h;
}

// ---------------------------------------------------------------------------
// Building the tree

namespace {

struct FileInfo {
    double minLevel = 99;    // lowest heading level in the file and everything it includes
    int countAtMin = 0;
    bool ownHasMin = false;  // the file itself (not an included one) has a heading at minLevel
    int ownFirstMinLine = -1;
};

struct Builder {
    Model &m;
    QHash<QString, QStringList> cache;
    QHash<QString, FileInfo> info;
    QSet<QString> visiting;
    QVector<QPair<double, int>> stack;   // (level, node id)

    explicit Builder(Model &model) : m(model) {}

    const QStringList &lines(const QString &path)
    {
        auto it = cache.find(path);
        if (it == cache.end()) it = cache.insert(path, readText(path).split('\n'));
        return *it;
    }

    QString resolve(const QString &written) const
    {
        QFileInfo fi(QDir(m.root), written);
        if (!fi.exists() && fi.suffix() != "tex") fi = QFileInfo(QDir(m.root), written + ".tex");
        return clean(fi.absoluteFilePath());
    }

    FileInfo infoOf(const QString &path)
    {
        if (info.contains(path)) return info.value(path);
        FileInfo fi;
        if (visiting.contains(path) || !QFileInfo::exists(path)) return fi;
        visiting.insert(path);
        const QStringList ls = lines(path);   // a copy: the cache may grow while we recurse
        for (int i = 0; i < ls.size(); ++i) {
            const HeadingHit h = headingIn(ls[i]);
            if (h.level >= 0) {
                if (h.level < fi.minLevel) { fi.minLevel = h.level; fi.countAtMin = 1; fi.ownHasMin = true; fi.ownFirstMinLine = i; }
                else if (h.level == fi.minLevel) { fi.countAtMin++; }
                continue;
            }
            const IncludeHit inc = includeIn(ls[i]);
            if (!inc.found) continue;
            const FileInfo sub = infoOf(resolve(inc.written));
            if (sub.minLevel < fi.minLevel) { fi.minLevel = sub.minLevel; fi.countAtMin = sub.countAtMin; fi.ownHasMin = false; fi.ownFirstMinLine = -1; }
            else if (sub.minLevel == fi.minLevel && sub.minLevel < 99) { fi.countAtMin += sub.countAtMin; }
        }
        visiting.remove(path);
        info.insert(path, fi);
        return fi;
    }

    int addNode(Node n)
    {
        n.id = m.nodes.size();
        m.nodes.push_back(n);
        return n.id;
    }

    int attach(int id, double level)
    {
        while (stack.size() > 1 && stack.last().first >= level) stack.removeLast();
        const int parent = stack.last().second;
        m.nodes[id].parent = parent;
        m.nodes[parent].children.push_back(id);
        return parent;
    }

    void attachLeaf(int id)
    {
        const int parent = stack.last().second;
        m.nodes[id].parent = parent;
        m.nodes[parent].children.push_back(id);
    }

    void popTo(int id)
    {
        for (int k = stack.size() - 1; k >= 1; --k)
            if (stack[k].second == id) { stack.resize(k); return; }
    }

    void walk(const QString &path, bool commented, int skipLine, int depth)
    {
        if (depth > 30 || visiting.contains(path)) return;
        visiting.insert(path);
        const QStringList ls = lines(path);
        for (int i = 0; i < ls.size(); ++i) {
            if (i == skipLine) continue;
            const HeadingHit h = headingIn(ls[i]);
            if (h.level >= 0) {
                Node n;
                n.kind = Kind::Heading;
                n.level = h.level;
                n.title = h.title;
                n.file = path;
                n.line = i;
                n.commented = commented;
                const int id = addNode(n);
                attach(id, h.level);
                stack.push_back({double(h.level), id});
                continue;
            }
            const IncludeHit inc = includeIn(ls[i]);
            if (!inc.found) continue;
            Node n;
            n.file = path;
            n.line = i;
            n.written = inc.written;
            n.target = resolve(inc.written);
            n.commented = commented || inc.commented;
            n.lockedLine = !inc.atStart;
            if (!QFileInfo::exists(n.target)) {
                n.kind = Kind::Missing;
                n.title = inc.written;
                attachLeaf(addNode(n));
                continue;
            }
            visiting.remove(path);
            const FileInfo fi = infoOf(n.target);
            visiting.insert(path);
            if (fi.minLevel >= 99) {
                n.kind = Kind::Plain;
                n.title = QDir(m.root).relativeFilePath(n.target);
                attachLeaf(addNode(n));
                continue;
            }
            if (fi.ownHasMin && fi.countAtMin == 1) {
                n.kind = Kind::Unit;
                n.level = fi.minLevel;
                n.title = headingIn(lines(n.target).value(fi.ownFirstMinLine)).title;
                const int id = addNode(n);
                attach(id, n.level);
                stack.push_back({n.level, id});
                walk(n.target, n.commented, fi.ownFirstMinLine, depth + 1);
                popTo(id);
            } else {
                n.kind = Kind::Container;
                n.level = fi.minLevel;
                n.title = QDir(m.root).relativeFilePath(n.target);
                const int id = addNode(n);
                attach(id, n.level);
                stack.push_back({n.level - 0.5, id});
                walk(n.target, n.commented, -1, depth + 1);
                popTo(id);
            }
        }
        visiting.remove(path);
    }
};

} // namespace

Model build(const QString &masterFile)
{
    Model m;
    m.master = clean(QFileInfo(masterFile).absoluteFilePath());
    m.root = dirOf(m.master);
    Node root;
    root.kind = Kind::Master;
    root.level = -1;
    root.title = QFileInfo(m.master).fileName();
    root.file = m.master;
    root.target = m.master;
    root.id = 0;
    m.nodes.push_back(root);

    Builder b(m);
    b.stack.push_back({-1e9, 0});
    b.walk(m.master, false, -1, 0);

    QDirIterator it(m.root, {"*.tex"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = clean(it.next());
        if (p.contains("/.mastertex/")) continue;
        m.texFiles << p;
    }
    m.texFiles.sort();
    return m;
}

Tree originalTree(const Model &m)
{
    Tree t;
    for (const Node &n : m.nodes)
        if (!n.children.isEmpty()) t.insert(n.id, n.children);
    return t;
}

bool isMovable(const Node &n)
{
    return (n.kind == Kind::Unit || n.kind == Kind::Container) && !n.lockedLine && QFileInfo::exists(n.target);
}

bool isGitRepo(const QString &root)
{
    return QFileInfo::exists(root + "/.git");
}

// ---------------------------------------------------------------------------
// Planning

namespace {

struct LineRec {
    QString text;
    int id;
    bool removed = false;
};

struct Planner {
    const Model &m;
    const Tree &tree;
    Plan plan;
    QHash<int, int> newParent;
    QHash<QString, QVector<LineRec>> files;            // original path -> lines being edited
    QHash<QPair<QString, int>, int> chainAfter;         // (file, anchor id) -> id inserted after it
    QHash<int, QPair<QString, int>> lineLoc;            // node -> (file, line id) of its include line
    QSet<int> moved;
    int nextLineId = 1000000;
    QHash<QString, QString> textCache;

    const QString &text(const QString &path)
    {
        auto it = textCache.find(path);
        if (it == textCache.end()) it = textCache.insert(path, readText(path));
        return *it;
    }

    Planner(const Model &model, const Tree &t) : m(model), tree(t) {}

    const Node &node(int id) const { return m.nodes[id]; }

    QString rel(const QString &p) const { return QDir(m.root).relativeFilePath(p); }

    QVector<LineRec> &linesOf(const QString &path)
    {
        auto it = files.find(path);
        if (it == files.end()) {
            QVector<LineRec> v;
            const QStringList ls = text(path).split('\n');
            for (int i = 0; i < ls.size(); ++i) v.push_back({ls[i], i});
            it = files.insert(path, v);
        }
        return *it;
    }

    int indexOfId(QVector<LineRec> &v, int id) const
    {
        for (int i = 0; i < v.size(); ++i) if (v[i].id == id) return i;
        return -1;
    }

    int insertAfter(const QString &file, int anchor, const QString &text)
    {
        QVector<LineRec> &v = linesOf(file);
        int a = anchor;
        while (chainAfter.contains({file, a})) a = chainAfter.value({file, a});
        const int idx = indexOfId(v, a);
        const int id = nextLineId++;
        v.insert(idx < 0 ? v.size() : idx + 1, {text, id});
        chainAfter.insert({file, a}, id);
        return id;
    }

    int insertBefore(const QString &file, int anchor, const QString &text)
    {
        QVector<LineRec> &v = linesOf(file);
        const int idx = indexOfId(v, anchor);
        const int id = nextLineId++;
        v.insert(idx < 0 ? v.size() : idx, {text, id});
        return id;
    }

    // Where a parent's content lives.
    QString fileOf(int parentId) const
    {
        const Node &p = node(parentId);
        return p.kind == Kind::Heading ? p.file : p.target;
    }

    int endDocumentLine(const QString &file)
    {
        QVector<LineRec> &v = linesOf(file);
        for (const LineRec &r : v)
            if (r.id < 1000000) {
                const int cp = commentPos(r.text);
                const QString code = cp >= 0 ? r.text.left(cp) : r.text;
                if (code.contains("\\end{document}")) return r.id;
            }
        return -1;
    }

    // Last original line of a heading's region in its file.
    int regionEnd(const Node &h)
    {
        int end = int(text(h.file).count('\n'));   // last line index
        for (const Node &o : m.nodes)
            if (o.kind == Kind::Heading && o.file == h.file && o.line > h.line && o.level <= h.level)
                end = qMin(end, o.line - 1);
        const int ed = endDocumentLine(h.file);
        if (ed >= 0 && ed > h.line) end = qMin(end, ed - 1);
        // Don't end on trailing blank lines.
        QVector<LineRec> &v = linesOf(h.file);
        while (end > h.line && end < v.size() && v[end].text.trimmed().isEmpty()) --end;
        return end;
    }

    double childLevel(int parentId) const
    {
        const Node &p = node(parentId);
        switch (p.kind) {
        case Kind::Master: {
            double lo = 99;
            for (int c : p.children) {
                const Node &n = node(c);
                if (n.kind == Kind::Heading || n.kind == Kind::Unit) lo = qMin(lo, n.level);
                if (n.kind == Kind::Container) lo = qMin(lo, n.level);
            }
            return lo < 99 ? lo : 1;
        }
        case Kind::Container: return p.level;
        default: return p.level + 1;
        }
    }

    // Folder that holds a unit: a chapter's own folder lives in its parent folder.
    bool ownsFolder(const Node &u) const
    {
        return QFileInfo(u.target).completeBaseName() == "main" && dirOf(u.target) != m.root;
    }
    QString home(const Node &u) const { return ownsFolder(u) ? dirOf(dirOf(u.target)) : dirOf(u.target); }
};

} // namespace

static QString finalPathOf(const QString &p, const QVector<Relocation> &rel)
{
    const Relocation *best = nullptr;
    for (const Relocation &r : rel)
        if (isInside(p, r.from) && (!best || r.from.size() > best->from.size())) best = &r;
    if (!best) return p;
    return clean(finalPathOf(best->toDir, rel) + "/" + best->newName + p.mid(best->from.size()));
}

static QString shiftHeadings(const QString &text, int delta, int *count, QString *error)
{
    static const QRegularExpression re(R"(\\(part|chapter|section|subsection|subsubsection|paragraph|subparagraph)(?=[\s*\[{]))");
    QString out;
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        auto mm = it.next();
        const int lvl = int(headingNames().indexOf(mm.captured(1))) + delta;
        if (lvl < 0 || lvl >= headingNames().size()) {
            if (error) *error = QString("\\%1 would have to become a level that LaTeX does not have").arg(mm.captured(1));
            return text;
        }
        out += text.mid(last, mm.capturedStart() - last) + "\\" + headingNames()[lvl];
        last = mm.capturedEnd();
        ++*count;
    }
    out += text.mid(last);
    return out;
}

Plan makePlan(const Model &m, const Tree &tree, bool moveExclusiveFigures)
{
    Planner P(m, tree);
    Plan &plan = P.plan;

    for (auto it = tree.cbegin(); it != tree.cend(); ++it)
        for (int c : it.value()) P.newParent.insert(c, it.key());


    // Pre-order walk of the new tree.
    QVector<int> order;
    std::function<void(int)> visit = [&](int id) {
        order << id;
        for (int c : tree.value(id)) visit(c);
    };
    visit(0);

    // 1. Which nodes moved. Per parent, keep the largest set of children that are
    //    still in their original order (fixed headings weigh most); the rest moved.
    for (int id : order) {
        const QVector<int> kids = tree.value(id);
        QVector<int> same;   // positions in kids of children originally under this parent
        for (int k = 0; k < kids.size(); ++k) {
            const Node &c = P.node(kids[k]);
            if (c.parent == id) same << k;
            else if (isMovable(c)) P.moved.insert(kids[k]);
            else plan.errors << QString("“%1” is written inside %2 and can't be moved yet.").arg(c.title, P.rel(c.file));
        }
        // Weighted longest increasing subsequence on original positions.
        const int n = same.size();
        QVector<int> origIdx(n), weight(n), best(n), prev(n, -1);
        const QVector<int> ok = P.node(id).children;
        for (int k = 0; k < n; ++k) {
            const Node &c = P.node(kids[same[k]]);
            origIdx[k] = ok.indexOf(c.id);
            weight[k] = isMovable(c) ? 1 : 1000;
        }
        int bestEnd = -1;
        for (int k = 0; k < n; ++k) {
            best[k] = weight[k];
            for (int j = 0; j < k; ++j)
                if (origIdx[j] < origIdx[k] && best[j] + weight[k] > best[k]) { best[k] = best[j] + weight[k]; prev[k] = j; }
            if (bestEnd < 0 || best[k] > best[bestEnd]) bestEnd = k;
        }
        QSet<int> kept;
        for (int k = bestEnd; k >= 0; k = prev[k]) kept.insert(k);
        for (int k = 0; k < n; ++k) {
            if (kept.contains(k)) continue;
            const Node &c = P.node(kids[same[k]]);
            if (isMovable(c)) P.moved.insert(c.id);
            else plan.errors << QString("“%1” is written inside %2 and can't be moved yet.").arg(c.title, P.rel(c.file));
        }
    }

    // New levels: a moved item takes the level its new place calls for; everything
    // else keeps its level, shifted along with any moved ancestor.
    QHash<int, double> newLevel;
    newLevel.insert(0, -1);
    for (int id : order) {
        if (id == 0) continue;
        const Node &n = P.node(id);
        const int np = P.newParent.value(id);
        const Node &p = P.node(np);
        if (P.moved.contains(id) && n.kind != Kind::Plain && n.kind != Kind::Missing) {
            double base;
            if (p.kind == Kind::Master) base = P.childLevel(0);
            else if (p.kind == Kind::Container) base = newLevel.value(np);
            else base = newLevel.value(np) + 1;
            newLevel.insert(id, base);
        } else {
            const double parentOffset = newLevel.value(np) - p.level;
            newLevel.insert(id, n.level + parentOffset);
        }
    }

    if (P.moved.isEmpty()) {
        if (plan.errors.isEmpty()) plan.steps << "No changes yet — drag items in the tree to rearrange the book.";
        return plan;
    }

    // Original include line locations.
    for (const Node &n : m.nodes)
        if (n.kind != Kind::Master && n.kind != Kind::Heading) P.lineLoc.insert(n.id, {n.file, n.line});

    // 2. Move include lines (in pre-order so earlier siblings are already placed).
    for (int id : order) {
        if (!P.moved.contains(id)) continue;
        const Node &u = P.node(id);
        const int np = P.newParent.value(id);
        const QString destFile = P.fileOf(np);

        // Take the line out of its old place.
        const auto oldLoc = P.lineLoc.value(id);
        QVector<LineRec> &src = P.linesOf(oldLoc.first);
        const int si = P.indexOfId(src, oldLoc.second);
        if (si < 0) { plan.errors << QString("Internal error locating the line for “%1”.").arg(u.title); continue; }
        QString text = src[si].text;
        src[si].removed = true;

        // Keep the item compiled (or not) exactly as before the move.
        std::function<bool(int)> disabledInNewTree = [&](int nid) -> bool {
            if (nid <= 0) return false;
            const Node &x = P.node(nid);
            if (x.kind == Kind::Unit || x.kind == Kind::Container) {
                const QString own = P.text(x.file).split('\n').value(x.line);
                if (includeIn(own).commented) return true;
            }
            return disabledInNewTree(P.newParent.value(nid, -1));
        };
        const bool ownCommented = includeIn(text).commented;
        const bool parentDisabled = disabledInNewTree(np);
        if (u.commented && !ownCommented && !parentDisabled) {
            text.insert(text.indexOf('\\'), "%");
            plan.steps << QString("    “%1” was inside a commented-out part of the book; its line is commented out so it stays excluded").arg(u.title);
        } else if (!u.commented && parentDisabled) {
            plan.warnings << QString("“%1” goes into a commented-out part of the book, so it won't be compiled until that part is enabled.").arg(u.title);
        }

        // Put it in the new place.
        const QVector<int> sibs = tree.value(np);
        const int i = sibs.indexOf(id);
        int newId = -1;
        for (int k = i - 1; k >= 0 && newId < 0; --k) {
            const Node &s = P.node(sibs[k]);
            if (s.kind == Kind::Heading) {
                if (s.file == destFile) newId = P.insertAfter(destFile, P.regionEnd(s), text);
                break;
            }
            const auto sl = P.lineLoc.value(sibs[k]);
            if (sl.first == destFile) newId = P.insertAfter(destFile, sl.second, text);
            break;
        }
        if (newId < 0)
            for (int k = i + 1; k < sibs.size() && newId < 0; ++k) {
                const Node &s = P.node(sibs[k]);
                if (s.kind == Kind::Heading) {
                    if (s.file == destFile) newId = P.insertBefore(destFile, s.line, text);
                    break;
                }
                if (P.moved.contains(sibs[k])) continue;   // not placed yet
                const auto sl = P.lineLoc.value(sibs[k]);
                if (sl.first == destFile) newId = P.insertBefore(destFile, sl.second, text);
                break;
            }
        if (newId < 0) {
            const Node &p = P.node(np);
            if (p.kind == Kind::Heading) newId = P.insertAfter(destFile, P.regionEnd(p), text);
            else {
                const int ed = P.endDocumentLine(destFile);
                if (ed >= 0) newId = P.insertBefore(destFile, ed, text);
                else {
                    QVector<LineRec> &v = P.linesOf(destFile);
                    int last = v.size() - 1;
                    while (last > 0 && v[last].text.trimmed().isEmpty()) --last;
                    newId = P.insertAfter(destFile, v[last].id, text);
                }
            }
        }
        P.lineLoc.insert(id, {destFile, newId});

        const Node &oldP = P.node(u.parent), &newP = P.node(np);
        const QString what = QString("%1 “%2”").arg(kindName(u.level), u.title);
        if (u.parent == np)
            plan.steps << QString("Reorder %1 within %2").arg(what, newP.kind == Kind::Heading ? "“" + newP.title + "”" : P.rel(destFile));
        else
            plan.steps << QString("Move %1 from “%2” to “%3”").arg(what, oldP.title, newP.title);
        if (oldLoc.first == destFile)
            plan.steps << QString("    edit %1: move the \\%2 line").arg(P.rel(destFile), includeIn(text).cmd);
        else
            plan.steps << QString("    edit %1: remove its \\%2 line; edit %3: insert it")
                              .arg(P.rel(oldLoc.first), includeIn(text).cmd, P.rel(destFile));
    }

    // 3. Promote / demote headings of moved units (and everything they contain).
    QHash<QString, int> shiftOf;   // file -> delta
    for (int id : order) {
        if (!P.moved.contains(id)) continue;
        const Node &u = P.node(id);
        const int delta = int(std::lround(newLevel.value(id) - u.level));
        if (delta == 0) continue;
        std::function<void(int)> collect = [&](int nid) {
            const Node &n = P.node(nid);
            if ((n.kind == Kind::Unit || n.kind == Kind::Container || n.kind == Kind::Plain) && (nid == id || !P.moved.contains(nid)))
                if (!shiftOf.contains(n.target)) shiftOf.insert(n.target, delta);
            for (int c : tree.value(nid))
                if (!P.moved.contains(c)) collect(c);
        };
        collect(id);
        plan.steps << QString("%1 “%2”: %3 → %4 (and everything inside it shifts by %5 level%6)")
                          .arg(delta < 0 ? "Promote" : "Demote", u.title, kindName(u.level),
                               kindName(newLevel.value(id)), QString::number(std::abs(delta)), std::abs(delta) > 1 ? "s" : "");
    }

    // 4. Files and folders to move.
    QVector<Relocation> &rels = plan.relocations;
    QSet<QString> shared;
    for (int id : order) {
        if (!P.moved.contains(id)) continue;
        const Node &u = P.node(id);
        const int np = P.newParent.value(id);
        // Destination folder: where the unmoved siblings live, else beside the parent's file.
        QHash<QString, int> votes;
        for (int s : tree.value(np)) {
            const Node &sn = P.node(s);
            if (s == id || P.moved.contains(s) || !isMovable(sn)) continue;
            votes[P.home(sn)]++;
        }
        QString destHome = dirOf(P.fileOf(np));
        int best = 0;
        for (auto v = votes.cbegin(); v != votes.cend(); ++v)
            if (v.value() > best) { best = v.value(); destHome = v.key(); }

        const bool owns = P.ownsFolder(u);
        const bool becomesChapter = std::lround(newLevel.value(id)) == 1 && std::lround(u.level) != 1;
        Relocation r;
        if (owns) {
            r.from = dirOf(u.target);
            r.isDir = true;
            r.newName = QFileInfo(r.from).fileName();
        } else {
            r.from = u.target;
            r.newName = becomesChapter ? QFileInfo(u.target).completeBaseName() + "/main.tex" : QFileInfo(u.target).fileName();
        }
        r.toDir = destHome;
        if (destHome == P.home(u) && !(becomesChapter && !owns)) continue;
        if (owns && isInside(destHome, r.from)) {
            plan.errors << QString("“%1” can't be moved into itself.").arg(u.title);
            continue;
        }
        rels << r;

        // Figures used only by this text travel with it.
        if (!owns && moveExclusiveFigures) {
            static const QRegularExpression gr(R"(\\includegraphics\s*(?:\[[^\]]*\])?\s*\{([^}]+)\})");
            const QString oldHome = P.home(u);
            auto gi = gr.globalMatch(P.text(u.target));
            while (gi.hasNext()) {
                const QString arg = gi.next().captured(1).trimmed();
                QString img;
                for (const QString &e : {"", ".pdf", ".png", ".jpg", ".jpeg", ".eps", ".svg"}) {
                    const QString c = clean(m.root + "/" + arg + e);
                    if (QFileInfo(c).isFile()) { img = c; break; }
                }
                if (img.isEmpty() || !isInside(img, oldHome)) continue;
                bool isShared = false;
                for (const QString &f : m.texFiles) {
                    if (f == u.target) continue;
                    const QString &t = P.text(f);
                    if (t.contains("{" + arg + "}") || t.contains("{" + arg + ".")) { isShared = true; break; }
                }
                if (isShared) { shared.insert(QDir(m.root).relativeFilePath(img)); continue; }
                Relocation fr;
                fr.from = img;
                // Same place relative to the text file as before (e.g. Figures/ beside it).
                const QString newTextDir = clean(destHome + "/" + QFileInfo(r.newName).path());
                fr.toDir = clean(newTextDir + "/" + QDir(dirOf(u.target)).relativeFilePath(dirOf(img)));
                fr.newName = QFileInfo(img).fileName();
                bool dup = false;
                for (const Relocation &x : rels) if (x.from == fr.from) dup = true;
                if (!dup) rels << fr;
            }
        }
    }
    for (const Relocation &r : rels) {
        const QString fin = finalPathOf(r.from, rels);
        if (fin == r.from) continue;
        bool vacated = false;
        for (const Relocation &x : rels) if (isInside(fin, x.from)) vacated = true;
        if (QFileInfo::exists(fin) && !vacated)
            plan.errors << QString("%1 already exists — rename one of them first.").arg(P.rel(fin));
        if (r.isDir) {
            int count = 0;
            QDirIterator di(r.from, QDir::Files, QDirIterator::Subdirectories);
            while (di.hasNext()) { di.next(); ++count; }
            plan.steps << QString("Move folder %1/ → %2/  (%3 files: text, figures, papers, notes…)")
                              .arg(P.rel(r.from), P.rel(fin)).arg(count);
        } else {
            plan.steps << QString("Move file %1 → %2").arg(P.rel(r.from), P.rel(fin));
        }
    }
    for (const QString &s : shared)
        plan.warnings << QString("Figure %1 is also used elsewhere, so it stays where it is (its path still works).").arg(s);

    // 5. Produce new texts: line edits, heading shifts, path rewrites.
    auto currentText = [&](const QString &path) -> QString {
        if (P.files.contains(path)) {
            QStringList out;
            for (const LineRec &r : P.files.value(path)) if (!r.removed) out << r.text;
            return out.join('\n');
        }
        return P.text(path);
    };
    QSet<QString> all(m.texFiles.begin(), m.texFiles.end());
    for (auto it = P.files.cbegin(); it != P.files.cend(); ++it) all.insert(it.key());
    for (auto it = shiftOf.cbegin(); it != shiftOf.cend(); ++it) all.insert(it.key());

    int pathEdits = 0, pathFiles = 0, headingEdits = 0;
    static const QStringList exts = {"", ".tex", ".pdf", ".png", ".jpg", ".jpeg", ".eps", ".svg"};
    static const QRegularExpression arg(R"(\{([^{}\n]*)\})");
    for (const QString &path : all) {
        const QString before = P.text(path);
        QString text = currentText(path);
        if (shiftOf.contains(path)) {
            QString err;
            int n = 0;
            text = shiftHeadings(text, shiftOf.value(path), &n, &err);
            if (!err.isEmpty()) plan.errors << QString("%1: %2").arg(P.rel(path), err);
            headingEdits += n;
        }
        if (!rels.isEmpty()) {
            QString out;
            int last = 0, edits = 0;
            auto it = arg.globalMatch(text);
            while (it.hasNext()) {
                auto mm = it.next();
                const QString a = mm.captured(1).trimmed();
                if (a.isEmpty() || a.contains(':') || a.contains('\\')) continue;
                const QString cand = clean(m.root + "/" + a);
                // The most specific move wins: a file moved out of a folder that
                // also moves must follow the file, not the folder.
                int bestLen = -1;
                QString bestExt;
                for (const QString &e : exts)
                    for (const Relocation &r : rels)
                        if (isInside(cand + e, r.from) && int(r.from.size()) > bestLen) { bestLen = int(r.from.size()); bestExt = e; }
                if (bestLen < 0) continue;
                const QString mapped = finalPathOf(cand + bestExt, rels);
                if (mapped == cand + bestExt) continue;
                QString na = QDir(m.root).relativeFilePath(mapped);
                if (!bestExt.isEmpty()) na.chop(bestExt.size());
                out += text.mid(last, mm.capturedStart(1) - last) + na;
                last = mm.capturedEnd(1);
                ++edits;
            }
            out += text.mid(last);
            text = out;
            if (edits) { pathEdits += edits; ++pathFiles; }
        }
        if (text != before) plan.newContents.insert(path, text);
    }
    if (headingEdits) plan.steps << QString("Change %1 sectioning command%2 to their new level").arg(headingEdits).arg(headingEdits > 1 ? "s" : "");
    if (pathEdits) plan.steps << QString("Rewrite %1 path%2 (\\input, \\includegraphics …) in %3 file%4, commented-out lines included")
                                     .arg(pathEdits).arg(pathEdits > 1 ? "s" : "").arg(pathFiles).arg(pathFiles > 1 ? "s" : "");
    plan.title = "Restructure";
    plan.steps << QString("Edit %1 file%2 in all; labels, \\ref and \\cite need no changes")
                      .arg(plan.newContents.size()).arg(plan.newContents.size() == 1 ? "" : "s");
    return plan;
}

// ---------------------------------------------------------------------------
// Applying and undoing

static bool runGit(const QString &root, const QString &message)
{
    QProcess p;
    p.setWorkingDirectory(root);
    p.start("git", {"add", "-A"});
    if (!p.waitForFinished(60000) || p.exitCode() != 0) return false;
    p.start("git", {"commit", "-q", "-m", message});
    p.waitForFinished(60000);
    return true;   // "nothing to commit" is fine
}

static bool moveItem(const QString &from, const QString &to)
{
    QDir().mkpath(dirOf(to));
    if (QFileInfo(from).isDir()) return QDir().rename(from, to);
    return QFile::rename(from, to);
}

static void createdDirsFor(const QString &dir, QStringList *created)
{
    QStringList chain;
    QString d = dir;
    while (!QFileInfo::exists(d)) {
        chain.prepend(d);
        const QString up = dirOf(d);
        if (up == d) break;
        d = up;
    }
    for (const QString &c : chain) if (!created->contains(c)) created->append(c);
}

Result apply(const Model &m, const Plan &plan, bool gitCommit)
{
    Result res;
    if (!plan.errors.isEmpty()) { res.error = plan.errors.join('\n'); return res; }
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const QString J = m.root + "/.mastertex/restructure/" + stamp;
    if (!QDir().mkpath(J + "/originals") || !QDir().mkpath(J + "/staging")) {
        res.error = "Could not create the journal folder " + J;
        return res;
    }
    res.journalDir = J;
    // The journal is MasterTex's private bookkeeping: keep it out of Git.
    if (!QFileInfo::exists(m.root + "/.mastertex/.gitignore")) writeText(m.root + "/.mastertex/.gitignore", "*\n", nullptr);

    // Keep the original text of every file we will change.
    QJsonArray written;
    for (auto it = plan.newContents.cbegin(); it != plan.newContents.cend(); ++it) {
        const QString rel = QDir(m.root).relativeFilePath(it.key());
        QDir().mkpath(dirOf(J + "/originals/" + rel));
        if (!QFile::copy(it.key(), J + "/originals/" + rel)) {
            res.error = "Could not back up " + rel;
            return res;
        }
        QJsonObject w;
        w["original"] = it.key();
        w["final"] = finalPathOf(it.key(), plan.relocations);
        w["rel"] = rel;
        written.append(w);
    }

    const QString what = plan.title.isEmpty() ? QString("restructure") : plan.title;
    if (gitCommit && isGitRepo(m.root)) runGit(m.root, "MasterTex: before " + what + " " + stamp);

    // Order: stage deepest sources first, place shallowest destinations first.
    QVector<Relocation> rels = plan.relocations;
    QVector<int> stageOrder(rels.size()), placeOrder(rels.size());
    for (int i = 0; i < rels.size(); ++i) stageOrder[i] = placeOrder[i] = i;
    std::sort(stageOrder.begin(), stageOrder.end(), [&](int a, int b) { return rels[a].from.count('/') > rels[b].from.count('/'); });
    QStringList finals;
    for (const Relocation &r : rels) finals << finalPathOf(r.from, rels);
    std::sort(placeOrder.begin(), placeOrder.end(), [&](int a, int b) { return finals[a].count('/') < finals[b].count('/'); });

    QJsonArray moves;
    for (int i = 0; i < rels.size(); ++i) {
        QJsonObject o;
        o["from"] = rels[i].from;
        o["to"] = finals[i];
        moves.append(o);
    }
    QStringList created;
    auto saveJournal = [&](const QString &state) {
        QJsonObject j;
        j["stamp"] = stamp;
        j["title"] = plan.title.isEmpty() ? QString("Restructure") : plan.title;
        j["root"] = m.root;
        j["state"] = state;
        j["moves"] = moves;
        j["written"] = written;
        j["created"] = QJsonArray::fromStringList(created);
        QFile f(J + "/journal.json");
        if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(j).toJson());
    };
    saveJournal("started");

    QVector<int> staged, placed;
    QString error;
    auto rollback = [&]() {
        for (int k = placed.size() - 1; k >= 0; --k) moveItem(finals[placed[k]], J + "/staging/" + QString::number(placed[k]));
        for (int k = staged.size() - 1; k >= 0; --k) moveItem(J + "/staging/" + QString::number(staged[k]), rels[staged[k]].from);
        for (auto it = plan.newContents.cbegin(); it != plan.newContents.cend(); ++it) {
            const QString rel = QDir(m.root).relativeFilePath(it.key());
            QFile::remove(it.key());
            QFile::copy(J + "/originals/" + rel, it.key());
        }
        for (int k = created.size() - 1; k >= 0; --k) QDir().rmdir(created[k]);
        saveJournal("rolled-back");
    };

    for (int i : stageOrder) {
        if (!moveItem(rels[i].from, J + "/staging/" + QString::number(i))) {
            error = "Could not move " + rels[i].from;
            rollback();
            res.error = error;
            return res;
        }
        staged << i;
    }
    for (int i : placeOrder) {
        createdDirsFor(dirOf(finals[i]), &created);
        if (QFileInfo::exists(finals[i]) || !moveItem(J + "/staging/" + QString::number(i), finals[i])) {
            error = "Could not place " + finals[i];
            rollback();
            res.error = error;
            return res;
        }
        placed << i;
    }
    for (auto it = plan.newContents.cbegin(); it != plan.newContents.cend(); ++it) {
        const QString fin = finalPathOf(it.key(), rels);
        QString werr;
        if (!writeText(fin, it.value(), &werr)) {
            // Put any already-written files back first.
            for (auto jt = plan.newContents.cbegin(); jt != it; ++jt) {
                const QString f2 = finalPathOf(jt.key(), rels);
                QFile::remove(f2);
                QFile::copy(J + "/originals/" + QDir(m.root).relativeFilePath(jt.key()), f2);
            }
            rollback();
            res.error = "Could not write " + fin + ": " + werr;
            return res;
        }
        res.changedFiles << fin;
    }
    QDir(J + "/staging").removeRecursively();   // empty by now
    saveJournal("applied");
    if (gitCommit && isGitRepo(m.root)) runGit(m.root, "MasterTex: " + what + " " + stamp);

    for (int i = 0; i < rels.size(); ++i) res.pathMap.insert(rels[i].from, finals[i]);
    res.ok = true;
    return res;
}

QString lastJournal(const QString &root)
{
    QDir d(root + "/.mastertex/restructure");
    QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QString &e : entries) {
        if (e.endsWith("-undone")) continue;
        QFile f(d.absoluteFilePath(e) + "/journal.json");
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (QJsonDocument::fromJson(f.readAll()).object().value("state").toString() == "applied")
            return d.absoluteFilePath(e);
    }
    return {};
}

QString describeJournal(const QString &journalDir)
{
    QFile f(journalDir + "/journal.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    const QDateTime when = QDateTime::fromString(j.value("stamp").toString(), "yyyyMMdd-HHmmss");
    const QString title = j.value("title").toString().isEmpty() ? QString("Restructure") : j.value("title").toString();
    return QString("“%1” of %2: %3 moved item(s), %4 edited file(s)")
        .arg(title, QLocale().toString(when, QLocale::ShortFormat))
        .arg(j.value("moves").toArray().size())
        .arg(j.value("written").toArray().size());
}

QStringList editedSince(const QString &J)
{
    QStringList out;
    QFile f(J + "/journal.json");
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QDateTime done = QFileInfo(f).lastModified().addSecs(2);
    const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : j.value("written").toArray()) {
        const QString fin = v.toObject().value("final").toString();
        if (QFileInfo(fin).lastModified() > done) out << QDir(j.value("root").toString()).relativeFilePath(fin);
    }
    return out;
}

Result undo(const QString &J)
{
    Result res;
    QFile f(J + "/journal.json");
    if (!f.open(QIODevice::ReadOnly)) { res.error = "No journal found."; return res; }
    QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    if (j.value("state").toString() != "applied") { res.error = "That restructure is not in an applied state."; return res; }
    const QJsonArray moves = j.value("moves").toArray();
    QVector<QPair<QString, QString>> mv;   // (to, from)
    for (const QJsonValue &v : moves) mv.push_back({v.toObject().value("to").toString(), v.toObject().value("from").toString()});

    const QString stage = J + "/undo-staging";
    QDir().mkpath(stage);
    QVector<int> a(mv.size()), b(mv.size());
    for (int i = 0; i < mv.size(); ++i) a[i] = b[i] = i;
    std::sort(a.begin(), a.end(), [&](int x, int y) { return mv[x].first.count('/') > mv[y].first.count('/'); });
    std::sort(b.begin(), b.end(), [&](int x, int y) { return mv[x].second.count('/') < mv[y].second.count('/'); });
    for (int i : a)
        if (!moveItem(mv[i].first, stage + "/" + QString::number(i))) { res.error = "Could not move back " + mv[i].first; return res; }
    for (int i : b)
        if (!moveItem(stage + "/" + QString::number(i), mv[i].second)) { res.error = "Could not restore " + mv[i].second; return res; }
    QDir(stage).removeRecursively();

    for (const QJsonValue &v : j.value("written").toArray()) {
        const QJsonObject w = v.toObject();
        const QString original = w.value("original").toString();
        QFile::remove(original);
        if (!QFile::copy(J + "/originals/" + w.value("rel").toString(), original)) {
            res.error = "Could not restore " + original;
            return res;
        }
        res.changedFiles << original;
    }
    QStringList created;
    for (const QJsonValue &v : j.value("created").toArray()) created << v.toString();
    for (int k = created.size() - 1; k >= 0; --k) QDir().rmdir(created[k]);   // only if empty

    j["state"] = "undone";
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(j).toJson()); f.close(); }
    QDir().rename(J, J + "-undone");
    const QString root = j.value("root").toString();
    if (isGitRepo(root)) runGit(root, "MasterTex: undo " + j.value("title").toString() + " " + j.value("stamp").toString());
    for (const auto &p : mv) res.pathMap.insert(p.first, p.second);
    res.ok = true;
    return res;
}

} // namespace Restructure
