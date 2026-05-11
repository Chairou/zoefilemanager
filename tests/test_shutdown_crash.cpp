// Regression test for the SIGSEGV inside pthread_rwlock_rdlock that used
// to happen at process exit when a live SFTP session was still owned by
// FileSystemRouter (dyld unload of libcrypto raced C++ static destruction).
//
// The fix: FileSystemRouter::~FileSystemRouter calls SftpClient::markShuttingDown,
// and SftpClient::disconnect degrades to a plain ::close() in that state.
// Calling clear() via aboutToQuit is the *preferred* path; this test
// exercises the fallback (no explicit clear) to prove we no longer crash.
#include "../src/FileSystemRouter.h"
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

    auto client = std::make_unique<SftpClient>();
    bool ok = client->connectAndAuth(c, true, 10000);
    if (!ok) { qCritical() << "connect failed:" << client->lastError(); return 2; }

    FileSystemRouter::instance().mount("sftp://demo@test.rebex.net:22",
                                        std::move(client));

    // Touch it to make sure the session was used, as in the real app.
    auto entries = FileSystemRouter::instance().listDirectory("sftp://demo@test.rebex.net:22/");
    qInfo() << "remote root entries =" << entries.size();

    // Deliberately DO NOT call FileSystemRouter::instance().clear() here.
    // Let the process exit naturally so the static destructor runs exactly
    // like it did in the crash report. Before the fix, this crashed in
    // libssh2_sftp_shutdown -> RAND_bytes_ex -> pthread_rwlock_rdlock(NULL).
    qInfo() << "exiting - static destructors will run now";
    return 0;
}
