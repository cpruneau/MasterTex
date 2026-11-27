#pragma once
#include <QDialog>
#include <QProcessEnvironment>

class QLineEdit;
class QSpinBox;

// Options > Settings: compiler command and editor preferences (stored in QSettings).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    void accept() override;

    static QString latexmkProgram();
    static QStringList latexmkArguments();
    static QString extraPath();
    // Finds a TeX program, looking in the extra search path first.
    static QString findProgram(const QString &program);
    // Environment for running TeX tools, with the extra search path added.
    static QProcessEnvironment environment();

private:
    QLineEdit *m_program;
    QLineEdit *m_args;
    QLineEdit *m_path;
    QSpinBox *m_fontSize;
};
