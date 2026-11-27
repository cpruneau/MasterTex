#pragma once
// BookTools: book-wide editing and checking.
//  - bookFiles():   the .tex files of a book (everything reached from the master,
//                   commented-out chapters included), optionally all .tex in the folder.
//  - Replace in All Files: search + plan.
//  - Rebase labels:  eq:Part:Chapter:Section:…:Name built from heading titles,
//                   with every reference updated and clashes numbered and reported.
//  - Checks:        equations / tables / figures without labels, references to
//                   labels that don't exist, duplicates, labels placed before captions.
//  - Figure files:  missing or misplaced image files, with fixes.
// Every change is returned as a Restructure::Plan, so it is applied through the
// same journal and can be undone exactly.

#include "Restructure.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace BookTools {

QStringList bookFiles(const QString &master, bool includeAllTexInFolder);

// ---- Replace in All Files
struct Match {
    QString file;
    int line = 0;            // 1-based
    int start = 0, length = 0;   // offset in the whole file text
    QString lineText;
    QString replacement;     // what this match will become
};
QVector<Match> search(const QStringList &files, const QRegularExpression &re, const QString &replacement, bool useCaptures);
Restructure::Plan replacePlan(const QVector<Match> &chosen, const QString &title);

// ---- Rebase labels
struct RebaseOptions {
    bool includePart = false;
    bool normalizePrefixes = true;
    QString onlyFile;        // empty: every label in the book; else only labels defined in this file
    int deepestLevel = 3;    // deepest heading level in the path: 1 chapter, 2 section, 3 subsection, 4 subsubsection
};
struct LabelChange {
    QString oldKey, newKey;
    QString file;
    int line = 0;            // 1-based
    int refs = 0;            // references updated
    bool clash = false;      // numbered because another label got the same name
    bool duplicateOld = false;   // the old label was defined more than once
};
struct RebaseResult {
    QVector<LabelChange> changes;
    QStringList skipped;     // labels left alone, with the reason
    Restructure::Plan plan;
};
RebaseResult rebase(const QString &master, const QStringList &files, const RebaseOptions &opt);
QString titleToTag(const QString &title);

// ---- Checks
struct Finding {
    QString category;        // e.g. "Equation without a label"
    QString message;
    QString file;
    int line = 0;
    int severity = 1;        // 0 error, 1 warning, 2 info
};
enum class CheckKind { Equations, Tables, Figures };
QVector<Finding> check(const QString &master, const QStringList &files, CheckKind kind, bool includeCommented = false);

// ---- Figure files
struct FigureIssue {
    QString kind;            // "missing", "misplaced", "shared", "unused"
    QString message;
    QString file;            // .tex file using it (or the image for "unused")
    int line = 0;
    QString image;           // resolved image (if any)
    bool fixable = false;
};
QVector<FigureIssue> checkFigureFiles(const QString &master, const QStringList &files, bool includeCommented = false);
Restructure::Plan fixFigureFiles(const QString &master, const QStringList &files, const QVector<FigureIssue> &toFix);

} // namespace BookTools
