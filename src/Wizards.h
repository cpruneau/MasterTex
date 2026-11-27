#pragma once
// Wizards: small dialogs that generate LaTeX. Each returns the snippet to
// insert (with "%|" marking the cursor position), or an empty string if the
// user cancelled.

#include <QString>
#include <QStringList>

class QWidget;

namespace Wizards {
QString figure(QWidget *parent, const QString &projectRoot);
QString table(QWidget *parent);
QString list(QWidget *parent);
QString matrix(QWidget *parent);
QString bibEntry(QWidget *parent);

// Creates a new chapter file on disk. Returns the \include line to insert,
// and sets *createdFile to the new file's path.
QString chapterFile(QWidget *parent, const QString &projectRoot, QString *createdFile);

QString makeLabel(const QString &prefix, const QString &text);
}
