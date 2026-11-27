#include "DocumentManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextDocumentLayout>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTextDocument>

DocumentManager *DocumentManager::instance()
{
    static DocumentManager *mgr = new DocumentManager();
    return mgr;
}

DocumentManager::DocumentManager(QObject *parent) : QObject(parent)
{
    m_autosaveTimer.setInterval(60 * 1000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, &DocumentManager::autosaveAll);
    m_autosaveTimer.start();
}

QString DocumentManager::normalize(const QString &path)
{
    QFileInfo fi(path);
    QString canon = fi.canonicalFilePath();
    return canon.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : canon;
}

bool DocumentManager::readFile(const QString &path, QString *text, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QByteArray bytes = f.readAll();
    auto toUtf16 = QStringDecoder(QStringDecoder::Utf8);
    QString decoded = toUtf16(bytes);
    if (toUtf16.hasError()) {
        // Not valid UTF-8: fall back to Latin-1 so nothing is lost.
        decoded = QString::fromLatin1(bytes);
    }
    *text = decoded;
    return true;
}

bool DocumentManager::writeFile(const QString &path, const QString &text, QString *error)
{
    QSaveFile f(path);   // writes to a temp file, then atomically renames
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(text.toUtf8());
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

QTextDocument *DocumentManager::acquire(const QString &path, QString *error)
{
    const QString key = normalize(path);
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        it->refs++;
        return it->doc;
    }
    QString text;
    if (!readFile(key, &text, error))
        return nullptr;

    auto *doc = new QTextDocument(this);
    doc->setDocumentLayout(new QPlainTextDocumentLayout(doc));
    doc->setPlainText(text);
    doc->setModified(false);
    doc->clearUndoRedoStacks();

    Entry e;
    e.doc = doc;
    e.refs = 1;
    m_entries.insert(key, e);
    m_paths.insert(doc, key);

    connect(doc, &QTextDocument::contentsChanged, this, [this, doc]() {
        emit documentTextChanged(m_paths.value(doc));
    });
    return doc;
}

void DocumentManager::release(QTextDocument *doc)
{
    const QString key = m_paths.value(doc);
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (--it->refs > 0)
        return;
    if (!doc->isModified())
        discardAutosave(key);
    m_entries.erase(it);
    m_paths.remove(doc);
    doc->deleteLater();
}

QTextDocument *DocumentManager::find(const QString &path) const
{
    return m_entries.value(normalize(path)).doc;
}

QString DocumentManager::pathOf(const QTextDocument *doc) const
{
    return m_paths.value(doc);
}

int DocumentManager::refCount(const QTextDocument *doc) const
{
    return m_entries.value(m_paths.value(doc)).refs;
}

bool DocumentManager::save(QTextDocument *doc, QString *error)
{
    const QString path = m_paths.value(doc);
    if (path.isEmpty())
        return false;
    if (!writeFile(path, doc->toPlainText(), error))
        return false;
    doc->setModified(false);
    discardAutosave(path);
    emit documentSaved(path);
    return true;
}

bool DocumentManager::saveAs(QTextDocument *doc, const QString &newPath, QString *error)
{
    const QString oldKey = m_paths.value(doc);
    if (!writeFile(newPath, doc->toPlainText(), error))
        return false;
    const QString newKey = normalize(newPath);
    Entry e = m_entries.take(oldKey);
    discardAutosave(oldKey);
    m_entries.insert(newKey, e);
    m_paths.insert(doc, newKey);
    doc->setModified(false);
    emit documentRenamed(oldKey, newKey);
    emit documentSaved(newKey);
    return true;
}

QString DocumentManager::textFor(const QString &path, bool *exists) const
{
    if (QTextDocument *doc = find(path)) {
        if (exists) *exists = true;
        return doc->toPlainText();
    }
    QString text;
    bool ok = readFile(path, &text, nullptr);
    if (exists) *exists = ok;
    return ok ? text : QString();
}

QList<QTextDocument *> DocumentManager::modifiedDocuments() const
{
    QList<QTextDocument *> out;
    for (const Entry &e : m_entries)
        if (e.doc->isModified())
            out << e.doc;
    return out;
}

QString DocumentManager::autosavePath(const QString &path) const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/autosave";
    QDir().mkpath(dir);
    QByteArray hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return dir + "/" + QFileInfo(path).fileName() + "." + QString::fromLatin1(hash) + ".autosave";
}

void DocumentManager::autosaveAll()
{
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->doc->isModified())
            writeFile(autosavePath(it.key()), it->doc->toPlainText(), nullptr);
    }
}

bool DocumentManager::hasNewerAutosave(const QString &path) const
{
    QFileInfo backup(autosavePath(normalize(path)));
    QFileInfo original(path);
    return backup.exists() && (!original.exists() || backup.lastModified() > original.lastModified());
}

bool DocumentManager::recoverAutosave(QTextDocument *doc)
{
    QString text;
    if (!readFile(autosavePath(m_paths.value(doc)), &text, nullptr))
        return false;
    doc->setPlainText(text);
    doc->setModified(true);
    return true;
}

void DocumentManager::discardAutosave(const QString &path)
{
    QFile::remove(autosavePath(path));
}
