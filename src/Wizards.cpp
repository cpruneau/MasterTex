#include "Wizards.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>

namespace {

struct Dialog : QDialog {
    QFormLayout *form;
    Dialog(QWidget *parent, const QString &title) : QDialog(parent)
    {
        setWindowTitle(title);
        auto *outer = new QVBoxLayout(this);
        form = new QFormLayout;
        outer->addLayout(form);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        outer->addWidget(bb);
        setMinimumWidth(460);
    }
};

QString slug(const QString &text)
{
    QString s = text.toLower();
    s.replace(QRegularExpression("[^a-z0-9]+"), "-");
    s.remove(QRegularExpression("^-+|-+$"));
    return s.left(40);
}

} // namespace

QString Wizards::makeLabel(const QString &prefix, const QString &text)
{
    const QString s = slug(text);
    return s.isEmpty() ? QString() : prefix + ":" + s;
}

QString Wizards::figure(QWidget *parent, const QString &projectRoot)
{
    Dialog dlg(parent, QObject::tr("Figure Wizard"));
    auto *file = new QLineEdit;
    auto *browse = new QPushButton(QObject::tr("Browse…"));
    auto *row = new QHBoxLayout;
    row->addWidget(file);
    row->addWidget(browse);
    auto *caption = new QLineEdit;
    auto *label = new QLineEdit;
    auto *width = new QDoubleSpinBox;
    width->setRange(0.1, 1.0);
    width->setSingleStep(0.05);
    width->setValue(0.8);
    auto *placement = new QLineEdit("htbp");
    auto *center = new QCheckBox(QObject::tr("Center"));
    center->setChecked(true);

    dlg.form->addRow(QObject::tr("Image file:"), row);
    dlg.form->addRow(QObject::tr("Caption:"), caption);
    dlg.form->addRow(QObject::tr("Label:"), label);
    dlg.form->addRow(QObject::tr("Width (× \\linewidth):"), width);
    dlg.form->addRow(QObject::tr("Placement:"), placement);
    dlg.form->addRow(QString(), center);

    QObject::connect(browse, &QPushButton::clicked, &dlg, [&]() {
        QString f = QFileDialog::getOpenFileName(&dlg, QObject::tr("Choose image"), projectRoot,
                                                 QObject::tr("Images (*.pdf *.png *.jpg *.jpeg *.eps *.svg)"));
        if (f.isEmpty()) return;
        QFileInfo fi(QDir(projectRoot).relativeFilePath(f));
        // LaTeX finds the extension itself; leave it off so the source can change format.
        file->setText(fi.path() == "." ? fi.completeBaseName() : fi.path() + "/" + fi.completeBaseName());
        if (label->text().isEmpty()) label->setText(makeLabel("fig", fi.completeBaseName()));
    });
    QObject::connect(caption, &QLineEdit::textEdited, &dlg, [&](const QString &t) {
        if (label->isModified()) return;
        if (file->text().isEmpty()) label->setText(makeLabel("fig", t));
    });

    if (dlg.exec() != QDialog::Accepted) return {};
    QString s = QString("\\begin{figure}[%1]\n").arg(placement->text().trimmed());
    if (center->isChecked()) s += "  \\centering\n";
    s += QString("  \\includegraphics[width=%1\\linewidth]{%2}\n").arg(width->value()).arg(file->text().trimmed());
    s += QString("  \\caption{%1%|}\n").arg(caption->text());
    if (!label->text().trimmed().isEmpty()) s += QString("  \\label{%1}\n").arg(label->text().trimmed());
    s += "\\end{figure}\n";
    return s;
}

QString Wizards::table(QWidget *parent)
{
    Dialog dlg(parent, QObject::tr("Table Wizard"));
    auto *rows = new QSpinBox; rows->setRange(1, 100); rows->setValue(3);
    auto *cols = new QSpinBox; cols->setRange(1, 30); cols->setValue(3);
    auto *align = new QLineEdit;
    auto *caption = new QLineEdit;
    auto *label = new QLineEdit;
    auto *header = new QCheckBox(QObject::tr("Header row"));
    header->setChecked(true);
    auto *booktabs = new QCheckBox(QObject::tr("Use booktabs rules (\\toprule, \\midrule, \\bottomrule)"));
    booktabs->setChecked(true);
    dlg.form->addRow(QObject::tr("Rows:"), rows);
    dlg.form->addRow(QObject::tr("Columns:"), cols);
    dlg.form->addRow(QObject::tr("Alignment (e.g. lcr):"), align);
    dlg.form->addRow(QObject::tr("Caption:"), caption);
    dlg.form->addRow(QObject::tr("Label:"), label);
    dlg.form->addRow(QString(), header);
    dlg.form->addRow(QString(), booktabs);
    QObject::connect(caption, &QLineEdit::textEdited, &dlg, [&](const QString &t) {
        if (!label->isModified()) label->setText(makeLabel("tab", t));
    });
    if (dlg.exec() != QDialog::Accepted) return {};

    const int nc = cols->value();
    QString a = align->text().trimmed();
    if (a.isEmpty()) a = QString(nc, 'c');
    const bool bt = booktabs->isChecked();
    QStringList cells;
    for (int c = 0; c < nc; ++c) cells << QString();
    const QString emptyRow = "    " + cells.join(" & ") + " \\\\\n";

    QString s = "\\begin{table}[htbp]\n  \\centering\n";
    s += QString("  \\caption{%1}\n").arg(caption->text());
    if (!label->text().trimmed().isEmpty()) s += QString("  \\label{%1}\n").arg(label->text().trimmed());
    s += QString("  \\begin{tabular}{%1}\n").arg(a);
    s += bt ? "    \\toprule\n" : "    \\hline\n";
    int r = rows->value();
    bool cursorPlaced = false;
    auto rowText = [&]() {
        if (!cursorPlaced) { cursorPlaced = true; return QString("    %|") + QString(" &").repeated(nc - 1) + " \\\\\n"; }
        return emptyRow;
    };
    if (header->isChecked()) {
        s += rowText();
        s += bt ? "    \\midrule\n" : "    \\hline\n";
        --r;
    }
    for (int i = 0; i < r; ++i) s += rowText();
    s += bt ? "    \\bottomrule\n" : "    \\hline\n";
    s += "  \\end{tabular}\n\\end{table}\n";
    return s;
}

QString Wizards::list(QWidget *parent)
{
    Dialog dlg(parent, QObject::tr("List Wizard"));
    auto *type = new QComboBox;
    type->addItems({"itemize", "enumerate", "description"});
    auto *n = new QSpinBox; n->setRange(1, 50); n->setValue(3);
    dlg.form->addRow(QObject::tr("Type:"), type);
    dlg.form->addRow(QObject::tr("Items:"), n);
    if (dlg.exec() != QDialog::Accepted) return {};
    const QString env = type->currentText();
    const QString item = env == "description" ? "  \\item[] " : "  \\item ";
    QString s = QString("\\begin{%1}\n").arg(env);
    for (int i = 0; i < n->value(); ++i) s += item + (i == 0 ? "%|" : "") + "\n";
    s += QString("\\end{%1}\n").arg(env);
    return s;
}

QString Wizards::matrix(QWidget *parent)
{
    Dialog dlg(parent, QObject::tr("Matrix Wizard"));
    auto *type = new QComboBox;
    type->addItem("( ) pmatrix", "pmatrix");
    type->addItem("[ ] bmatrix", "bmatrix");
    type->addItem("| | vmatrix", "vmatrix");
    type->addItem("‖ ‖ Vmatrix", "Vmatrix");
    type->addItem("{ } Bmatrix", "Bmatrix");
    type->addItem("plain matrix", "matrix");
    auto *rows = new QSpinBox; rows->setRange(1, 20); rows->setValue(2);
    auto *cols = new QSpinBox; cols->setRange(1, 20); cols->setValue(2);
    dlg.form->addRow(QObject::tr("Type:"), type);
    dlg.form->addRow(QObject::tr("Rows:"), rows);
    dlg.form->addRow(QObject::tr("Columns:"), cols);
    if (dlg.exec() != QDialog::Accepted) return {};
    const QString env = type->currentData().toString();
    QString s = QString("\\begin{%1}\n").arg(env);
    for (int r = 0; r < rows->value(); ++r) {
        QStringList cells;
        for (int c = 0; c < cols->value(); ++c) cells << (r == 0 && c == 0 ? "%|" : "");
        s += "  " + cells.join(" & ") + (r + 1 < rows->value() ? " \\\\\n" : "\n");
    }
    s += QString("\\end{%1}").arg(env);
    return s;
}

QString Wizards::bibEntry(QWidget *parent)
{
    Dialog dlg(parent, QObject::tr("New Bibliography Entry"));
    auto *type = new QComboBox;
    type->addItems({"article", "book", "inproceedings", "incollection", "phdthesis", "techreport", "misc"});
    auto *key = new QLineEdit;
    auto *author = new QLineEdit;
    auto *title = new QLineEdit;
    auto *year = new QLineEdit;
    auto *venue = new QLineEdit;
    auto *doi = new QLineEdit;
    dlg.form->addRow(QObject::tr("Type:"), type);
    dlg.form->addRow(QObject::tr("Key:"), key);
    dlg.form->addRow(QObject::tr("Author(s):"), author);
    dlg.form->addRow(QObject::tr("Title:"), title);
    dlg.form->addRow(QObject::tr("Year:"), year);
    dlg.form->addRow(QObject::tr("Journal / Publisher / Booktitle:"), venue);
    dlg.form->addRow(QObject::tr("DOI:"), doi);
    auto suggest = [&]() {
        if (key->isModified()) return;
        QString last = author->text().split(QRegularExpression("\\s+and\\s+|,")).value(0).trimmed().split(' ').last();
        QString word = title->text().split(' ', Qt::SkipEmptyParts).value(0);
        key->setText(slug(last) + year->text().trimmed() + slug(word));
    };
    QObject::connect(author, &QLineEdit::textEdited, &dlg, suggest);
    QObject::connect(year, &QLineEdit::textEdited, &dlg, suggest);
    QObject::connect(title, &QLineEdit::textEdited, &dlg, suggest);
    if (dlg.exec() != QDialog::Accepted) return {};

    const QString t = type->currentText();
    const QString venueField = t == "article" ? "journal"
                             : (t == "book" || t == "techreport") ? (t == "book" ? "publisher" : "institution")
                             : (t == "phdthesis") ? "school"
                             : (t == "misc") ? "howpublished" : "booktitle";
    QString s = QString("@%1{%2,\n").arg(t, key->text().trimmed());
    s += QString("  author = {%1},\n").arg(author->text());
    s += QString("  title = {%1},\n").arg(title->text());
    s += QString("  %1 = {%2},\n").arg(venueField, venue->text());
    s += QString("  year = {%1},\n").arg(year->text());
    if (!doi->text().trimmed().isEmpty()) s += QString("  doi = {%1},\n").arg(doi->text().trimmed());
    s += "}\n";
    return s;
}

QString Wizards::chapterFile(QWidget *parent, const QString &projectRoot, QString *createdFile)
{
    Dialog dlg(parent, QObject::tr("New Chapter File"));
    auto *title = new QLineEdit;
    auto *folder = new QLineEdit("chapters");
    auto *name = new QLineEdit;
    auto *figDir = new QCheckBox(QObject::tr("Create a figures folder for this chapter"));
    figDir->setChecked(true);
    auto *useInclude = new QComboBox;
    useInclude->addItems({"\\include", "\\input"});
    dlg.form->addRow(QObject::tr("Chapter title:"), title);
    dlg.form->addRow(QObject::tr("Folder (relative to project):"), folder);
    dlg.form->addRow(QObject::tr("File name:"), name);
    dlg.form->addRow(QObject::tr("Insert with:"), useInclude);
    dlg.form->addRow(QString(), figDir);
    QObject::connect(title, &QLineEdit::textEdited, &dlg, [&](const QString &t) {
        if (!name->isModified()) name->setText(slug(t));
    });
    if (dlg.exec() != QDialog::Accepted) return {};

    QString base = name->text().trimmed();
    if (base.endsWith(".tex")) base.chop(4);
    if (base.isEmpty()) return {};
    const QString rel = QDir::cleanPath(folder->text().trimmed() + "/" + base);
    const QString abs = QDir(projectRoot).absoluteFilePath(rel + ".tex");
    if (QFileInfo::exists(abs)) {
        QMessageBox::warning(parent, QObject::tr("New Chapter File"), QObject::tr("%1 already exists.").arg(abs));
        return {};
    }
    QDir().mkpath(QFileInfo(abs).absolutePath());
    if (figDir->isChecked())
        QDir().mkpath(QFileInfo(abs).absolutePath() + "/figures/" + base);

    QFile f(abs);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(parent, QObject::tr("New Chapter File"), f.errorString());
        return {};
    }
    QString body = QString("% %1\n\\chapter{%2}\n\\label{%3}\n\n").arg(rel + ".tex", title->text(), makeLabel("ch", title->text()));
    f.write(body.toUtf8());
    f.close();
    if (createdFile) *createdFile = abs;
    return QString("%1{%2}\n%|").arg(useInclude->currentText(), rel);
}
