#pragma once
// Assorted dialogs: Preferences, document class and package pickers
// (from the TeX installation), Unicode character, Lorem ipsum.
// Each insert-style function returns the text to insert, or an empty string
// if the user cancelled.

#include <QDialog>
#include <QString>
#include <QStringList>

class QWidget;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);
};

namespace Dialogs {
QString documentClass(QWidget *parent);
QString usePackage(QWidget *parent);
QString unicodeCharacter(QWidget *parent);
QString loremIpsum(QWidget *parent);

// Names of installed .cls or .sty files (without extension), cached per session.
QStringList installedTexFiles(const QString &extension);
}
