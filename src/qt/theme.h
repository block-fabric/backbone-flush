// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_THEME_H
#define BITCOIN_QT_THEME_H

#include <QList>
#include <QString>

/** Colour themes of the GUI. */
namespace Theme {
struct Info {
    //! Name the theme is saved under in the settings.
    QString id;
    //! Name shown to the user.
    QString name;
};

/** The themes, starting with the one that follows the desktop. */
QList<Info> Available();

/** The theme saved in the settings. */
QString Saved();

/** Save `id` as the theme to use and apply it. */
void Save(const QString& id);

/** Give the application the look of the theme `id`; an unknown one means the look of the desktop. */
void Apply(const QString& id);
inline constexpr int MIN_FONT_SIZE_ADJUSTMENT{-4};
inline constexpr int MAX_FONT_SIZE_ADJUSTMENT{12};

/** The font family saved in the settings; empty for the one of the desktop. */
QString SavedFontFamily();
/** How many points larger (or smaller, if negative) than on the desktop the text is. */
int SavedFontSizeAdjustment();
/** Save the font settings and apply them. */
void SaveFont(const QString& family, int size_adjustment);
/** Give the application the font of the settings. */
void ApplyFont();
} // namespace Theme

#endif // BITCOIN_QT_THEME_H
