#ifndef PZTOOLS_TILESETFILEINDEX_H
#define PZTOOLS_TILESETFILEINDEX_H

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

namespace PZTools {

class TilesetFileIndex
{
public:
    QString findImage(const QDir &root, const QString &fileName,
                      const QString &excludedTree = QString())
    {
        const QString excludedPrefix = excludedTree.isEmpty()
                ? QString()
                : QDir::cleanPath(QFileInfo(excludedTree).absoluteFilePath())
                  + QDir::separator();
        QStringList candidates;
        const QStringList matchingFiles = filesNamed(root, fileName);
        for (const QString &candidate : matchingFiles) {
            if (!excludedPrefix.isEmpty()
                    && candidate.startsWith(excludedPrefix, Qt::CaseInsensitive)) {
                continue;
            }
            if (QImageReader(candidate).size().isValid())
                candidates += candidate;
        }
        candidates.sort(Qt::CaseInsensitive);
        return candidates.isEmpty() ? QString() : candidates.first();
    }

    void invalidate()
    {
        QMutexLocker locker(&mMutex);
        mTrees.clear();
    }

private:
    struct DirectoryTree
    {
        QHash<QString, QStringList> filesByName;
        QHash<QString, QDateTime> directoryTimes;
        QElapsedTimer validated;
    };

    static const int revalidationIntervalMs = 500;

    QStringList filesNamed(const QDir &root, const QString &fileName)
    {
        const QString rootPath = root.absolutePath();
        if (fileName.contains(QLatin1Char('*')) || fileName.contains(QLatin1Char('?'))
                || fileName.contains(QLatin1Char('[')))
            return scanFilesNamed(rootPath, fileName);

        QMutexLocker locker(&mMutex);
        DirectoryTree &tree = mTrees[rootPath];
        if (!isCurrent(tree))
            rebuild(tree, rootPath);
        return tree.filesByName.value(fileName.toCaseFolded());
    }

    static QStringList scanFilesNamed(const QString &rootPath, const QString &fileName)
    {
        QStringList files;
        QDirIterator iterator(rootPath, QStringList() << fileName, QDir::Files,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext())
            files += QDir::cleanPath(QFileInfo(iterator.next()).absoluteFilePath());
        return files;
    }

    static bool isCurrent(DirectoryTree &tree)
    {
        if (!tree.validated.isValid())
            return false;
        if (tree.validated.elapsed() < revalidationIntervalMs)
            return true;
        for (auto directory = tree.directoryTimes.cbegin();
             directory != tree.directoryTimes.cend(); ++directory) {
            if (QFileInfo(directory.key()).lastModified() != directory.value())
                return false;
        }
        tree.validated.restart();
        return true;
    }

    static void rebuild(DirectoryTree &tree, const QString &rootPath)
    {
        tree.filesByName.clear();
        tree.directoryTimes.clear();
        tree.directoryTimes.insert(rootPath, QFileInfo(rootPath).lastModified());
        QDirIterator directories(rootPath, QDir::Dirs | QDir::NoDotAndDotDot,
                                 QDirIterator::Subdirectories);
        while (directories.hasNext()) {
            const QString directoryPath = directories.next();
            tree.directoryTimes.insert(directoryPath, QFileInfo(directoryPath).lastModified());
        }
        QDirIterator files(rootPath, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QFileInfo file(files.next());
            tree.filesByName[file.fileName().toCaseFolded()]
                    += QDir::cleanPath(file.absoluteFilePath());
        }
        tree.validated.start();
    }

    QMutex mMutex;
    QHash<QString, DirectoryTree> mTrees;
};

}

#endif
