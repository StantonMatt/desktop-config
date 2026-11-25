#include "../clipboard-stack.h"
#include <QTest>
using namespace ClipboardStack;
class ClipboardStackTest final : public QObject {
    Q_OBJECT
private slots:
    void statePathDoesNotIncludeApplicationName() {
        const auto previous = qgetenv("XDG_STATE_HOME");
        const auto application = QCoreApplication::applicationName();
        qputenv("XDG_STATE_HOME", "/tmp/qro-state-location-test");
        QCoreApplication::setApplicationName(QStringLiteral("Wrong App"));
        QCOMPARE(stateDirectory(), QStringLiteral("/tmp/qro-state-location-test/quick-region-overlay/stack"));
        QCoreApplication::setApplicationName(application);
        if (previous.isNull()) qunsetenv("XDG_STATE_HOME"); else qputenv("XDG_STATE_HOME", previous);
    }
    void classification() {
        QCOMPARE(kind({{QStringLiteral("text/plain"), "hi"}, {QStringLiteral("text/html"), "<b>hi</b>"}}), Kind::Text);
        QCOMPARE(kind({{QStringLiteral("text/plain"), "alt"}, {QStringLiteral("image/png"), "png"}}), Kind::Image);
        QCOMPARE(kind({{QStringLiteral("application/x-qt-image"), "png"}}), Kind::Image);
        QCOMPARE(kind({{QStringLiteral("text/uri-list"), "file:///tmp/a"}}), Kind::Other);
        QCOMPARE(kind({{QStringLiteral("text/plain"), "name"}, {QStringLiteral("application/custom"), "raw"}}), Kind::Text);
        QCOMPARE(kind({{QStringLiteral("application/custom"), "raw"}}), Kind::Other);
        QCOMPARE(kind({{QStringLiteral("UTF8_STRING"), "text"}}), Kind::Text);
        QCOMPARE(kind({{QStringLiteral("text/plain;charset=iso-8859-1"), "text"}}), Kind::Text);
        QCOMPARE(kind({{QStringLiteral("text/plain"), "file"}, {QStringLiteral("text/uri-list"), "file:///tmp/a"}}), Kind::Other);
        QCOMPARE(kind({{QStringLiteral("text/html"), "<b>html only</b>"}}), Kind::Other);
        QCOMPARE(kind({}), Kind::Other);
    }
    void realAppAuxiliaryFormats() {
        const Item first{{QStringLiteral("text/plain"), "one"}, {QStringLiteral("text/html"), "<b>one</b>"}, {QStringLiteral("text/markdown"), "**one**"}, {QStringLiteral("application/vnd.oasis.opendocument.text"), "odt bytes"}};
        const Item second{{QStringLiteral("text/plain;charset=utf-8"), "two"}, {QStringLiteral("chromium/x-web-custom-data"), "browser bytes"}};
        QCOMPARE(plan({first}), QList<Item>{first});
        const auto merged = plan({first, second});
        QCOMPARE(merged.size(), 1);
        QCOMPARE(merged[0].size(), 2);
        QCOMPARE(merged[0].value(QStringLiteral("text/plain")), QByteArray("one\ntwo"));
        QCOMPARE(merged[0].value(QStringLiteral("text/html")), QByteArray("<b>one</b><br>two"));
    }
    void separatorsAndOrder() {
        const Item a{{QStringLiteral("text/plain"), "a"}}, b{{QStringLiteral("text/plain"), "b"}};
        const Item image{{QStringLiteral("image/png"), "png"}}, file{{QStringLiteral("text/uri-list"), "file:///tmp/a"}};
        const auto result = plan({a, b, image, a, image, image, file, b});
        QCOMPARE(result.size(), 7);
        QCOMPARE(result[0].value(QStringLiteral("text/plain")), QByteArray("a\nb"));
        QCOMPARE(result[1], image);
        QCOMPARE(result[2], a);
        QCOMPARE(result[3], image);
        QCOMPARE(result[4], image);
        QCOMPARE(result[5], file);
        QCOMPARE(result[6], b);
        QCOMPARE(plan({a}), QList<Item>{a});
    }
    void htmlRunEscapesPlain() {
        const Item rich{{QStringLiteral("text/plain"), "rich"}, {QStringLiteral("text/html"), "<b>rich</b>"}};
        const Item plainItem{{QStringLiteral("text/plain"), "a<&\nx"}};
        const auto result = plan({rich, plainItem});
        QCOMPARE(result[0].value(QStringLiteral("text/html")), QByteArray("<b>rich</b><br>a&lt;&amp;<br>x"));
        QCOMPARE(result[0].value(QStringLiteral("text/plain")), QByteArray("rich\na<&\nx"));
        QCOMPARE(plain({{QStringLiteral("text/html"), "<p>only</p>"}}), QStringLiteral("only"));
        QCOMPARE(htmlFragment("<!doctype html><html><head><meta charset='utf-8'></head><body><b>one</b></body></html>"), QStringLiteral("<b>one</b>"));
        QCOMPARE(htmlFragment("<html><body><!--StartFragment--><b>one</b><!--EndFragment--></body></html>"), QStringLiteral("<b>one</b>"));
        QCOMPARE(htmlFragment("<html><body>outside<!--StartFragment-->inside<!--EndFragment-->after</body></html>"), QStringLiteral("inside"));
        QCOMPARE(htmlFragment("<html><body><p>outside</p><img src=x><p><!--StartFragment-->inside<!--EndFragment--></p><p><br></p></body></html>"),
                 QStringLiteral("<span>inside</span>"));
        QCOMPARE(htmlFragment("<html><body>\n<p style=\"a\"><!--StartFragment-->one</p>\n<p style=\"b\"><br /><!--EndFragment--></p></body></html>"),
                 QStringLiteral("<span style=\"a\">one</span><br><span style=\"b\"></span>"));
        QCOMPARE(plain({{QStringLiteral("text/plain;charset=iso-8859-1"), QByteArray::fromHex("636166e9")}}), QStringLiteral("café"));
        QCOMPARE(plain({{QStringLiteral("Text/Plain; charset=\"ISO-8859-1\""), QByteArray::fromHex("636166e9")}}), QStringLiteral("café"));
        QCOMPARE(plain({{QStringLiteral("text/plain;charset=windows-1252"), QByteArray::fromHex("93802071756f746594")}}), QStringLiteral("“€ quote”"));
        const Item qtFirst{{QStringLiteral("text/plain"), "one"}, {QStringLiteral("text/html"), "<html><body><p style='margin:0'>one</p></body></html>"}};
        const Item qtSecond{{QStringLiteral("text/plain"), "two"}, {QStringLiteral("text/html"), "<html><body><p style='margin:0'>two</p></body></html>"}};
        QTextDocument document;
        document.setHtml(QString::fromUtf8(plan({qtFirst, qtSecond})[0].value(QStringLiteral("text/html"))));
        QCOMPARE(document.toPlainText(), QStringLiteral("one\ntwo"));
        const Item trailing{{QStringLiteral("text/plain"), "one\n"}, {QStringLiteral("text/html"), "<html><body><p>one</p><p><br /><!--EndFragment--></p></body></html>"}};
        document.setHtml(QString::fromUtf8(plan({trailing, qtSecond})[0].value(QStringLiteral("text/html"))));
        QCOMPARE(document.toPlainText(), QStringLiteral("one\n\ntwo"));
    }
    void persistencePreservesEveryByte() {
        const Item original{{QStringLiteral("image/png"), QByteArray("\0\1\377", 3)}, {QStringLiteral("text/uri-list"), "file:///tmp/a\r\n"}, {QStringLiteral("custom/empty"), {}}};
        Item decoded;
        QVERIFY(decode(encode(original), decoded));
        QCOMPARE(decoded, original);
        auto malformed = encode(original);
        malformed.chop(1);
        QVERIFY(!decode(malformed, decoded));
        QVERIFY(!decode(encode(original) + "trailing", decoded));
        QVERIFY(!decode(QByteArray::fromHex("515253310000000100000010"), decoded));
        QVERIFY(!decode(QByteArray::fromHex("51525331000000010000000178ffffffff"), decoded));
    }
    void caps() {
        QCOMPARE(maxItems, 100);
        QCOMPARE(maxBytes, 67108864);
        Item big{{QStringLiteral("a/b"), QByteArray(maxBytes - 3, 'x')}};
        QCOMPARE(size(big), maxBytes);
        Item decoded;
        QVERIFY(decode(encode(big), decoded));
        big[QStringLiteral("a/b")] += 'x';
        QVERIFY(size(big) > maxBytes);
        QVERIFY(!decode(encode(big), decoded));
        // A shutdown snapshot preserves an original clipboard even when it exceeds the stack's item cap.
        QVERIFY(decode(encode(big), decoded, std::numeric_limits<qint64>::max(), true));
        QCOMPARE(decoded, big);
    }
    void holderSnapshots() {
        Item decoded;
        QVERIFY(!decode(encode({}), decoded));
        QVERIFY(decode(encode({}), decoded, std::numeric_limits<qint64>::max(), true));
        QVERIFY(decoded.isEmpty());
        const Item original{{secretHint, "secret"}, {QStringLiteral("custom/binary"), QByteArray::fromHex("00ff01")}};
        QVERIFY(decode(encode(original), decoded, std::numeric_limits<qint64>::max(), true));
        QCOMPARE(decoded, original);
        QVERIFY(!decode(QByteArray::fromHex("51525331000000010000000178ffffffff"), decoded, std::numeric_limits<qint64>::max(), true));
    }
    void terminals() {
        for (const QString &name : {QStringLiteral("konsole"), QStringLiteral("org.kde.konsole"), QStringLiteral("Alacritty"), QStringLiteral("org.wezfurlong.wezterm"), QStringLiteral("gnome-terminal-server"), QStringLiteral("com.mitchellh.ghostty")}) QVERIFY(terminal(name));
        QVERIFY(!terminal(QStringLiteral("org.kde.kwrite")));
        QVERIFY(!terminal(QString()));
        QVERIFY(!terminal(QStringLiteral("not-konsole")));
    }
};
QTEST_APPLESS_MAIN(ClipboardStackTest)
#include "clipboard-stack-test.moc"
