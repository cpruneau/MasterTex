#pragma once
// Restructure: reorganising a multi-file book from its structure tree.
//
//  build()    reads the book into a tree of parts/chapters/sections. An
//             \input/\include line becomes a "unit" node (movable); headings
//             written inside a file are "heading" nodes (fixed for now).
//             Commented-out includes are kept, so disabled chapters can move too.
//  makePlan() compares a rearranged tree with the original and works out every
//             change: include lines to move, files/folders to relocate,
//             headings to promote/demote, paths to rewrite, figures to carry.
//  apply()    carries the plan out, journaling every step so it can roll back
//             on failure, and undoLast() can restore the book exactly.

#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Restructure {

enum class Kind {
    Master,     // the master .tex file (root)
    Heading,    // \part, \chapter … written inside a file (not movable yet)
    Unit,       // an \input/\include of a file that starts with its own heading (movable)
    Container,  // an included file holding several top headings, or none of its own (movable)
    Plain,      // an included file without headings (preamble, macros …)
    Missing     // an include whose file does not exist
};

struct Node {
    int id = -1;
    Kind kind = Kind::Plain;
    double level = 99;      // 0 part, 1 chapter, 2 section … 5 paragraph; master -1
    QString title;
    bool commented = false; // the include line (or an enclosing one) is commented out

    QString file;           // file holding the heading / include line (absolute)
    int line = -1;          // 0-based line of the heading / include line in `file`

    // includes
    QString target;         // included file (absolute)
    QString written;        // the path as written in the source
    bool lockedLine = false; // include is not alone at the start of its line

    int parent = -1;
    QVector<int> children;
};

struct Model {
    QString root;           // folder of the master file
    QString master;
    QVector<Node> nodes;    // nodes[0] is the master
    QStringList texFiles;   // every .tex file under root (for path rewriting)
};

using Tree = QHash<int, QVector<int>>;   // node id -> ordered child ids

Model build(const QString &masterFile);
Tree originalTree(const Model &m);
bool isMovable(const Node &n);
QString kindName(double level);

struct Relocation {
    QString from;            // absolute path (file or folder), original coordinates
    QString toDir;           // destination folder, original coordinates
    QString newName;         // name inside toDir (may contain one "/" for wrapping)
    bool isDir = false;
};

struct Plan {
    QString title;           // what this change is, for the journal and Undo ("Restructure", "Rebase labels" …)
    QStringList steps;       // human-readable, in order
    QStringList errors;      // any error blocks applying
    QStringList warnings;
    QHash<QString, QString> newContents;   // original path -> new full text
    QVector<Relocation> relocations;
    bool isEmpty() const { return newContents.isEmpty() && relocations.isEmpty(); }
};

Plan makePlan(const Model &m, const Tree &tree, bool moveExclusiveFigures);

struct Result {
    bool ok = false;
    QString error;
    QString journalDir;
    QHash<QString, QString> pathMap;   // old absolute path -> new absolute path (moved items)
    QStringList changedFiles;          // final paths of files whose text changed
};

Result apply(const Model &m, const Plan &plan, bool gitCommit);

// The most recent restructure that has not been undone (empty if none).
QString lastJournal(const QString &root);
QString describeJournal(const QString &journalDir);
Result undo(const QString &journalDir);
// Files touched by that restructure that have been edited since (their edits would be lost by Undo).
QStringList editedSince(const QString &journalDir);

bool isGitRepo(const QString &root);

} // namespace Restructure
