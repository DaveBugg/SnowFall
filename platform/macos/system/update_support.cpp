#include "system/update_support.h"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "update/update.h"

namespace snow::mac::updates {

bool verifySignature(const void* data, std::size_t size, const void* derSig, std::size_t sigSize,
                     const std::uint8_t* publicKey, std::size_t keySize) {
    if (!publicKey) {
        publicKey = update::kReleasePublicKey;
        keySize = sizeof update::kReleasePublicKey;
    }
    if (!data || !derSig || sigSize == 0 || keySize != 65 || publicKey[0] != 0x04) return false;

    CFDataRef keyData = CFDataCreate(nullptr, publicKey, static_cast<CFIndex>(keySize));
    int bits = 256;
    CFNumberRef bitsRef = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
    const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits};
    const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic, bitsRef};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFErrorRef error = nullptr;
    SecKeyRef key = keyData && attrs ? SecKeyCreateWithData(keyData, attrs, &error) : nullptr;
    if (error) {
        CFRelease(error);
        error = nullptr;
    }
    bool ok = false;
    if (key) {
        CFDataRef message = CFDataCreate(nullptr, static_cast<const UInt8*>(data), static_cast<CFIndex>(size));
        CFDataRef signature = CFDataCreate(nullptr, static_cast<const UInt8*>(derSig), static_cast<CFIndex>(sigSize));
        if (message && signature)
            ok = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, message, signature, &error);
        if (error) CFRelease(error);
        if (message) CFRelease(message);
        if (signature) CFRelease(signature);
        CFRelease(key);
    }
    if (attrs) CFRelease(attrs);
    if (bitsRef) CFRelease(bitsRef);
    if (keyData) CFRelease(keyData);
    return ok;
}

std::string sha256Hex(const void* data, std::size_t size) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        const CC_LONG n = static_cast<CC_LONG>(size > (1u << 30) ? (1u << 30) : size);
        CC_SHA256_Update(&ctx, p, n);
        p += n;
        size -= n;
    }
    std::uint8_t md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx);
    return update::toHex(md, sizeof md);
}

bool sha256FileHex(const std::string& path, std::string& hex) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<std::uint8_t> buf(1 << 20);
    bool ok = true;
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n > 0) CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(n));
        if (n < buf.size()) {
            ok = !std::ferror(f);
            break;
        }
    }
    std::fclose(f);
    if (!ok) return false;
    std::uint8_t md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx);
    hex = update::toHex(md, sizeof md);
    return true;
}

bool isTranslocatedPath(std::string_view bundlePath) {
    return bundlePath.find("/AppTranslocation/") != std::string_view::npos;
}

namespace {
bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
std::string trimSlashes(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    return std::string(path);
}
}  // namespace

std::string installBlocker(std::string_view bundlePath, const BundleFacts& facts) {
    const std::string p = trimSlashes(bundlePath);
    if (p.empty() || p[0] != '/' || !endsWith(p, ".app") || !facts.exists) return "not-a-bundle";
    if (isTranslocatedPath(p)) return "translocated";
    if (facts.readOnlyVolume) return "read-only-volume";
    if (p.rfind("/Volumes/", 0) == 0 && facts.ejectableVolume) return "disk-image";
    if (!facts.parentWritable) return "not-writable";
    return {};
}

BundleFacts bundleFacts(const std::string& bundlePath) {
    BundleFacts f;
    struct stat st{};
    f.exists = ::stat(bundlePath.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    if (!f.exists) return f;
    struct statfs fs{};
    if (::statfs(bundlePath.c_str(), &fs) == 0) f.readOnlyVolume = (fs.f_flags & MNT_RDONLY) != 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(bundlePath.data()),
                                                           static_cast<CFIndex>(bundlePath.size()), true);
    if (url) {
        CFTypeRef v = nullptr;
        if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeIsEjectableKey, &v, nullptr) && v) {
            f.ejectableVolume = CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(v));
            CFRelease(v);
        }
        v = nullptr;
        if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeIsReadOnlyKey, &v, nullptr) && v) {
            if (CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(v))) f.readOnlyVolume = true;
            CFRelease(v);
        }
        CFRelease(url);
    }
    const std::string parent = parentDirectory(bundlePath);
    f.parentWritable = !parent.empty() && ::access(parent.c_str(), W_OK | X_OK) == 0;
    return f;
}

std::string parentDirectory(std::string_view path) {
    const std::string p = trimSlashes(path);
    const std::size_t slash = p.rfind('/');
    if (slash == std::string::npos) return {};
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

std::string stagingBundlePath(std::string_view bundlePath) {
    const std::string parent = parentDirectory(bundlePath);
    return (parent == "/" ? "" : parent) + "/.SnowFall-update.app";
}

std::string backupBundlePath(std::string_view bundlePath) {
    const std::string parent = parentDirectory(bundlePath);
    return (parent == "/" ? "" : parent) + "/.SnowFall-old.app";
}

const char* swapHelperScript() {
    return R"SH(#!/bin/sh
# SnowFall update helper, written and started by the app (platform/macos/app/updater.mm).
# usage: helper.sh <pid> <current.app> <staged.app> <backup.app> <own-dir> <log> <reopen 1|0>
pid="$1"; cur="$2"; new="$3"; old="$4"; work="$5"; log="$6"; reopen="$7"
exec </dev/null >>"$log" 2>&1
say() { echo "$(date '+%Y-%m-%d %H:%M:%S') update helper: $*"; }
relaunch() { if [ "$reopen" = "1" ]; then /usr/bin/open "$1"; fi; }
finish() { rm -rf "$work"; exit "$1"; }
if [ -z "$cur" ] || [ -z "$new" ] || [ -z "$old" ] || [ "$cur" = "$new" ] || [ "$cur" = "$old" ]; then
  say "bad arguments"; finish 2
fi
say "waiting for the app to quit"
n=0
while kill -0 "$pid" 2>/dev/null; do
  n=$((n + 1))
  if [ "$n" -gt 600 ]; then say "the app did not quit, update cancelled"; rm -rf "$new"; finish 1; fi
  sleep 0.1
done
if [ ! -d "$new" ]; then say "staged app missing, nothing changed"; relaunch "$cur"; finish 1; fi
rm -rf "$old"
if ! mv "$cur" "$old"; then
  say "cannot move the current app aside, nothing changed"; rm -rf "$new"; relaunch "$cur"; finish 1
fi
if ! mv "$new" "$cur"; then
  say "cannot move the new app in place, rolling back"
  mv "$old" "$cur" || say "ROLLBACK FAILED: the previous app is at $old"
  rm -rf "$new"; relaunch "$cur"; finish 1
fi
rm -rf "$old"
say "bundle replaced"
lsreg=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
if [ -x "$lsreg" ]; then "$lsreg" -f "$cur" >/dev/null 2>&1; fi
relaunch "$cur"
say "done"
finish 0
)SH";
}

std::string redactHome(std::string text) {
    const char* home = ::getenv("HOME");
    if (!home || std::string_view(home).size() < 2) return text;
    const std::string h = home;
    for (std::size_t at = text.find(h); at != std::string::npos; at = text.find(h, at + 1)) text.replace(at, h.size(), "~");
    return text;
}

}  // namespace snow::mac::updates
