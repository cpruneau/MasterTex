#pragma once
// DocumentManager: one shared QTextDocument per file on disk.
// Every editor in every window that shows the same file uses the same
// document, so edits, undo and modified-state stay in sync everywhere.
// Saves are atomic (QSaveFile) and modified documents are autosaved to a
// private backup folder so work survives a crash.

#include <QObject>
#include <QHash>
#include <QString>
#include <QTimer>

class QTextDocument;

class DocumentManager : public QObject {
    Q_OBJECT
public:
    static DocumentManager *instance();

    static QString normalize(const QString &path);

    // Returns the shared document for a file, loading it if needed.
    // Each successful call must be balanced by release().
    QTextDocument *acquire(const QString &path, QString *error = nullptr);
    void release(QTextDocument *doc);

    QTextDocument *find(const QString &path) const;
    QString pathOf(const QTextDocument *doc) const;
    int refCount(const QTextDocument *doc) const;

    bool save(QTextDocument *doc, QString *error = nullptr);
    bool saveAs(QTextDocument *doc, const QString &newPath, QString *error = nullptr);

    // Current text of a file: the live buffer if open, otherwise the disk content.
    QString textFor(const QString &path, bool *exists = nullptr) const;

    QList<QTextDocument *> modifiedDocuments() const;

    // Crash recovery
    bool hasNewerAutosave(const QString &path) const;
    bool recoverAutosave(QTextDocument *doc);
    void discardAutosave(const QString &path);

signals:
    void documentTextChanged(const QString &path);
    void documentSaved(const QString &path);
    void documentRenamed(const QString &oldPath, const QString &newPath);

private:
    explicit DocumentManager(QObject *parent = nullptr);
    QString autosavePath(const QString &path) const;
    void autosaveAll();
    static bool readFile(const QString &path, QString *text, QString *error);
    static bool writeFile(const QString &path, const QString &text, QString *error);

    struct Entry {
        QTextDocument *doc = nullptr;
        int refs = 0;
    };
    QHash<QString, Entry> m_entries;   // normalized path -> entry
    QHash<const QTextDocument *, QString> m_paths;
    QTimer m_autosaveTimer;
};
