// End-to-end test for the FileSystemRouter against a live SFTP server.
// Server: test.rebex.net:22  user: demo  password: password
//
// Validates:
//   1. mount() + listDirectory() on the mount root returns entries
//   2. sub-directory navigation returns plausible results
//   3. isDirectory / exists work for remote paths
//   4. after unmount(), the same path no longer resolves as remote and
//      RealFileSystem takes over (falls through to local "/" listing).
#include "../src/FileSystemRouter.h"
#include "../src/SftpClient.h"
#include <QCoreApplication>
#include <QDebug>

static int rc = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { qCritical().noquote() << "FAIL:" << msg; rc = 1; } \
    else         { qInfo().noquote()    << " ok :" << msg; } \
} while (0)

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    RemoteConnection c;
    c.protocol = "SFTP";
    c.host     = "test.rebex.net";
    c.port     = 22;
    c.username = "demo";
    c.password = "password";

    auto client = std::make_unique<SftpClient>();
    bool ok = client->connectAndAuth(c, /*openSftpChannel=*/true, 10000);
    CHECK(ok, "SftpClient connectAndAuth()");
    if (!ok) { qCritical() << client->lastError(); return 1; }

    const QString mount = "sftp://demo@test.rebex.net:22";
    FileSystemRouter::instance().mount(mount, std::move(client));
    CHECK(FileSystemRouter::instance().isRemote(mount + "/"),         "isRemote(mount+'/')");
    CHECK(FileSystemRouter::instance().mountFor(mount + "/foo") == mount, "mountFor(path)");

    // 1) list remote root
    auto root = FileSystemRouter::instance().listDirectory(mount + "/");
    qInfo() << "remote root entries =" << root.size();
    CHECK(!root.isEmpty(), "listDirectory(remote root) non-empty");
    for (const auto& e : root) {
        qInfo().noquote() << "   " << (e.isDirectory ? "d" : "-")
                          << e.permissions << e.size << e.path;
    }

    // 2) navigate into the first directory
    QString sub;
    for (const auto& e : root) if (e.isDirectory) { sub = e.path; break; }
    if (!sub.isEmpty()) {
        auto subEntries = FileSystemRouter::instance().listDirectory(sub);
        qInfo().noquote() << "remote subdir" << sub << "entries =" << subEntries.size();
        CHECK(FileSystemRouter::instance().isDirectory(sub), "isDirectory(sub)");
        CHECK(FileSystemRouter::instance().exists(sub),      "exists(sub)");
    } else {
        CHECK(false, "no directory found under remote root");
    }

    // 3) getSubDirectories reflects only directories
    auto subDirs = FileSystemRouter::instance().getSubDirectories(mount + "/");
    qInfo() << "getSubDirectories count =" << subDirs.size();
    for (const auto& d : subDirs) CHECK(d.startsWith(mount), "subdir prefixed with mount");

    // 4) unmount -> same path should now fall through to local FS and thus
    //    NOT claim to be remote any more.
    FileSystemRouter::instance().unmount(mount);
    CHECK(!FileSystemRouter::instance().isRemote(mount + "/"), "after unmount, !isRemote");

    qInfo() << (rc == 0 ? "ALL PASSED" : "SOME FAILED");
    return rc;
}
