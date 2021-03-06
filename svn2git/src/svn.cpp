/*
 *  Copyright (C) 2007  Thiago Macieira <thiago@kde.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Based on svn-fast-export by Chris Lee <clee@kde.org>
 * License: MIT <http://www.opensource.org/licenses/mit-license.php>
 * URL: git://repo.or.cz/fast-import.git http://repo.or.cz/w/fast-export.git
 */

#define _XOPEN_SOURCE
#define _LARGEFILE_SUPPORT
#define _LARGEFILE64_SUPPORT

#include "svn.h"
#include "CommandLineParser.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <apr_lib.h>
#include <apr_getopt.h>
#include <apr_general.h>

#include <svn_fs.h>
#include <svn_pools.h>
#include <svn_repos.h>
#include <svn_types.h>
#include <svn_version.h>

#include <QDir>
#include <QFile>
#include <QDebug>
#include <QRegularExpression>

#include "repository.h"

#undef SVN_ERR
#define SVN_ERR(expr) SVN_INT_ERR(expr)

#if SVN_VER_MAJOR == 1 && SVN_VER_MINOR < 9
#define svn_stream_read_full svn_stream_read
#endif

typedef QList<Rules::Match> MatchRuleList;
typedef QHash<QString, Repository *> RepositoryHash;
typedef QHash<QByteArray, QByteArray> IdentityHash;

class AprAutoPool
{
    apr_pool_t *pool;
    AprAutoPool(const AprAutoPool &);
    AprAutoPool &operator=(const AprAutoPool &);
public:
    inline AprAutoPool(apr_pool_t *parent = NULL)
        {
            pool = svn_pool_create(parent);
        }
        inline ~AprAutoPool()
        {
            svn_pool_destroy(pool);
        }

    inline void clear() { svn_pool_clear(pool); }
    inline apr_pool_t *data() const { return pool; }
    inline operator apr_pool_t *() const { return pool; }
};

class SvnPrivate
{
public:
    QList<MatchRuleList> allMatchRules;
    RepositoryHash repositories;
    IdentityHash identities;
    QString userdomain;

    SvnPrivate(const QString &pathToRepository);
    ~SvnPrivate();
    int youngestRevision();
    int exportRevision(int revnum);

    int openRepository(const QString &pathToRepository);

private:
    AprAutoPool global_pool;
    AprAutoPool scratch_pool;
    svn_fs_t *fs;
    svn_revnum_t youngest_rev;
    QString svn_repo_path;
};

void Svn::initialize()
{
    // initialize APR or exit
    if (apr_initialize() != APR_SUCCESS) {
        fprintf(stderr, "You lose at apr_initialize().\n");
        exit(1);
    }

    // static destructor
    static struct Destructor { ~Destructor() { apr_terminate(); } } destructor;
}

Svn::Svn(const QString &pathToRepository)
    : d(new SvnPrivate(pathToRepository))
{
}

Svn::~Svn()
{
    delete d;
}

void Svn::setMatchRules(const QList<MatchRuleList> &allMatchRules)
{
    d->allMatchRules = allMatchRules;
}

void Svn::setRepositories(const RepositoryHash &repositories)
{
    d->repositories = repositories;
}

void Svn::setIdentityMap(const IdentityHash &identityMap)
{
    d->identities = identityMap;
}

void Svn::setIdentityDomain(const QString &identityDomain)
{
    d->userdomain = identityDomain;
}

int Svn::youngestRevision()
{
    return d->youngestRevision();
}

bool Svn::exportRevision(int revnum)
{
    return d->exportRevision(revnum) == EXIT_SUCCESS;
}

SvnPrivate::SvnPrivate(const QString &pathToRepository)
    : global_pool(NULL) , scratch_pool(NULL), svn_repo_path(pathToRepository)
{
    if( openRepository(pathToRepository) != EXIT_SUCCESS) {
        qCritical() << "Failed to open repository";
        exit(1);
    }
    svn_repo_path.prepend("file:///");

    // get the youngest revision
    svn_fs_youngest_rev(&youngest_rev, fs, global_pool);
}

SvnPrivate::~SvnPrivate() {}

int SvnPrivate::youngestRevision()
{
    return youngest_rev;
}

int SvnPrivate::openRepository(const QString &pathToRepository)
{
    svn_repos_t *repos;
    QString path = pathToRepository;
    while (path.endsWith('/')) // no trailing slash allowed
        path = path.mid(0, path.length()-1);
#if SVN_VER_MAJOR == 1 && SVN_VER_MINOR < 9
    SVN_ERR(svn_repos_open2(&repos, QFile::encodeName(path), NULL, global_pool));
#else
    SVN_ERR(svn_repos_open3(&repos, QFile::encodeName(path), NULL, global_pool, scratch_pool));
#endif
    fs = svn_repos_fs(repos);

    return EXIT_SUCCESS;
}

enum RuleType { AnyRule = 0, NoIgnoreRule = 0x01, NoRecurseRule = 0x02 };

static MatchRuleList::ConstIterator
findMatchRule(const MatchRuleList &matchRules, int revnum, const QString &current,
              int ruleMask = AnyRule)
{
    MatchRuleList::ConstIterator it = matchRules.constBegin(),
                                end = matchRules.constEnd();
    for ( ; it != end; ++it) {
        if (it->minRevision > revnum)
            continue;
        if (it->maxRevision != -1 && it->maxRevision < revnum)
            continue;
        if (it->action == Rules::Match::Ignore && ruleMask & NoIgnoreRule)
            continue;
        if (it->action == Rules::Match::Recurse && ruleMask & NoRecurseRule)
            continue;
        if (it->rx.indexIn(current) == 0) {
            Stats::instance()->ruleMatched(*it, revnum);
            return it;
        }
    }

    // no match
    return end;
}

static int pathMode(svn_fs_root_t *fs_root, const char *pathname, apr_pool_t *pool)
{
    svn_string_t *propvalue;
    SVN_ERR(svn_fs_node_prop(&propvalue, fs_root, pathname, "svn:executable", pool));
    int mode = 0100644;
    if (propvalue)
        mode = 0100755;

    return mode;
}

svn_error_t *QIODevice_write(void *baton, const char *data, apr_size_t *len)
{
    QIODevice *device = reinterpret_cast<QIODevice *>(baton);
    device->write(data, *len);

    while (device->bytesToWrite() > 32*1024) {
        if (!device->waitForBytesWritten(-1)) {
            qFatal("Failed to write to process: %s", qPrintable(device->errorString()));
            return svn_error_createf(APR_EOF, SVN_NO_ERROR, "Failed to write to process: %s",
                                     qPrintable(device->errorString()));
        }
    }
    return SVN_NO_ERROR;
}

static svn_stream_t *streamForDevice(QIODevice *device, apr_pool_t *pool)
{
    svn_stream_t *stream = svn_stream_create(device, pool);
    svn_stream_set_write(stream, QIODevice_write);

    return stream;
}

static int dumpBlob(Repository::Transaction *txn, svn_fs_root_t *fs_root,
                    const char *pathname, const QString &finalPathName, apr_pool_t *pool)
{
    AprAutoPool dumppool(pool);
    // what type is it?
    int mode = pathMode(fs_root, pathname, dumppool);

    svn_filesize_t stream_length;

    SVN_ERR(svn_fs_file_length(&stream_length, fs_root, pathname, dumppool));

    svn_stream_t *in_stream, *out_stream;
    if (!CommandLineParser::instance()->contains("dry-run")) {
        // open the file
        SVN_ERR(svn_fs_file_contents(&in_stream, fs_root, pathname, dumppool));
    }

    // maybe it's a symlink?
    svn_string_t *propvalue;
    SVN_ERR(svn_fs_node_prop(&propvalue, fs_root, pathname, "svn:special", dumppool));
    if (propvalue) {
        apr_size_t len = strlen("link ");
        if (!CommandLineParser::instance()->contains("dry-run")) {
            QByteArray buf;
            buf.reserve(len);
            SVN_ERR(svn_stream_read_full(in_stream, buf.data(), &len));
            if (len == strlen("link ") && strncmp(buf, "link ", len) == 0) {
                mode = 0120000;
                stream_length -= len;
            } else {
                //this can happen if a link changed into a file in one commit
                qWarning("file %s is svn:special but not a symlink", pathname);
                // re-open the file as we tried to read "link "
                svn_stream_close(in_stream);
                SVN_ERR(svn_fs_file_contents(&in_stream, fs_root, pathname, dumppool));
            }
        }
    }

    QIODevice *io = txn->addFile(finalPathName, mode, stream_length);

    if (!CommandLineParser::instance()->contains("dry-run")) {
        // open a generic svn_stream_t for the QIODevice
        out_stream = streamForDevice(io, dumppool);
        SVN_ERR(svn_stream_copy3(in_stream, out_stream, NULL, NULL, dumppool));

        // print an ending newline
        io->putChar('\n');
    }

    return EXIT_SUCCESS;
}

static bool wasDir(svn_fs_t *fs, int revnum, const char *pathname, apr_pool_t *pool)
{
    AprAutoPool subpool(pool);
    svn_fs_root_t *fs_root;
    if (svn_fs_revision_root(&fs_root, fs, revnum, subpool) != SVN_NO_ERROR)
        return false;

    svn_boolean_t is_dir;
    if (svn_fs_is_dir(&is_dir, fs_root, pathname, subpool) != SVN_NO_ERROR)
        return false;

    return is_dir;
}

static int recursiveDumpDir(Repository::Transaction *txn, svn_fs_t *fs, svn_fs_root_t *fs_root,
                            const QByteArray &pathname, const QString &finalPathName,
                            apr_pool_t *pool, svn_revnum_t revnum,
                            const Rules::Match &rule, const MatchRuleList &matchRules,
                            bool ruledebug)
{
    if (!wasDir(fs, revnum, pathname.data(), pool)) {
        if (dumpBlob(txn, fs_root, pathname, finalPathName, pool) == EXIT_FAILURE)
            return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }

    // get the dir listing
    apr_hash_t *entries;
    SVN_ERR(svn_fs_dir_entries(&entries, fs_root, pathname, pool));
    AprAutoPool dirpool(pool);

    // While we get a hash, put it in a map for sorted lookup, so we can
    // repeat the conversions and get the same git commit hashes.
    QMap<QByteArray, svn_node_kind_t> map;
    for (apr_hash_index_t *i = apr_hash_first(pool, entries); i; i = apr_hash_next(i)) {
        const void *vkey;
        void *value;
        apr_hash_this(i, &vkey, NULL, &value);
        svn_fs_dirent_t *dirent = reinterpret_cast<svn_fs_dirent_t *>(value);
        map.insertMulti(QByteArray(dirent->name), dirent->kind);
    }

    QMapIterator<QByteArray, svn_node_kind_t> i(map);
    while (i.hasNext()) {
        dirpool.clear();
        i.next();
        QByteArray entryName = pathname + '/' + i.key();
        QString entryFinalName = finalPathName + QString::fromUtf8(i.key());

        if (i.value() == svn_node_dir) {
            entryFinalName += '/';
            QString entryNameQString = entryName + '/';

            MatchRuleList::ConstIterator match = findMatchRule(matchRules, revnum, entryNameQString);
            if (match == matchRules.constEnd()) continue; // no match of parent repo? (should not happen)

            const Rules::Match &matchedRule = *match;
            if (matchedRule.action != Rules::Match::Export || matchedRule.repository != rule.repository) {
                if (ruledebug)
                    qDebug() << "recursiveDumpDir:" << entryNameQString << "skip entry for different/ignored repository";
                continue;
            }

            if (recursiveDumpDir(txn, fs, fs_root, entryName, entryFinalName, dirpool, revnum, rule, matchRules, ruledebug) == EXIT_FAILURE)
                return EXIT_FAILURE;
        } else if (i.value() == svn_node_file) {
            printf("+");
            fflush(stdout);
            if (dumpBlob(txn, fs_root, entryName, entryFinalName, dirpool) == EXIT_FAILURE)
                return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

time_t get_epoch(const char* svn_date)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    QByteArray date(svn_date, strlen(svn_date) - 8);
    strptime(date, "%Y-%m-%dT%H:%M:%S", &tm);
    return timegm(&tm);
}

class SvnRevision
{
public:
    AprAutoPool pool;
    QMap<QString, Repository::Transaction *> transactions;
    QList<MatchRuleList> allMatchRules;
    RepositoryHash repositories;
    IdentityHash identities;
    QString userdomain;

    svn_fs_t *fs;
    svn_fs_root_t *fs_root;
    int revnum;

    // must call fetchRevProps first:
    QByteArray authorident;
    QByteArray log;
    uint epoch;
    bool ruledebug;
    bool propsFetched;
    bool needCommit;

    QSet<int> logged_already_;

    QString& svn_repo_path;
    QString merge_from_branch_;
    QString merge_from_rev_;
    QSet<QString> to_branches_;

    QMap<QString, QSet<QString>> deletions_;

    // There are some handful of mergeinfo changes that are bogus and need to be skipped
    // r306199 - Revert svn:mergeinfo added inadvertantly in last commit r306197
    // r305318 - Handle missed mergeinfo by merging r305031 (the missing revision according to svn merge)
    // r299143
    // r288036 - Fixup mergeinfo on sys/contrib/ipfilter/netinet/ip_fil_freebsd.c.
    // r287679
    // r281784
    // r276402 - Remove "svn:mergeinfo" property that was dragged along when these files were svn copied in r273375.
    // r273097
    // r272415
    // r262613
    // r255958 - Add missing mergeinfo associated with r255852.
    // r208239 - Adjust svn:mergeinfo for revision 204546.  This commit moves mergeinfo to lib/ and removes mergeinfo on individual file.
    // r206971 - remove svn:mergeinfo properties committed during my MFCs.
    // r203808 - Migrate mergeinfo which was done on wrong target back to etc/ (203163).
    // r203516 - Fix mergeinfo from r197799
    // r202105
    // r198498 - Trim empty mergeinfo.
    // r197807 - Properly record merginfo for r197681 into lib/libc instead of lib/libc/gen.
    // r194300 
    // r190749 - Remove pointeless mergeinfo that crept in from r190633.
    // r190634 - Remove some pointless mergeinfo that is the result of doing a local 'svn cp'
    // r186082 - Bootstrapping merge history for resolver.
    // r185539 - Delete a bunch of empty mergeinfo records caused by local copies.
    // r182315 - Consolidate mergeinfo
    // r182274 - Move mergeinfo around.
    //
    // lots more, especially on stable/X branches

    SvnRevision(int revision, svn_fs_t *f, apr_pool_t *parent_pool, QString& svn_repo_path)
        : pool(parent_pool), fs(f), fs_root(0), revnum(revision), propsFetched(false), svn_repo_path(svn_repo_path)
    {
        ruledebug = CommandLineParser::instance()->contains( QLatin1String("debug-rules"));
    }

    int open()
    {
        SVN_ERR(svn_fs_revision_root(&fs_root, fs, revnum, pool));
        return EXIT_SUCCESS;
    }

    int prepareTransactions();
    int fetchRevProps();
    int commit();

    int exportEntry(const char *path, const svn_fs_path_change2_t *change, apr_hash_t *changes);
    int exportDispatch(const char *path, const svn_fs_path_change2_t *change,
                       const char *path_from, svn_revnum_t rev_from,
                       apr_hash_t *changes, const QString &current, const Rules::Match &rule,
                       const MatchRuleList &matchRules, apr_pool_t *pool);
    int exportInternal(const char *path, const svn_fs_path_change2_t *change,
                       const char *path_from, svn_revnum_t rev_from,
                       const QString &current, const Rules::Match &rule, const MatchRuleList &matchRules);
    int recurse(const char *path, const svn_fs_path_change2_t *change,
                const char *path_from, const MatchRuleList &matchRules, svn_revnum_t rev_from,
                apr_hash_t *changes, apr_pool_t *pool);
    int addGitIgnore(apr_pool_t *pool, const char *key, QString path,
                     svn_fs_root_t *fs_root, Repository::Transaction *txn, const char *content = NULL);
    int fetchIgnoreProps(QString *ignore, apr_pool_t *pool, const char *key, svn_fs_root_t *fs_root);
    int fetchUnknownProps(apr_pool_t *pool, const char *key, svn_fs_root_t *fs_root);
private:
    void splitPathName(const Rules::Match &rule, const QString &pathName, QString *svnprefix_p,
                       QString *repository_p, QString *effectiveRepository_p, QString *branch_p, QString *path_p);
    bool maybeParseSimpleMergeinfo(const int revnum, struct mi* mi);
};

int SvnPrivate::exportRevision(int revnum)
{
    SvnRevision rev(revnum, fs, global_pool, svn_repo_path);
    rev.allMatchRules = allMatchRules;
    rev.repositories = repositories;
    rev.identities = identities;
    rev.userdomain = userdomain;

    // open this revision:
    printf("Exporting revision %d ", revnum);
    fflush(stdout);

    if (rev.open() == EXIT_FAILURE)
        return EXIT_FAILURE;

    if (rev.prepareTransactions() == EXIT_FAILURE)
        return EXIT_FAILURE;

    if (!rev.needCommit) {
        printf(" nothing to do\n");
        return EXIT_SUCCESS;    // no changes?
    }

    if (rev.commit() == EXIT_FAILURE)
        return EXIT_FAILURE;

    printf(" done\n");
    return EXIT_SUCCESS;
}

void SvnRevision::splitPathName(const Rules::Match &rule, const QString &pathName, QString *svnprefix_p,
                                QString *repository_p, QString *effectiveRepository_p, QString *branch_p, QString *path_p)
{
    QString svnprefix = pathName;
    svnprefix.truncate(rule.rx.matchedLength());

    if (svnprefix_p) {
        *svnprefix_p = svnprefix;
    }

    if (repository_p) {
        *repository_p = svnprefix;
        repository_p->replace(rule.rx, rule.repository);
        foreach (Rules::Match::Substitution subst, rule.repo_substs) {
            subst.apply(*repository_p);
        }
    }

    if (effectiveRepository_p) {
        *effectiveRepository_p = svnprefix;
        effectiveRepository_p->replace(rule.rx, rule.repository);
        foreach (Rules::Match::Substitution subst, rule.repo_substs) {
            subst.apply(*effectiveRepository_p);
        }
        Repository *repository = repositories.value(*effectiveRepository_p, 0);
        if (repository) {
            *effectiveRepository_p = repository->getEffectiveRepository()->getName();
        }
    }

    if (branch_p) {
        *branch_p = svnprefix;
        branch_p->replace(rule.rx, rule.branch);
        foreach (Rules::Match::Substitution subst, rule.branch_substs) {
            subst.apply(*branch_p);
        }
    }

    if (path_p) {
        QString prefix = svnprefix;
        prefix.replace(rule.rx, rule.prefix);
        QString suffix = pathName.mid(svnprefix.length());
        if (suffix.startsWith(rule.strip))
            suffix.replace(0, rule.strip.length(), "");
        *path_p = prefix + suffix;
    }
}

bool SvnRevision::maybeParseSimpleMergeinfo(const int revnum, struct mi* mi) {
    QProcess svn;
    // svn diff -c 179481 --properties-only file:///$PWD/base
    svn.start("svn",
            QStringList() << "diff"
            << "-c" << QString::number(revnum)
            << "--properties-only" << svn_repo_path);

    if (!svn.waitForFinished(-1)) {
        fprintf(stderr, "svn fork terminated abnormally for rev %d\n", revnum);
        exit(1);
    }

    const QString result = QString(svn.readAll()).remove("\\ No newline at end of property\n");

    // If there are N mergeinfo hits, and they all look like so, we have fully
    // empty mergeinfo and can skip this rev.
    // Added: svn:mergeinfo
    // ## -0,0 +0,0 ##
    // or
    // Deleted: svn:mergeinfo
    // ## -0,0 +0,0 ##
    // see r183713 for an interesting case. Maybe we should just delete all the
    // above strings?
    int del_mi = result.count(QRegularExpression(R"(^Deleted: svn:mergeinfo$)", QRegularExpression::MultilineOption));
    int add_mi = result.count(QRegularExpression(R"(^Added: svn:mergeinfo$)", QRegularExpression::MultilineOption));
    int diff_mi = result.count(QRegularExpression(R"(^Modified: svn:mergeinfo$)", QRegularExpression::MultilineOption));

    int del_mi_empty = result.count(QRegularExpression(R"(^Deleted: svn:mergeinfo
## -0,0 \+0,0 ##$)", QRegularExpression::MultilineOption));
    int add_mi_empty = result.count(QRegularExpression(R"(^Added: svn:mergeinfo
## -0,0 \+0,0 ##$)", QRegularExpression::MultilineOption));
    int diff_mi_empty = result.count(QRegularExpression(R"(^Modified: svn:mergeinfo
## -0,0 \+0,0 ##$)", QRegularExpression::MultilineOption));

    if ((del_mi+add_mi+diff_mi) == 0) {
        qFatal("Something went wrong parsing the mergeinfo!");
    }

    if ((del_mi+add_mi+diff_mi) > 0 && del_mi == del_mi_empty && add_mi == add_mi_empty && diff_mi == diff_mi_empty) {
        printf(" ===Skipping empty mergeinfo on %d=== ", revnum);
        return true;
    }

    if (del_mi >0 && add_mi == 0 && diff_mi == 0) {
        printf(" ===Skipping delete-only (%d) mergeinfo on %d=== ", del_mi, revnum);
        return true;
    }

    qDebug() << "=START=";
    qDebug() << qPrintable(result);
    qDebug() << "=END=";
    qDebug() << "mergeinfo parsing: del/add/mod=" << del_mi << del_mi_empty << add_mi << add_mi_empty << diff_mi << diff_mi_empty;
    {
      QDir dir;
      if (dir.mkpath("mi")) {
        // This should create only about 3k files or so.
        QFile file(QString("mi/r%1.txt").arg(revnum));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
          QTextStream out(&file);
          out << qPrintable(result);
        }
      }
    }

    // NOTE: need to use a fully anchored match, otherwise e.g. r238926 gets
    // handled wrong, as it uses the first mergeinfo to deduce the merge-from,
    // which is incorrect and off-by-one! r240415 also merges up to 240357 but
    // ends up with 240326 instead. This reduces the "handled" mergeinfo from
    // 2000 out of 3000 down to 1125. The rest should be hard-coded.
    static QRegularExpression re = QRegularExpression(
           R"(^(Index: ([-_.\d\w/]+)
=============*
... ([-_.\d\w/]+).\([^)]+\)
... ([-_.\d\w/]+).\([^)]+\)

Property changes on: (?<path>[-_.\d\w/]+)
_____________*
((Added|Deleted): svn:(keywords|eol-style|mime-type)
## -[\d,]+ \+[\d,]+ ##
[-+].*
)*)*(Modified|Added): svn:mergeinfo
## \-0,0 \+0,1 ##
   Merged (?<from>[-_.\d\w/]+):r([0-9]*[-,])*(?<rev>[0-9]*)
*$)");
    if (!re.isValid()) {
        qWarning() << re.errorString();
        exit(1);
    }
    QRegularExpressionMatch match = re.match(result);
    if (match.hasMatch()) {
        qDebug() << "Matched" <<  match.captured(0);
        qDebug() << "Matched  1" <<  match.captured(1);
        qDebug() << "Matched  2" <<  match.captured(2);
        qDebug() << "Matched  3" <<  match.captured(3);
        qDebug() << "Matched  4" <<  match.captured(4);
        qDebug() << "Matched  5" <<  match.captured(5);
        qDebug() << "Matched  6" <<  match.captured(6);
        qDebug() << "Matched  7" <<  match.captured(7);
        qDebug() << "Matched  8" <<  match.captured(8);
        qDebug() << "Matched  9" <<  match.captured(9);
        qDebug() << "Matched 10" <<  match.captured(10);
        qDebug() << "Matched 11" <<  match.captured(11);
        QString f = "/" + match.captured("path") + "/";
        QString p = match.captured("from") + "/";  // Our rules expect a trailing '/'
        mi->rev = match.captured("rev").toInt(nullptr, 10);

        // There's really just 1 rule file ...
        foreach (const MatchRuleList matchRules, allMatchRules) {
            MatchRuleList::ConstIterator match = findMatchRule(matchRules, revnum, p);
            if (match != matchRules.constEnd()) {
                const Rules::Match &rule = *match;
                QString svnprefix, repository, effectiveRepository, branch, path;
                switch (rule.action) {
                    case Rules::Match::Export:
                        splitPathName(rule, p, &svnprefix, &repository, &effectiveRepository, &branch, &path);
                        mi->from = branch;
                        break;
                    default:
                        qFatal("Rule match had unexpected action on %s", qPrintable(p));
                        break;
                }
            }

            match = findMatchRule(matchRules, revnum, f);
            if (match != matchRules.constEnd()) {
                const Rules::Match &rule = *match;
                QString svnprefix, repository, effectiveRepository, branch, path;
                switch (rule.action) {
                    case Rules::Match::Export:
                        splitPathName(rule, f, &svnprefix, &repository, &effectiveRepository, &branch, &path);
                        mi->to = branch;
                        break;
                    default:
                        qFatal("Rule match had unexpected action on %s", qPrintable(p));
                        break;
                }
            }
        }
        if (!mi->to.isEmpty() && !mi->from.isEmpty()) {
            return true;
        }
        qDebug("Couldn't parse mergeinfo via rules file for %s or %s", qPrintable(p), qPrintable(f));
    }
    return false;
}

int SvnRevision::prepareTransactions()
{
    // find out what was changed in this revision:
    apr_hash_t *changes;
    SVN_ERR(svn_fs_paths_changed2(&changes, fs_root, pool));

    QMap<QByteArray, svn_fs_path_change2_t*> map;
    for (apr_hash_index_t *i = apr_hash_first(pool, changes); i; i = apr_hash_next(i)) {
        const void *vkey;
        void *value;
        apr_hash_this(i, &vkey, NULL, &value);
        const char *key = reinterpret_cast<const char *>(vkey);
        svn_fs_path_change2_t *change = reinterpret_cast<svn_fs_path_change2_t *>(value);
        // If we mix path deletions with path adds/replaces we might erase a
        // branch after that it has been reset -> history truncated
        if (map.contains(QByteArray(key))) {
            // If the same path is deleted and added, we need to put the
            // deletions into the map first, then the addition.
            if (change->change_kind == svn_fs_path_change_delete) {
                // XXX
            }
            fprintf(stderr, "\nDuplicate key found in rev %d: %s\n", revnum, key);
            fprintf(stderr, "This needs more code to be handled, file a bug report\n");
            fflush(stderr);
            exit(1);
        }
        map.insertMulti(QByteArray(key), change);
    }

    QMapIterator<QByteArray, svn_fs_path_change2_t*> i(map);
    bool mergeinfo_found = false;
    while (i.hasNext()) {
        i.next();
        if (i.value()->mergeinfo_mod == svn_tristate_true) {
            mergeinfo_found = true;
        }
        if (exportEntry(i.key(), i.value(), changes) == EXIT_FAILURE)
            return EXIT_FAILURE;
    }

    // Handle the deletions that we collected throughout the path/rule matching
    // and issue them once.
    for (const auto& rbp : deletions_.toStdMap()) {
        const QString& repo_branch = rbp.first;
        // FIXME: must exist, for now. might have to relax this requirement ...
        Repository::Transaction *txn = transactions[repo_branch];
        for (const auto& path : rbp.second) {
            if(ruledebug)
                qDebug() << "delete (" << txn->getBranch() << path << ")";
            txn->deleteFile(path);
        }
    }

    // No svn:mergeinfo found, not in src or in the pre-svn days, skip.
    if (!mergeinfo_found || !svn_repo_path.endsWith("base") || revnum < 179447)
        return EXIT_SUCCESS;

    // List of revisions to skip as their mergeinfo is complex and irrelevant in
    // terms of git.
    static QSet<int> skip_mergeinfo = {
        196075,
        179468,
        244485,
        244487,
        262833,
        262834,
        355814,
    };
    if (skip_mergeinfo.contains(revnum)) {
        return EXIT_SUCCESS;
    }

    // List of revisions to skip as their mergeinfo is empty and consists of
    // -0,0 +0,0 changes only. We hardcode the list here as it a) never changes
    // and so we can b) skip a whole lot of forking into svn(1).
    static QSet<int> empty_mergeinfo = {
        179566, 179790, 180332, 181027, 181074, 181522, 181524, 181601, 181738,
        181739, 181740, 181741, 181872, 181905, 182044, 182326, 182724, 183198,
        183226, 183430, 183431, 183432, 183433, 183654, 183714, 183910, 184330,
        184425, 184521, 184562, 185160, 185305, 185307, 185402, 185539, 185626,
        185631, 186256, 186261, 186535, 186934, 187064, 187220, 187258, 187962,
        // we keep these to test the code that finds them
        //189628, 189705, 193734, 194143, 194148, 194150, 194153, 194155, 194157,
        194159, 194674, 196280, 196696, 197352, 197509, 198498, 203325, 205176,
        282800, 282913, 283041, 283078, 283177, 283180, 283608, 283619, 283621,
        283626, 283628, 284185, 284187, 284235, 284244, 284396, 284678, 284680,
        284992, 285102, 285164, 285166, 285194, 285210, 286426, 286428, 286498,
        286500, 286502, 286508, 286749, 287451, 287504, 287511, 287513, 287515,
        287517, 287519, 287629, 287916, 288141, 288150, 288244, 289062, 289069,
        289285, 289721, 290005,
        // These are delete-only mergeinfos, usually deleting Reverse-merged
        // mergeinfo, whatever that is.
        190634, 194300, 196219, 196322, 196330, 196698, 198048, 198052, 198057,
        198058, 199096, 199141, 202103, 202105, 253750, 259935, 261839, 262486,
        // we keep these to test the code that finds them
        //276402, 293215, 298094, 337607, 349592,
        // Other mergeinfo that copies everything from head around
        296962,
    };
    if (empty_mergeinfo.contains(revnum)) {
        return EXIT_SUCCESS;
    }

    // Apparently, we already recorded some form of merge, could be to a
    // different branch though.
    if (merge_from_branch_ != "" && merge_from_rev_ != "")
        return EXIT_SUCCESS;

    // We don't "merge" into stable branches, so silently skip all those.
    QStringList branches;
    bool non_stable = false;
    foreach (const QString &value, to_branches_) {
        if (!value.startsWith("stable/") && !value.startsWith("releng/")) {
            non_stable = true;
        }
        branches << value;
    }
    if (to_branches_.size() > 0 && !non_stable)
        return EXIT_SUCCESS;

    printf(" MERGEINFO: rev %d has pure mergeinfo w/o path copies going into %d branches: %s",
           revnum, to_branches_.size(), qPrintable(branches.join(" ")));

    if (to_branches_.size() == 0) {
        printf(" MONKEYMERGE don't know how to handle empty branches!");
        return EXIT_SUCCESS;
    }
    fflush(stdout);

    // Things we patch up manually as the SVN history around them is ...
    // creative.
    static QMap<int, struct mi> manual_merges = {
        { 182352, { .from = "vendor/sendmail/dist", .rev = 182351, .to = "master" } },
        { 229307, { .from = "releng/9.0", .rev = 229306, .to = "refs/tags/release/9.0.0" } },
        // These 2 were merged from "/vendor" (sic!)
        { 357636, { .from = "vendor/NetBSD/tests/dist", .rev = 357635, .to = "master" } },
        { 357688, { .from = "vendor/NetBSD/tests/dist", .rev = 357687, .to = "master" } },
    };

    bool parse_ok = false;
    struct mi mi = { .from = "", .rev = -1, .to = "" };
    if (manual_merges.contains(revnum)) {
        const auto& val = manual_merges.value(revnum);
        mi = val;
        parse_ok = true;
    } else {
        // There are quite a number of revisions touching many branches and having a
        // "change" in svn:mergeinfo, except it's all empty, e.g. r182326. Try to
        // parse this and silently skip it if the mergeinfo is empty.
        parse_ok = maybeParseSimpleMergeinfo(revnum, &mi);
    }
    if (parse_ok && mi.rev == -1) {
        // all empty, ignore, this happens when we have -0,0 +0,0 changes
        // only.
        return EXIT_SUCCESS;
    }

    if (transactions.size() != 1) {
        printf(" MONKEYMERGE not sure how to handle %d transactions over %d branches!", transactions.size(), to_branches_.size());
        return EXIT_SUCCESS;
    }

    // For now, we only care when a branch is merged into head.
    if (to_branches_.size() != 1) {
        printf(" MONKEYMERGE don't know how to handle multiple branches: %s", qPrintable(to_branches_.values().join(" ")));
        return EXIT_SUCCESS;
    }

    const QString to = to_branches_.values().front();
    // TODO: scan through all merges into vendor to make sure they DTRT.
    // r229307 re-tagged 9.0 release, allow it to be a proper merge from releng
    if (to != "master" && !to.startsWith("projects/") && !to.startsWith("user/")
            && !to.startsWith("vendor/") && !to.startsWith("vendor-sys/")
            && to != "refs/tags/release/9.0.0") {
        printf(" MONKEYMERGE ignoring merge into %s", qPrintable(to));
        return EXIT_SUCCESS;
    }

    if (!parse_ok) {
        printf(" Couldn't parse mergeinfo!");
        return EXIT_SUCCESS;
    }

    // This is redundant with the WARN log about the branch copies.
    qDebug() << "Ended up with " + mi.from + "@" + QString::number(mi.rev) + " into " + mi.to;
    if (mi.from.startsWith("user") || mi.from.startsWith("user")) {
        printf(" MONKEYMERGE not merging from user, please inspect me: %s", qPrintable(mi.from));
        return EXIT_SUCCESS;
    }
    printf(" MONKEYMERGE IS HAPPENING!");
    Repository::Transaction *txn;
    txn = transactions.first();
    txn->noteCopyFromBranch(mi.from, mi.rev);

    return EXIT_SUCCESS;
}

int SvnRevision::fetchRevProps()
{
    if( propsFetched )
        return EXIT_SUCCESS;

    apr_hash_t *revprops;
    SVN_ERR(svn_fs_revision_proplist(&revprops, fs, revnum, pool));
    svn_string_t *svnauthor = (svn_string_t*)apr_hash_get(revprops, "svn:author", APR_HASH_KEY_STRING);
    svn_string_t *svndate = (svn_string_t*)apr_hash_get(revprops, "svn:date", APR_HASH_KEY_STRING);
    svn_string_t *svnlog = (svn_string_t*)apr_hash_get(revprops, "svn:log", APR_HASH_KEY_STRING);

    if (svnlog)
        log = svnlog->data;
    else
        log.clear();
    authorident = svnauthor ? identities.value(svnauthor->data) : QByteArray();
    epoch = svndate ? get_epoch(svndate->data) : 0;
    if (authorident.isEmpty()) {
        if (!svnauthor || svn_string_isempty(svnauthor))
            authorident = "nobody <nobody@localhost>";
        else
            authorident = svnauthor->data + QByteArray(" <") + svnauthor->data +
                QByteArray("@") + userdomain.toUtf8() + QByteArray(">");
    }
    propsFetched = true;
    return EXIT_SUCCESS;
}

int SvnRevision::commit()
{
    // now create the commit
    if (fetchRevProps() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    foreach (Repository *repo, repositories.values()) {
        repo->commit();
    }

    foreach (Repository::Transaction *txn, transactions) {
        txn->setAuthor(authorident);
        txn->setDateTime(epoch);
        txn->setLog(log);

        if (txn->commit() != EXIT_SUCCESS)
            return EXIT_FAILURE;
        delete txn;
    }

    return EXIT_SUCCESS;
}

int SvnRevision::exportEntry(const char *key, const svn_fs_path_change2_t *change,
                             apr_hash_t *changes)
{
    AprAutoPool revpool(pool.data());
    QString current = QString::fromUtf8(key);

    // was this copied from somewhere?
    svn_revnum_t rev_from = SVN_INVALID_REVNUM;
    const char *path_from = NULL;
    if (change->change_kind != svn_fs_path_change_delete) {
        // svn_fs_copied_from would fail on deleted paths, because the path
        // obviously no longer exists in the current revision
        SVN_ERR(svn_fs_copied_from(&rev_from, &path_from, fs_root, key, revpool));
    }
    // Is there mergeinfo attached? Only do this once per revnum
    // We abuse the logged_already hash for this.
    if (change->mergeinfo_mod == svn_tristate_true &&
        rev_from == SVN_INVALID_REVNUM) {
#if 0
        AprAutoPool mipool(pool.data());
        svn_mergeinfo_catalog_t catalog;
        apr_array_header_t *paths = apr_array_make(mipool, 10, sizeof(const char *));
        APR_ARRAY_PUSH(paths, const char *) = "/";

        // Seems to grab *all* mergeinfo in the repo, takes quite some time to run.
        SVN_ERR(svn_fs_get_mergeinfo(&catalog, fs_root, paths, svn_mergeinfo_explicit, true, mipool));

        for (apr_hash_index_t *i = apr_hash_first(mipool, catalog); i; i = apr_hash_next(i)) {
            const void *vkey;
            void *value;
            // XXX value is actually arrays of merge ranges
            apr_hash_this(i, &vkey, NULL, &value);
            qWarning() << "Got mergeinfo for " << (const char *)vkey;
        }
#endif
    }

    // is this a directory?
    svn_boolean_t is_dir;
    SVN_ERR(svn_fs_is_dir(&is_dir, fs_root, key, revpool));
    // Adding newly created directories
    if (is_dir && change->change_kind == svn_fs_path_change_add && path_from == NULL
        && CommandLineParser::instance()->contains("empty-dirs")) {
        QString keyQString = key;
        // Skipping SVN-directory-layout
        if (keyQString.endsWith("/trunk") || keyQString.endsWith("/branches") || keyQString.endsWith("/tags")) {
            //qDebug() << "Skipping SVN-directory-layout:" << keyQString;
            return EXIT_SUCCESS;
        }
        needCommit = true;
        //qDebug() << "Adding directory:" << key;
    }
    // svn:ignore-properties
    else if (is_dir && (change->change_kind == svn_fs_path_change_add || change->change_kind == svn_fs_path_change_modify || change->change_kind == svn_fs_path_change_replace)
             && path_from == NULL && CommandLineParser::instance()->contains("svn-ignore")) {
        needCommit = true;
    }
    else if (is_dir) {
        if (change->change_kind == svn_fs_path_change_modify ||
            change->change_kind == svn_fs_path_change_add) {
            if (path_from == NULL) {
                // freshly added directory, or modified properties
                // Git doesn't handle directories, so we don't either
                //qDebug() << "   mkdir ignored:" << key;
                return EXIT_SUCCESS;
            }

            qDebug() << "   " << key << "was copied from" << path_from << "rev" << rev_from;
        } else if (change->change_kind == svn_fs_path_change_replace) {
            if (path_from == NULL)
                qDebug() << "   " << key << "was replaced";
            else
                qDebug() << "   " << key << "was replaced from" << path_from << "rev" << rev_from;
        } else if (change->change_kind == svn_fs_path_change_reset) {
            qCritical() << "   " << key << "was reset, panic!";
            return EXIT_FAILURE;
        } else {
            // if change_kind == delete, it shouldn't come into this arm of the 'is_dir' test
            qCritical() << "   " << key << "has unhandled change kind " << change->change_kind << ", panic!";
            return EXIT_FAILURE;
        }
    } else if (change->change_kind == svn_fs_path_change_delete) {
        is_dir = wasDir(fs, revnum - 1, key, revpool);
    }

    if (is_dir)
        current += '/';

    //MultiRule: loop start
    //Replace all returns with continue,
    bool isHandled = false;
    foreach ( const MatchRuleList matchRules, allMatchRules ) {
        // find the first rule that matches this pathname
        MatchRuleList::ConstIterator match = findMatchRule(matchRules, revnum, current);
        if (match != matchRules.constEnd()) {
            const Rules::Match &rule = *match;
            if ( exportDispatch(key, change, path_from, rev_from, changes, current, rule, matchRules, revpool) == EXIT_FAILURE )
                return EXIT_FAILURE;
            isHandled = true;
        } else if (is_dir && path_from != NULL) {
            qDebug() << current << "is a copy-with-history, auto-recursing";
            if ( recurse(key, change, path_from, matchRules, rev_from, changes, revpool) == EXIT_FAILURE )
                return EXIT_FAILURE;
            isHandled = true;
        } else if (is_dir && change->change_kind == svn_fs_path_change_delete) {
            qDebug() << current << "deleted, auto-recursing";
            if ( recurse(key, change, path_from, matchRules, rev_from, changes, revpool) == EXIT_FAILURE )
                return EXIT_FAILURE;
            isHandled = true;
        }
    }
    if ( isHandled ) {
        return EXIT_SUCCESS;
    }
    if (wasDir(fs, revnum - 1, key, revpool)) {
        qDebug() << current << "was a directory; ignoring";
    } else if (change->change_kind == svn_fs_path_change_delete) {
        qDebug() << current << "is being deleted but I don't know anything about it; ignoring";
    } else {
        qCritical() << current << "did not match any rules; cannot continue";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int SvnRevision::exportDispatch(const char *key, const svn_fs_path_change2_t *change,
                                const char *path_from, svn_revnum_t rev_from,
                                apr_hash_t *changes, const QString &current,
                                const Rules::Match &rule, const MatchRuleList &matchRules, apr_pool_t *pool)
{
    switch (rule.action) {
    case Rules::Match::Ignore:
        if(ruledebug)
            qDebug() << "rev" << revnum << qPrintable(current) << "matched rule:" << rule.info() << "  " << "ignoring.";
        return EXIT_SUCCESS;

    case Rules::Match::Recurse:
        if(ruledebug)
            qDebug() << "rev" << revnum << qPrintable(current) << "matched rule:" << rule.info() << "  " << "recursing.";
        return recurse(key, change, path_from, matchRules, rev_from, changes, pool);

    case Rules::Match::Export:
        if(ruledebug)
            qDebug() << "rev" << revnum << qPrintable(current) << "matched rule:" << rule.info() << "  " << "exporting.";
        if (exportInternal(key, change, path_from, rev_from, current, rule, matchRules) == EXIT_SUCCESS)
            return EXIT_SUCCESS;
        if (change->change_kind != svn_fs_path_change_delete) {
            if(ruledebug)
                qDebug() << "rev" << revnum << qPrintable(current) << "matched rule:" << rule.info() << "  " << "Unable to export non path removal.";
            return EXIT_FAILURE;
        }
        // we know that the default action inside recurse is to recurse further or to ignore,
        // either of which is reasonably safe for deletion
        qWarning() << "WARN: deleting unknown path" << current << "; auto-recursing";
        return recurse(key, change, path_from, matchRules, rev_from, changes, pool);
    }

    // never reached
    return EXIT_FAILURE;
}

int SvnRevision::exportInternal(const char *key, const svn_fs_path_change2_t *change,
                                const char *path_from, svn_revnum_t rev_from,
                                const QString &current, const Rules::Match &rule, const MatchRuleList &matchRules)
{
    needCommit = true;
    QString svnprefix, repository, effectiveRepository, branch, path;
    splitPathName(rule, current, &svnprefix, &repository, &effectiveRepository, &branch, &path);

    to_branches_.insert(branch);

    Repository *repo = repositories.value(repository, 0);
    if (!repo) {
        if (change->change_kind != svn_fs_path_change_delete)
            qCritical() << "Rule" << rule
                        << "references unknown repository" << repository;
        return EXIT_FAILURE;
    }

    printf(".");
    fflush(stdout);
//                qDebug() << "   " << qPrintable(current) << "rev" << revnum << "->"
//                         << qPrintable(repository) << qPrintable(branch) << qPrintable(path);

    if (change->change_kind == svn_fs_path_change_delete && current == svnprefix && path.isEmpty() && !repo->hasPrefix()) {
        if(ruledebug)
            qDebug() << "repository" << repository << "branch" << branch << "deleted";
        return repo->deleteBranch(branch, revnum);
    }

    QString previous;
    QString prevsvnprefix, prevrepository, preveffectiverepository, prevbranch, prevpath;

    if (path_from != NULL) {
        previous = QString::fromUtf8(path_from);
        if (wasDir(fs, rev_from, path_from, pool.data())) {
            previous += '/';
        }
        MatchRuleList::ConstIterator prevmatch =
            findMatchRule(matchRules, rev_from, previous, NoIgnoreRule);
        if (prevmatch != matchRules.constEnd()) {
            splitPathName(*prevmatch, previous, &prevsvnprefix, &prevrepository,
                          &preveffectiverepository, &prevbranch, &prevpath);

        } else {
            qWarning() << "WARN: SVN reports a \"copy from\" @" << revnum << "from" << path_from << "@" << rev_from << "but no matching rules found! Ignoring copy, treating as a modification";
            path_from = NULL;
        }
    }

    // current == svnprefix => we're dealing with the contents of the whole branch here
    if (path_from != NULL && current == svnprefix && path.isEmpty()) {
        if (previous != prevsvnprefix) {
            // source is not the whole of its branch
            qDebug() << qPrintable(current) << "is a partial branch of repository"
                     << qPrintable(prevrepository) << "branch"
                     << qPrintable(prevbranch) << "subdir"
                     << qPrintable(prevpath);
        } else if (preveffectiverepository != effectiveRepository) {
            qWarning() << "WARN:" << qPrintable(current) << "rev" << revnum
                       << "is a cross-repository copy (from repository"
                       << qPrintable(prevrepository) << "branch"
                       << qPrintable(prevbranch) << "path"
                       << qPrintable(prevpath) << "rev" << rev_from << ")";
        } else if (path != prevpath) {
            // NOTE(uqs): this will happen when using the `prefix` action and a
            // vendor branch gets copied to a tag. It'll lead to a disconnected
            // tag. This shouldn't happen and needs more work.
            qWarning() << qPrintable(current)
                     << "is a branch copy which renames base directory of all contents"
                     << qPrintable(prevpath) << "to" << qPrintable(path);
            qFatal("This must not happen. Vendor tags will be disconnected.");
            // FIXME: Handle with fast-import 'file rename' facility
            //        ??? Might need special handling when path == / or prevpath == /
        } else {
            if (prevbranch == branch) {
                // same branch and same repository
                qDebug() << qPrintable(current) << "rev" << revnum
                         << "is reseating branch" << qPrintable(branch)
                         << "to an earlier revision"
                         << qPrintable(previous) << "rev" << rev_from;
            } else {
                // same repository but not same branch
                // this means this is a plain branch
                qDebug() << qPrintable(repository) << ": branch"
                         << qPrintable(branch) << "is branching from"
                         << qPrintable(prevbranch);
            }

            if (repo->createBranch(branch, revnum, prevbranch, rev_from) == EXIT_FAILURE)
                return EXIT_FAILURE;

            if(CommandLineParser::instance()->contains("svn-branches")) {
                Repository::Transaction *txn = transactions.value(repository + branch, 0);
                if (!txn) {
                    txn = repo->newTransaction(branch, svnprefix, revnum);
                    if (!txn)
                        return EXIT_FAILURE;

                    transactions.insert(repository + branch, txn);
                }
                if(ruledebug)
                    qDebug() << "Create a true SVN copy of branch (" << key << "->" << branch << path << ")";
                txn->deleteFile(path);
                recursiveDumpDir(txn, fs, fs_root, key, path, pool, revnum, rule, matchRules, ruledebug);
            }
            if (rule.annotate) {
                // create an annotated tag
                fetchRevProps();
                repo->createAnnotatedTag(branch, svnprefix, revnum, authorident,
                                         epoch, log);
            }
            return EXIT_SUCCESS;
        }
    }
    Repository::Transaction *txn = transactions.value(repository + branch, 0);
    if (!txn) {
        txn = repo->newTransaction(branch, svnprefix, revnum);
        if (!txn)
            return EXIT_FAILURE;

        transactions.insert(repository + branch, txn);
    }

    //
    // If this path was copied from elsewhere, use it to infer _some_
    // merge points.  This heuristic is fairly useful for tracking
    // changes across directory re-organizations and wholesale branch
    // imports.
    //
    // NOTE(uqs): HACK ALERT! Only merge between head, projects, and user
    // branches for the FreeBSD repositories. Never merge into stable or
    // releng, as we only ever cherry-pick changes to those branches.
    // Also, never merge from stable, like was done in SVN r306097, as it pulls
    // in all history.
    // FIXME: Needs to move into the ruleset ...
    if (path_from != NULL && prevrepository == repository && prevbranch != branch
            && (branch.startsWith("master") || branch.startsWith("head") ||
                branch.startsWith("projects") || branch.startsWith("user"))
            && !prevbranch.startsWith("stable")) {
        QStringList log = QStringList()
                          << "copy from branch" << prevbranch << "to branch"
                          << branch << "@rev" << QString::number(rev_from);
        if (!logged_already_.contains(qHash(log))) {
            logged_already_.insert(qHash(log));
            qDebug() << "copy from branch" << prevbranch << "to branch"
                     << branch << "@rev" << rev_from;
        }
        merge_from_rev_ = rev_from;
        merge_from_branch_ = prevbranch;
        txn->noteCopyFromBranch (prevbranch, rev_from);
    }

    if (change->change_kind == svn_fs_path_change_replace && path_from == NULL) {
        if(ruledebug)
            qDebug() << "replaced with empty path (" << branch << path << ")";
        txn->deleteFile(path);
    }
    if (change->change_kind == svn_fs_path_change_delete) {
        if(ruledebug)
            qDebug() << "delete (" << branch << path << ")";
        txn->deleteFile(path);
    } else if (!current.endsWith('/')) {
        if(ruledebug)
            qDebug() << "add/change file (" << key << "->" << branch << path << ")";
        dumpBlob(txn, fs_root, key, path, pool);
    } else {
        if(ruledebug)
            qDebug() << "add/change dir (" << key << "->" << branch << path << ")";

        // Check unknown svn-properties
        if (((path_from == NULL && change->prop_mod==1) || (path_from != NULL && (change->change_kind == svn_fs_path_change_add || change->change_kind == svn_fs_path_change_replace)))
            && CommandLineParser::instance()->contains("propcheck")) {
            if (fetchUnknownProps(pool, key, fs_root) != EXIT_SUCCESS) {
                qWarning() << "Error checking svn-properties (" << key << ")";
            }
        }

        txn->deleteFile(path);

        // Add GitIgnore with svn:ignore
        int ignoreSet = false;
        if (((path_from == NULL && change->prop_mod==1) || (path_from != NULL && (change->change_kind == svn_fs_path_change_add || change->change_kind == svn_fs_path_change_replace)))
            && CommandLineParser::instance()->contains("svn-ignore")) {
            QString svnignore;
            // TODO: Check if svn:ignore or other property was changed, but always set on copy/rename (path_from != NULL)
            if (fetchIgnoreProps(&svnignore, pool, key, fs_root) != EXIT_SUCCESS) {
                qWarning() << "Error fetching svn-properties (" << key << ")";
            } else if (!svnignore.isNull()) {
                addGitIgnore(pool, key, path, fs_root, txn, svnignore.toStdString().c_str());
                ignoreSet = true;
            }
        }

        // Add GitIgnore for empty directories (if GitIgnore was not set previously)
        if (CommandLineParser::instance()->contains("empty-dirs") && ignoreSet == false) {
            if (addGitIgnore(pool, key, path, fs_root, txn) == EXIT_SUCCESS) {
                return EXIT_SUCCESS;
            }
        }

        recursiveDumpDir(txn, fs, fs_root, key, path, pool, revnum, rule, matchRules, ruledebug);
    }

    if (rule.annotate) {
        // create an annotated tag
        fetchRevProps();
        repo->createAnnotatedTag(branch, svnprefix, revnum, authorident,
                                 epoch, log);
    }

    // This is a once per-rule action, but we end up here for every path that
    // has matched. That is, we emit tons and tons of redundant deletes into
    // the fast-import stream. We can't drain the list either, as the rule
    // match is marked const. Gather them all up for handling in
    // SvnRevision::prepareTransactions
    if (!rule.deletes.empty()) {
        const QString key = repository + branch;
        if (!deletions_.contains(key)) {
            deletions_[key] = QSet<QString>();
        }
        for (auto const& path : rule.deletes) {
            deletions_[key].insert(path);
        }
    }

    return EXIT_SUCCESS;
}

int SvnRevision::recurse(const char *path, const svn_fs_path_change2_t *change,
                         const char *path_from, const MatchRuleList &matchRules, svn_revnum_t rev_from,
                         apr_hash_t *changes, apr_pool_t *pool)
{
    svn_fs_root_t *fs_root = this->fs_root;
    if (change->change_kind == svn_fs_path_change_delete)
        SVN_ERR(svn_fs_revision_root(&fs_root, fs, revnum - 1, pool));

    // get the dir listing
    svn_node_kind_t kind;
    SVN_ERR(svn_fs_check_path(&kind, fs_root, path, pool));
    if(kind == svn_node_none) {
        qWarning() << "WARN: Trying to recurse using a nonexistant path" << path << ", ignoring";
        return EXIT_SUCCESS;
    } else if(kind != svn_node_dir) {
        qWarning() << "WARN: Trying to recurse using a non-directory path" << path << ", ignoring";
        return EXIT_SUCCESS;
    }

    apr_hash_t *entries;
    SVN_ERR(svn_fs_dir_entries(&entries, fs_root, path, pool));
    AprAutoPool dirpool(pool);

    // While we get a hash, put it in a map for sorted lookup, so we can
    // repeat the conversions and get the same git commit hashes.
    QMap<QByteArray, svn_node_kind_t> map;
    for (apr_hash_index_t *i = apr_hash_first(pool, entries); i; i = apr_hash_next(i)) {
        dirpool.clear();
        const void *vkey;
        void *value;
        apr_hash_this(i, &vkey, NULL, &value);
        svn_fs_dirent_t *dirent = reinterpret_cast<svn_fs_dirent_t *>(value);
        map.insertMulti(QByteArray(dirent->name), dirent->kind);
    }

    QMapIterator<QByteArray, svn_node_kind_t> i(map);
    while (i.hasNext()) {
        dirpool.clear();
        i.next();
        QByteArray entry = path + QByteArray("/") + i.key();
        QByteArray entryFrom;
        if (path_from)
            entryFrom = path_from + QByteArray("/") + i.key();

        // check if this entry is in the changelist for this revision already
        svn_fs_path_change2_t *otherchange =
            (svn_fs_path_change2_t*)apr_hash_get(changes, entry.constData(), APR_HASH_KEY_STRING);
        if (otherchange && otherchange->change_kind == svn_fs_path_change_add) {
            qDebug() << entry << "rev" << revnum
                     << "is in the change-list, deferring to that one";
            continue;
        }

        QString current = QString::fromUtf8(entry);
        if (i.value() == svn_node_dir)
            current += '/';

        // find the first rule that matches this pathname
        MatchRuleList::ConstIterator match = findMatchRule(matchRules, revnum, current);
        if (match != matchRules.constEnd()) {
            if (exportDispatch(entry, change, entryFrom.isNull() ? 0 : entryFrom.constData(),
                               rev_from, changes, current, *match, matchRules, dirpool) == EXIT_FAILURE)
                return EXIT_FAILURE;
        } else {
            if (i.value() == svn_node_dir) {
                qDebug() << current << "rev" << revnum
                         << "did not match any rules; auto-recursing";
                if (recurse(entry, change, entryFrom.isNull() ? 0 : entryFrom.constData(),
                            matchRules, rev_from, changes, dirpool) == EXIT_FAILURE)
                    return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}

int SvnRevision::addGitIgnore(apr_pool_t *pool, const char *key, QString path,
                              svn_fs_root_t *fs_root, Repository::Transaction *txn, const char *content)
{
    // Check for number of subfiles if no content
    if (!content) {
        apr_hash_t *entries;
        SVN_ERR(svn_fs_dir_entries(&entries, fs_root, key, pool));
        // Return if any subfiles
        if (apr_hash_count(entries)!=0) {
            return EXIT_FAILURE;
        }
    }

    // Add gitignore-File
    QString gitIgnorePath = path + ".gitignore";
    if (content) {
        QIODevice *io = txn->addFile(gitIgnorePath, 33188, strlen(content));
        if (!CommandLineParser::instance()->contains("dry-run")) {
            io->write(content);
            io->putChar('\n');
        }
    } else {
        QIODevice *io = txn->addFile(gitIgnorePath, 33188, 0);
        if (!CommandLineParser::instance()->contains("dry-run")) {
            io->putChar('\n');
        }
    }

    return EXIT_SUCCESS;
}

int SvnRevision::fetchIgnoreProps(QString *ignore, apr_pool_t *pool, const char *key, svn_fs_root_t *fs_root)
{
    // Get svn:ignore
    svn_string_t *prop = NULL;
    SVN_ERR(svn_fs_node_prop(&prop, fs_root, key, "svn:ignore", pool));
    if (prop) {
        *ignore = QString(prop->data);
        // remove patterns with slashes or backslashes,
        // they didn't match anything in Subversion but would in Git eventually
        ignore->remove(QRegExp("^[^\\r\\n]*[\\\\/][^\\r\\n]*(?:[\\r\\n]|$)|[\\r\\n][^\\r\\n]*[\\\\/][^\\r\\n]*(?=[\\r\\n]|$)"));
        // add a slash in front to have the same meaning in Git of only working on the direct children
        ignore->replace(QRegExp("(^|[\\r\\n])\\s*(?![\\r\\n]|$)"), "\\1/");
    } else {
        *ignore = QString();
    }

    // Get svn:global-ignores
    prop = NULL;
    SVN_ERR(svn_fs_node_prop(&prop, fs_root, key, "svn:global-ignores", pool));
    if (prop) {
        QString global_ignore = QString(prop->data);
        // remove patterns with slashes or backslashes,
        // they didn't match anything in Subversion but would in Git eventually
        global_ignore.remove(QRegExp("^[^\\r\\n]*[\\\\/][^\\r\\n]*(?:[\\r\\n]|$)|[\\r\\n][^\\r\\n]*[\\\\/][^\\r\\n]*(?=[\\r\\n]|$)"));
        ignore->append(global_ignore);
    }

    // replace multiple asterisks Subversion meaning by Git meaning
    ignore->replace(QRegExp("\\*+"), "*");

    return EXIT_SUCCESS;
}

int SvnRevision::fetchUnknownProps(apr_pool_t *pool, const char *key, svn_fs_root_t *fs_root)
{
    // Check all properties
    apr_hash_t *table;
    SVN_ERR(svn_fs_node_proplist(&table, fs_root, key, pool));
    apr_hash_index_t *hi;
    void *propVal;
    const void *propKey;
    for (hi = apr_hash_first(pool, table); hi; hi = apr_hash_next(hi)) {
        apr_hash_this(hi, &propKey, NULL, &propVal);
        if (strcmp((char*)propKey, "svn:ignore")!=0 && strcmp((char*)propKey, "svn:global-ignores")!=0 && strcmp((char*)propKey, "svn:mergeinfo") !=0) {
            qWarning() << "WARN: Unknown svn-property" << (char*)propKey << "set to" << ((svn_string_t*)propVal)->data << "for" << key;
        }
    }

    return EXIT_SUCCESS;
}
