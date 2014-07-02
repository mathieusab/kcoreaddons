/******************************************************************************
 *  Copyright 2013 Sebastian Kügler <sebas@kde.org>                           *
 *  Copyright 2014 Alex Richardson <arichardson.kde@gmail.com>                *
 *                                                                            *
 *  This library is free software; you can redistribute it and/or             *
 *  modify it under the terms of the GNU Lesser General Public                *
 *                                                                            *
 *  License as published by the Free Software Foundation; either              *
 *  version 2.1 of the License, or (at your option) version 3, or any         *
 *  later version accepted by the membership of KDE e.V. (or its              *
 *  successor approved by the membership of KDE e.V.), which shall            *
 *  act as a proxy defined in Section 6 of version 3 of the license.          *
 *                                                                            *
 *  This library is distributed in the hope that it will be useful,           *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU         *
 *  Lesser General Public License for more details.                           *
 *                                                                            *
 *  You should have received a copy of the GNU Lesser General Public          *
 *  License along with this library.  If not, see                             *
 *  <http://www.gnu.org/licenses/>.                                           *
 *                                                                            *
 ******************************************************************************/

#include "desktoptojson.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>
#include <QJsonArray>

static QTextStream cout(stdout);
static QTextStream cerr(stderr);

DesktopToJson::DesktopToJson(QCommandLineParser *parser, const QCommandLineOption &i, const QCommandLineOption &o)
    : m_parser(parser),
      input(i),
      output(o)
{
}

int DesktopToJson::runMain()
{
    if (!m_parser->isSet(input)) {
        cout << "Usage --help. In short: desktoptojson -i inputfile.desktop -o outputfile.json" << endl;
        return 1;
    }

    if (!resolveFiles()) {
        cerr << "Failed to resolve filenames" << m_inFile << m_outFile << endl;
        return 1;
    }

    return convert(m_inFile, m_outFile) ? 0 : 1;
}

bool DesktopToJson::resolveFiles()
{
    if (m_parser->isSet(input)) {
        m_inFile = m_parser->value(input);
        const QFileInfo fi(m_inFile);
        if (!fi.exists()) {
            cerr << "File not found: " << m_inFile << endl;
            return false;
        }
        if (!fi.isAbsolute()) {
            m_inFile = fi.absoluteFilePath();
        }
    }

    if (m_parser->isSet(output)) {
        m_outFile = m_parser->value(output);
    } else if (!m_inFile.isEmpty()) {
        m_outFile = m_inFile;
        m_outFile.replace(QStringLiteral(".desktop"), QStringLiteral(".json"));
    }

    return m_inFile != m_outFile && !m_inFile.isEmpty() && !m_outFile.isEmpty();
}

// This code was taken from KConfigGroupPrivate::deserializeList
static QStringList deserializeList(const QString &data)
{
    if (data.isEmpty()) {
        return QStringList();
    }
    if (data == QLatin1String("\\0")) {
        return QStringList(QString());
    }
    QStringList value;
    QString val;
    val.reserve(data.size());
    bool quoted = false;
    for (int p = 0; p < data.length(); p++) {
        if (quoted) {
            val += data[p];
            quoted = false;
        } else if (data[p].unicode() == '\\') {
            quoted = true;
        } else if (data[p].unicode() == ',') {
            val.squeeze(); // release any unused memory
            value.append(val);
            val.clear();
            val.reserve(data.size() - p);
        } else {
            val += data[p];
        }
    }
    value.append(val);
    return value;
}

static QByteArray escapeValue(const QByteArray& input)
{
    // we could do this in place, but this code is simpler
    // this tool is probably only transitional, so no need to optimize
    QByteArray result;
    for (int i = 0; i < input.length(); ++i) {
        if (input[i] != '\\') {
            result.append(input[i]);
        } else {
            if (i + 1 >= input.length()) {
                // just append the backslash if we are at end of line
                result.append(input[i]);
                break;
            }
            i++; // consume next character
            char nextChar = input[i];
            switch (nextChar) {
            case 's':
                result.append(' ');
                break;
            case 'n':
                result.append('\n');
                break;
            case 't':
                result.append('\t');
                break;
            case 'r':
                result.append('\r');
                break;
            case '\\':
                result.append('\\');
                break;
            default:
                result.append('\\');
                result.append(nextChar); // just ignore the escape sequence
            }
        }
    }
    return result;
}

bool DesktopToJson::convert(const QString &src, const QString &dest)
{
    QFile df(src);
    if (!df.open(QFile::ReadOnly)) {
        cerr << "Error: Failed to open " << src << endl;
        return false;
    }
    int lineNr = 0;
    // we only convert data inside the [Desktop Entry] group
    while (!df.atEnd()) {
        QByteArray line = df.readLine().trimmed();
        lineNr++;
        if (line == "[Desktop Entry]") {
            cout << "Found desktop group in line " << lineNr << endl;
            break;
        }
    }
    if (df.atEnd()) {
        cerr << "Error: Could not find [Desktop Entry] group in " << src << endl;
        return false;
    }


    // XXX: Hidden=true doesn't make sense with json plugins since the metadata is inside the .so
    static const QStringList boolkeys = QStringList()
            << QStringLiteral("Hidden") << QStringLiteral("X-KDE-PluginInfo-EnabledByDefault");
    static const QStringList stringlistkeys = QStringList()
            << QStringLiteral("X-KDE-ServiceTypes") << QStringLiteral("X-KDE-PluginInfo-Depends");

    QJsonObject json;
    while (!df.atEnd()) {
        const QByteArray line = df.readLine().trimmed();
        lineNr++;
        if (line.isEmpty()) {
            cout << "Line " << lineNr << ": empty" << endl;
            continue;
        }
        if (line.startsWith('#')) {
            cout << "Line " << lineNr << ": comment" << endl;
            continue; // skip comments
        }
        if (line.startsWith('[')) {
            // start of new group -> doesn't interest us anymore
            cout << "Line " << lineNr << ": start of new group " << line << endl;
            break;
        }
        // must have form key=value now
        const int equalsIndex = line.indexOf('=');
        if (equalsIndex == -1) {
            cerr << "Warning: " << src << ':' << lineNr << ": Line is neither comment nor group "
                "and doesn't contain an '=' character: \"" << line << '\"' << endl;
            continue;
        }
        // trim key and value to remove spaces around the '=' char
        const QString key = QString::fromLatin1(line.mid(0, equalsIndex).trimmed());
        const QByteArray valueRaw = line.mid(equalsIndex + 1).trimmed();
        const QByteArray valueEscaped = escapeValue(valueRaw);
        const QString value = QString::fromUtf8(valueEscaped);
        cout << "Line " << lineNr << ": key=\"" << key << "\", value=\"" << value << '"' << endl;
        if (valueEscaped != valueRaw) {
            cout << "Line " << lineNr << " contained escape sequences" << endl;
        }
        if (boolkeys.contains(key)) {
            // should only be lower case, but be tolerant here
            if (value.toLower() == QLatin1String("true")) {
                json[key] = true;
            } else {
                if (value.toLower() != QLatin1String("false")) {
                    cerr << "Warning: " << src << ':' << lineNr << ": Expected boolean value for key \""
                        << key << "\" but got \"" << value << "\" instead.";
                }
                json[key] = false;
            }
        } else if (stringlistkeys.contains(key)) {
            json[key] = QJsonArray::fromStringList(deserializeList(value));
        } else {
            json[key] = value;
        }
    }

    QJsonDocument jdoc;
    jdoc.setObject(json);

    QFile file(dest);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        cerr << "Failed to open " << dest << endl;
        return false;
    }

    file.write(jdoc.toJson());
    cout << "Generated " << dest << endl;
    return true;
}