// SPDX-License-Identifier: MIT
#include "core/Config.h"
#include "launcher/AppsProvider.h"
#include "launcher/Calculator.h"
#include "launcher/CalculatorProvider.h"
#include "launcher/DesktopEntry.h"
#include "launcher/FilesProvider.h"
#include "launcher/FuzzyMatcher.h"
#include "launcher/Launch.h"
#include "launcher/LauncherEngine.h"
#include "launcher/UsageStore.h"
#include "launcher/WebProvider.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace selsearch;

static const QString kApps = QStringLiteral(SELSEARCH_TEST_DATA "/applications");

static void touch(const QString &path, const QByteArray &contents = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(contents);
}

static QStringList ids(const QList<LauncherItem> &items)
{
    QStringList out;
    for (const auto &i : items)
        out << i.id;
    return out;
}

static QStringList titles(const QList<LauncherItem> &items)
{
    QStringList out;
    for (const auto &i : items)
        out << i.title;
    return out;
}

class TestLauncher : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // ---------------------------------------------------------------- desktop entries

    void localeKeys()
    {
        QCOMPARE(desktopentry::localeKeys(QStringLiteral("sr_RS.UTF-8@latin")),
                 (QStringList{QStringLiteral("sr_RS@latin"), QStringLiteral("sr_RS"), QStringLiteral("sr@latin"),
                              QStringLiteral("sr")}));
        QCOMPARE(desktopentry::localeKeys(QStringLiteral("de_DE.UTF-8")),
                 (QStringList{QStringLiteral("de_DE"), QStringLiteral("de")}));
        QVERIFY(desktopentry::localeKeys(QStringLiteral("C")).isEmpty());
        QVERIFY(desktopentry::localeKeys(QStringLiteral("C.UTF-8")).isEmpty());
    }

    void parseLocalized()
    {
        const QString path = kApps + QStringLiteral("/firefox.desktop");
        auto en = desktopentry::parseFile(path, QStringLiteral("firefox.desktop"), QStringLiteral("en_US.UTF-8"));
        QVERIFY(en);
        QCOMPARE(en->name, QStringLiteral("Firefox"));
        QCOMPARE(en->genericName, QStringLiteral("Web Browser"));
        QCOMPARE(en->icon, QStringLiteral("firefox"));
        QCOMPARE(en->exec, QStringLiteral("firefox %u"));
        QVERIFY(en->keywords.contains(QStringLiteral("Explorer")));
        QVERIFY(!en->terminal);
        // The action without Exec is dropped; the order of Actions= is kept.
        QCOMPARE(en->actions.size(), 2);
        QCOMPARE(en->actions[1].id, QStringLiteral("new-private-window"));
        QCOMPARE(en->actions[1].name, QStringLiteral("New Private Window"));
        QCOMPARE(en->filePath, QFileInfo(path).absoluteFilePath());

        auto de = desktopentry::parseFile(path, QStringLiteral("firefox.desktop"), QStringLiteral("de_DE.UTF-8"));
        QVERIFY(de);
        QCOMPARE(de->genericName, QStringLiteral("Webbrowser"));
        QCOMPARE(de->comment, QStringLiteral("Im Internet surfen"));
        QVERIFY(de->keywords.contains(QStringLiteral("Surfen")));
        QCOMPARE(de->actions[0].name, QStringLiteral("Neues Fenster"));

        auto sr = desktopentry::parseFile(path, QStringLiteral("firefox.desktop"), QStringLiteral("sr_RS@latin"));
        QVERIFY(sr);
        QCOMPARE(sr->genericName, QStringLiteral("Veb pregledač"));
        QCOMPARE(sr->comment, QStringLiteral("Browse the World Wide Web"));  // falls back to unlocalised
    }

    void parseRejectsAndFlags()
    {
        QVERIFY(!desktopentry::parseFile(kApps + QStringLiteral("/link.desktop"), QStringLiteral("link.desktop"), QStringLiteral("C")));
        QVERIFY(!desktopentry::parse("[Other]\nName=x\nExec=y\n", QStringLiteral("x.desktop"), QStringLiteral("C")));
        QVERIFY(!desktopentry::parse("[Desktop Entry]\nType=Application\nName=NoExec\n", QStringLiteral("x.desktop"), QStringLiteral("C")));

        auto hidden = desktopentry::parseFile(kApps + QStringLiteral("/hidden.desktop"), QStringLiteral("hidden.desktop"), QStringLiteral("C"));
        QVERIFY(hidden && hidden->hidden);
        QVERIFY(!hidden->shouldShow({}));

        auto kde = desktopentry::parseFile(kApps + QStringLiteral("/kdeonly.desktop"), QStringLiteral("kdeonly.desktop"), QStringLiteral("C"));
        QVERIFY(kde);
        QVERIFY(!kde->shouldShow({QStringLiteral("GNOME")}));
        QVERIFY(kde->shouldShow({QStringLiteral("KDE")}));
        QVERIFY(kde->shouldShow({QStringLiteral("ubuntu"), QStringLiteral("KDE")}));

        auto notGnome = desktopentry::parseFile(kApps + QStringLiteral("/notgnome.desktop"), QStringLiteral("notgnome.desktop"), QStringLiteral("C"));
        QVERIFY(notGnome);
        QVERIFY(!notGnome->shouldShow({QStringLiteral("GNOME")}));
        QVERIFY(notGnome->shouldShow({QStringLiteral("sway")}));

        auto tryExec = desktopentry::parseFile(kApps + QStringLiteral("/tryexec.desktop"), QStringLiteral("tryexec.desktop"), QStringLiteral("C"));
        QVERIFY(tryExec);
        QVERIFY(!tryExec->shouldShow({}));

        auto htop = desktopentry::parseFile(kApps + QStringLiteral("/htop.desktop"), QStringLiteral("htop.desktop"), QStringLiteral("C"));
        QVERIFY(htop);
        QVERIFY(htop->terminal);
        QCOMPARE(htop->workingDir, QStringLiteral("/tmp"));
    }

    void listsAndEscapes()
    {
        auto q = desktopentry::parseFile(kApps + QStringLiteral("/quoting.desktop"), QStringLiteral("quoting.desktop"), QStringLiteral("C"));
        QVERIFY(q);
        QCOMPARE(q->keywords, (QStringList{QStringLiteral("alpha"), QStringLiteral("beta;gamma")}));
        QCOMPARE(desktopentry::unescapeValue(u"a\\sb\\nc\\\\d"), QStringLiteral("a b\nc\\d"));
        QCOMPARE(desktopentry::desktopFileId(kApps, kApps + QStringLiteral("/vendor/tool.desktop")), QStringLiteral("vendor-tool.desktop"));
    }

    void execSplitting()
    {
        auto q = desktopentry::parseFile(kApps + QStringLiteral("/quoting.desktop"), QStringLiteral("quoting.desktop"), QStringLiteral("C"));
        QVERIFY(q);
        bool ok = false;
        const QStringList argv = desktopentry::splitExec(q->exec, &ok);
        QVERIFY(ok);
        QCOMPARE(argv, (QStringList{QStringLiteral("sh"), QStringLiteral("-c"),
                                    QStringLiteral("echo \"hello world\" $HOME \\ done"), QStringLiteral("%f"),
                                    QStringLiteral("--name=%c"), QStringLiteral("--icon-arg"), QStringLiteral("%i"),
                                    QStringLiteral("--file=%k"), QStringLiteral("100%%")}));

        // Field codes: %f dropped without files, %i -> --icon <Icon>, %c -> Name,
        // %k -> file path, %% -> %.
        const QStringList expanded = desktopentry::expandFieldCodes(argv, *q);
        QCOMPARE(expanded, (QStringList{QStringLiteral("sh"), QStringLiteral("-c"),
                                        QStringLiteral("echo \"hello world\" $HOME \\ done"),
                                        QStringLiteral("--name=Quoting Test"), QStringLiteral("--icon-arg"),
                                        QStringLiteral("--icon"), QStringLiteral("quoting-icon"),
                                        QStringLiteral("--file=") + q->filePath, QStringLiteral("100%")}));

        // With files.
        const QStringList withFiles = desktopentry::expandFieldCodes(argv, *q, {QStringLiteral("/a b.txt")});
        QCOMPARE(withFiles.at(3), QStringLiteral("/a b.txt"));

        auto vendor = desktopentry::parseFile(kApps + QStringLiteral("/vendor/tool.desktop"), QStringLiteral("vendor-tool.desktop"), QStringLiteral("C"));
        QVERIFY(vendor);
        QCOMPARE(desktopentry::expandFieldCodes(desktopentry::splitExec(vendor->exec), *vendor, {QStringLiteral("/x"), QStringLiteral("/y")}),
                 (QStringList{QStringLiteral("vendor-tool"), QStringLiteral("--open"), QStringLiteral("/x"), QStringLiteral("/y")}));
        QCOMPARE(desktopentry::expandFieldCodes(desktopentry::splitExec(vendor->exec), *vendor),
                 (QStringList{QStringLiteral("vendor-tool"), QStringLiteral("--open")}));

        auto chromium = desktopentry::parseFile(kApps + QStringLiteral("/chromium.desktop"), QStringLiteral("chromium.desktop"), QStringLiteral("C"));
        QVERIFY(chromium);
        QCOMPARE(AppsProvider::launchArgv(*chromium),
                 (QStringList{QStringLiteral("/usr/bin/chromium"), QStringLiteral("--flag=a b")}));

        // Unbalanced quotes are rejected.
        desktopentry::splitExec(QStringLiteral("foo \"bar"), &ok);
        QVERIFY(!ok);
        // An %i without Icon disappears.
        DesktopEntry noIcon;
        QCOMPARE(desktopentry::expandFieldCodes({QStringLiteral("app"), QStringLiteral("%i"), QStringLiteral("%U")}, noIcon),
                 QStringList{QStringLiteral("app")});
    }

    void terminalAndDetach()
    {
#ifdef Q_OS_WIN
        QSKIP("terminals and systemd-run are Linux-only");
#endif
        const QByteArray saved = qgetenv("TERMINAL");
        qputenv("TERMINAL", "/bin/sh");
        auto htop = desktopentry::parseFile(kApps + QStringLiteral("/htop.desktop"), QStringLiteral("htop.desktop"), QStringLiteral("C"));
        QVERIFY(htop);
        QCOMPARE(AppsProvider::launchArgv(*htop), (QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-e"), QStringLiteral("htop")}));
        if (saved.isNull())
            qunsetenv("TERMINAL");
        else
            qputenv("TERMINAL", saved);

        const QStringList cmd = detachedCommand({QStringLiteral("firefox"), QStringLiteral("--new-window")},
                                                QStringLiteral("org.mozilla.firefox.desktop"), true);
        QCOMPARE(cmd.mid(0, 4), (QStringList{QStringLiteral("systemd-run"), QStringLiteral("--user"),
                                             QStringLiteral("--scope"), QStringLiteral("--slice=app.slice")}));
        QVERIFY(cmd.join(QLatin1Char(' ')).contains(QLatin1String("--unit=app-selsearch-org.mozilla.firefox-")));
        QCOMPARE(cmd.mid(cmd.indexOf(QStringLiteral("--")) + 1), (QStringList{QStringLiteral("firefox"), QStringLiteral("--new-window")}));
        QCOMPARE(detachedCommand({QStringLiteral("a")}, {}, false), QStringList{QStringLiteral("a")});
    }

    // ---------------------------------------------------------------- fuzzy matcher

    void fuzzyFolding()
    {
        QCOMPARE(foldForMatch(u"Chromé"), QStringLiteral("chrome"));
        QCOMPARE(foldForMatch(u"Straße"), QStringLiteral("strasse"));
        QCOMPARE(foldForMatch(u"ÅNGSTRÖM"), QStringLiteral("angstrom"));
        QCOMPARE(fuzzyScoreRaw(QStringLiteral("chrome"), QStringLiteral("Chromé")), 1000.0);
        QVERIFY(fuzzyScoreRaw(QStringLiteral("CHROME"), QStringLiteral("chromé browser")) > 800);
    }

    void fuzzyOrdering()
    {
        auto s = [](const char *p, const char *t) { return fuzzyScoreRaw(QString::fromUtf8(p), QString::fromUtf8(t)); };
        // exact > prefix > word boundary > substring > subsequence > none
        QVERIFY(s("firefox", "Firefox") > s("fire", "Firefox"));
        QVERIFY(s("fire", "Firefox") > s("fire", "Bonfire Tools") + 0);
        QVERIFY(s("fox", "Fire Fox") > s("fox", "Firefox"));
        QVERIFY(s("fox", "Firefox") > s("ffx", "Firefox"));
        QVERIFY(s("ffx", "Firefox") > 0);
        QVERIFY(s("xyz", "Firefox") < 0);
        QVERIFY(s("abc", "ab") < 0);
        // Shorter texts win within a tier.
        QVERIFY(s("files", "Files") > s("files", "Files and Folders Manager"));
        QVERIFY(s("fil", "Files") > s("fil", "FileZilla Client"));
        // Word starts and consecutive runs beat scattered mid-word hits.
        QVERIFY(s("gimp", "GNU Image Manipulation Program") > s("gimp", "rigidmapping"));
        QVERIFY(s("vsc", "Visual Studio Code") > s("vsc", "average scores"));
        QVERIFY(s("term", "Terminal") > s("term", "Determination"));
    }

    // ---------------------------------------------------------------- apps provider

    void appsProvider()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString user = tmp.filePath(QStringLiteral("user/applications"));
        // User dir overrides firefox and hides chromium by desktop-file id.
        touch(user + QStringLiteral("/firefox.desktop"),
              "[Desktop Entry]\nType=Application\nName=Firefox Nightly\nExec=firefox-nightly %u\nIcon=firefox-nightly\n");
        touch(user + QStringLiteral("/chromium.desktop"), "[Desktop Entry]\nType=Application\nName=Chromium\nExec=x\nHidden=true\n");

        AppsProvider::Options o;
        o.applicationDirs = {user, kApps, tmp.filePath(QStringLiteral("missing"))};
        o.locale = QStringLiteral("C");
        o.desktops = {QStringLiteral("GNOME")};
        o.usagePath = tmp.filePath(QStringLiteral("state/usage.json"));
        o.watch = false;
        AppsProvider apps(o);
        apps.refresh();

        QStringList visible;
        for (const auto &e : apps.entries())
            visible << e.id;
        visible.sort();
        QCOMPARE(visible, (QStringList{QStringLiteral("files.desktop"), QStringLiteral("filezilla.desktop"),
                                       QStringLiteral("firefox.desktop"), QStringLiteral("htop.desktop"),
                                       QStringLiteral("profiler.desktop"), QStringLiteral("quoting.desktop"),
                                       QStringLiteral("vendor-tool.desktop")}));
        QCOMPARE(apps.entry(QStringLiteral("firefox.desktop"))->name, QStringLiteral("Firefox Nightly"));

        // Fuzzy over name, generic name, keywords and exec.
        QCOMPARE(apps.query(QStringLiteral("nightly"), 5).value(0).id, QStringLiteral("firefox.desktop"));
        QCOMPARE(apps.query(QStringLiteral("process"), 5).value(0).id, QStringLiteral("htop.desktop"));
        QCOMPARE(apps.query(QStringLiteral("sysprof"), 5).value(0).id, QStringLiteral("profiler.desktop"));
        QCOMPARE(apps.query(QStringLiteral("nautilus"), 5).value(0).id, QStringLiteral("files.desktop"));
        QVERIFY(apps.query(QStringLiteral("zzzzqqq"), 5).isEmpty());
        // Empty query with no usage yet: nothing.
        QVERIFY(apps.query(QString(), 5).isEmpty());
    }

    void appsActionsAndLocale()
    {
        QTemporaryDir tmp;
        AppsProvider::Options o;
        o.applicationDirs = {kApps};
        o.locale = QStringLiteral("C");
        o.desktops = {QStringLiteral("KDE")};
        o.usagePath = tmp.filePath(QStringLiteral("usage.json"));
        o.watch = false;
        AppsProvider apps(o);
        apps.refresh();
        QVERIFY(apps.entry(QStringLiteral("kdeonly.desktop")));
        QVERIFY(apps.entry(QStringLiteral("chromium.desktop")));

        // Diacritics-insensitive.
        QCOMPARE(apps.query(QStringLiteral("chrome"), 5).value(0).id, QStringLiteral("chromium.desktop"));

        const auto fire = apps.query(QStringLiteral("fire"), 10);
        QCOMPARE(fire.value(0).id, QStringLiteral("firefox.desktop"));
        QCOMPARE(fire.value(0).kind, LauncherItem::App);
        QCOMPARE(fire.value(0).actions.size(), 2);
        QVERIFY(titles(fire).contains(QStringLiteral("Firefox — New Private Window")));

        const auto priv = apps.query(QStringLiteral("private"), 10);
        QCOMPARE(priv.value(0).kind, LauncherItem::Action);
        QCOMPARE(priv.value(0).title, QStringLiteral("Firefox — New Private Window"));
        QCOMPARE(priv.value(0).data.value(QStringLiteral("action")).toString(), QStringLiteral("new-private-window"));
        auto ff = apps.entry(QStringLiteral("firefox.desktop"));
        QCOMPARE(AppsProvider::launchArgv(*ff, QStringLiteral("new-private-window")),
                 (QStringList{QStringLiteral("firefox"), QStringLiteral("--private-window")}));

        // Localised names are searchable.
        AppsProvider::Options de = o;
        de.locale = QStringLiteral("de_DE.UTF-8");
        AppsProvider appsDe(de);
        appsDe.refresh();
        QCOMPARE(appsDe.query(QStringLiteral("webbrowser"), 5).value(0).id, QStringLiteral("firefox.desktop"));
    }

    void frecency()
    {
        QTemporaryDir tmp;
        AppsProvider::Options o;
        o.applicationDirs = {kApps};
        o.locale = QStringLiteral("C");
        o.usagePath = tmp.filePath(QStringLiteral("state/selsearch/usage.json"));
        o.watch = false;
        {
            AppsProvider apps(o);
            apps.refresh();
            QCOMPARE(apps.query(QStringLiteral("fil"), 5).value(0).id, QStringLiteral("files.desktop"));
            for (int i = 0; i < 4; ++i)
                apps.usage().record(QStringLiteral("filezilla.desktop"));
            apps.usage().record(QStringLiteral("htop.desktop"));
            QCOMPARE(apps.query(QStringLiteral("fil"), 5).value(0).id, QStringLiteral("filezilla.desktop"));
        }
        QVERIFY(QFile::exists(o.usagePath));
        // Persisted; empty query lists the most used apps.
        AppsProvider again(o);
        again.refresh();
        QCOMPARE(ids(again.query(QString(), 5)), (QStringList{QStringLiteral("filezilla.desktop"), QStringLiteral("htop.desktop")}));

        // Old usage decays.
        UsageStore store(tmp.filePath(QStringLiteral("decay.json")));
        const qint64 now = 2000000000;
        store.record(QStringLiteral("a"), now - 200 * 86400);
        store.record(QStringLiteral("a"), now - 200 * 86400);
        store.record(QStringLiteral("b"), now);
        QVERIFY(store.frecency(QStringLiteral("b"), now) > store.frecency(QStringLiteral("a"), now));
        QCOMPARE(store.top(5, now).first(), QStringLiteral("b"));
    }

    // ---------------------------------------------------------------- calculator

    void calculator_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<double>("expected");
        auto row = [](const char *in, double v) { QTest::newRow(in) << QString::fromUtf8(in) << v; };
        row("1+2", 3);
        row("2*3+4", 10);
        row("2 + 3 * 4", 14);
        row("(1+2)*3", 9);
        row("2^3^2", 512);
        row("2**10", 1024);
        row("-2^2", -4);
        row("2^-1", 0.5);
        row("10 % 3", 1);
        row("7/2", 3.5);
        row("2pi", 2 * M_PI);
        row("3(4+5)", 27);
        row("2 sqrt(4)", 4);
        row("sqrt(16)", 4);
        row("√16 + 1", 5);
        row("cbrt(27)", 3);
        row("sin(0) + cos(0)", 1);
        row("cos(pi)", -1);
        row("tan(0)+1", 1);
        row("asin(1)*2", M_PI);
        row("acos(1)+1", 1);
        row("atan(1)*4", M_PI);
        row("ln(e)", 1);
        row("log(1000)", 3);
        row("log2(8)", 3);
        row("exp(0)+1", 2);
        row("abs(-5)", 5);
        row("round(2.5)", 3);
        row("floor(2.7)", 2);
        row("ceil(2.1)", 3);
        row("min(3, 1, 2)", 1);
        row("max(1,5)", 5);
        row("tau/2", M_PI);
        row("1e3 + 1", 1001);
        row(".5 * 4", 2);
        row("0x1f + 1", 32);
        row("0b101 * 2", 10);
        row("0o17 + 1", 16);
        row("0xff", 255);
        row("3 × 4 − 2", 10);
        row("8 ÷ 2", 4);
        row("20% of 150", 30);
        row("200 + 10%", 220);
        row("200 - 25%", 150);
        row("50% * 8", 4);
        row("= 6*7", 42);
        // Conversions
        row("0x1f in dec", 31);
        row("255 in hex", 255);
        row("5 km in mi", 3.1068559611866697);
        row("100 f to c", 37.77777777777778);
        row("0 c to f", 32);
        row("300 K in C", 26.85);
        row("3 GiB in MB", 3221.225472);
        row("1 Gb in MB", 125);
        row("1 kg in lb", 2.2046226218487757);
        row("16 oz to lb", 1);
        row("2 h in min", 120);
        row("1 day in hours", 24);
        row("1 mi to km", 1.609344);
        row("5 in to cm", 12.7);
        row("12 inches in ft", 1);
        row("2 l in ml", 2000);
        row("1024 KiB to MiB", 1);
        row("(2+3) km in m", 5000);
    }

    void calculator()
    {
        QFETCH(QString, input);
        QFETCH(double, expected);
        const auto r = calc::evaluate(input, QLocale(QLocale::English));
        QVERIFY2(r, qPrintable(input));
        QVERIFY2(std::fabs(r->value - expected) <= 1e-9 * std::max(1.0, std::fabs(expected)),
                 qPrintable(QStringLiteral("%1 = %2, expected %3").arg(input).arg(r->value, 0, 'g', 17).arg(expected, 0, 'g', 17)));
    }

    void calculatorFormatting()
    {
        const QLocale en(QLocale::English);
        auto r = calc::evaluate(QStringLiteral("0.1+0.2"), en);
        QVERIFY(r);
        QCOMPARE(r->copyText, QStringLiteral("0.3"));
        r = calc::evaluate(QStringLiteral("1234567.5*2"), en);
        QVERIFY(r);
        QCOMPARE(r->copyText, QStringLiteral("2469135"));
        QCOMPARE(r->grouped, QStringLiteral("2,469,135"));
        r = calc::evaluate(QStringLiteral("100/3"), en);
        QVERIFY(r);
        QCOMPARE(r->copyText, QStringLiteral("33.3333333333333"));
        QCOMPARE(r->title, QStringLiteral("33.3333333333"));
        r = calc::evaluate(QStringLiteral("2^70"), en);
        QVERIFY(r);
        QVERIFY(r->copyText.contains(QLatin1Char('e')));
        QCOMPARE(calc::formatGrouped(-1234.5, en), QStringLiteral("-1,234.5"));
        QCOMPARE(calc::formatGrouped(1234.5, QLocale(QLocale::German)), QStringLiteral("1.234,5"));

        r = calc::evaluate(QStringLiteral("255 in hex"), en);
        QCOMPARE(r->copyText, QStringLiteral("0xff"));
        r = calc::evaluate(QStringLiteral("5 in bin"), en);
        QCOMPARE(r->copyText, QStringLiteral("0b101"));
        r = calc::evaluate(QStringLiteral("8 to oct"), en);
        QCOMPARE(r->copyText, QStringLiteral("0o10"));
        r = calc::evaluate(QStringLiteral("0xffff in dec"), en);
        QCOMPARE(r->copyText, QStringLiteral("65535"));
        QCOMPARE(r->grouped, QStringLiteral("65,535"));
        r = calc::evaluate(QStringLiteral("5 km in mi"), en);
        QCOMPARE(r->unit, QStringLiteral("mi"));
        QCOMPARE(r->title, QStringLiteral("3.106855961 mi"));
        QCOMPARE(r->copyText, QStringLiteral("3.10685596118667"));
        r = calc::evaluate(QStringLiteral("1500 m in ft"), en);
        QCOMPARE(r->grouped, QStringLiteral("4,921.259843 ft"));
    }

    void calculatorRejects_data()
    {
        QTest::addColumn<QString>("input");
        for (const char *s : {"", "42", "-5", "3.14", "(5)", "pi", "e", "hello", "sin", "firefox", "2+", "(1+2",
                              "1+2)", "1/0", "2 3", "2024-01-15", "1.2.3", "5 km", "5 km in kg", "foo in hex",
                              "1.5 in hex", "x+1", "2x3", "ln(-1)", "hello world", "sqrt", "% of 5", "1 + hello",
                              "0xZZ + 1", "5 parsecs in m", "what is 2+2", "1,5"})
            QTest::newRow(s) << QString::fromUtf8(s);
    }

    void calculatorRejects()
    {
        QFETCH(QString, input);
        const auto r = calc::evaluate(input);
        QVERIFY2(!r, qPrintable(QStringLiteral("%1 gave %2").arg(input, r ? r->copyText : QString())));
    }

    void calculatorProvider()
    {
        CalculatorProvider p;
        const auto rows = p.query(QStringLiteral("1234*1000"), 5);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].kind, LauncherItem::Calculator);
        QCOMPARE(rows[0].group, QStringLiteral("Calculator"));
        QCOMPARE(rows[0].data.value(QStringLiteral("value")).toString(), QStringLiteral("1234000"));
        QVERIFY(rows[0].subtitle.startsWith(QLatin1String("1234*1000 = ")));
        QVERIFY(p.query(QStringLiteral("firefox"), 5).isEmpty());
    }

    // ---------------------------------------------------------------- web / URLs

    void urlDetection_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");  // empty = not a URL
        auto row = [](const char *in, const char *out) { QTest::newRow(in) << QString::fromUtf8(in) << QString::fromUtf8(out); };
        row("https://example.com", "https://example.com");
        row("http://example.com/a?b=c#d", "http://example.com/a?b=c#d");
        row("example.com", "https://example.com");
        row("Example.COM/Path", "https://Example.COM/Path");
        row("sub.example.co.uk/path?x=1", "https://sub.example.co.uk/path?x=1");
        row("claude.ai", "https://claude.ai");
        row("github.com/foo/bar", "https://github.com/foo/bar");
        row("localhost", "http://localhost");
        row("localhost:8080", "http://localhost:8080");
        row("localhost:3000/api", "http://localhost:3000/api");
        row("192.168.1.1", "http://192.168.1.1");
        row("10.0.0.1:3000/x", "http://10.0.0.1:3000/x");
        row("www.example.md", "https://www.example.md");
        row("example.md/docs", "https://example.md/docs");
        row("file:///etc/hosts", "file:///etc/hosts");
        row("mailto:a@b.org", "mailto:a@b.org");
        row("münchen.de", "https://münchen.de");
        row("hello", "");
        row("3.14", "");
        row("notes.txt", "");
        row("main.py", "");
        row("readme.md", "");
        row("run.sh", "");
        row("foo.bar", "");
        row("hello world.com", "");
        row("999.1.1.1", "");
        row("a..b.com", "");
        row("-a.com", "");
        row("http://", "");
        row("localhost:99999", "");
        row("2+2", "");
        row("5 km in mi", "");
    }

    void urlDetection()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const QUrl u = WebProvider::detectUrl(input);
        if (expected.isEmpty()) {
            QVERIFY2(!u.isValid(), qPrintable(u.toString()));
        } else {
            QVERIFY2(u.isValid(), qPrintable(input));
            QCOMPARE(u, QUrl(expected));
        }
    }

    void webProvider()
    {
        WebProvider web;
        auto rows = web.query(QStringLiteral("example.com"), 5);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].kind, LauncherItem::Url);
        QCOMPARE(rows[0].title, QStringLiteral("Open example.com"));
        QCOMPARE(WebProvider::urlFor(rows[0]), QUrl(QStringLiteral("https://example.com")));
        QCOMPARE(rows[1].kind, LauncherItem::WebSearch);
        QCOMPARE(rows[1].title, QStringLiteral("Search DuckDuckGo for “example.com”"));
        QCOMPARE(rows[1].group, QStringLiteral("Web"));

        rows = web.query(QStringLiteral("cats & dogs"), 5);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(WebProvider::urlFor(rows[0]).toString(QUrl::FullyEncoded),
                 QStringLiteral("https://duckduckgo.com/?q=cats%20%26%20dogs"));
        QVERIFY(web.query(QStringLiteral("  "), 5).isEmpty());

        QTemporaryDir tmp;
        Config config;
        QVERIFY(config.load(tmp.filePath(QStringLiteral("config.toml"))));
        config.setValue(QStringLiteral("search.engine"), QStringLiteral("brave"));
        WebProvider brave(&config);
        rows = brave.query(QStringLiteral("x"), 5);
        QCOMPARE(rows.last().title, QStringLiteral("Search Brave Search for “x”"));
        QCOMPARE(WebProvider::urlFor(rows.last()), QUrl(QStringLiteral("https://search.brave.com/search?q=x")));
    }

    // ---------------------------------------------------------------- files

    void filesIndex()
    {
        QTemporaryDir tmp;
        const QString root = tmp.path();
        touch(root + QStringLiteral("/docs/report.pdf"));
        touch(root + QStringLiteral("/docs/Résumé.odt"));
        touch(root + QStringLiteral("/Reports/2024/q1.ods"));
        touch(root + QStringLiteral("/projects/selsearch/src/main.cpp"));
        touch(root + QStringLiteral("/projects/selsearch/node_modules/junk/index.js"));
        touch(root + QStringLiteral("/projects/selsearch/.git/HEAD"));
        touch(root + QStringLiteral("/projects/app/build/out.o"));
        touch(root + QStringLiteral("/.hidden/secret.txt"));
        touch(root + QStringLiteral("/.cache/cachedfile"));
        touch(root + QStringLiteral("/tagged/CACHEDIR.TAG"), "Signature: 8a477f597d28d172789f06886806bc55");
        touch(root + QStringLiteral("/tagged/tagged-content.bin"));
        touch(root + QStringLiteral("/a/b/c/d/e/f/report-deep.txt"));

        FilesProvider::Options o;
        o.roots = {root};
        o.allowPlocate = false;
        FilesProvider files(o);
        QVERIFY(!files.usingPlocate());
        // Not indexed yet: returns immediately with nothing.
        QVERIFY(files.query(QStringLiteral("report"), 5).isEmpty());
        files.waitForIndex();
        files.refresh();
        QVERIFY(files.indexedCount() > 0);

        auto rows = files.query(QStringLiteral("report"), 10);
        const QStringList t = titles(rows);
        QVERIFY2(t.contains(QStringLiteral("report.pdf")), qPrintable(t.join(u',')));
        QVERIFY(t.contains(QStringLiteral("Reports")));
        QVERIFY(t.contains(QStringLiteral("report-deep.txt")));
        // Shallow beats deep for the same tier.
        QVERIFY(t.indexOf(QStringLiteral("report.pdf")) < t.indexOf(QStringLiteral("report-deep.txt")));

        // Exact name: the folder comes first.
        rows = files.query(QStringLiteral("reports"), 5);
        QCOMPARE(rows.value(0).title, QStringLiteral("Reports"));
        QCOMPARE(rows.value(0).kind, LauncherItem::Folder);
        QCOMPARE(rows.value(0).group, QStringLiteral("Files"));
        QCOMPARE(rows.value(0).id, root + QStringLiteral("/Reports"));
        QCOMPARE(rows.value(0).actions.value(0).id, QStringLiteral("open-folder"));

        // Diacritics-insensitive.
        QCOMPARE(files.query(QStringLiteral("resume"), 5).value(0).title, QStringLiteral("Résumé.odt"));
        QCOMPARE(files.query(QStringLiteral("main.cpp"), 5).value(0).kind, LauncherItem::File);

        // Skipped: hidden, node_modules, .git, build dirs, CACHEDIR.TAG dirs.
        for (const char *q : {"index.js", "junk", "HEAD", "out.o", "secret", "cachedfile", "tagged-content"})
            QVERIFY2(files.query(QString::fromLatin1(q), 5).isEmpty(), q);

        // Entry cap.
        FilesProvider::Options capped = o;
        capped.maxEntries = 3;
        FilesProvider small(capped);
        small.refresh();
        QCOMPARE(small.indexedCount(), 3);

        // Depth cap.
        FilesProvider::Options shallow = o;
        shallow.maxDepth = 2;
        FilesProvider sh(shallow);
        sh.refresh();
        QVERIFY(sh.query(QStringLiteral("report-deep"), 5).isEmpty());
        QVERIFY(!sh.query(QStringLiteral("report.pdf"), 5).isEmpty());
    }

    void filesBackgroundIndexing()
    {
        QTemporaryDir tmp;
        touch(tmp.filePath(QStringLiteral("x/needle-file.txt")));
        FilesProvider::Options o;
        o.roots = {tmp.path()};
        o.allowPlocate = false;
        FilesProvider files(o);
        QSignalSpy indexed(&files, &FilesProvider::indexed);
        files.startIndexing();
        QTRY_VERIFY(indexed.count() >= 1);
        QCOMPARE(files.query(QStringLiteral("needle"), 5).value(0).title, QStringLiteral("needle-file.txt"));
    }

    void filesPlocate()
    {
        FilesProvider::Options o;
        o.roots = {QStringLiteral("/etc")};
        FilesProvider files(o);
        if (!files.usingPlocate())
            QSKIP("plocate or its database is not available");
        const auto rows = files.query(QStringLiteral("passwd"), 10);
        QVERIFY2(ids(rows).contains(QStringLiteral("/etc/passwd")), qPrintable(ids(rows).join(u',')));
        for (const auto &r : rows)
            QVERIFY(r.id.startsWith(QLatin1String("/etc/")));  // filtered to the roots
        QCOMPARE(rows.value(0).title, QStringLiteral("passwd"));  // exact name first
    }

    // ---------------------------------------------------------------- engine

    void mergeGroups()
    {
        QList<LauncherItem> rows;
        auto make = [](const QString &group, LauncherItem::Kind kind, double score, const QString &id) {
            LauncherItem i;
            i.group = group;
            i.kind = kind;
            i.score = score;
            i.id = id;
            return i;
        };
        rows << make(QStringLiteral("Web"), LauncherItem::WebSearch, 0, QStringLiteral("search"));
        for (int i = 0; i < 10; ++i)
            rows << make(QStringLiteral("Files"), LauncherItem::File, i, QStringLiteral("f%1").arg(i));
        for (int i = 0; i < 10; ++i)
            rows << make(QStringLiteral("Apps"), LauncherItem::App, i, QStringLiteral("a%1").arg(i));
        rows << make(QStringLiteral("Web"), LauncherItem::Url, 1000, QStringLiteral("url"));
        rows << make(QStringLiteral("Calculator"), LauncherItem::Calculator, 1000, QStringLiteral("calc"));

        const auto merged = LauncherEngine::merge(rows, 8);
        QCOMPARE(merged.size(), 10);  // 8 + the two web rows
        QCOMPARE(merged.first().id, QStringLiteral("calc"));
        QCOMPARE(merged.at(1).id, QStringLiteral("a9"));  // best app first
        QCOMPARE(merged.at(merged.size() - 2).id, QStringLiteral("url"));
        QCOMPARE(merged.last().id, QStringLiteral("search"));
        int apps = 0, files = 0;
        for (const auto &m : merged) {
            apps += m.group == QLatin1String("Apps");
            files += m.group == QLatin1String("Files");
        }
        QVERIFY(apps >= 4 && apps <= 5);
        QVERIFY(files >= 2);
        QCOMPARE(apps + files, 7);

        // A short group leaves its budget to the others.
        QList<LauncherItem> few;
        for (int i = 0; i < 10; ++i)
            few << make(QStringLiteral("Apps"), LauncherItem::App, i, QStringLiteral("a%1").arg(i));
        few << make(QStringLiteral("Files"), LauncherItem::File, 1, QStringLiteral("f"));
        few << make(QStringLiteral("Web"), LauncherItem::WebSearch, 0, QStringLiteral("search"));
        const auto m2 = LauncherEngine::merge(few, 8);
        QCOMPARE(m2.size(), 9);
        QCOMPARE(m2.at(7).id, QStringLiteral("f"));
    }

    void engineEndToEnd()
    {
#ifdef Q_OS_WIN
        QSKIP("finds the test apps through XDG_DATA_DIRS; Windows reads the Start Menu");
#endif
        QTemporaryDir tmp;
        touch(tmp.filePath(QStringLiteral("home/Documents/quarterly-report.txt")));
        qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("nodata")).toUtf8());
        qputenv("XDG_DATA_DIRS", QFileInfo(kApps).absolutePath().toUtf8());
        qputenv("XDG_STATE_HOME", tmp.filePath(QStringLiteral("state")).toUtf8());

        Config config;
        QVERIFY(config.load(tmp.filePath(QStringLiteral("config.toml"))));
        config.setValue(QStringLiteral("launcher.file_roots"), QStringList{tmp.filePath(QStringLiteral("home"))});
        LauncherEngine engine(&config);
        engine.files()->waitForIndex();
        QSignalSpy spy(&engine, &LauncherEngine::resultsReady);
        engine.start();
        QTRY_VERIFY(!engine.apps()->entries().isEmpty());
        QTRY_VERIFY(engine.files()->usingPlocate() || engine.files()->indexedCount() > 0);

        spy.clear();
        const quint64 gen = engine.query(QStringLiteral("2+2"));
        QVERIFY(spy.count() >= 1);
        auto items = spy.last().at(0).value<QList<LauncherItem>>();
        QCOMPARE(spy.last().at(1).value<quint64>(), gen);
        QCOMPARE(items.first().kind, LauncherItem::Calculator);
        QCOMPARE(items.first().title, QStringLiteral("4"));
        QCOMPARE(items.last().kind, LauncherItem::WebSearch);

        // Web rows activate to a URL for the daemon to open.
        const ActivateResult web = engine.activate(items.last());
        QCOMPARE(web.status, ActivateResult::OpenUrl);
        QCOMPARE(web.url, QUrl(QStringLiteral("https://duckduckgo.com/?q=2%2B2")));

        // Apps and files arrive (files asynchronously).
        spy.clear();
        engine.query(QStringLiteral("firefox"));
        items = spy.last().at(0).value<QList<LauncherItem>>();
        QCOMPARE(items.first().id, QStringLiteral("firefox.desktop"));
        QCOMPARE(items.first().provider, QStringLiteral("apps"));

        if (!engine.files()->usingPlocate()) {
            spy.clear();
            const quint64 g = engine.query(QStringLiteral("quarterly"));
            QTRY_VERIFY(!spy.isEmpty() && [&] {
                for (const auto &args : spy) {
                    if (args.at(1).value<quint64>() != g)
                        continue;
                    for (const auto &i : args.at(0).value<QList<LauncherItem>>())
                        if (i.title == QLatin1String("quarterly-report.txt"))
                            return true;
                }
                return false;
            }());
        }

        // Stale generations are dropped: only the newest query's files result arrives.
        spy.clear();
        engine.query(QStringLiteral("quart"));
        const quint64 last = engine.query(QStringLiteral("quarterly-r"));
        QTest::qWait(200);
        for (const auto &args : spy) {
            const quint64 g = args.at(1).value<quint64>();
            if (g != last) {
                for (const auto &i : args.at(0).value<QList<LauncherItem>>())
                    QVERIFY(i.group != QLatin1String("Files"));
            }
        }

        // Toggles.
        config.setValue(QStringLiteral("launcher.calculator"), false);
        config.setValue(QStringLiteral("launcher.apps"), false);
        spy.clear();
        engine.query(QStringLiteral("2+2"));
        items = spy.last().at(0).value<QList<LauncherItem>>();
        for (const auto &i : items) {
            QVERIFY(i.kind != LauncherItem::Calculator);
            QVERIFY(i.group != QLatin1String("Apps"));
        }
        QCOMPARE(items.last().kind, LauncherItem::WebSearch);

        // Unknown app -> error result.
        LauncherItem gone;
        gone.kind = LauncherItem::App;
        gone.id = QStringLiteral("not-installed.desktop");
        const ActivateResult err = engine.activate(gone);
        QCOMPARE(err.status, ActivateResult::Error);
        QVERIFY(!err.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLauncher)
#include "tst_launcher.moc"
