// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2017 Intel Corporation.
// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Qt 6.8.3 qtbase/src/corelib/io/qprocess_win.cpp, commit
// c07c2d5a527a644d36e7853d55132ae38921682f. Original licensing offers
// LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only;
// this copy uses the GPL-3.0-only option. See README.md and LICENSE.txt.
#pragma once
#include <QString>
#include <QStringList>

namespace gsw::qt683 {
// Changes: namespace/name; raw nativeArguments intentionally omitted. The
// argument escaping algorithm is unchanged. No shell is involved.
inline QString createCommandLine(const QString &program, const QStringList &arguments)
{
    QString args;
    if (!program.isEmpty()) {
        QString programName = program;
        if (!programName.startsWith(u'\"') && !programName.endsWith(u'\"') && programName.contains(u' '))
            programName = u'\"' + programName + u'\"';
        programName.replace(u'/', u'\\');

        // add the program as the first arg ... it works better
        args = programName + u' ';
    }

    for (qsizetype i = 0; i < arguments.size(); ++i) {
        QString tmp = arguments.at(i);
        // Quotes are escaped and their preceding backslashes are doubled.
        qsizetype index = tmp.indexOf(u'"');
        while (index >= 0) {
            // Escape quote
            tmp.insert(index++, u'\\');
            // Double preceding backslashes (ignoring the one we just inserted)
            for (qsizetype i = index - 2 ; i >= 0 && tmp.at(i) == u'\\' ; --i) {
                tmp.insert(i, u'\\');
                index++;
            }
            index = tmp.indexOf(u'"', index + 1);
        }
        if (tmp.isEmpty() || tmp.contains(u' ') || tmp.contains(u'\t')) {
            // The argument must not end with a \ since this would be interpreted
            // as escaping the quote -- rather put the \ behind the quote: e.g.
            // rather use "foo"\ than "foo\"
            qsizetype i = tmp.length();
            while (i > 0 && tmp.at(i - 1) == u'\\')
                --i;
            tmp.insert(i, u'"');
            tmp.prepend(u'"');
        }
        args += u' ' + tmp;
    }
    return args;
}
} // namespace gsw::qt683
