#import "app/updater.h"

#import <AppKit/AppKit.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "diagnostics/log.h"
#include "system/update_support.h"

extern char** environ;

using snow::update::Stage;

namespace snow::mac {
namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::string str(NSString* s) { return s.UTF8String ?: ""; }

std::string shortText(std::string s, std::size_t max = 300) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() > max) s = s.substr(0, max) + "...";
    return updates::redactHome(s);
}

// Runs a tool (absolute path) and waits for it; stdout + stderr go to `output` (first 8 KB).
int runProcess(const std::vector<std::string>& args, std::string* output) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        if (output) *output = "spawn failed";
        return -1;
    }
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0) {
            if (output && output->size() < 8192) output->append(buf, static_cast<std::size_t>(n));
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string makeTempDir(const char* prefix) {
    std::string t = str(NSTemporaryDirectory());
    if (t.empty()) t = "/tmp/";
    if (t.back() != '/') t += '/';
    t += prefix;
    t += ".XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    if (!::mkdtemp(buf.data())) return {};
    return buf.data();
}

void removeFile(const std::string& path) { ::unlink(path.c_str()); }

bool removeTree(const std::string& path) {
    if (path.empty() || path == "/") return false;
    NSFileManager* fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:ns(path)]) return true;
    return [fm removeItemAtPath:ns(path) error:nil];
}

bool sameVersion(const std::string& a, const std::string& b) {
    const update::Version va = update::parseVersion(a), vb = update::parseVersion(b);
    return va.valid && vb.valid && update::compare(va, vb) == 0;
}

}  // namespace

std::string updateWorkDirectory() {
    NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString* base = dirs.firstObject ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches"];
    return str([base stringByAppendingPathComponent:@"SnowFall/updates"]);
}

bool stageAppFromDiskImage(const std::string& dmg, const std::string& stagedApp, const std::string& expectedVersion,
                           const std::string& expectedBundleId, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (stagedApp.empty() || stagedApp == "/") return fail("bad staging path");
    const std::string mnt = makeTempDir("snowfall-mnt");
    if (mnt.empty()) return fail("cannot create a mount point");
    std::string out;
    int rc = runProcess({"/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mnt, dmg}, &out);
    if (rc != 0) {
        ::rmdir(mnt.c_str());
        return fail("hdiutil attach: exit " + std::to_string(rc) + ": " + shortText(out));
    }
    SNOW_LOGI("updates: disk image mounted");

    std::string source = mnt + "/SnowFall.app";
    struct stat st{};
    if (::stat(source.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        source.clear();
        for (NSString* name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:ns(mnt) error:nil]) {
            if ([name.pathExtension isEqualToString:@"app"]) {
                source = mnt + "/" + str(name);
                break;
            }
        }
    }
    bool copied = false;
    std::string copyError;
    if (!source.empty()) {
        removeTree(stagedApp);
        out.clear();
        rc = runProcess({"/usr/bin/ditto", source, stagedApp}, &out);
        copied = rc == 0;
        if (!copied) copyError = "ditto: exit " + std::to_string(rc) + ": " + shortText(out);
    } else {
        copyError = "no .app in the disk image";
    }
    out.clear();
    if (runProcess({"/usr/bin/hdiutil", "detach", mnt, "-quiet"}, &out) != 0) {
        out.clear();
        runProcess({"/usr/bin/hdiutil", "detach", mnt, "-force", "-quiet"}, &out);
    }
    ::rmdir(mnt.c_str());
    if (!copied) {
        removeTree(stagedApp);
        return fail(copyError);
    }
    SNOW_LOGI("updates: app copied from the disk image");

    out.clear();
    rc = runProcess({"/usr/bin/codesign", "--verify", "--deep", "--strict", stagedApp}, &out);
    if (rc != 0) {
        removeTree(stagedApp);
        return fail("codesign --verify: exit " + std::to_string(rc) + ": " + shortText(out));
    }
    NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:ns(stagedApp + "/Contents/Info.plist")];
    NSString* version = [info[@"CFBundleShortVersionString"] isKindOfClass:[NSString class]] ? info[@"CFBundleShortVersionString"] : @"";
    NSString* bundleId = [info[@"CFBundleIdentifier"] isKindOfClass:[NSString class]] ? info[@"CFBundleIdentifier"] : @"";
    if (!sameVersion(str(version), expectedVersion)) {
        removeTree(stagedApp);
        return fail("version of the new app is '" + str(version) + "', expected " + expectedVersion);
    }
    if (!expectedBundleId.empty() && str(bundleId) != expectedBundleId) {
        removeTree(stagedApp);
        return fail("bundle identifier of the new app is '" + str(bundleId) + "'");
    }
    SNOW_LOGI("updates: new app verified (signature valid, version %s)", str(version).c_str());
    return true;
}

bool launchSwapHelper(const SwapPlan& plan, pid_t* helperPid, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (plan.waitForPid <= 0 || plan.currentApp.empty() || plan.stagedApp.empty() || plan.backupApp.empty())
        return fail("bad swap plan");
    const std::string dir = makeTempDir("snowfall-update");
    if (dir.empty()) return fail("cannot create a temp directory");
    const std::string script = dir + "/helper.sh";
    FILE* f = std::fopen(script.c_str(), "w");
    if (!f) return fail("cannot write the helper script");
    const char* text = updates::swapHelperScript();
    const bool written = std::fputs(text, f) >= 0;
    std::fclose(f);
    if (!written) return fail("cannot write the helper script");
    ::chmod(script.c_str(), 0700);

    const std::string pidText = std::to_string(plan.waitForPid);
    const std::string log = plan.logPath.empty() ? dir + "/helper.log" : plan.logPath;
    std::vector<std::string> args = {"/bin/sh", script, pidText, plan.currentApp, plan.stagedApp, plan.backupApp,
                                     dir, log, plan.reopen ? "1" : "0"};
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Own session (survives our exit, no terminal), no inherited descriptors (the instance lock).
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/bin/sh", &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        removeTree(dir);
        return fail("posix_spawn: " + std::to_string(rc));
    }
    if (helperPid) *helperPid = pid;
    return true;
}

}  // namespace snow::mac

using namespace snow::mac;

// ---- NSURLSession delegate: redirects, download progress, download file -------------------

@interface SFUpdateSessionDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionDownloadDelegate>
@property(nonatomic, strong) NSURLSessionTask* download;
@property(nonatomic, copy) NSString* destination;   // where the finished download is moved
@property(nonatomic) long long limit;
@property(nonatomic) BOOL tooLarge;
@property(nonatomic, copy) NSString* fileError;     // set by didFinishDownloadingToURL
@property(nonatomic) BOOL fileReady;
@property(nonatomic, copy) void (^progress)(long long written, long long expected);
@property(nonatomic, copy) void (^completed)(NSError* error);
@end

@implementation SFUpdateSessionDelegate

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session;
    (void)task;
    (void)response;
    NSString* url = request.URL.absoluteString ?: @"";
    if (snow::update::isAllowedUrl(url.UTF8String ?: "")) {
        completionHandler(request);
    } else {
        SNOW_LOGW("updates: redirect to host '%s' refused", request.URL.host.UTF8String ?: "?");
        completionHandler(nil);   // the redirect response itself is delivered -> not 200 -> failure
    }
}

- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)task
                 didWriteData:(int64_t)bytesWritten
            totalBytesWritten:(int64_t)total
    totalBytesExpectedToWrite:(int64_t)expected {
    (void)session;
    (void)bytesWritten;
    if (task != self.download) return;
    if (total > self.limit || expected > self.limit) {
        self.tooLarge = YES;
        [task cancel];
        return;
    }
    if (self.progress) self.progress(total, expected);
}

- (void)URLSession:(NSURLSession*)session downloadTask:(NSURLSessionDownloadTask*)task didFinishDownloadingToURL:(NSURL*)location {
    (void)session;
    if (task != self.download) return;
    NSHTTPURLResponse* http = [task.response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)task.response : nil;
    if (!http || http.statusCode != 200) {
        self.fileError = [NSString stringWithFormat:@"HTTP %ld", static_cast<long>(http ? http.statusCode : 0)];
        return;
    }
    if (!snow::update::isAllowedUrl(http.URL.absoluteString.UTF8String ?: "")) {
        self.fileError = @"final URL not allowed";
        return;
    }
    NSNumber* size = nil;
    [location getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    if (size.longLongValue > self.limit) {
        self.tooLarge = YES;
        return;
    }
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm removeItemAtPath:self.destination error:nil];
    NSError* err = nil;
    if (![fm moveItemAtURL:location toURL:[NSURL fileURLWithPath:self.destination] error:&err]) {
        self.fileError = err.localizedDescription ?: @"move failed";
        return;
    }
    self.fileReady = YES;
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    if (task != self.download) return;
    self.download = nil;
    void (^done)(NSError*) = self.completed;
    self.completed = nil;
    self.progress = nil;
    if (done) done(error);
}

@end

// ---- SFUpdater -------------------------------------------------------------------------------

@implementation SFUpdater {
    BOOL _allowed;
    snow::update::Status _status;
    snow::update::ReleaseInfo _release;
    BOOL _hasRelease;
    NSURLSession* _session;
    SFUpdateSessionDelegate* _delegate;
    NSUInteger _generation;
    std::string _workDir;
}

- (instancetype)initWithNetworkAllowed:(BOOL)networkAllowed {
    if ((self = [super init])) {
        _allowed = networkAllowed;
        _workDir = snow::mac::updateWorkDirectory();
        const snow::update::Version v = snow::update::parseVersion(SNOW_VERSION);
        _status.current = v.valid ? v.text() : SNOW_VERSION;
        _status.pageUrl = snow::update::kReleasesPage;
    }
    return self;
}

- (void)dealloc {
    [_session invalidateAndCancel];
}

- (BOOL)networkAllowed {
    return _allowed;
}

- (const snow::update::Status&)status {
    return _status;
}

- (BOOL)busy {
    return _status.stage == Stage::Checking || _status.stage == Stage::Downloading || _status.stage == Stage::Installing;
}

- (std::string)installBlocker {
    const std::string bundle = str(NSBundle.mainBundle.bundlePath);
    return snow::mac::updates::installBlocker(bundle, snow::mac::updates::bundleFacts(bundle));
}

- (void)notify {
    if (self.onStatus) self.onStatus();
}

- (NSURLSession*)session {
    if (_session) return _session;
    NSURLSessionConfiguration* c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    c.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    c.HTTPShouldSetCookies = NO;
    c.HTTPCookieStorage = nil;
    c.URLCredentialStorage = nil;
    c.URLCache = nil;
    c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    c.timeoutIntervalForRequest = 15;
    c.timeoutIntervalForResource = 15 * 60;
    c.waitsForConnectivity = NO;
    _delegate = [SFUpdateSessionDelegate new];
    _session = [NSURLSession sessionWithConfiguration:c delegate:_delegate delegateQueue:NSOperationQueue.mainQueue];
    return _session;
}

- (NSURLRequest*)requestFor:(const std::string&)url accept:(NSString*)accept {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u) return nil;
    NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:u
                                                     cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                 timeoutInterval:15];
    r.HTTPShouldHandleCookies = NO;
    [r setValue:[NSString stringWithFormat:@"SnowFall/%s", SNOW_VERSION] forHTTPHeaderField:@"User-Agent"];
    [r setValue:accept forHTTPHeaderField:@"Accept"];
    return r;
}

// One GET into memory, at most `limit` bytes; `done(nil)` on any failure. Main queue.
- (void)fetch:(const std::string&)url accept:(NSString*)accept limit:(std::size_t)limit done:(void (^)(NSData*))done {
    NSURLRequest* req = _allowed && snow::update::isAllowedUrl(url) ? [self requestFor:url accept:accept] : nil;
    if (!req) {
        SNOW_LOGW("updates: request refused (URL not allowed or network disabled)");
        dispatch_async(dispatch_get_main_queue(), ^{
          done(nil);
        });
        return;
    }
    const NSUInteger gen = _generation;
    __weak SFUpdater* weakSelf = self;
    NSURLSessionDataTask* task =
        [[self session] dataTaskWithRequest:req
                          completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                            SFUpdater* me = weakSelf;
                            if (!me || me->_generation != gen) return;
                            NSHTTPURLResponse* http =
                                [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)response : nil;
                            if (error) {
                                SNOW_LOGW("updates: request failed (%s %ld)", error.domain.UTF8String ?: "?",
                                          static_cast<long>(error.code));
                                done(nil);
                            } else if (!http || http.statusCode != 200) {
                                SNOW_LOGW("updates: HTTP %ld", static_cast<long>(http ? http.statusCode : 0));
                                done(nil);
                            } else if (!data || data.length > limit) {
                                SNOW_LOGW("updates: response too large or empty");
                                done(nil);
                            } else if (!snow::update::isAllowedUrl(http.URL.absoluteString.UTF8String ?: "")) {
                                SNOW_LOGW("updates: final URL not allowed");
                                done(nil);
                            } else {
                                done(data);
                            }
                          }];
    [task resume];
}

- (void)setStage:(Stage)stage {
    _status.stage = stage;
    if (stage != Stage::Failed) _status.error.clear();
    if (stage != Stage::Downloading) _status.progress = 0;
}

- (void)fail:(const char*)errorKey why:(const std::string&)why {
    SNOW_LOGW("updates: %s (%s)", errorKey, snow::mac::updates::redactHome(why).c_str());
    [self setStage:Stage::Failed];
    _status.error = errorKey;
    [self notify];
}

// ---- check ---------------------------------------------------------------------------------

- (BOOL)releaseInstallable {
    using namespace snow::update;
    const Asset* dmg = installerAsset(_release, Platform::MacOS);
    if (!dmg || !findAsset(_release, kChecksumsName) || !findAsset(_release, kSignatureName)) {
        SNOW_LOGI("updates: release has no dmg / SHA256SUMS / SHA256SUMS.sig - manual install only");
        return NO;
    }
    if (dmg->size > static_cast<long long>(kMaxInstallerBytes)) {
        SNOW_LOGW("updates: installer is too large - manual install only");
        return NO;
    }
    const std::string blocker = [self installBlocker];
    if (!blocker.empty()) {
        SNOW_LOGI("updates: this copy of the app cannot be replaced in place (%s) - manual install only", blocker.c_str());
        return NO;
    }
    return YES;
}

- (void)check {
    if ([self busy]) return;
    if (!_allowed) {
        SNOW_LOGI("updates: check skipped (network disabled in this run)");
        return;
    }
    if (self.onCheckAttempt) self.onCheckAttempt();
    [self setStage:Stage::Checking];
    [self notify];
    SNOW_LOGI("updates: checking the latest release (running %s)", SNOW_VERSION);
    __weak SFUpdater* weakSelf = self;
    [self fetch:snow::update::kLatestReleaseApi
         accept:@"application/vnd.github+json"
          limit:snow::update::kMaxApiBytes
           done:^(NSData* data) {
             [weakSelf checkFinished:data];
           }];
}

- (void)checkFinished:(NSData*)data {
    using namespace snow::update;
    if (!data) {
        [self fail:"UpdateErrNetwork" why:"no release information"];
        return;
    }
    ReleaseInfo r;
    std::string err;
    if (!parseLatestRelease(std::string_view(static_cast<const char*>(data.bytes), data.length), r, &err)) {
        [self fail:"UpdateErrNetwork" why:"release information rejected: " + err];
        return;
    }
    const Version current = parseVersion(SNOW_VERSION);
    _release = r;
    _hasRelease = YES;
    _status.current = current.valid ? current.text() : SNOW_VERSION;
    _status.pageUrl = isAllowedUrl(r.pageUrl) ? r.pageUrl : kReleasesPage;
    if (isNewer(r.version, current)) {
        [self setStage:Stage::Available];
        _status.latest = r.version.text();
        _status.notes = r.notes;
        _status.canInstall = [self releaseInstallable];
        SNOW_LOGI("updates: version %s is available (automatic install: %s)", _status.latest.c_str(),
                  _status.canInstall ? "yes" : "no");
    } else {
        [self setStage:Stage::UpToDate];
        _status.latest.clear();
        _status.notes.clear();
        _status.canInstall = false;
        SNOW_LOGI("updates: up to date (latest release %s)", r.version.text().c_str());
    }
    [self notify];
}

// ---- install -------------------------------------------------------------------------------

- (void)install {
    using namespace snow::update;
    if (!_allowed || !_hasRelease || _status.stage != Stage::Available || !_status.canInstall) return;
    const Asset* dmg = installerAsset(_release, Platform::MacOS);
    const Asset* sums = findAsset(_release, kChecksumsName);
    const Asset* sig = findAsset(_release, kSignatureName);
    if (!dmg || !sums || !sig) return;
    const std::string dmgName = dmg->name, dmgUrl = dmg->url, sigUrl = sig->url;
    SNOW_LOGI("updates: installing version %s", _status.latest.c_str());
    [NSFileManager.defaultManager createDirectoryAtPath:ns(_workDir) withIntermediateDirectories:YES attributes:nil error:nil];
    [self setStage:Stage::Downloading];
    [self notify];

    __weak SFUpdater* weakSelf = self;
    [self fetch:sums->url
         accept:@"application/octet-stream"
          limit:kMaxSmallFileBytes
           done:^(NSData* sumsData) {
             SFUpdater* me = weakSelf;
             if (!me) return;
             if (!sumsData) {
                 [me fail:"UpdateErrNetwork" why:"SHA256SUMS download failed"];
                 return;
             }
             [me fetch:sigUrl
                 accept:@"application/octet-stream"
                  limit:kMaxSmallFileBytes
                   done:^(NSData* sigData) {
                     [weakSelf verifySums:sumsData signature:sigData installerName:dmgName installerUrl:dmgUrl];
                   }];
           }];
}

- (void)verifySums:(NSData*)sums signature:(NSData*)sig installerName:(std::string)name installerUrl:(std::string)url {
    if (!sig) {
        [self fail:"UpdateErrNetwork" why:"SHA256SUMS.sig download failed"];
        return;
    }
    if (!snow::mac::updates::verifySignature(sums.bytes, sums.length, sig.bytes, sig.length)) {
        [self fail:"UpdateErrVerify" why:"signature of SHA256SUMS is not valid"];
        return;
    }
    SNOW_LOGI("updates: SHA256SUMS signature is valid");
    const auto table = snow::update::parseChecksums(std::string_view(static_cast<const char*>(sums.bytes), sums.length));
    const auto it = table.find(name);
    if (it == table.end()) {
        [self fail:"UpdateErrVerify" why:"the installer is not listed in SHA256SUMS"];
        return;
    }
    [self downloadInstaller:url expectedHex:it->second];
}

- (void)downloadInstaller:(const std::string&)url expectedHex:(std::string)expected {
    NSURLRequest* req = [self requestFor:url accept:@"application/octet-stream"];
    if (!req || !snow::update::isAllowedUrl(url)) {
        [self fail:"UpdateErrNetwork" why:"installer URL not allowed"];
        return;
    }
    NSURLSession* session = [self session];
    const std::string path = _workDir + "/update.dmg";
    SFUpdateSessionDelegate* d = _delegate;
    d.destination = ns(path);
    d.limit = static_cast<long long>(snow::update::kMaxInstallerBytes);
    d.tooLarge = NO;
    d.fileReady = NO;
    d.fileError = nil;
    const NSUInteger gen = _generation;
    __weak SFUpdater* weakSelf = self;
    d.progress = ^(long long written, long long total) {
      SFUpdater* me = weakSelf;
      if (!me || me->_generation != gen || total <= 0) return;
      const int pct = static_cast<int>(written * 100 / total);
      if (pct != me->_status.progress && pct >= 0 && pct <= 100) {
          me->_status.progress = pct;
          [me notify];
      }
    };
    d.completed = ^(NSError* error) {
      SFUpdater* me = weakSelf;
      if (!me || me->_generation != gen) return;
      SFUpdateSessionDelegate* del = me->_delegate;
      if (del.tooLarge) {
          removeFile(path);
          [me fail:"UpdateErrVerify" why:"installer larger than allowed"];
      } else if (error || !del.fileReady) {
          removeFile(path);
          [me fail:"UpdateErrNetwork"
                why:"installer download failed" + std::string(del.fileError ? ": " : "") + str(del.fileError ?: @"")];
      } else {
          SNOW_LOGI("updates: installer downloaded");
          [me verifyInstaller:path expectedHex:expected];
      }
    };
    NSURLSessionDownloadTask* task = [session downloadTaskWithRequest:req];
    d.download = task;
    [task resume];
}

- (void)verifyInstaller:(std::string)path expectedHex:(std::string)expected {
    const NSUInteger gen = _generation;
    __weak SFUpdater* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      std::string hex;
      const bool ok = snow::mac::updates::sha256FileHex(path, hex) && hex == expected;
      dispatch_async(dispatch_get_main_queue(), ^{
        SFUpdater* me = weakSelf;
        if (!me || me->_generation != gen) return;
        if (!ok) {
            removeFile(path);
            [me fail:"UpdateErrVerify" why:"installer checksum does not match SHA256SUMS"];
            return;
        }
        SNOW_LOGI("updates: installer checksum matches SHA256SUMS");
        [me stageAndSwap:path];
      });
    });
}

- (void)stageAndSwap:(std::string)dmgPath {
    [self setStage:Stage::Installing];
    [self notify];
    const std::string bundle = str(NSBundle.mainBundle.bundlePath);
    const std::string staged = snow::mac::updates::stagingBundlePath(bundle);
    const std::string backup = snow::mac::updates::backupBundlePath(bundle);
    const std::string version = _status.latest;
    const std::string bundleId = str(NSBundle.mainBundle.bundleIdentifier ?: @"");
    const std::string helperLog = _workDir + "/helper.log";
    const NSUInteger gen = _generation;
    __weak SFUpdater* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string why;
      bool ok = snow::mac::stageAppFromDiskImage(dmgPath, staged, version, bundleId, &why);
      if (ok) {
          snow::mac::SwapPlan plan;
          plan.waitForPid = ::getpid();
          plan.currentApp = bundle;
          plan.stagedApp = staged;
          plan.backupApp = backup;
          plan.logPath = helperLog;
          plan.reopen = true;
          ok = snow::mac::launchSwapHelper(plan, nullptr, &why);
          if (!ok) removeTree(staged);
      }
      removeFile(dmgPath);
      dispatch_async(dispatch_get_main_queue(), ^{
        SFUpdater* me = weakSelf;
        if (!me) return;
        if (!ok) {
            [me fail:"UpdateErrInstall" why:why];
            return;
        }
        if (me->_generation != gen) return;
        SNOW_LOGI("updates: helper started, quitting to finish the update");
        if (me.onReadyToQuit) me.onReadyToQuit();
      });
    });
}

// ---- misc ------------------------------------------------------------------------------------

- (void)openReleasePage {
    std::string url = _status.pageUrl;
    if (!snow::update::isAllowedUrl(url)) url = snow::update::kReleasesPage;
    if (NSURL* u = [NSURL URLWithString:ns(url)]) [NSWorkspace.sharedWorkspace openURL:u];
}

- (void)cleanUpPreviousRun {
    NSFileManager* fm = NSFileManager.defaultManager;
    const std::string log = _workDir + "/helper.log";
    if (NSString* text = [NSString stringWithContentsOfFile:ns(log) encoding:NSUTF8StringEncoding error:nil]) {
        NSArray<NSString*>* lines = [text componentsSeparatedByString:@"\n"];
        const NSUInteger from = lines.count > 20 ? lines.count - 20 : 0;
        for (NSUInteger i = from; i < lines.count; ++i)
            if (lines[i].length) SNOW_LOGI("updates: previous run: %s", snow::mac::updates::redactHome(str(lines[i])).c_str());
    }
    removeTree(_workDir);
    [fm createDirectoryAtPath:ns(_workDir) withIntermediateDirectories:YES attributes:nil error:nil];
    // Left-overs of an interrupted install next to the bundle (never the bundle itself).
    const std::string bundle = str(NSBundle.mainBundle.bundlePath);
    if (!snow::mac::updates::isTranslocatedPath(bundle) && [ns(bundle) hasSuffix:@".app"]) {
        for (const std::string& p : {snow::mac::updates::stagingBundlePath(bundle), snow::mac::updates::backupBundlePath(bundle)}) {
            if (p != bundle && [fm fileExistsAtPath:ns(p)]) {
                SNOW_LOGI("updates: removing a left-over %s", p == snow::mac::updates::stagingBundlePath(bundle) ? "staged app" : "backup app");
                removeTree(p);
            }
        }
    }
}

- (void)cancel {
    ++_generation;
    [_session invalidateAndCancel];
    _session = nil;
    _delegate = nil;
    if (_status.stage == Stage::Downloading || _status.stage == Stage::Checking) {
        removeFile(_workDir + "/update.dmg");
        [self setStage:Stage::Idle];
    }
}

@end
