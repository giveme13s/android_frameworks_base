/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "NativeLibraryHelper"
//#define LOG_NDEBUG 0

#include <android_runtime/AndroidRuntime.h>

#include <ScopedUtfChars.h>
#include <UniquePtr.h>
#include <androidfw/ZipFileRO.h>
#include <androidfw/ZipUtils.h>
#include <utils/Log.h>
#include <utils/Vector.h>

#include <zlib.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dlfcn.h>


#define APK_LIB "lib/"
#define APK_LIB_LEN (sizeof(APK_LIB) - 1)

#define LIB_PREFIX "/lib"
#define LIB_PREFIX_LEN (sizeof(LIB_PREFIX) - 1)

#define LIB_SUFFIX ".so"
#define LIB_SUFFIX_LEN (sizeof(LIB_SUFFIX) - 1)

#define RS_BITCODE_SUFFIX ".bc"
#define RS_BITCODE_SUFFIX_LEN (sizeof(RS_BITCODE_SUFFIX) -1)

#define GDBSERVER "gdbserver"
#define GDBSERVER_LEN (sizeof(GDBSERVER) - 1)

#define TMP_FILE_PATTERN "/tmp.XXXXXX"
#define TMP_FILE_PATTERN_LEN (sizeof(TMP_FILE_PATTERN) - 1)


typedef void* PFilterObject;

typedef PFilterObject (*PRegistFilterObject)(int fd);

typedef void (*PUnRegistFilterObject)(int fd);

typedef PFilterObject (*PGetFilterObject)(int fd);

typedef int (*PNameFilter)(char* name, int length, void* param);

typedef int (*PFilterLibrary)(PFilterObject obj, PNameFilter filter, void* param);

typedef int (*PHasRenderScript)(PFilterObject obj);

#define LIB_UNINIT             0
#define LIB_INITED_AND_FAIL    -1
#define LIB_INITED_AND_SUCCESS 1
static int g_libInit = LIB_UNINIT;

static PRegistFilterObject RegistFilterObjectFunc = NULL;
static PUnRegistFilterObject UnRegistFilterObjectFunc = NULL;
static PGetFilterObject GetFilterObjectFunc = NULL;
static PFilterLibrary FilterLibraryFunc = NULL;
static PHasRenderScript HasRenderScriptFunc = NULL;

static int initApkScanLib() {
    if (g_libInit!=LIB_UNINIT)
        return g_libInit;
    void* handle = dlopen("libapkscanner.so", RTLD_NOW);
    if (handle != NULL) {
        RegistFilterObjectFunc = (PRegistFilterObject)dlsym(handle, "RegistFilterObject");
        UnRegistFilterObjectFunc = (PUnRegistFilterObject)dlsym(handle, "UnRegistFilterObject");
        GetFilterObjectFunc = (PGetFilterObject)dlsym(handle, "GetFilterObject");
        FilterLibraryFunc = (PFilterLibrary)dlsym(handle, "FilterLibrary");
        HasRenderScriptFunc = (PHasRenderScript)dlsym(handle, "HasRenderScript");
        if (NULL != RegistFilterObjectFunc &&
            NULL != UnRegistFilterObjectFunc &&
            NULL != GetFilterObjectFunc &&
            NULL != FilterLibraryFunc &&
            NULL != HasRenderScriptFunc) {
            g_libInit = LIB_INITED_AND_SUCCESS;
        } else {
            g_libInit = LIB_INITED_AND_FAIL;
        }
    } else {
        g_libInit = LIB_INITED_AND_FAIL;
    }
    return g_libInit;
}
namespace android {

// These match PackageManager.java install codes
enum install_status_t {
    INSTALL_SUCCEEDED = 1,
    INSTALL_FAILED_INVALID_APK = -2,
    INSTALL_FAILED_INSUFFICIENT_STORAGE = -4,
    INSTALL_FAILED_CONTAINER_ERROR = -18,
    INSTALL_FAILED_INTERNAL_ERROR = -110,
    INSTALL_FAILED_NO_MATCHING_ABIS = -113,
    NO_NATIVE_LIBRARIES = -114
};

typedef install_status_t (*iterFunc)(JNIEnv*, void*, ZipFileRO*, ZipEntryRO, const char*);

typedef int (*PGetAssetsStatusFunc) (ZipFileRO*, Vector<ScopedUtfChars*>, const int);
static PGetAssetsStatusFunc GetAssetsStatusFunc = NULL;
static int g_assetLibInit = LIB_UNINIT;

static int initAssetsVerifierLib() {
    if (g_assetLibInit != LIB_UNINIT) return g_assetLibInit;
    void* handle = dlopen("libassetsverifier.so", RTLD_NOW);
    if (handle != NULL) {
        GetAssetsStatusFunc = (PGetAssetsStatusFunc)dlsym(handle, "getAssetsStatus");
        if (GetAssetsStatusFunc != NULL) {
            g_assetLibInit = LIB_INITED_AND_SUCCESS;
        } else {
            g_assetLibInit = LIB_INITED_AND_FAIL;
        }
    }
    return g_assetLibInit;
}

// Equivalent to android.os.FileUtils.isFilenameSafe
static bool
isFilenameSafe(const char* filename)
{
    off_t offset = 0;
    for (;;) {
        switch (*(filename + offset)) {
        case 0:
            // Null.
            // If we've reached the end, all the other characters are good.
            return true;

        case 'A' ... 'Z':
        case 'a' ... 'z':
        case '0' ... '9':
        case '+':
        case ',':
        case '-':
        case '.':
        case '/':
        case '=':
        case '_':
            offset++;
            break;

        default:
            // We found something that is not good.
            return false;
        }
    }
    // Should not reach here.
}

static bool
isFileDifferent(const char* filePath, size_t fileSize, time_t modifiedTime,
        long zipCrc, struct stat64* st)
{
    if (lstat64(filePath, st) < 0) {
        // File is not found or cannot be read.
        ALOGV("Couldn't stat %s, copying: %s\n", filePath, strerror(errno));
        return true;
    }

    if (!S_ISREG(st->st_mode)) {
        return true;
    }

    if (st->st_size != fileSize) {
        return true;
    }

    // For some reason, bionic doesn't define st_mtime as time_t
    if (time_t(st->st_mtime) != modifiedTime) {
        ALOGV("mod time doesn't match: %ld vs. %ld\n", st->st_mtime, modifiedTime);
        return true;
    }

    int fd = TEMP_FAILURE_RETRY(open(filePath, O_RDONLY));
    if (fd < 0) {
        ALOGV("Couldn't open file %s: %s", filePath, strerror(errno));
        return true;
    }

    long crc = crc32(0L, Z_NULL, 0);
    unsigned char crcBuffer[16384];
    ssize_t numBytes;
    while ((numBytes = TEMP_FAILURE_RETRY(read(fd, crcBuffer, sizeof(crcBuffer)))) > 0) {
        crc = crc32(crc, crcBuffer, numBytes);
    }
    close(fd);

    ALOGV("%s: crc = %lx, zipCrc = %lx\n", filePath, crc, zipCrc);

    if (crc != zipCrc) {
        return true;
    }

    return false;
}

static install_status_t
sumFiles(JNIEnv*, void* arg, ZipFileRO* zipFile, ZipEntryRO zipEntry, const char*)
{
    size_t* total = (size_t*) arg;
    size_t uncompLen;

    if (!zipFile->getEntryInfo(zipEntry, NULL, &uncompLen, NULL, NULL, NULL, NULL)) {
        return INSTALL_FAILED_INVALID_APK;
    }

    *total += uncompLen;

    return INSTALL_SUCCEEDED;
}

/*
 * Copy the native library if needed.
 *
 * This function assumes the library and path names passed in are considered safe.
 */
static install_status_t
copyFileIfChanged(JNIEnv *env, void* arg, ZipFileRO* zipFile, ZipEntryRO zipEntry, const char* fileName)
{
    jstring* javaNativeLibPath = (jstring*) arg;
    ScopedUtfChars nativeLibPath(env, *javaNativeLibPath);

    size_t uncompLen;
    long when;
    long crc;
    time_t modTime;

    if (!zipFile->getEntryInfo(zipEntry, NULL, &uncompLen, NULL, NULL, &when, &crc)) {
        ALOGD("Couldn't read zip entry info\n");
        return INSTALL_FAILED_INVALID_APK;
    } else {
        struct tm t;
        ZipUtils::zipTimeToTimespec(when, &t);
        modTime = mktime(&t);
    }

    // Build local file path
    const size_t fileNameLen = strlen(fileName);
    char localFileName[nativeLibPath.size() + fileNameLen + 2];

    if (strlcpy(localFileName, nativeLibPath.c_str(), sizeof(localFileName)) != nativeLibPath.size()) {
        ALOGD("Couldn't allocate local file name for library");
        return INSTALL_FAILED_INTERNAL_ERROR;
    }

    *(localFileName + nativeLibPath.size()) = '/';

    if (strlcpy(localFileName + nativeLibPath.size() + 1, fileName, sizeof(localFileName)
                    - nativeLibPath.size() - 1) != fileNameLen) {
        ALOGD("Couldn't allocate local file name for library");
        return INSTALL_FAILED_INTERNAL_ERROR;
    }

    // Only copy out the native file if it's different.
    struct stat64 st;
    if (!isFileDifferent(localFileName, uncompLen, modTime, crc, &st)) {
        return INSTALL_SUCCEEDED;
    }

    char localTmpFileName[nativeLibPath.size() + TMP_FILE_PATTERN_LEN + 2];
    if (strlcpy(localTmpFileName, nativeLibPath.c_str(), sizeof(localTmpFileName))
            != nativeLibPath.size()) {
        ALOGD("Couldn't allocate local file name for library");
        return INSTALL_FAILED_INTERNAL_ERROR;
    }

    *(localFileName + nativeLibPath.size()) = '/';

    if (strlcpy(localTmpFileName + nativeLibPath.size(), TMP_FILE_PATTERN,
                    TMP_FILE_PATTERN_LEN - nativeLibPath.size()) != TMP_FILE_PATTERN_LEN) {
        ALOGI("Couldn't allocate temporary file name for library");
        return INSTALL_FAILED_INTERNAL_ERROR;
    }

    int fd = mkstemp(localTmpFileName);
    if (fd < 0) {
        ALOGI("Couldn't open temporary file name: %s: %s\n", localTmpFileName, strerror(errno));
        return INSTALL_FAILED_CONTAINER_ERROR;
    }

    if (!zipFile->uncompressEntry(zipEntry, fd)) {
        ALOGI("Failed uncompressing %s to %s\n", fileName, localTmpFileName);
        close(fd);
        unlink(localTmpFileName);
        return INSTALL_FAILED_CONTAINER_ERROR;
    }

    close(fd);

    // Set the modification time for this file to the ZIP's mod time.
    struct timeval times[2];
    times[0].tv_sec = st.st_atime;
    times[1].tv_sec = modTime;
    times[0].tv_usec = times[1].tv_usec = 0;
    if (utimes(localTmpFileName, times) < 0) {
        ALOGI("Couldn't change modification time on %s: %s\n", localTmpFileName, strerror(errno));
        unlink(localTmpFileName);
        return INSTALL_FAILED_CONTAINER_ERROR;
    }

    // Set the mode to 755
    static const mode_t mode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |  S_IXGRP | S_IROTH | S_IXOTH;
    if (chmod(localTmpFileName, mode) < 0) {
        ALOGI("Couldn't change permissions on %s: %s\n", localTmpFileName, strerror(errno));
        unlink(localTmpFileName);
        return INSTALL_FAILED_CONTAINER_ERROR;
    }

    // Finally, rename it to the final name.
    if (rename(localTmpFileName, localFileName) < 0) {
        ALOGI("Couldn't rename %s to %s: %s\n", localTmpFileName, localFileName, strerror(errno));
        unlink(localTmpFileName);
        return INSTALL_FAILED_CONTAINER_ERROR;
    }

    ALOGV("Successfully moved %s to %s\n", localTmpFileName, localFileName);

    return INSTALL_SUCCEEDED;
}

/*
 * An iterator over all shared libraries in a zip file. An entry is
 * considered to be a shared library if all of the conditions below are
 * satisfied :
 *
 * - The entry is under the lib/ directory.
 * - The entry name ends with ".so" and the entry name starts with "lib",
 *   an exception is made for entries whose name is "gdbserver".
 * - The entry filename is "safe" (as determined by isFilenameSafe).
 *
 */
class NativeLibrariesIterator {
private:
    NativeLibrariesIterator(ZipFileRO* zipFile, void* cookie)
        : mZipFile(zipFile), mCookie(cookie), mLastSlash(NULL) {
        fileName[0] = '\0';
    }

public:
    static NativeLibrariesIterator* create(ZipFileRO* zipFile) {
        void* cookie = NULL;
        if (!zipFile->startIteration(&cookie)) {
            return NULL;
        }

        return new NativeLibrariesIterator(zipFile, cookie);
    }

    ZipEntryRO next() {
        ZipEntryRO next = NULL;
        while ((next = mZipFile->nextEntry(mCookie)) != NULL) {
            // Make sure this entry has a filename.
            if (mZipFile->getEntryFileName(next, fileName, sizeof(fileName))) {
                continue;
            }

            // Make sure we're in the lib directory of the ZIP.
            if (strncmp(fileName, APK_LIB, APK_LIB_LEN)) {
                continue;
            }

            // Make sure the filename is at least to the minimum library name size.
            const size_t fileNameLen = strlen(fileName);
            static const size_t minLength = APK_LIB_LEN + 2 + LIB_PREFIX_LEN + 1 + LIB_SUFFIX_LEN;
            if (fileNameLen < minLength) {
                continue;
            }

            const char* lastSlash = strrchr(fileName, '/');
            ALOG_ASSERT(lastSlash != NULL, "last slash was null somehow for %s\n", fileName);

            // Exception: If we find the gdbserver binary, return it.
            if (!strncmp(lastSlash + 1, GDBSERVER, GDBSERVER_LEN)) {
                mLastSlash = lastSlash;
                break;
            }

            // Make sure the filename starts with lib and ends with ".so".
            if (strncmp(fileName + fileNameLen - LIB_SUFFIX_LEN, LIB_SUFFIX, LIB_SUFFIX_LEN)
                || strncmp(lastSlash, LIB_PREFIX, LIB_PREFIX_LEN)) {
                continue;
            }

            // Make sure the filename is safe.
            if (!isFilenameSafe(lastSlash + 1)) {
                continue;
            }

            mLastSlash = lastSlash;
            break;
        }

        return next;
    }

    inline const char* currentEntry() const {
        return fileName;
    }

    inline const char* lastSlash() const {
        return mLastSlash;
    }

    virtual ~NativeLibrariesIterator() {
        mZipFile->endIteration(mCookie);
    }
private:

    char fileName[PATH_MAX];
    ZipFileRO* const mZipFile;
    void* mCookie;
    const char* mLastSlash;
};

typedef struct _LibFileDealer
{
    ZipFileRO* zipFile;
    iterFunc callFunc;
    JNIEnv *env;
    void* callArg;
    const ScopedUtfChars* cpuAbi;
    install_status_t ret;
} LibFileDealer, *PLibFileDealer;

typedef struct _LibFileAbiDealer
{
    int status;
    int numAbis;
    Vector<ScopedUtfChars*>* supportedAbis;
} LibFileAbiDealer, *PLibFileAbiDealer;

static int dealLibFile(char* fileName, int fileNameLen, void* param)
{
    PLibFileDealer dealer = (PLibFileDealer)param;
    char temp = fileName[fileNameLen];
    fileName[fileNameLen] = '\0';
    // Check to make sure the CPU ABI of this file is one we support.
    const char* lastSlash = strrchr(fileName, '/');
    const char* cpuAbiOffset = fileName + APK_LIB_LEN;
    const size_t cpuAbiRegionSize = lastSlash - cpuAbiOffset;
    int ret = 0;
    if (dealer->cpuAbi->size() == cpuAbiRegionSize
            && !strncmp(cpuAbiOffset, dealer->cpuAbi->c_str(), cpuAbiRegionSize)) {
        ZipEntryRO entry = dealer->zipFile->findEntryByName(fileName);
        dealer->ret = dealer->callFunc(dealer->env, dealer->callArg,
                dealer->zipFile, entry, lastSlash + 1);
        if (dealer->ret != INSTALL_SUCCEEDED) {
            ALOGV("Failure for entry %s", lastSlash + 1);
            ret = 1;
        }
    }
    fileName[fileNameLen] = temp;
    return ret;
}

static int dealLibAbiFile(char* fileName, int fileNameLen, void* param)
{
    PLibFileAbiDealer dealer = (PLibFileAbiDealer)param;
    char temp = fileName[fileNameLen];
    fileName[fileNameLen] = '\0';
    int status = dealer->status;
    if (status == NO_NATIVE_LIBRARIES) {
        status = dealer->status = INSTALL_FAILED_NO_MATCHING_ABIS;
    }
    const char* abiOffset = fileName + APK_LIB_LEN;
    const char* lastSlash = strrchr(fileName, '/');
    const size_t abiSize = lastSlash - abiOffset;
    int ret = 0;
    for (int i = 0; i < dealer->numAbis; i++) {
        const ScopedUtfChars* abi = (*(dealer->supportedAbis))[i];
        if (abi != NULL && abi->size() == abiSize && !strncmp(abiOffset, abi->c_str(), abiSize)) {
            // The entry that comes in first (i.e. with a lower index) has the higher priority.
            if (((i < status) && (status >= 0)) || (status < 0) ) {
                status = dealer->status = i;
                if (0 == status) {
                    ret = 1;
                    break;
                }
            }
        }
    }
    fileName[fileNameLen] = temp;
    return ret;
}

static install_status_t
iterateOverNativeFiles(JNIEnv *env, jlong apkHandle, jstring javaCpuAbi,
                       iterFunc callFunc, void* callArg) {
    ZipFileRO* zipFile = reinterpret_cast<ZipFileRO*>(apkHandle);
    if (zipFile == NULL) {
        return INSTALL_FAILED_INVALID_APK;
    }
    if (initApkScanLib() == LIB_INITED_AND_SUCCESS) {
        PFilterObject filter = GetFilterObjectFunc(zipFile->getFileDescriptor());
        if (filter != NULL) {
            const ScopedUtfChars cpuAbi(env, javaCpuAbi);
            if (cpuAbi.c_str() == NULL) {
                // This would've thrown, so this return code isn't observable by
                // Java.
                return INSTALL_FAILED_INVALID_APK;
            }
            LibFileDealer param;
            param.zipFile = zipFile;
            param.callFunc = callFunc;
            param.env = env;
            param.callArg = callArg;
            param.cpuAbi = &cpuAbi;
            param.ret = INSTALL_SUCCEEDED;
            if (0 == FilterLibraryFunc(filter, dealLibFile, &param)) {
                return param.ret;
            }
        }
    }
    UniquePtr<NativeLibrariesIterator> it(NativeLibrariesIterator::create(zipFile));
    if (it.get() == NULL) {
        return INSTALL_FAILED_INVALID_APK;
    }

    const ScopedUtfChars cpuAbi(env, javaCpuAbi);
    if (cpuAbi.c_str() == NULL) {
        // This would've thrown, so this return code isn't observable by
        // Java.
        return INSTALL_FAILED_INVALID_APK;
    }
    ZipEntryRO entry = NULL;
    while ((entry = it->next()) != NULL) {
        const char* fileName = it->currentEntry();
        const char* lastSlash = it->lastSlash();

        // Check to make sure the CPU ABI of this file is one we support.
        const char* cpuAbiOffset = fileName + APK_LIB_LEN;
        const size_t cpuAbiRegionSize = lastSlash - cpuAbiOffset;

        if (cpuAbi.size() == cpuAbiRegionSize && !strncmp(cpuAbiOffset, cpuAbi.c_str(), cpuAbiRegionSize)) {
            install_status_t ret = callFunc(env, callArg, zipFile, entry, lastSlash + 1);

            if (ret != INSTALL_SUCCEEDED) {
                ALOGV("Failure for entry %s", lastSlash + 1);
                return ret;
            }
        }
    }

    return INSTALL_SUCCEEDED;
}


static int findSupportedAbi(JNIEnv *env, jlong apkHandle, jobjectArray supportedAbisArray) {
    const int numAbis = env->GetArrayLength(supportedAbisArray);
    Vector<ScopedUtfChars*> supportedAbis;

    for (int i = 0; i < numAbis; ++i) {
        supportedAbis.add(new ScopedUtfChars(env,
            (jstring) env->GetObjectArrayElement(supportedAbisArray, i)));
    }

    ZipFileRO* zipFile = reinterpret_cast<ZipFileRO*>(apkHandle);
    if (zipFile == NULL) {
        return INSTALL_FAILED_INVALID_APK;
    }

    int status = NO_NATIVE_LIBRARIES;
    bool isFilterLib = false;
    if (initApkScanLib() == LIB_INITED_AND_SUCCESS) {
        PFilterObject filter = GetFilterObjectFunc(zipFile->getFileDescriptor());
        if (filter != NULL) {
            LibFileAbiDealer param;
            param.status = status;
            param.numAbis = numAbis;
            param.supportedAbis = &supportedAbis;
            if (0 == FilterLibraryFunc(filter, dealLibAbiFile, &param)) {
                status = param.status;
                isFilterLib = true;
                if (status == NO_NATIVE_LIBRARIES) {
                    int rc = initAssetsVerifierLib();
                    if (rc == LIB_INITED_AND_SUCCESS) {
                        status = GetAssetsStatusFunc(zipFile, supportedAbis, numAbis);
                    } else {
                        ALOGE("Failed to load assets verifier: %d", rc);
                    }
                }
                for (int i = 0; i < numAbis; ++i) {
                    delete supportedAbis[i];
                }
                return status;
            }
        }
    }
    UniquePtr<NativeLibrariesIterator> it(NativeLibrariesIterator::create(zipFile));
    if (it.get() == NULL) {
        return INSTALL_FAILED_INVALID_APK;
    }

    ZipEntryRO entry = NULL;
    char fileName[PATH_MAX];
    while ((entry = it->next()) != NULL) {
        // We're currently in the lib/ directory of the APK, so it does have some native
        // code. We should return INSTALL_FAILED_NO_MATCHING_ABIS if none of the
        // libraries match.
        if (status == NO_NATIVE_LIBRARIES) {
            status = INSTALL_FAILED_NO_MATCHING_ABIS;
        }

        const char* fileName = it->currentEntry();
        const char* lastSlash = it->lastSlash();

        // Check to see if this CPU ABI matches what we are looking for.
        const char* abiOffset = fileName + APK_LIB_LEN;
        const size_t abiSize = lastSlash - abiOffset;
        for (int i = 0; i < numAbis; i++) {
            const ScopedUtfChars* abi = supportedAbis[i];
            if (abi->size() == abiSize && !strncmp(abiOffset, abi->c_str(), abiSize)) {
                // The entry that comes in first (i.e. with a lower index) has the higher priority.
                if (((i < status) && (status >= 0)) || (status < 0) ) {
                    status = i;
                }
            }
        }
    }
    int asset_status = NO_NATIVE_LIBRARIES;

    if (status >= 0 && !isFilterLib) {
        int rc = initAssetsVerifierLib();
        if (rc == LIB_INITED_AND_SUCCESS) {
            asset_status = GetAssetsStatusFunc(zipFile, supportedAbis, numAbis);
        } else {
            ALOGE("Failed to load assets verifier: %d", rc);
        }
    }
    if (asset_status != NO_NATIVE_LIBRARIES) {
        status = asset_status;
    }

    for (int i = 0; i < numAbis; ++i) {
        delete supportedAbis[i];
    }

    return status;
}

static jint
com_android_internal_content_NativeLibraryHelper_copyNativeBinaries(JNIEnv *env, jclass clazz,
        jlong apkHandle, jstring javaNativeLibPath, jstring javaCpuAbi)
{
    return (jint) iterateOverNativeFiles(env, apkHandle, javaCpuAbi,
            copyFileIfChanged, &javaNativeLibPath);
}

static jlong
com_android_internal_content_NativeLibraryHelper_sumNativeBinaries(JNIEnv *env, jclass clazz,
        jlong apkHandle, jstring javaCpuAbi)
{
    size_t totalSize = 0;

    iterateOverNativeFiles(env, apkHandle, javaCpuAbi, sumFiles, &totalSize);

    return totalSize;
}

static jint
com_android_internal_content_NativeLibraryHelper_findSupportedAbi(JNIEnv *env, jclass clazz,
        jlong apkHandle, jobjectArray javaCpuAbisToSearch)
{
    return (jint) findSupportedAbi(env, apkHandle, javaCpuAbisToSearch);
}

enum bitcode_scan_result_t {
  APK_SCAN_ERROR = -1,
  NO_BITCODE_PRESENT = 0,
  BITCODE_PRESENT = 1,
};

static jint
com_android_internal_content_NativeLibraryHelper_hasRenderscriptBitcode(JNIEnv *env, jclass clazz,
        jlong apkHandle) {
    ZipFileRO* zipFile = reinterpret_cast<ZipFileRO*>(apkHandle);
    if (initApkScanLib() == LIB_INITED_AND_SUCCESS) {
        PFilterObject filter = GetFilterObjectFunc(zipFile->getFileDescriptor());
        if (filter != NULL) {
            int ret = HasRenderScriptFunc(filter);
            if (1 == ret)
                return BITCODE_PRESENT;
            else if(0 == ret)
                return NO_BITCODE_PRESENT;
        }
    }
    void* cookie = NULL;
    if (!zipFile->startIteration(&cookie)) {
        return APK_SCAN_ERROR;
    }

    char fileName[PATH_MAX];
    ZipEntryRO next = NULL;
    while ((next = zipFile->nextEntry(cookie)) != NULL) {
        if (zipFile->getEntryFileName(next, fileName, sizeof(fileName))) {
            continue;
        }

        const size_t fileNameLen = strlen(fileName);
        const char* lastSlash = strrchr(fileName, '/');
        const char* baseName = (lastSlash == NULL) ? fileName : fileName + 1;
        if (!strncmp(fileName + fileNameLen - RS_BITCODE_SUFFIX_LEN, RS_BITCODE_SUFFIX,
                     RS_BITCODE_SUFFIX_LEN) && isFilenameSafe(baseName)) {
            zipFile->endIteration(cookie);
            return BITCODE_PRESENT;
        }
    }

    zipFile->endIteration(cookie);
    return NO_BITCODE_PRESENT;
}

static jlong
com_android_internal_content_NativeLibraryHelper_openApk(JNIEnv *env, jclass, jstring apkPath)
{
    ScopedUtfChars filePath(env, apkPath);
    ZipFileRO* zipFile = ZipFileRO::open(filePath.c_str());
    if (zipFile != NULL && initApkScanLib() == LIB_INITED_AND_SUCCESS) {
        RegistFilterObjectFunc(zipFile->getFileDescriptor());
    }
    return reinterpret_cast<jlong>(zipFile);
}

static void
com_android_internal_content_NativeLibraryHelper_close(JNIEnv *env, jclass, jlong apkHandle)
{
    if (initApkScanLib() == LIB_INITED_AND_SUCCESS) {
        ZipFileRO* zipFile = reinterpret_cast<ZipFileRO*>(apkHandle);
        if (zipFile != NULL) {
            UnRegistFilterObjectFunc(zipFile->getFileDescriptor());
        }
    }
    delete reinterpret_cast<ZipFileRO*>(apkHandle);
}

static JNINativeMethod gMethods[] = {
    {"nativeOpenApk",
            "(Ljava/lang/String;)J",
            (void *)com_android_internal_content_NativeLibraryHelper_openApk},
    {"nativeClose",
            "(J)V",
            (void *)com_android_internal_content_NativeLibraryHelper_close},
    {"nativeCopyNativeBinaries",
            "(JLjava/lang/String;Ljava/lang/String;)I",
            (void *)com_android_internal_content_NativeLibraryHelper_copyNativeBinaries},
    {"nativeSumNativeBinaries",
            "(JLjava/lang/String;)J",
            (void *)com_android_internal_content_NativeLibraryHelper_sumNativeBinaries},
    {"nativeFindSupportedAbi",
            "(J[Ljava/lang/String;)I",
            (void *)com_android_internal_content_NativeLibraryHelper_findSupportedAbi},
    {"hasRenderscriptBitcode", "(J)I",
            (void *)com_android_internal_content_NativeLibraryHelper_hasRenderscriptBitcode},
};


int register_com_android_internal_content_NativeLibraryHelper(JNIEnv *env)
{
    return AndroidRuntime::registerNativeMethods(env,
                "com/android/internal/content/NativeLibraryHelper", gMethods, NELEM(gMethods));
}

};
