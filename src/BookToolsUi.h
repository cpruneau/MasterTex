#pragma once
// Windows for the book-wide tools: Replace in All Files, Rebase Labels, and
// the Checks pane. Changes are handed to an "applier" (MainWindow) that saves
// open files, applies the plan through the journal and refreshes everything.

#include "BookTools.h"

#include <QDialog>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

using PlanApplier = std::function<bool(const Restructure::Plan &)>;

class ReplaceAllDialog : public QDialog {
    Q_OBJECT
public:
    ReplaceAllDialog(const QString &master, PlanApplier apply, QWidget *parent = nullptr);
    void setFindText(const QString &t);

signals:
    void openLocation(const QString &file, int line);

private:
    void runSearch();
    void replaceChecked();
    QRegularExpression regex() const;

    QString m_master;
    PlanApplier m_apply;
    QLineEdit *m_find, *m_replace;
    QCheckBox *m_case, *m_word, *m_regex, *m_all;
    QTreeWidget *m_results;
    QLabel *m_status;
    QPushButton *m_replaceBtn;
    QVector<BookTools::Match> m_matches;
};

class RebaseDialog : public QDialog {
    Q_OBJECT
public:
    RebaseDialog(const QString &master, const QString &onlyFile, QWidget *parent = nullptr);
    const BookTools::RebaseResult &result() const { return m_result; }

signals:
    void openLocation(const QString &file, int line);

private:
    void recompute();
    QString m_master, m_onlyFile;
    QCheckBox *m_part, *m_prefix, *m_all, *m_onlyReview;
    QComboBox *m_depth;
    QTreeWidget *m_table;
    QLabel *m_summary;
    QPushButton *m_ok;
    BookTools::RebaseResult m_result;
};

class ChecksPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChecksPanel(QWidget *parent = nullptr);
    void setMaster(const QString &master) { m_master = master; }
    void setApplier(PlanApplier a) { m_apply = std::move(a); }

    void runCheck(BookTools::CheckKind kind);
    void runFigureFiles();
    void showRebaseReview(const QVector<BookTools::LabelChange> &changes, const QStringList &skipped);

signals:
    void openLocation(const QString &file, int line);

private:
    void fixChecked();
    QString m_master;
    PlanApplier m_apply;
    QTreeWidget *m_tree;
    QLabel *m_title;
    QCheckBox *m_commented;
    QPushButton *m_fix;
    QVector<BookTools::FigureIssue> m_figIssues;
};
