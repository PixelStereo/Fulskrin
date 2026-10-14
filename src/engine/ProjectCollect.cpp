// Collect Files: the project and every file it uses gathered in one folder (Engine.h, CollectPlan).
#include "EngineInternal.h"
#include "Isf.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <algorithm>
#include <functional>

namespace {

QString folderFor(const QString &path)
{
    static const QSet<QString> movies = {"mov", "mp4", "m4v", "avi", "mkv", "mxf", "webm", "mpg", "mpeg", "nut",
                                         "wmv", "ts", "mts", "m2ts", "flv", "3gp", "ogv", "dv", "hap"};
    static const QSet<QString> picts = {"png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "webp", "exr", "psd",
                                        "tga", "heic", "heif", "svg", "dds", "hdr", "jp2"};
    static const QSet<QString> audio = {"wav", "aif", "aiff", "mp3", "aac", "m4a", "flac", "ogg", "opus", "wma", "caf"};
    static const QSet<QString> isf = {"fs", "frag", "vs", "vert"};
    const QString ext = QFileInfo(path).suffix().toLower();
    if (movies.contains(ext)) return QStringLiteral("MOVIES");
    if (picts.contains(ext)) return QStringLiteral("PICTS");
    if (audio.contains(ext)) return QStringLiteral("AUDIO");
    if (isf.contains(ext)) return QStringLiteral("ISF");
    return QStringLiteral("OTHER");
}

bool isShader(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("fs") || ext == QLatin1String("frag");
}

// The same file, whatever the spelling of its path
QString key(const QString &path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : canonical;
}

// Every string of the project, its key and the object it is in visited (read-only)
void visit(const QJsonValue &v, const QString &key, const QJsonObject *in,
           const std::function<void(const QString &, const QString &, const QJsonObject *)> &fn)
{
    if (v.isString()) fn(key, v.toString(), in);
    else if (v.isArray())
        for (const QJsonValue &x : v.toArray()) visit(x, key, in, fn);
    else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) visit(it.value(), it.key(), &o, fn);
    }
}

// The same tree, its strings that are a collected file's path pointing to the copy; "relative_path" next to them
// made again from the new project's folder
QJsonValue relocate(const QJsonValue &v, const QHash<QString, QString> &to, const QDir &projectDir)
{
    if (v.isString()) {
        const QString s = v.toString();
        if (QDir::isAbsolutePath(s)) {
            const auto it = to.constFind(key(s));
            if (it != to.constEnd()) return *it;
        }
        return v;
    }
    if (v.isArray()) {
        QJsonArray a;
        for (const QJsonValue &x : v.toArray()) a.append(relocate(x, to, projectDir));
        return a;
    }
    if (!v.isObject()) return v;
    QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) it.value() = relocate(it.value(), to, projectDir);
    if (o.contains("relative_path") && o.value("path").isString()) // from the new project's folder
        o["relative_path"] = projectDir.relativeFilePath(o.value("path").toString());
    return o;
}

} // namespace

Engine::CollectPlan Engine::planCollect(const QString &parent, const QString &name, const QJsonObject &uiState) const
{
    CollectPlan plan;
    const QString base = name.trimmed().isEmpty() ? QStringLiteral("Project") : name.trimmed();
    plan.folder = QDir::cleanPath(QDir(parent).absoluteFilePath(base));
    plan.projectFile = plan.folder + '/' + base + QStringLiteral(".pinol");
    const QDir projectDir(plan.folder);
    const QString media = plan.folder + QStringLiteral("/MEDIA");

    const QJsonObject root = projectToJson(plan.folder, uiState);
    const QString oldPath = projectPath();
    const QDir oldDir = oldPath.isEmpty() ? QDir() : QFileInfo(oldPath).absoluteDir();

    // What the project refers to: the paths of its files (absolute; a relative one beside a path that is missing
    // resolved from the project's own folder), a layer's transition, an ISF's images
    QStringList found;
    QSet<QString> seenFound, seenMissing;
    QHash<QString, QString> alias; // a path that is not there → the file found from its relative path
    visit(root, QString(), nullptr, [&](const QString &k, const QString &s, const QJsonObject *in) {
        if (s.isEmpty()) return;
        if (k == QLatin1String("relative_path")) return; // with its "path"
        QString file;
        if (QDir::isAbsolutePath(s) && QFileInfo(s).isFile()) {
            file = s;
        } else if (k == QLatin1String("path") && QDir::isAbsolutePath(s)) {
            const QString rel = in ? in->value("relative_path").toString() : QString();
            const QString alt = rel.isEmpty() || oldPath.isEmpty() ? QString() : oldDir.absoluteFilePath(rel);
            if (!alt.isEmpty() && QFileInfo(alt).isFile()) {
                file = alt;
                alias.insert(key(s), alt);
            } else if (!seenMissing.contains(s)) {
                seenMissing.insert(s);
                plan.missing << s;
            }
        }
        if (file.isEmpty() || seenFound.contains(key(file))) return;
        seenFound.insert(key(file));
        found << file;
    });

    // Their places: by kind, a name taken once (another file of the same name gets " 2"); a shader that imports
    // images in a folder of its own with them, at the same relative places
    QSet<QString> taken; // destination paths, lower case (case-insensitive file systems)
    auto freeName = [&](const QString &folder, const QString &fileName) {
        const QFileInfo fi(fileName);
        const QString stem = fi.completeBaseName(), ext = fi.suffix();
        for (int n = 1;; ++n) {
            const QString candidate = n == 1 ? fileName : QStringLiteral("%1 %2%3").arg(stem).arg(n).arg(ext.isEmpty() ? QString() : '.' + ext);
            const QString full = folder + '/' + candidate;
            if (!taken.contains(full.toLower())) {
                taken.insert(full.toLower());
                return full;
            }
        }
    };
    QHash<QString, QString> to; // key of the original → new absolute path
    auto add = [&](const QString &from, const QString &dest) {
        if (to.contains(key(from))) return;
        to.insert(key(from), dest);
        CollectFile f;
        f.from = QFileInfo(from).absoluteFilePath();
        f.to = dest;
        f.bytes = QFileInfo(from).size();
        plan.bytes += f.bytes;
        plan.files.push_back(f);
    };
    // Shaders first: their companions keep their names next to them
    for (const QString &file : found) {
        if (!isShader(file)) continue;
        const auto deps = IsfInstance::dependencies(file);
        bool imports = false;
        for (const auto &[abs, rel] : deps)
            if (!rel.endsWith(QLatin1String(".vs")) && !rel.endsWith(QLatin1String(".vert"))) imports = true;
        const QString stem = QFileInfo(file).completeBaseName();
        QString folder = media + QStringLiteral("/ISF");
        if (imports) { // a folder of its own: its images keep their places beside it
            const QString own = freeName(folder, stem);
            folder = own;
        }
        const QString dest = imports ? folder + '/' + QFileInfo(file).fileName() : freeName(folder, QFileInfo(file).fileName());
        if (imports) taken.insert(dest.toLower());
        add(file, dest);
        const QString destStem = QFileInfo(dest).completeBaseName();
        for (const auto &[abs, rel] : deps) {
            const bool vertex = rel.endsWith(QLatin1String(".vs")) || rel.endsWith(QLatin1String(".vert"));
            if (vertex) { // named as its shader is
                const QString vs = folder + '/' + destStem + '.' + QFileInfo(rel).suffix();
                taken.insert(vs.toLower());
                add(abs, vs);
            } else if (rel.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(rel)) {
                plan.skipped << abs; // outside its folder: where it is, the shader would not find it any more
            } else {
                const QString img = folder + '/' + rel;
                taken.insert(img.toLower());
                add(abs, img);
            }
        }
    }
    for (const QString &file : found) {
        if (isShader(file) || to.contains(key(file))) continue;
        add(file, freeName(media + '/' + folderFor(file), QFileInfo(file).fileName()));
    }
    for (auto it = alias.cbegin(); it != alias.cend(); ++it) // its old spelling points to the copy too
        if (to.contains(key(it.value()))) to.insert(it.key(), to.value(key(it.value())));
    QJsonObject relocated = relocate(root, to, projectDir).toObject();
    plan.project = relocated;
    return plan;
}

bool Engine::collect(const CollectPlan &plan, QString *err)
{
    auto fail = [err](const QString &why) {
        if (err) *err = why;
        return false;
    };
    if (plan.folder.isEmpty() || plan.projectFile.isEmpty()) return fail(QStringLiteral("No folder."));
    QDir parent = QFileInfo(plan.folder).absoluteDir();
    if (QFileInfo::exists(plan.folder))
        return fail(QStringLiteral("%1 exists already: choose another name or place.").arg(plan.folder));
    if (!parent.mkpath(QFileInfo(plan.folder).fileName())) return fail(QStringLiteral("Cannot create %1.").arg(plan.folder));
    auto undo = [&] { // a half-collected folder is not left behind
        QDir(plan.folder).removeRecursively();
    };
    for (const CollectFile &f : plan.files) {
        if (!QDir().mkpath(QFileInfo(f.to).absolutePath())) {
            undo();
            return fail(QStringLiteral("Cannot create %1.").arg(QFileInfo(f.to).absolutePath()));
        }
        if (!QFile::copy(f.from, f.to)) {
            undo();
            return fail(QStringLiteral("Cannot copy %1 to %2.").arg(f.from, f.to));
        }
    }
    QSaveFile out(plan.projectFile);
    if (!out.open(QIODevice::WriteOnly)) {
        undo();
        return fail(out.errorString());
    }
    out.write(QJsonDocument(plan.project).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        undo();
        return fail(out.errorString());
    }
    return true;
}

int Engine::removeCollectedOriginals(const CollectPlan &plan, QStringList *failed)
{
    int removed = 0;
    for (const CollectFile &f : plan.files) {
        if (!QFileInfo::exists(f.to)) continue; // never without its copy
        if (key(f.from) == key(f.to)) continue;
        if (QFile::remove(f.from)) ++removed;
        else if (failed) *failed << f.from;
    }
    return removed;
}
