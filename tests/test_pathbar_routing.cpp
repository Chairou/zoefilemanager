// Unit-ish test: validates the routing logic the PathBar / FileListView
// rely on when decomposing a remote URL into breadcrumbs and ".." targets.
//
// We don't spin up a real SSH session here - we just mount a fake client
// (uninitialized SftpClient) to register the prefix in the router. None
// of the paths below actually touch the network.
#include "../src/FileSystemRouter.h"
#include "../src/SftpClient.h"
#include <QCoreApplication>
#include <QDebug>

static int rc = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { qCritical().noquote() << "FAIL:" << msg; rc = 1; } \
    else         { qInfo().noquote()    << " ok :" << msg; } \
} while (0)

// Replicates the ".." parent computation in FileListView.cpp.
static QString computeParent(const QString& current) {
    QString mount = FileSystemRouter::instance().mountFor(current);
    QString mountRoot = mount.isEmpty() ? QString() : (mount + "/");
    bool atMountRoot = !mount.isEmpty() &&
                       (current == mountRoot || current == mount);
    if (atMountRoot) return QString();  // ".." should not exist
    QString parent = current.left(current.lastIndexOf('/'));
    if (parent.isEmpty()) parent = "/";
    if (!mount.isEmpty() && parent == mount) parent += "/";
    return parent;
}

// Replicates the breadcrumb decomposition in PathBar::rebuildButtons.
static QStringList computeCrumbTargets(const QString& path) {
    QString mount = FileSystemRouter::instance().mountFor(path);
    QString localRoot, relPath;
    if (!mount.isEmpty()) {
        localRoot = mount + "/";
        relPath   = path.mid(mount.size());
        if (relPath.startsWith('/')) relPath = relPath.mid(1);
    } else {
        localRoot = "/";
        relPath = (path == "/") ? QString() : path.mid(1);
    }
    QStringList crumbs;
    crumbs << localRoot;
    QString acc = localRoot;
    if (acc.endsWith('/')) acc.chop(1);
    for (const auto& part : relPath.split('/', Qt::SkipEmptyParts)) {
        acc += "/" + part;
        crumbs << acc;
    }
    return crumbs;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const QString mount = "sftp://demo@test.rebex.net:22";
    // Register a stub client just to anchor the prefix in the router.
    // We never call any network method on it.
    FileSystemRouter::instance().mount(mount, std::make_unique<SftpClient>());

    // ---- breadcrumbs ----
    auto crumbs1 = computeCrumbTargets(mount + "/");
    qInfo() << "crumbs at remote root:" << crumbs1;
    CHECK(crumbs1.size() == 1 && crumbs1[0] == mount + "/",
          "remote root: only root crumb, target is mount+'/'");

    auto crumbs2 = computeCrumbTargets(mount + "/pub/example");
    qInfo() << "crumbs at /pub/example:" << crumbs2;
    CHECK(crumbs2.size() == 3,                                   "3 crumbs");
    CHECK(crumbs2[0] == mount + "/",                              "crumb[0] = mount root");
    CHECK(crumbs2[1] == mount + "/pub",                           "crumb[1] = mount+/pub");
    CHECK(crumbs2[2] == mount + "/pub/example",                   "crumb[2] = full path");

    auto crumbs3 = computeCrumbTargets("/Users/chairou");
    qInfo() << "crumbs at local /Users/chairou:" << crumbs3;
    CHECK(crumbs3.size() == 3 && crumbs3[0] == "/"
          && crumbs3[1] == "/Users" && crumbs3[2] == "/Users/chairou",
          "local path crumbs unchanged");

    // ---- ".." targets ----
    CHECK(computeParent(mount + "/").isEmpty(),
          "at mount root -> no '..' shown");
    CHECK(computeParent(mount + "/pub") == mount + "/",
          "one level deep -> parent is mount root (with trailing slash)");
    CHECK(computeParent(mount + "/pub/example") == mount + "/pub",
          "two levels deep -> strip last segment, keep mount");
    CHECK(computeParent("/Users/chairou") == "/Users",
          "local parent unchanged");
    CHECK(computeParent("/Users") == "/",
          "local parent of /Users is /");

    FileSystemRouter::instance().unmount(mount);
    qInfo() << (rc == 0 ? "ALL PASSED" : "SOME FAILED");
    return rc;
}
