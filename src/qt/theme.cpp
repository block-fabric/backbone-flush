// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/theme.h>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QObject>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <optional>

namespace Theme {
namespace {
const char* const SETTING{"Theme"};
const char* const SYSTEM{"system"};

/** The colours a theme is made of. The rest of the palette is derived from them. */
struct Colors {
    const char* id;
    const char* name;
    //! Background of windows, and the text on it.
    QColor window, window_text;
    //! Background of lists and text fields, of every other row of a list, and the text on them.
    QColor base, alternate_base, text;
    QColor button, button_text;
    //! Selected items and other accents, and the text on them.
    QColor highlight, highlighted_text;
    QColor link;
    //! Resource with a complete style sheet, for a theme that restyles every kind of widget.
    const char* style_sheet{nullptr};
};

const Colors THEMES[]{
    {"light", QT_TRANSLATE_NOOP("Theme", "Light"),
     "#f3f4f6", "#1b1f24", "#ffffff", "#f0f2f5", "#1b1f24", "#e6e8ec", "#1b1f24", "#2f6fde", "#ffffff", "#1a5fd0"},
    {"dark", QT_TRANSLATE_NOOP("Theme", "Dark"),
     "#2b2d31", "#e6e6e6", "#1f2023", "#27292d", "#e6e6e6", "#3a3d42", "#e6e6e6", "#3d8bfd", "#ffffff", "#6aa9ff"},
    // The dark theme of the Drivechain and Skydoge wallets, based on QDarkStyleSheet.
    {"qdarkstyle", QT_TRANSLATE_NOOP("Theme", "Dark (classic Drivechain)"),
     "#19232d", "#f0f0f0", "#19232d", "#1c2a36", "#f0f0f0", "#32414b", "#f0f0f0", "#1464a0", "#f0f0f0", "#148cd2", ":/themes/qdarkstyle"},
    {"midnight", QT_TRANSLATE_NOOP("Theme", "Midnight Blue"),
     "#101a2c", "#dbe6f7", "#0a1220", "#0f192b", "#dbe6f7", "#1b2a45", "#dbe6f7", "#22b8cf", "#04121a", "#5cd6e8"},
    {"gold", QT_TRANSLATE_NOOP("Theme", "Chains Gold"),
     "#1c1a17", "#ece4d6", "#131210", "#1a1815", "#ece4d6", "#2c2823", "#ece4d6", "#e0a526", "#1a1300", "#f2c14e"},
    {"forest", QT_TRANSLATE_NOOP("Theme", "Forest"),
     "#16211b", "#dcebe0", "#0f1813", "#141f19", "#dcebe0", "#22342a", "#dcebe0", "#3fb873", "#04160b", "#7bd9a2"},
    {"solarized-dark", QT_TRANSLATE_NOOP("Theme", "Solarized Dark"),
     "#073642", "#93a1a1", "#002b36", "#04313d", "#a7b4b4", "#0d4452", "#a7b4b4", "#268bd2", "#fdf6e3", "#2aa198"},
    {"solarized-light", QT_TRANSLATE_NOOP("Theme", "Solarized Light"),
     "#eee8d5", "#586e75", "#fdf6e3", "#f5efdc", "#4a5f66", "#e4ddc8", "#4a5f66", "#268bd2", "#fdf6e3", "#2176b4"},
    {"rose", QT_TRANSLATE_NOOP("Theme", "Rose"),
     "#fbf1f3", "#3a2228", "#ffffff", "#f8e9ec", "#3a2228", "#f1dde2", "#3a2228", "#c2416b", "#ffffff", "#a8325a"},
};

const Colors* Find(const QString& id)
{
    for (const Colors& theme : THEMES) {
        if (id == theme.id) return &theme;
    }
    return nullptr;
}

/** A colour between two others. */
QColor Mix(const QColor& a, const QColor& b, double share_of_b)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * share_of_b,
                            a.greenF() + (b.greenF() - a.greenF()) * share_of_b,
                            a.blueF() + (b.blueF() - a.blueF()) * share_of_b);
}

QPalette MakePalette(const Colors& c)
{
    QPalette palette;
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::WindowText, c.window_text);
    palette.setColor(QPalette::Base, c.base);
    palette.setColor(QPalette::AlternateBase, c.alternate_base);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.button);
    palette.setColor(QPalette::ButtonText, c.button_text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Highlight, c.highlight);
    palette.setColor(QPalette::HighlightedText, c.highlighted_text);
    palette.setColor(QPalette::Link, c.link);
    palette.setColor(QPalette::LinkVisited, Mix(c.link, c.window_text, 0.35));
    palette.setColor(QPalette::ToolTipBase, c.button);
    palette.setColor(QPalette::ToolTipText, c.button_text);
    palette.setColor(QPalette::PlaceholderText, Mix(c.text, c.base, 0.55));
    // The shades that frames and bevels are drawn with.
    palette.setColor(QPalette::Light, Mix(c.button, c.window_text, 0.18));
    palette.setColor(QPalette::Midlight, Mix(c.button, c.window_text, 0.09));
    palette.setColor(QPalette::Mid, Mix(c.window, c.window_text, 0.22));
    palette.setColor(QPalette::Dark, Mix(c.window, c.window_text, 0.32));
    palette.setColor(QPalette::Shadow, Mix(c.window, Qt::black, 0.6));

    const QColor disabled{Mix(c.window_text, c.window, 0.55)};
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, disabled);
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, Mix(c.highlight, c.window, 0.6));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return palette;
}

QString MakeStyleSheet(const Colors& c)
{
    return QStringLiteral(
               "QToolTip { color: %1; background-color: %2; border: 1px solid %3; }"
               "QToolBar { border: none; }"
               "QToolBar#sidebar { border-right: 2px solid %3; background-color: %2; spacing: 2px; padding: 4px 0px; }"
               "QToolBar#sidebar QToolButton { padding: 6px 8px; border-radius: 4px; text-align: left; }"
               "QToolBar QToolButton:checked { background-color: %3; color: %4; border-radius: 4px; }"
               "QGroupBox { font-weight: bold; }"
               "QHeaderView::section { background-color: %2; color: %1; padding: 4px; border: none; border-bottom: 1px solid %3; }")
        .arg(c.button_text.name(), c.button.name(), c.highlight.name(), c.highlighted_text.name());
}

/** The look the application had before a theme was applied. */
struct Original {
    QPalette palette;
    QString style;
};
std::optional<Original> g_original;
} // namespace

QList<Info> Available()
{
    QList<Info> themes{{SYSTEM, QObject::tr("Same as the desktop")}};
    for (const Colors& theme : THEMES) themes.push_back({theme.id, QApplication::translate("Theme", theme.name)});
    return themes;
}

QString Saved()
{
    const QString id{QSettings().value(SETTING, SYSTEM).toString()};
    return Find(id) ? id : QString{SYSTEM};
}

void Save(const QString& id)
{
    QSettings().setValue(SETTING, id);
    Apply(id);
}

void Apply(const QString& id)
{
    const Colors* theme{Find(id)};
    if (!g_original) {
        if (!theme) return;
        g_original = Original{QApplication::palette(), QApplication::style()->objectName()};
    }
    if (theme) {
        // The Fusion style draws everything with the palette, which the styles of some desktops do not.
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(MakePalette(*theme));
        QString style_sheet{MakeStyleSheet(*theme)};
        if (theme->style_sheet) {
            QFile file{theme->style_sheet};
            if (file.open(QFile::ReadOnly | QFile::Text)) style_sheet = QString::fromUtf8(file.readAll());
        }
        qApp->setStyleSheet(style_sheet);
    } else {
        qApp->setStyleSheet({});
        QApplication::setStyle(QStyleFactory::create(g_original->style));
        QApplication::setPalette(g_original->palette);
    }
}

} // namespace Theme
