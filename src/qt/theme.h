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
} // namespace Theme

#endif // BITCOIN_QT_THEME_H
