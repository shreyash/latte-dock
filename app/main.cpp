/*
*  Copyright 2016  Smith AR <audoban@openmailbox.org>
*                  Michail Vourlakos <mvourlakos@gmail.com>
*
*  This file is part of Latte-Dock
*
*  Latte-Dock is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License as
*  published by the Free Software Foundation; either version 2 of
*  the License, or (at your option) any later version.
*
*  Latte-Dock is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dockcorona.h"

#include <memory>

#include <QApplication>
#include <QDebug>
#include <QQuickWindow>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDebug>

#include <KLocalizedString>

//! COLORS
#define CNORMAL  "\e[0m"
#define CIGREEN  "\e[1;32m"
#define CGREEN   "\e[0;32m"
#define CICYAN   "\e[1;36m"
#define CCYAN    "\e[0;36m"
#define CIRED    "\e[1;31m"
#define CRED     "\e[0;31m"

static const char version[] = "0.1";

void noMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(type);
    Q_UNUSED(context);
    Q_UNUSED(msg);
}

int main(int argc, char **argv)
{
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("latte-dock");
    app.setApplicationVersion(version);
    app.setOrganizationDomain(QStringLiteral("latte-dock"));
    app.setApplicationName(QStringLiteral("lattedock"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("latte-dock")));
    //! set pattern for debug messages
    //! [%{type}] [%{function}:%{line}] - %{message} [%{backtrace}]
    qSetMessagePattern(QStringLiteral(
                           CIGREEN "[%{type} " CGREEN "%{time h:mm:ss.zz}" CIGREEN "]" CNORMAL
#ifndef QT_NO_DEBUG
                           CIRED " [" CCYAN "%{function}" CIRED ":" CCYAN "%{line}" CIRED "]"
#endif
                           CICYAN " - " CNORMAL "%{message}"
                           CIRED "%{if-fatal}\n%{backtrace depth=8 separator=\"\n\"}%{endif}"
                           "%{if-critical}\n%{backtrace depth=8 separator=\"\n\"}%{endif}" CNORMAL));

    //  qputenv("QT_QUICK_CONTROLS_1_STYLE", "Desktop");
    if (!app.arguments().contains(QLatin1String("--debug"))) {
        qInstallMessageHandler(noMessageOutput);
    }

    Latte::DockCorona corona;
    return app.exec();


}
