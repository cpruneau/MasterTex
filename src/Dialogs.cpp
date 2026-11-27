#include "Dialogs.h"
#include "SettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFormLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

// ---------------------------------------------------------------------------

PreferencesDialog::PreferencesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    auto *lay = new QVBoxLayout(this);
    lay->addStretch();   // line endings, encoding … will go here
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    resize(480, 320);
}

// ---------------------------------------------------------------------------
// Installed classes and packages

QStringList Dialogs::installedTexFiles(const QString &extension)
{
    static QHash<QString, QStringList> cache;
    if (cache.contains(extension)) return cache.value(extension);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList roots;
    const QString kpse = SettingsDialog::findProgram("kpsewhich");
    if (!kpse.isEmpty()) {
        for (const char *var : {"TEXMFDIST", "TEXMFLOCAL", "TEXMFHOME"}) {
            QProcess p;
            p.setProcessEnvironment(SettingsDialog::environment());
            p.start(kpse, {QString("-var-value=%1").arg(var)});
            if (!p.waitForFinished(5000)) continue;
            const QString root = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
            if (!root.isEmpty() && QDir(root + "/tex/latex").exists()) roots << root + "/tex/latex";
        }
    }
    QSet<QString> names;
    for (const QString &root : roots) {
        QDirIterator it(root, {"*." + extension}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            names.insert(it.fileInfo().completeBaseName());
        }
    }
    QApplication::restoreOverrideCursor();

    QStringList list(names.begin(), names.end());
    if (list.isEmpty()) {
        // No TeX installation found: offer the common ones.
        list = extension == "cls"
            ? QStringList{"article", "book", "report", "letter", "memoir", "scrbook", "scrartcl", "scrreprt",
                          "amsart", "amsbook", "beamer", "revtex4-2", "elsarticle", "standalone"}
            : QStringList{"amsmath", "amssymb", "amsthm", "graphicx", "hyperref", "cleveref", "booktabs",
                          "siunitx", "physics", "tikz", "xcolor", "geometry", "biblatex", "natbib",
                          "caption", "subcaption", "float", "multirow", "longtable", "fontenc", "inputenc",
                          "microtype", "enumitem", "makeidx", "glossaries", "slashed", "feynmp-auto"};
    }
    std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    cache.insert(extension, list);
    return list;
}

namespace {

// A filterable list dialog with an extra options area.
struct PickerDialog : QDialog {
    QLineEdit *filter;
    QListWidget *list;
    QFormLayout *options;
    QLabel *count;

    PickerDialog(QWidget *parent, const QString &title, const QStringList &items, bool multi) : QDialog(parent)
    {
        setWindowTitle(title);
        auto *lay = new QVBoxLayout(this);
        filter = new QLineEdit;
        filter->setPlaceholderText(tr("Type to filter…"));
        filter->setClearButtonEnabled(true);
        list = new QListWidget;
        list->addItems(items);
        list->setSelectionMode(multi ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
        count = new QLabel;
        options = new QFormLayout;
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        lay->addWidget(filter);
        lay->addWidget(list, 1);
        lay->addWidget(count);
        lay->addLayout(options);
        lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
        connect(filter, &QLineEdit::textChanged, this, [this](const QString &t) {
            int shown = 0;
            for (int i = 0; i < list->count(); ++i) {
                const bool hide = !list->item(i)->text().contains(t, Qt::CaseInsensitive);
                list->item(i)->setHidden(hide);
                if (!hide) ++shown;
            }
            count->setText(tr("%1 of %2 shown").arg(shown).arg(list->count()));
        });
        connect(filter, &QLineEdit::returnPressed, this, [this]() {
            for (int i = 0; i < list->count(); ++i)
                if (!list->item(i)->isHidden()) { list->setCurrentRow(i); break; }
        });
        count->setText(tr("%1 available").arg(items.size()));
        resize(460, 560);
    }

    void select(const QString &name)
    {
        auto found = list->findItems(name, Qt::MatchExactly);
        if (!found.isEmpty()) {
            list->setCurrentItem(found.first());
            list->scrollToItem(found.first(), QAbstractItemView::PositionAtCenter);
        }
    }

    QStringList chosen() const
    {
        QStringList out;
        for (QListWidgetItem *it : list->selectedItems()) out << it->text();
        return out;
    }
};

} // namespace

QString Dialogs::documentClass(QWidget *parent)
{
    PickerDialog dlg(parent, QObject::tr("\\documentclass — installed classes"), installedTexFiles("cls"), false);
    auto *size = new QComboBox;
    size->addItems({"(default)", "10pt", "11pt", "12pt"});
    auto *paper = new QComboBox;
    paper->addItems({"(default)", "letterpaper", "a4paper", "b5paper"});
    auto *twoside = new QCheckBox("twoside");
    auto *openany = new QCheckBox("openany");
    auto *draft = new QCheckBox("draft");
    auto *flags = new QHBoxLayout;
    flags->addWidget(twoside);
    flags->addWidget(openany);
    flags->addWidget(draft);
    flags->addStretch();
    dlg.options->addRow(QObject::tr("Font size:"), size);
    dlg.options->addRow(QObject::tr("Paper:"), paper);
    dlg.options->addRow(QObject::tr("Options:"), flags);
    dlg.select("book");
    if (dlg.exec() != QDialog::Accepted || dlg.chosen().isEmpty()) return {};

    QStringList opts;
    if (size->currentIndex() > 0) opts << size->currentText();
    if (paper->currentIndex() > 0) opts << paper->currentText();
    for (QCheckBox *c : {twoside, openany, draft})
        if (c->isChecked()) opts << c->text();
    return QString("\\documentclass%1{%2}\n%|")
        .arg(opts.isEmpty() ? QString() : "[" + opts.join(",") + "]", dlg.chosen().first());
}

QString Dialogs::usePackage(QWidget *parent)
{
    PickerDialog dlg(parent, QObject::tr("\\usepackage — installed packages"), installedTexFiles("sty"), true);
    auto *opts = new QLineEdit;
    opts->setPlaceholderText(QObject::tr("optional, e.g. margin=1in"));
    dlg.options->addRow(QObject::tr("Package options:"), opts);
    auto *hint = new QLabel(QObject::tr("Cmd-click to choose several packages."));
    hint->setStyleSheet("color: gray;");
    dlg.options->addRow(QString(), hint);
    if (dlg.exec() != QDialog::Accepted || dlg.chosen().isEmpty()) return {};
    const QString o = opts->text().trimmed().isEmpty() ? QString() : "[" + opts->text().trimmed() + "]";
    QString out;
    for (const QString &p : dlg.chosen()) out += QString("\\usepackage%1{%2}\n").arg(o, p);
    return out + "%|";
}

QString Dialogs::unicodeCharacter(QWidget *parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Insert Unicode Character"));
    auto *lay = new QFormLayout(&dlg);
    auto *code = new QLineEdit;
    code->setPlaceholderText(QObject::tr("Code point, e.g. 03B1 or U+212B"));
    auto *preview = new QLabel;
    QFont f = preview->font();
    f.setPointSize(f.pointSize() * 3);
    preview->setFont(f);
    preview->setMinimumHeight(60);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addRow(QObject::tr("Code point:"), code);
    lay->addRow(QObject::tr("Preview:"), preview);
    lay->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto parse = [code]() -> QString {
        QString t = code->text().trimmed();
        if (t.startsWith("U+", Qt::CaseInsensitive) || t.startsWith("0x", Qt::CaseInsensitive)) t = t.mid(2);
        bool ok = false;
        const uint cp = t.toUInt(&ok, 16);
        if (!ok || cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return {};
        const char32_t c = cp;
        return QString::fromUcs4(&c, 1);
    };
    QObject::connect(code, &QLineEdit::textChanged, &dlg, [preview, parse]() { preview->setText(parse()); });
    if (dlg.exec() != QDialog::Accepted) return {};
    return parse();
}

QString Dialogs::loremIpsum(QWidget *parent)
{
    static const QStringList paragraphs = {
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et "
        "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip "
        "ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu "
        "fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt "
        "mollit anim id est laborum.",
        "Curabitur pretium tincidunt lacus. Nulla gravida orci a odio. Nullam varius, turpis et commodo pharetra, "
        "est eros bibendum elit, nec luctus magna felis sollicitudin mauris. Integer in mauris eu nibh euismod "
        "gravida. Duis ac tellus et risus vulputate vehicula. Donec lobortis risus a elit. Etiam tempor.",
        "Ut ullamcorper, ligula eu tempor congue, eros est euismod turpis, id tincidunt sapien risus a quam. "
        "Maecenas fermentum consequat mi. Donec fermentum. Pellentesque malesuada nulla a mi. Duis sapien sem, "
        "aliquet nec, commodo eget, consequat quis, neque. Aliquam faucibus, elit ut dictum aliquet, felis nisl "
        "adipiscing sapien, sed malesuada diam lacus eget erat.",
        "Cras mollis scelerisque nunc. Nullam arcu. Aliquam consequat. Curabitur augue lorem, dapibus quis, "
        "laoreet et, pretium ac, nisi. Aenean magna nisl, mollis quis, molestie eu, feugiat in, orci. In hac "
        "habitasse platea dictumst. Fusce convallis, mauris imperdiet gravida bibendum, nisl turpis suscipit "
        "mauris, sed placerat ipsum urna sed risus.",
        "Phasellus viverra nulla ut metus varius laoreet. Quisque rutrum. Aenean imperdiet. Etiam ultricies nisi "
        "vel augue. Curabitur ullamcorper ultricies nisi. Nam eget dui. Etiam rhoncus. Maecenas tempus, tellus "
        "eget condimentum rhoncus, sem quam semper libero, sit amet adipiscing sem neque sed ipsum.",
    };
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Generate Random Text"));
    auto *lay = new QFormLayout(&dlg);
    auto *n = new QSpinBox;
    n->setRange(1, 50);
    n->setValue(3);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addRow(QObject::tr("Paragraphs:"), n);
    lay->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return {};
    QStringList out;
    for (int i = 0; i < n->value(); ++i) out << paragraphs[i % paragraphs.size()];
    return out.join("\n\n") + "\n%|";
}
