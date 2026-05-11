// Smoke test for SftpClient against a real public SFTP server.
// Server: test.rebex.net:22  user: demo  password: password
//
// Build is handled inline below; this file is NOT part of the app target.
#include "../src/SftpClient.h"
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    RemoteConnection c;
    c.protocol = "SFTP";
    c.host     = "test.rebex.net";
    c.port     = 22;
    c.username = "demo";
    c.password = "password";

    SftpClient cli;
    qInfo().noquote() << "[test] connecting to" << c.host << "as" << c.username;
    bool ok = cli.connectAndAuth(c, /*openSftpChannel=*/true, /*timeoutMs=*/10000);
    qInfo().noquote() << "[test] ok =" << ok;
    qInfo().noquote() << "[test] fingerprint =" << cli.hostFingerprintSha256();
    qInfo().noquote() << "[test] lastError  =" << cli.lastError();

    // Negative case: wrong password must fail cleanly.
    RemoteConnection bad = c;
    bad.password = "definitely-wrong-" + QString::number((quint64)(qintptr)&cli);
    SftpClient cli2;
    bool ok2 = cli2.connectAndAuth(bad, true, 10000);
    qInfo().noquote() << "[test] wrong-password ok =" << ok2
                      << "err =" << cli2.lastError();

    // Negative case: bogus host must fail cleanly.
    RemoteConnection nohost = c;
    nohost.host = "no-such-host.invalid.example";
    SftpClient cli3;
    bool ok3 = cli3.connectAndAuth(nohost, true, 4000);
    qInfo().noquote() << "[test] bad-host ok =" << ok3
                      << "err =" << cli3.lastError();

    return (ok && !ok2 && !ok3) ? 0 : 1;
}
