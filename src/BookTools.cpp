#include "BookTools.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <climits>
#include <numeric>
#include <functional>

namespace BookTools {

// ---------------------------------------------------------------------------
// Small helpers

static QString clean(const QString &p) { return QDir::cleanPath(p); }
static QString dirOf(const QString &p) { return clean(QFileInfo(p).absolutePath()); }
static bool isInside(const QString &p, const QString &d) { return p == d || p.startsWith(d + "/"); }

static QHash<QString, QString> &textCache()
{
    static QHash<QString, QString> c;
    return c;
}

static QString readText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.readAll();
    auto dec = QStringDecoder(QStringDecoder::Utf8);
    QString s = dec(b);
    if (dec.hasError()) s = QString::fromLatin1(b);
    return s;
}

static int commentPos(const QString &line)
{
    for (int i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++i; continue; }
        if (line[i] == '%') return i;
    }
    return -1;
}

// Offsets of line starts, for offset -> line number.
struct LineIndex {
    QVector<int> starts{0};
    explicit LineIndex(const QString &t)
    {
        for (int i = 0; i < t.size(); ++i) if (t[i] == '\n') starts << i + 1;
    }
    int lineOf(int offset) const
    {
        return int(std::upper_bound(starts.begin(), starts.end(), offset) - starts.begin());
    }
};

static QString resolveTex(const QString &root, const QString &written)
{
    QFileInfo fi(QDir(root), written.trimmed());
    if (!fi.exists() && fi.suffix() != "tex") fi = QFileInfo(QDir(root), written.trimmed() + ".tex");
    return clean(fi.absoluteFilePath());
}

static const QStringList &imageExts()
{
    static const QStringList e = {"", ".pdf", ".png", ".jpg", ".jpeg", ".eps", ".svg", ".PDF", ".PNG", ".JPG"};
    return e;
}

static const QStringList &levelNames()
{
    static const QStringList n = {"part", "chapter", "section", "subsection", "subsubsection", "paragraph", "subparagraph"};
    return n;
}

static QString braceArg(const QString &s, int openPos, int *endPos = nullptr)
{
    // s[openPos] == '{'
    int depth = 0;
    QString out;
    for (int i = openPos; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '\\' && i + 1 < s.size()) { if (depth > 0) { out += c; out += s[i + 1]; } ++i; continue; }
        if (c == '{') { if (depth++ > 0) out += c; continue; }
        if (c == '}') { if (--depth == 0) { if (endPos) *endPos = i; return out; } out += c; continue; }
        if (depth > 0) out += c;
    }
    if (endPos) *endPos = s.size();
    return out;
}

// ---------------------------------------------------------------------------
// Scanning the book in reading order

namespace {

struct LabelDef {
    QString key;
    QString file;
    int line = 0;        // 1-based
    int keyPos = 0;      // offset of the key in the file text
    QStringList path;    // heading titles, index = level (0 part … 4 subsubsection); may contain empties
    int ownLevel = -1;   // >=0: this is the label of a heading of that level
    QString env;         // innermost environment
    bool commented = false;   // inside a commented-out chapter
};

struct EnvItem {
    QString env;
    QString file;
    int line = 0;
    int labelLine = 0, captionLine = 0;
    int labels = 0;
    bool nonumberAll = false;
    bool commented = false;
};

struct Graphic {
    QString arg;
    QString file;
    int line = 0;
    int argPos = 0;      // offset of the argument in the file text
    bool commented = false;
};

struct HeadingRec {
    int level = 0;
    QStringList path;    // titles from part down to this heading (index = level)
};

struct Scan {
    QString root;
    QVector<HeadingRec> headings;
    QStringList files;   // reading order
    QHash<QString, bool> fileCommented;
    QVector<LabelDef> labels;
    QVector<EnvItem> envs;
    QVector<Graphic> graphics;
    QStringList graphicsPath;
};

const QSet<QString> &eqEnvs()
{
    static const QSet<QString> s = {"equation", "align", "gather", "multline", "eqnarray", "flalign", "alignat", "subequations"};
    return s;
}

struct Walker {
    Scan &sc;
    QSet<QString> visited;
    QStringList stack = QStringList(7);   // current heading titles by level

    explicit Walker(Scan &s) : sc(s) {}

    void walk(const QString &path, bool commented)
    {
        if (visited.contains(path) || !QFileInfo(path).isFile()) return;
        visited.insert(path);
        sc.files << path;
        sc.fileCommented.insert(path, commented);
        const QString text = readText(path);
        textCache().insert(path, text);
        const QStringList lines = text.split('\n');

        static const QRegularExpression startInclude(R"(^\s*(%+\s*)?\\(input|include|subfile)\s*\{([^}]+)\})");
        static const QRegularExpression token(
            R"(\\(part|chapter|section|subsection|subsubsection|paragraph|subparagraph)\*?\s*(?:\[[^\]]*\])?\s*(?=\{))"
            R"(|\\begin\s*\{([^}]+)\})"
            R"(|\\end\s*\{([^}]+)\})"
            R"(|\\label\s*\{([^}]*)\})"
            R"(|\\caption\b)"
            R"(|\\(?:nonumber|notag)\b)"
            R"(|\\includegraphics\s*(?:\[[^\]]*\])?\s*\{([^}]*)\})"
            R"(|\\graphicspath\s*\{)");

        QVector<int> envStack;     // indexes into sc.envs for numbered environments; -1 for others
        QStringList envNames;
        int pendingOwn = -1;       // heading level whose own \label may follow
        int offset = 0;
        for (int li = 0; li < lines.size(); ++li) {
            const QString &line = lines[li];
            const int lineStart = offset;
            offset += line.size() + 1;

            auto inc = startInclude.match(line);
            if (inc.hasMatch()) {
                walk(resolveTex(sc.root, inc.captured(3)), commented || !inc.captured(1).isEmpty());
                continue;
            }
            const int cp = commentPos(line);
            const QString code = cp >= 0 ? line.left(cp) : line;
            if (code.trimmed().isEmpty()) continue;

            bool sawOther = false;
            int last = 0;
            auto it = token.globalMatch(code);
            while (it.hasNext()) {
                auto m = it.next();
                if (!code.mid(last, m.capturedStart() - last).trimmed().isEmpty()) sawOther = true;
                last = m.capturedEnd();
                if (!m.captured(1).isEmpty()) {                       // heading
                    const int lvl = int(levelNames().indexOf(m.captured(1)));
                    int end = 0;
                    const QString title = braceArg(code, m.capturedEnd(), &end);
                    last = end + 1;
                    if (lvl <= 4) {
                        stack[lvl] = title.simplified();
                        for (int k = lvl + 1; k < stack.size(); ++k) stack[k].clear();
                        pendingOwn = lvl;
                        sc.headings << HeadingRec{lvl, stack.mid(0, lvl + 1)};
                    } else {
                        pendingOwn = -1;
                    }
                    sawOther = false;
                    continue;
                }
                if (!m.captured(2).isEmpty()) {                       // \begin
                    const QString env = m.captured(2).trimmed();
                    envNames << env;
                    const QString base = env.endsWith('*') ? env.chopped(1) : env;
                    if (eqEnvs().contains(base) || base == "figure" || base == "table" || base == "wrapfigure"
                        || base == "wraptable" || base == "sidewaysfigure" || base == "sidewaystable") {
                        EnvItem e;
                        e.env = env;
                        e.file = path;
                        e.line = li + 1;
                        e.commented = commented;
                        sc.envs << e;
                        envStack << sc.envs.size() - 1;
                    } else {
                        envStack << -1;
                    }
                    pendingOwn = -1;
                    continue;
                }
                if (!m.captured(3).isEmpty()) {                       // \end
                    if (!envNames.isEmpty()) { envNames.removeLast(); envStack.removeLast(); }
                    continue;
                }
                if (m.capturedStart(4) >= 0 && m.captured(0).startsWith("\\label")) {   // \label
                    LabelDef d;
                    d.key = m.captured(4);
                    d.file = path;
                    d.line = li + 1;
                    d.keyPos = lineStart + m.capturedStart(4);
                    d.path = stack;
                    d.commented = commented;
                    d.env = envNames.isEmpty() ? QString() : envNames.last();
                    if (pendingOwn >= 0 && !sawOther && envNames.isEmpty()) d.ownLevel = pendingOwn;
                    sc.labels << d;
                    for (int k = envStack.size() - 1; k >= 0; --k)
                        if (envStack[k] >= 0) {
                            EnvItem &e = sc.envs[envStack[k]];
                            e.labels++;
                            if (!e.labelLine) e.labelLine = li + 1;
                            break;
                        }
                    continue;
                }
                if (m.captured(0).startsWith("\\caption")) {
                    for (int k = envStack.size() - 1; k >= 0; --k)
                        if (envStack[k] >= 0) { if (!sc.envs[envStack[k]].captionLine) sc.envs[envStack[k]].captionLine = li + 1; break; }
                    sawOther = true;
                    continue;
                }
                if (m.captured(0).startsWith("\\nonumber") || m.captured(0).startsWith("\\notag")) {
                    for (int k = envStack.size() - 1; k >= 0; --k)
                        if (envStack[k] >= 0) { sc.envs[envStack[k]].nonumberAll = true; break; }
                    continue;
                }
                if (m.captured(0).startsWith("\\includegraphics")) {
                    Graphic g;
                    g.arg = m.captured(5).trimmed();
                    g.file = path;
                    g.line = li + 1;
                    g.argPos = lineStart + m.capturedStart(5);
                    g.commented = commented;
                    sc.graphics << g;
                    sawOther = true;
                    continue;
                }
                if (m.captured(0).startsWith("\\graphicspath")) {
                    const QString all = braceArg(code, m.capturedEnd() - 1);
                    static const QRegularExpression inner(R"(\{([^}]*)\})");
                    auto jj = inner.globalMatch(all);   // {dir1/}{dir2/}
                    while (jj.hasNext()) sc.graphicsPath << jj.next().captured(1).trimmed();
                    continue;
                }
            }
            if (!code.mid(last).trimmed().isEmpty()) sawOther = true;
            if (sawOther) pendingOwn = -1;
        }
    }
};

Scan scanBook(const QString &master)
{
    Scan sc;
    sc.root = dirOf(QFileInfo(master).absoluteFilePath());
    Walker w(sc);
    w.walk(clean(QFileInfo(master).absoluteFilePath()), false);
    return sc;
}

} // namespace

QStringList bookFiles(const QString &master, bool includeAll)
{
    textCache().clear();
    Scan sc = scanBook(master);
    QStringList files = sc.files;
    if (includeAll) {
        QDirIterator it(sc.root, {"*.tex"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = clean(it.next());
            if (p.contains("/.mastertex/")) continue;
            if (!files.contains(p)) files << p;
        }
    }
    return files;
}

static QString textOf(const QString &path)
{
    auto it = textCache().find(path);
    if (it == textCache().end()) it = textCache().insert(path, readText(path));
    return *it;
}

// ---------------------------------------------------------------------------
// Replace in All Files

QVector<Match> search(const QStringList &files, const QRegularExpression &re, const QString &replacement, bool useCaptures)
{
    QVector<Match> out;
    if (!re.isValid() || re.pattern().isEmpty()) return out;
    const QRegularExpression anchored(QRegularExpression::anchoredPattern(re.pattern()), re.patternOptions());
    for (const QString &f : files) {
        const QString text = readText(f);
        textCache().insert(f, text);
        LineIndex idx(text);
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            if (m.capturedLength() == 0) continue;
            Match x;
            x.file = f;
            x.start = int(m.capturedStart());
            x.length = int(m.capturedLength());
            x.line = idx.lineOf(x.start);
            const int ls = idx.starts[x.line - 1];
            int le = text.indexOf('\n', ls);
            if (le < 0) le = text.size();
            x.lineText = text.mid(ls, le - ls);
            if (useCaptures) {
                QString s = m.captured(0);
                x.replacement = s.replace(anchored, replacement);
            } else {
                x.replacement = replacement;
            }
            out << x;
        }
    }
    return out;
}

Restructure::Plan replacePlan(const QVector<Match> &chosen, const QString &title)
{
    Restructure::Plan plan;
    plan.title = title;
    QHash<QString, QVector<Match>> byFile;
    for (const Match &m : chosen) byFile[m.file] << m;
    for (auto it = byFile.begin(); it != byFile.end(); ++it) {
        QVector<Match> v = it.value();
        std::sort(v.begin(), v.end(), [](const Match &a, const Match &b) { return a.start > b.start; });
        QString text = readText(it.key());
        for (const Match &m : v) text.replace(m.start, m.length, m.replacement);
        plan.newContents.insert(it.key(), text);
        plan.steps << QString("%1: %2 replacement(s)").arg(QFileInfo(it.key()).fileName()).arg(v.size());
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Rebase

// The significant words of a title, LaTeX removed.
static QStringList significantWords(const QString &title, QString *acronym)
{
    QString t = title;
    t.remove(QRegularExpression(R"(\$[^$]*\$)"));                         // inline math
    t.replace(QRegularExpression(R"(\\[A-Za-z@]+\*?)"), " ");               // commands
    t.replace("--", " ");
    static const QRegularExpression acro(R"(\(([A-Z][A-Za-z0-9]{1,7})\))");
    auto am = acro.match(t);
    if (am.hasMatch() && acronym) {
        int uppers = 0;
        for (QChar c : am.captured(1)) if (c.isUpper()) ++uppers;
        if (uppers >= 2) *acronym = am.captured(1);
    }
    static const QSet<QString> stop = {"a", "an", "the", "of", "and", "or", "for", "in", "on", "to", "with", "from",
                                       "by", "at", "as", "its", "their", "is", "are", "how", "why", "what", "into",
                                       "vs", "via", "some", "about", "this", "that", "used", "using", "brief"};
    QStringList words;
    const QStringList all = t.split(QRegularExpression(R"([^A-Za-z0-9\x{00C0}-\x{024F}]+)"), Qt::SkipEmptyParts);
    for (int wi = 0; wi < all.size(); ++wi) {
        const QString &w = all[wi];
        if (stop.contains(w.toLower()) && (w == w.toLower() || wi == 0)) continue;
        QString ascii;
        for (QChar c : w.normalized(QString::NormalizationForm_D)) if (c.unicode() < 128 && c.isLetterOrNumber()) ascii += c;
        if (ascii.isEmpty()) continue;
        if (!(ascii.size() > 1 && ascii == ascii.toUpper())) ascii = ascii.left(1).toUpper() + ascii.mid(1);
        words << ascii;
    }
    return words;
}

// Short name for a heading: the acronym in parentheses, else the first
// significant word (long words shortened), e.g. "Lagrangian Formulation of
// Classical Mechanics" -> "Lagrangian", "Quantum Chromodynamics (QCD)" -> "QCD".
QString titleToTag(const QString &title)
{
    QString acronym;
    const QStringList words = significantWords(title, &acronym);
    if (!acronym.isEmpty()) return acronym;
    if (words.isEmpty()) return {};
    QString w = words.first();
    // "3-vectors in Euclidean space" -> "3Vectors"
    bool numeric = true;
    for (QChar c : w) if (!c.isDigit()) numeric = false;
    if (numeric && words.size() > 1) w += words[1];
    if (w.size() > 14) w = w.left(10);
    return w;
}

// Short names for every heading, unique among the headings that share a parent:
// when two would be equal, the initials of their following words are added
// ("Charge balance functions", "Charge correlations" -> "ChargeBF", "ChargeC").
static QHash<QString, QString> headingTags(const QVector<HeadingRec> &headings)
{
    auto keyOf = [](const QStringList &path) { return path.join(QChar(0x1f)); };
    QHash<QString, QStringList> siblings;   // parent key -> titles (in order, unique)
    for (const HeadingRec &h : headings) {
        const QString parent = QString::number(h.level) + QChar(0x1e) + keyOf(h.path.mid(0, h.level));
        QStringList &v = siblings[parent];
        if (!v.contains(h.path.last())) v << h.path.last();
    }
    QHash<QString, QString> tags;
    for (const HeadingRec &h : headings) {
        const QString parent = QString::number(h.level) + QChar(0x1e) + keyOf(h.path.mid(0, h.level));
        const QString key = keyOf(h.path);
        if (tags.contains(key)) continue;
        const QStringList &sib = siblings.value(parent);
        QHash<QString, QString> chosen;   // title -> tag, for this sibling group
        QHash<QString, int> uses;
        // Don't repeat a name already used above ("Classical Mechanics" > "Classical (Newtonian)
        // Mechanics" gives Classical:Newtonian, not Classical:Classical).
        QSet<QString> above;
        for (int l = 0; l < h.level; ++l) above.insert(tags.value(keyOf(h.path.mid(0, l + 1))));
        // base tag and the index of the word it came from (-1: acronym)
        auto base = [&](const QString &t, int *wordIndex) {
            QString acronym;
            const QStringList w = significantWords(t, &acronym);
            *wordIndex = -1;
            if (!acronym.isEmpty() && !above.contains(acronym)) return acronym;
            for (int k = 0; k < w.size(); ++k) {
                QString x = w[k];
                bool numeric = true;
                for (QChar c : x) if (!c.isDigit()) numeric = false;
                if (numeric && k + 1 < w.size()) x += w[k + 1];
                if (x.size() > 14) x = x.left(10);
                if (!above.contains(x)) { *wordIndex = numeric ? k + 1 : k; return x; }
            }
            return titleToTag(t);
        };
        for (const QString &t : sib) { int wi; uses[base(t, &wi)]++; }
        QSet<QString> used;
        for (const QString &t : sib) {
            int wi;
            QString tag = base(t, &wi);
            if (uses.value(tag) > 1) {
                // tell same-named siblings apart with the initials of the words that follow
                QString acronym;
                const QStringList w = significantWords(t, &acronym);
                QString initials;
                for (int k = wi + 1; k < w.size() && initials.size() < 3; ++k) initials += w[k].left(1).toUpper();
                tag += initials;
            }
            QString unique = tag;
            for (int n = 2; used.contains(unique) || unique.isEmpty(); ++n) unique = (tag.isEmpty() ? QString("S") : tag) + QString::number(n);
            used.insert(unique);
            chosen.insert(t, unique);
        }
        // record all siblings of this group at once
        for (const HeadingRec &x : headings) {
            const QString p2 = QString::number(x.level) + QChar(0x1e) + keyOf(x.path.mid(0, x.level));
            if (p2 == parent) tags.insert(keyOf(x.path), chosen.value(x.path.last()));
        }
    }
    return tags;
}

static QString normalizePrefix(const QString &p)
{
    static const QHash<QString, QString> map = {
        {"eq", "eq"}, {"eqn", "eq"}, {"equation", "eq"}, {"Eq", "eq"},
        {"sec", "sec"}, {"Sec", "sec"}, {"section", "sec"}, {"subsec", "sec"}, {"ss", "sec"},
        {"ch", "ch"}, {"chap", "ch"}, {"Chap", "ch"}, {"chapter", "ch"}, {"Chapter", "ch"},
        {"part", "part"}, {"Part", "part"},
        {"fig", "fig"}, {"Fig", "fig"}, {"figure", "fig"},
        {"tab", "tab"}, {"Tab", "tab"}, {"table", "tab"},
        {"prob", "prob"}, {"Prob", "prob"}, {"problem", "prob"}, {"ex", "ex"}, {"exercise", "ex"},
        {"th", "thm"}, {"thm", "thm"}, {"theorem", "thm"}, {"lem", "lem"}, {"lemma", "lem"},
        {"def", "def"}, {"defn", "def"}, {"definition", "def"}, {"app", "app"}, {"appendix", "app"},
    };
    return map.value(p, p);
}

static QString prefixFromContext(const LabelDef &d)
{
    if (d.ownLevel == 0) return "part";
    if (d.ownLevel == 1) return "ch";
    if (d.ownLevel >= 2) return "sec";
    QString e = d.env;
    if (e.endsWith('*')) e.chop(1);
    if (eqEnvs().contains(e)) return "eq";
    if (e.contains("figure")) return "fig";
    if (e.contains("table")) return "tab";
    if (e == "theorem") return "thm";
    return {};
}

static const QRegularExpression &refRegex()
{
    static const QRegularExpression re(
        R"(\\(?:ref|eqref|pageref|autoref|Autoref|nameref|vref|Vref|cref|Cref|cpageref|Cpageref|labelcref|subref|hyperref)\*?\s*(?:\{([^}]*)\}|\[([^\]]*)\]))");
    return re;
}

RebaseResult rebase(const QString &master, const QStringList &files, const RebaseOptions &opt)
{
    RebaseResult R;
    textCache().clear();
    Scan sc = scanBook(master);
    const QHash<QString, QString> tags = headingTags(sc.headings);

    // Count definitions of every key in the whole book (also outside the scope).
    QHash<QString, int> defCount;
    for (const LabelDef &d : sc.labels) defCount[d.key]++;
    QSet<QString> finalKeys;
    for (const LabelDef &d : sc.labels) finalKeys.insert(d.key);

    struct Pending { int idx; QString newKey; };
    QVector<Pending> pend;
    QHash<QString, QVector<int>> byNew;
    for (int i = 0; i < sc.labels.size(); ++i) {
        const LabelDef &d = sc.labels[i];
        if (!opt.onlyFile.isEmpty() && d.file != opt.onlyFile) continue;
        if (opt.onlyFile.isEmpty() && !files.isEmpty() && !files.contains(d.file)) continue;
        const int colon = d.key.indexOf(':');
        QString prefix = colon > 0 ? d.key.left(colon) : QString();
        const QString name = d.key.mid(d.key.lastIndexOf(':') + 1).trimmed();
        if (prefix.isEmpty() || colon < 0) prefix = prefixFromContext(d);
        if (prefix.isEmpty()) { R.skipped << QString("%1 (%2:%3): can't tell what kind of label it is").arg(d.key, QFileInfo(d.file).fileName()).arg(d.line); continue; }
        if (opt.normalizePrefixes) prefix = d.ownLevel >= 0 ? prefixFromContext(d) : normalizePrefix(prefix);
        if (d.path.value(1).isEmpty() && !(d.ownLevel == 0 && opt.includePart)) {
            R.skipped << QString("%1 (%2:%3): not inside a chapter").arg(d.key, QFileInfo(d.file).fileName()).arg(d.line);
            continue;
        }
        QStringList comps;
        const int top = opt.includePart ? 0 : 1;
        const int deepest = d.ownLevel >= 0 ? qMin(d.ownLevel, opt.deepestLevel) : opt.deepestLevel;
        for (int l = top; l <= deepest; ++l) {
            if (d.path.value(l).isEmpty()) continue;
            QString tag = tags.value(d.path.mid(0, l + 1).join(QChar(0x1f)));
            if (tag.isEmpty()) tag = titleToTag(d.path.value(l));
            if (!tag.isEmpty()) comps << tag;
        }
        if (d.ownLevel < 0 || d.ownLevel > opt.deepestLevel) {
            if (d.ownLevel > opt.deepestLevel) {
                QString own = tags.value(d.path.mid(0, d.ownLevel + 1).join(QChar(0x1f)));
                comps << (own.isEmpty() ? name : own);
            } else {
                comps << (name.isEmpty() ? QString("x") : name);
            }
        }
        if (comps.isEmpty()) continue;
        const QString nk = prefix + ":" + comps.join(":");
        pend << Pending{i, nk};
        byNew[nk] << pend.size() - 1;
    }

    // Keys not being renamed keep their names; clashes get -2, -3 …
    QSet<QString> renamedOld;
    for (const Pending &p : pend) renamedOld.insert(sc.labels[p.idx].key);
    QSet<QString> taken;
    for (const QString &k : finalKeys) if (!renamedOld.contains(k)) taken.insert(k);
    QVector<bool> clash(pend.size(), false);
    for (auto it = byNew.cbegin(); it != byNew.cend(); ++it) if (it.value().size() > 1) for (int k : it.value()) clash[k] = true;
    for (int k = 0; k < pend.size(); ++k) {
        QString nk = pend[k].newKey;
        if (taken.contains(nk)) {
            clash[k] = true;
            int n = 2;
            while (taken.contains(nk + "-" + QString::number(n))) ++n;
            nk += "-" + QString::number(n);
        }
        taken.insert(nk);
        pend[k].newKey = nk;
    }

    // Old -> new for references. A key defined more than once maps, for each
    // reference, to the definition closest to it (same file, then same folder).
    QHash<QString, QVector<QPair<QString, QString>>> defsOf;   // old -> (file, new)
    for (const Pending &p : pend) {
        const LabelDef &d = sc.labels[p.idx];
        if (d.key != p.newKey) defsOf[d.key] << qMakePair(d.file, p.newKey);
    }
    auto target = [&](const QString &key, const QString &refFile) -> QString {
        const auto v = defsOf.value(key);
        if (v.isEmpty()) return {};
        int best = -1, bestScore = -1;
        for (int i = 0; i < v.size(); ++i) {
            const QString &df = v[i].first;
            int common = 0;
            while (common < df.size() && common < refFile.size() && df[common] == refFile[common]) ++common;
            const int score = df == refFile ? INT_MAX : int(df.left(common).lastIndexOf('/'));
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return v[best].second;
    };

    // Edits: definitions at their exact place, references everywhere in scope.
    struct Edit { int pos, len; QString text; };
    QHash<QString, QVector<Edit>> edits;
    QHash<QString, int> refCount;
    for (const Pending &p : pend) {
        const LabelDef &d = sc.labels[p.idx];
        if (d.key == p.newKey) continue;
        edits[d.file] << Edit{d.keyPos, int(d.key.size()), p.newKey};
    }
    // References are updated in every .tex file in the folder (drafts and copies too),
    // so renaming never leaves a stale reference anywhere.
    QStringList scope = files;
    for (const QString &f : sc.files) if (!scope.contains(f)) scope << f;
    {
        QDirIterator di(sc.root, {"*.tex"}, QDir::Files, QDirIterator::Subdirectories);
        while (di.hasNext()) {
            const QString p = clean(di.next());
            if (!p.contains("/.mastertex/") && !scope.contains(p)) scope << p;
        }
    }
    for (const QString &f : scope) {
        const QString text = textOf(f);
        auto it = refRegex().globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            const int grp = m.capturedStart(1) >= 0 ? 1 : 2;
            const QString arg = m.captured(grp);
            const int argPos = int(m.capturedStart(grp));
            int k = 0;
            for (const QString &part : arg.split(',')) {
                const QString key = part.trimmed();
                const int at = argPos + k + int(part.indexOf(key));
                const QString nk = target(key, f);
                if (!nk.isEmpty()) {
                    edits[f] << Edit{at, int(key.size()), nk};
                    refCount[key]++;
                }
                k += int(part.size()) + 1;
            }
        }
    }
    for (auto it = edits.begin(); it != edits.end(); ++it) {
        QVector<Edit> v = it.value();
        std::sort(v.begin(), v.end(), [](const Edit &a, const Edit &b) { return a.pos > b.pos; });
        QString text = textOf(it.key());
        int lastPos = INT_MAX;
        for (const Edit &e : v) {
            if (e.pos + e.len > lastPos) continue;   // overlapping (shouldn't happen)
            text.replace(e.pos, e.len, e.text);
            lastPos = e.pos;
        }
        R.plan.newContents.insert(it.key(), text);
    }

    for (int k = 0; k < pend.size(); ++k) {
        const LabelDef &d = sc.labels[pend[k].idx];
        if (d.key == pend[k].newKey) continue;
        LabelChange c;
        c.oldKey = d.key;
        c.newKey = pend[k].newKey;
        c.file = d.file;
        c.line = d.line;
        c.refs = refCount.value(d.key);
        c.clash = clash[k];
        c.duplicateOld = defCount.value(d.key) > 1;
        R.changes << c;
    }
    int clashes = 0, dups = 0;
    for (const LabelChange &c : R.changes) { if (c.clash) ++clashes; if (c.duplicateOld) ++dups; }
    R.plan.title = opt.onlyFile.isEmpty() ? QString("Rebase labels (all files)") : QString("Rebase labels (%1)").arg(QFileInfo(opt.onlyFile).fileName());
    R.plan.steps << QString("Rename %1 label(s) and update %2 reference(s) in %3 file(s)")
                        .arg(R.changes.size()).arg(std::accumulate(refCount.begin(), refCount.end(), 0)).arg(R.plan.newContents.size());
    if (clashes) R.plan.warnings << QString("%1 label(s) would have had the same name; they are numbered (-2, -3 …) — review them").arg(clashes);
    if (dups) R.plan.warnings << QString("%1 label(s) were defined more than once; each reference goes to the definition nearest to it — review them").arg(dups);
    if (!R.skipped.isEmpty()) R.plan.warnings << QString("%1 label(s) left unchanged (not inside a chapter, or kind unknown)").arg(R.skipped.size());
    return R;
}

// ---------------------------------------------------------------------------
// Checks

QVector<Finding> check(const QString &master, const QStringList &files, CheckKind kind, bool includeCommented)
{
    Q_UNUSED(files);
    QVector<Finding> out;
    textCache().clear();
    Scan sc = scanBook(master);

    QHash<QString, QVector<const LabelDef *>> defs;
    for (const LabelDef &d : sc.labels) defs[d.key] << &d;

    auto wantedEnv = [&](const QString &env) {
        QString e = env;
        if (e.endsWith('*')) e.chop(1);
        switch (kind) {
        case CheckKind::Equations: return eqEnvs().contains(e) && e != "subequations";
        case CheckKind::Tables: return e.contains("table");
        case CheckKind::Figures: return e.contains("figure");
        }
        return false;
    };
    const QString noun = kind == CheckKind::Equations ? "Equation" : kind == CheckKind::Tables ? "Table" : "Figure";
    const QStringList prefixes = kind == CheckKind::Equations ? QStringList{"eq", "Eq", "eqn", "equation"}
                               : kind == CheckKind::Tables ? QStringList{"tab", "Tab", "table"}
                               : QStringList{"fig", "Fig", "figure"};

    // Environments without labels (or with the label before the caption).
    for (const EnvItem &e : sc.envs) {
        if ((e.commented && !includeCommented) || !wantedEnv(e.env)) continue;
        const bool starred = e.env.endsWith('*');
        if (kind == CheckKind::Equations) {
            if (starred || e.labels > 0) continue;
            const QString base = e.env;
            if (base.startsWith("align") || base == "gather" || base == "eqnarray" || base == "flalign") {
                if (!e.nonumberAll)
                    out << Finding{noun + " without a label", QString("%1 environment has numbered lines but no \\label").arg(e.env), e.file, e.line, 1};
            } else {
                out << Finding{noun + " without a label", QString("numbered %1 has no \\label").arg(e.env), e.file, e.line, 1};
            }
        } else {
            if (e.labels == 0)
                out << Finding{noun + " without a label", QString("%1 has no \\label").arg(e.env), e.file, e.line, 1};
            else if (e.captionLine && e.labelLine && e.labelLine < e.captionLine)
                out << Finding{"Label before caption", "\\label comes before \\caption, so references show the wrong number", e.file, e.labelLine, 0};
            if (e.captionLine == 0)
                out << Finding{noun + " without a caption", QString("%1 has no \\caption").arg(e.env), e.file, e.line, 2};
        }
    }

    // References of this kind that point nowhere (or only into commented-out chapters).
    QHash<QString, int> uses;
    for (const QString &f : sc.files) {
        if (sc.fileCommented.value(f) && !includeCommented) continue;
        const QString text = textOf(f);
        LineIndex idx(text);
        const QStringList lines = text.split('\n');
        auto it = refRegex().globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            const int ln = idx.lineOf(int(m.capturedStart()));
            const QString &lt = lines.value(ln - 1);
            const int cp = commentPos(lt);
            if (cp >= 0 && int(m.capturedStart()) - idx.starts[ln - 1] > cp) continue;   // in a comment
            const bool isEqref = m.captured(0).startsWith("\\eqref");
            const QString arg = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
            for (const QString &part : arg.split(',')) {
                const QString key = part.trimmed();
                if (key.isEmpty()) continue;
                uses[key]++;
                const QString pre = key.left(key.indexOf(':'));
                const bool ofKind = prefixes.contains(pre) || (kind == CheckKind::Equations && isEqref);
                if (!ofKind) continue;
                const auto dl = defs.value(key);
                if (dl.isEmpty())
                    out << Finding{"Reference to a missing label", QString("\\ref to “%1”, which is not defined anywhere").arg(key), f, ln, 0};
                else if (!includeCommented && std::all_of(dl.begin(), dl.end(), [](const LabelDef *d) { return d->commented; }))
                    out << Finding{"Reference into a commented-out chapter", QString("“%1” is only defined in a chapter that is commented out").arg(key), f, ln, 1};
            }
        }
    }
    // Duplicates and unused labels of this kind.
    for (auto it = defs.cbegin(); it != defs.cend(); ++it) {
        const LabelDef *d = it.value().first();
        if (d->commented && !includeCommented) continue;
        QString pre = it.key().left(it.key().indexOf(':'));
        QString e = d->env;
        if (e.endsWith('*')) e.chop(1);
        const bool ofKind = prefixes.contains(pre) || wantedEnv(d->env);
        if (!ofKind) continue;
        if (it.value().size() > 1)
            for (const LabelDef *x : it.value())
                out << Finding{"Duplicate label", QString("“%1” is defined %2 times").arg(it.key()).arg(it.value().size()), x->file, x->line, 0};
        if (!uses.contains(it.key()))
            out << Finding{"Label never referenced", QString("“%1”").arg(it.key()), d->file, d->line, 2};
    }
    std::stable_sort(out.begin(), out.end(), [](const Finding &a, const Finding &b) {
        return a.severity != b.severity ? a.severity < b.severity : a.category < b.category;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Figure files

static QString resolveImage(const Scan &sc, const QString &arg)
{
    QStringList dirs{QString()};
    dirs << sc.graphicsPath;
    for (const QString &d : dirs)
        for (const QString &e : imageExts()) {
            const QFileInfo fi(QDir(sc.root), d + arg + e);
            if (fi.isFile()) return clean(fi.absoluteFilePath());
        }
    return {};
}

// The chapter folder a file belongs to: nearest folder (from the file upwards)
// whose main.tex starts a chapter.
static QString chapterDirOf(const QString &root, const QString &file)
{
    static const QRegularExpression ch(R"(^[^%\n]*\\chapter\*?\s*[\[{])", QRegularExpression::MultilineOption);
    for (QString d = dirOf(file); isInside(d, root) && d != root; d = dirOf(d)) {
        const QString mainTex = d + "/main.tex";
        if (QFileInfo(mainTex).isFile() && ch.match(textOf(mainTex)).hasMatch()) return d;
    }
    return {};
}

QVector<FigureIssue> checkFigureFiles(const QString &master, const QStringList &files, bool includeCommented)
{
    Q_UNUSED(files);
    QVector<FigureIssue> out;
    textCache().clear();
    Scan sc = scanBook(master);

    // Every image file in the book folder, by base name.
    QHash<QString, QStringList> byBase;
    QStringList allImages;
    QDirIterator di(sc.root, {"*.pdf", "*.png", "*.jpg", "*.jpeg", "*.eps", "*.svg", "*.PDF", "*.PNG", "*.JPG"},
                    QDir::Files, QDirIterator::Subdirectories);
    while (di.hasNext()) {
        const QString p = clean(di.next());
        if (p.contains("/.mastertex/")) continue;
        allImages << p;
        byBase[QFileInfo(p).completeBaseName()] << p;
    }

    QHash<QString, QSet<QString>> homesOf;   // image -> chapter folders using it
    QSet<QString> used;
    for (const Graphic &g : sc.graphics) {
        const QString img = resolveImage(sc, g.arg);
        const QString home = chapterDirOf(sc.root, g.file);
        if (img.isEmpty() && g.commented && !includeCommented) continue;
        if (img.isEmpty()) {
            QStringList cands = byBase.value(QFileInfo(g.arg).completeBaseName());
            if (cands.size() > 1) {
                // Prefer the file whose path ends with the most folders of the written path
                // ("PHOBOS/2010eyu/Figure15" -> …/PHOBOS/2010eyu/Figure15.pdf).
                const QStringList want = QFileInfo(g.arg).path().split('/', Qt::SkipEmptyParts);
                auto score = [&](const QString &c) {
                    const QStringList have = QFileInfo(c).path().split('/', Qt::SkipEmptyParts);
                    int n = 0;
                    while (n < want.size() && n < have.size() && want[want.size() - 1 - n] == have[have.size() - 1 - n]) ++n;
                    return n;
                };
                int best = -1, count = 0;
                for (const QString &c : cands) { const int sc2 = score(c); if (sc2 > best) { best = sc2; count = 1; } else if (sc2 == best) ++count; }
                if (best > 0 && count == 1) {
                    QStringList keep;
                    for (const QString &c : cands) if (score(c) == best) keep << c;
                    cands = keep;
                }
            }
            FigureIssue fi;
            fi.kind = "missing";
            fi.file = g.file;
            fi.line = g.line;
            if (cands.size() == 1) {
                fi.image = cands.first();
                fi.fixable = true;
                fi.message = QString("“%1” not found; found %2 — Fix points the path there")
                                 .arg(g.arg, QDir(sc.root).relativeFilePath(cands.first()));
            } else if (cands.size() > 1) {
                QStringList rel;
                for (const QString &c : cands) rel << QDir(sc.root).relativeFilePath(c);
                fi.message = QString("“%1” not found; several files have that name: %2").arg(g.arg, rel.join(", "));
            } else {
                fi.message = QString("“%1” not found anywhere in the book folder").arg(g.arg);
            }
            if (g.commented) fi.message += " (commented-out chapter)";
            out << fi;
            continue;
        }
        used.insert(img);
        if (!home.isEmpty()) homesOf[img].insert(home);
    }
    for (auto it = homesOf.cbegin(); it != homesOf.cend(); ++it) {
        const QString img = it.key();
        const QString rel = QDir(sc.root).relativeFilePath(img);
        if (rel.startsWith("Figures/")) continue;   // the book-wide figure library
        if (it.value().size() > 1) {
            bool insideOne = false;
            for (const QString &h : it.value()) if (isInside(img, h)) insideOne = true;
            if (!insideOne) {
                FigureIssue fi;
                fi.kind = "shared";
                fi.image = img;
                fi.message = QString("%1 is used by %2 chapters and lives in none of them — consider the book-wide Figures/ folder")
                                 .arg(rel).arg(it.value().size());
                out << fi;
            }
            continue;
        }
        const QString home = *it.value().begin();
        if (isInside(img, home)) continue;
        FigureIssue fi;
        fi.kind = "misplaced";
        fi.image = img;
        // first use, for jumping
        for (const Graphic &g : sc.graphics)
            if (resolveImage(sc, g.arg) == img) { fi.file = g.file; fi.line = g.line; break; }
        const QString target = home + "/Figures/" + QFileInfo(img).fileName();
        fi.fixable = !QFileInfo::exists(target);
        fi.message = QString("%1 is used only by %2 but lives elsewhere%3")
                         .arg(rel, QDir(sc.root).relativeFilePath(home),
                              fi.fixable ? " — Fix moves it to " + QDir(sc.root).relativeFilePath(target)
                                         : " (a file with that name is already there)");
        out << fi;
    }
    for (const QString &img : allImages) {
        if (used.contains(img)) continue;
        const QString rel = QDir(sc.root).relativeFilePath(img);
        if (!rel.contains("/Figures/") && !rel.startsWith("Figures/")) continue;   // papers etc. are not figures
        FigureIssue fi;
        fi.kind = "unused";
        fi.image = img;
        fi.file = img;
        fi.message = QString("%1 is not used anywhere in the book").arg(rel);
        out << fi;
    }
    return out;
}

Restructure::Plan fixFigureFiles(const QString &master, const QStringList &files, const QVector<FigureIssue> &toFix)
{
    Q_UNUSED(files);
    Restructure::Plan plan;
    plan.title = "Fix figure files";
    textCache().clear();
    Scan sc = scanBook(master);

    QHash<QString, QString> newPathOf;   // image (old abs) -> new abs
    for (const FigureIssue &fi : toFix) {
        if (!fi.fixable) continue;
        if (fi.kind == "misplaced") {
            const QString home = chapterDirOf(sc.root, fi.file);
            if (home.isEmpty()) continue;
            Restructure::Relocation r;
            r.from = fi.image;
            r.toDir = home + "/Figures";
            r.newName = QFileInfo(fi.image).fileName();
            plan.relocations << r;
            newPathOf.insert(fi.image, r.toDir + "/" + r.newName);
            plan.steps << QString("Move %1 → %2").arg(QDir(sc.root).relativeFilePath(fi.image),
                                                       QDir(sc.root).relativeFilePath(r.toDir + "/" + r.newName));
        }
    }
    struct Edit { int pos, len; QString text; };
    QHash<QString, QVector<Edit>> edits;
    for (const Graphic &g : sc.graphics) {
        const QString img = resolveImage(sc, g.arg);
        QString target;
        if (!img.isEmpty() && newPathOf.contains(img)) target = newPathOf.value(img);
        if (img.isEmpty())
            for (const FigureIssue &fi : toFix)
                if (fi.kind == "missing" && fi.fixable && fi.file == g.file && fi.line == g.line) target = fi.image;
        if (target.isEmpty()) continue;
        QString rel = QDir(sc.root).relativeFilePath(target);
        if (QFileInfo(g.arg).suffix().isEmpty()) rel = rel.left(rel.size() - QFileInfo(rel).suffix().size() - 1);
        edits[g.file] << Edit{g.argPos, int(g.arg.size()), rel};
        if (img.isEmpty()) plan.steps << QString("%1:%2: “%3” → “%4”").arg(QFileInfo(g.file).fileName()).arg(g.line).arg(g.arg, rel);
    }
    for (auto it = edits.begin(); it != edits.end(); ++it) {
        QVector<Edit> v = it.value();
        std::sort(v.begin(), v.end(), [](const Edit &a, const Edit &b) { return a.pos > b.pos; });
        QString text = textOf(it.key());
        for (const Edit &e : v) text.replace(e.pos, e.len, e.text);
        plan.newContents.insert(it.key(), text);
    }
    plan.steps << QString("Update %1 file(s)").arg(plan.newContents.size());
    return plan;
}

} // namespace BookTools
