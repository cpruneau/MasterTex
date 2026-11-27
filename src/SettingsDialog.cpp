#include "SettingsDialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QStandardPaths>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSettings>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

static const char *kDefaultArgs = "-pdf -synctex=1 -interaction=nonstopmode -file-line-error";
#ifdef Q_OS_MACOS
static const char *kDefaultPath = "/Library/TeX/texbin:/opt/homebrew/bin:/usr/local/bin";
#else
static const char *kDefaultPath = "/usr/local/bin";
#endif

QString SettingsDialog::latexmkProgram()
{
    return QSettings().value("build/program", "latexmk").toString();
}

QStringList SettingsDialog::latexmkArguments()
{
    return QProcess::splitCommand(QSettings().value("build/args", kDefaultArgs).toString());
}

QString SettingsDialog::extraPath()
{
    return QSettings().value("build/path", kDefaultPath).toString();
}

QString SettingsDialog::findProgram(const QString &program)
{
    if (QFileInfo(program).isAbsolute()) return program;
    QString found = QStandardPaths::findExecutable(program, extraPath().split(':', Qt::SkipEmptyParts));
    if (found.isEmpty()) found = QStandardPaths::findExecutable(program);
    return found;
}

QProcessEnvironment SettingsDialog::environment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PATH", extraPath() + ":" + env.value("PATH"));
    return env;
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("MasterTex Settings"));
    auto *outer = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    outer->addLayout(form);

    m_program = new QLineEdit(latexmkProgram());
    m_args = new QLineEdit(QSettings().value("build/args", kDefaultArgs).toString());
    m_path = new QLineEdit(extraPath());
    m_fontSize = new QSpinBox;
    m_fontSize->setRange(8, 32);
    m_fontSize->setValue(QSettings().value("editor/fontSize", 13).toInt());

    form->addRow(tr("Build program:"), m_program);
    form->addRow(tr("Arguments:"), m_args);
    form->addRow(tr("Extra search path:"), m_path);
    auto *note = new QLabel(tr("Apps started from the Dock don't see your shell's PATH, so TeX's "
                               "folder is added here (MacTeX: /Library/TeX/texbin)."));
    note->setWordWrap(true);
    note->setStyleSheet("color: gray;");
    form->addRow(QString(), note);
    form->addRow(tr("Editor font size:"), m_fontSize);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this]() {
        m_program->setText("latexmk");
        m_args->setText(kDefaultArgs);
        m_path->setText(kDefaultPath);
        m_fontSize->setValue(13);
    });
    outer->addWidget(bb);
    setMinimumWidth(560);
}

void SettingsDialog::accept()
{
    QSettings s;
    s.setValue("build/program", m_program->text().trimmed());
    s.setValue("build/args", m_args->text().trimmed());
    s.setValue("build/path", m_path->text().trimmed());
    s.setValue("editor/fontSize", m_fontSize->value());
    QDialog::accept();
}
