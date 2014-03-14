/*
 *  KUser - represent a user/account (Windows)
 *  Copyright (C) 2007 Bernhard Loos <nhuh.put@web.de>
 *  Copyright (C) 2014 Alex Richardson <arichardson.kde@gmail.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#include "kuser.h"

#include <QDebug>
#include <QDir>
#include <QStandardPaths>

#include <memory> // unique_ptr
#include <type_traits>

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x600 //Vista for SHGetKnownFolderPath
#include <qt_windows.h>
#include <LM.h> //Net*
#include <sddl.h> //ConvertSidToStringSidW
#include <Shlobj.h>

static const auto netApiBufferDeleter = [](void* buffer) {
    if (buffer) {
        NetApiBufferFree(buffer);
    }
};

template<typename T>
class ScopedNetApiBuffer : public std::unique_ptr<T, decltype(netApiBufferDeleter)> {
public:
    inline explicit ScopedNetApiBuffer(T* data)
        : std::unique_ptr<T, decltype(netApiBufferDeleter)>(data, netApiBufferDeleter) {}
};

const auto handleCloser = [](HANDLE h) {
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
};
typedef std::unique_ptr<std::remove_pointer<HANDLE>::type, decltype(handleCloser)> ScopedHANDLE;

static bool sidFromName(LPCWSTR name, char(*buffer)[SECURITY_MAX_SID_SIZE], PSID_NAME_USE sidType, QString* domainName = nullptr)
{
    DWORD sidLength = SECURITY_MAX_SID_SIZE;
    // ReferencedDomainName must be passed or LookupAccountNameW fails
    // Documentation says it is optional, however if not passed the function fails and returns the required size
    WCHAR domainBuffer[1024];
    DWORD domainBufferSize = 1024;
    bool ok = LookupAccountNameW(nullptr, name, *buffer, &sidLength, domainBuffer, &domainBufferSize, sidType);
    if (!ok) {
        qWarning() << "Failed to lookup account" << QString::fromWCharArray(name) << "error code =" << GetLastError();
        return false;
    }
    if (domainName) {
        *domainName = QString::fromWCharArray(domainBuffer);
    }
    return true;
}

class KUser::Private : public QSharedData
{
    typedef QExplicitlySharedDataPointer<Private> Ptr;
    Private() : userInfo(nullptr) {}
    //takes ownership over userInfo_
    Private(USER_INFO_11* userInfo_, KUserId uid_, const QString& nameWithDomain_)
        : userInfo(userInfo_), uid(uid_), nameWithDomain(nameWithDomain_)
    {
        Q_ASSERT(uid.isValid());
    }
public:
    static Ptr sharedNull;
    ScopedNetApiBuffer<USER_INFO_11> userInfo;
    KUserId uid;
    QString nameWithDomain;

    // takes ownership
    static Ptr create(USER_INFO_11* userInfo) {
        if (!userInfo) {
            return sharedNull;
        }
        QString qualifiedName;
        char sidBuffer[SECURITY_MAX_SID_SIZE];
        SID_NAME_USE sidType;
        if (!sidFromName(userInfo->usri11_name, &sidBuffer, &sidType, &qualifiedName)) {
            return sharedNull;
        }
        if (!qualifiedName.isEmpty()) {
            qualifiedName.append(QLatin1Char('\\'));
        }
        qualifiedName.append(QString::fromWCharArray(userInfo->usri11_name));
        KUserId uid(sidBuffer);
        if (sidType != SidTypeUser && sidType != SidTypeDeletedAccount) {
            qWarning().nospace() << "SID for " << qualifiedName << " (" << uid.toString()
                << ") is not of type user (" << SidTypeUser << " or " << SidTypeDeletedAccount
                << "). Got type " << sidType << " instead.";
            return sharedNull;
        }
        return Ptr(new Private(userInfo, uid, qualifiedName));
    }


    /** Creates a user info from a SID (always returns a valid object) */
    static Ptr create(KUserId sid)
    {
        if (!sid.isValid()) {
            return sharedNull;
        }
        // now find the fully qualified name for the user
        DWORD nameBufferLen = UNLEN + 1;
        WCHAR nameBuffer[UNLEN + 1];
        DWORD domainBufferLen = UNLEN + 1;
        WCHAR domainBuffer[UNLEN + 1];
        SID_NAME_USE use;
        if (!LookupAccountSidW(nullptr, sid.nativeId(), nameBuffer, &nameBufferLen, domainBuffer, &domainBufferLen, &use)) {
            qWarning() << "Could not lookup user " << sid.toString() << "error =" << GetLastError();
            return sharedNull;
        }
        QString qualifiedName = QString::fromWCharArray(domainBuffer);
        if (!qualifiedName.isEmpty()) {
            qualifiedName.append(QLatin1Char('\\'));
        }
        qualifiedName.append(QString::fromWCharArray(nameBuffer));
        qDebug() << "Qualified name for" << sid.toString() << "is" << qualifiedName << "SID type =" << use;
        if (use != SidTypeUser && use != SidTypeDeletedAccount) {
            qWarning().nospace() << "SID for " << qualifiedName << " (" << sid.toString()
                << ") is not of type user (" << SidTypeUser << " or " << SidTypeDeletedAccount
                << "). Got type " << use << " instead.";
            return sharedNull;
        }
        // now get the server name to query (could be null for local machine)
        LPWSTR servernameTmp = nullptr;
        NET_API_STATUS status = NetGetAnyDCName(nullptr, 0, (LPBYTE*)&servernameTmp);
        if (status != NERR_Success) {
            //qDebug("NetGetAnyDCName failed with error %d", status);
            servernameTmp = nullptr;
        }
        ScopedNetApiBuffer<WCHAR> servername(servernameTmp);

        // since level = 11 a USER_INFO_11 structure gets filled in and allocated by NetUserGetInfo()
        USER_INFO_11* userInfoTmp;
        // must NOT pass the qualified name here or lookup fails -> just the name from LookupAccountSid
        status = NetUserGetInfo(servername.get(), nameBuffer, 11, (LPBYTE *)&userInfoTmp);
        if (status != NERR_Success) {
            qDebug().nospace() << "Could not get information for user " << qualifiedName
                << ": error code = " << status;
            return sharedNull;
        }
        return Ptr(new Private(userInfoTmp, sid, qualifiedName));
    }

    ~Private()
    {
    }
};

KUser::Private::Ptr KUser::Private::sharedNull(new KUser::Private());

KUser::KUser(UIDMode mode)
{
    if (mode == UseEffectiveUID) {
        d = Private::create(KUserId::currentEffectiveUserId());
    } else if (mode == UseRealUserID) {
        d = Private::create(KUserId::currentUserId());
    }
    else {
        d = Private::sharedNull;
    }
}

KUser::KUser(K_UID uid)
    : d(Private::create(KUserId(uid)))
{
}

KUser::KUser(KUserId uid)
    : d(Private::create(uid))
{
}

KUser::KUser(const QString &name)
    : d(Private::create(KUserId::fromName(name)))
{
}

KUser::KUser(const char *name)
    : d(Private::create(KUserId::fromName(QString::fromLocal8Bit(name))))
{
}

KUser::KUser(const KUser &user)
    : d(user.d)
{
}

KUser &KUser::operator=(const KUser &user)
{
    d = user.d;
    return *this;
}

bool KUser::operator==(const KUser &user) const
{
    return d->uid == user.d->uid;
}

bool KUser::operator !=(const KUser &user) const
{
    return !operator==(user);
}

bool KUser::isValid() const
{
    return d->uid.isValid();
}

bool KUser::isSuperUser() const
{
    return d->userInfo && d->userInfo->usri11_priv == USER_PRIV_ADMIN;
}

QString KUser::loginName() const
{
    return (d->userInfo ? QString::fromWCharArray(d->userInfo->usri11_name) : QString());
}

QString KUser::homeDir() const
{
    if (!d->userInfo) {
        return QString();
    }
    QString homeDir = QString::fromWCharArray(d->userInfo->usri11_home_dir);
    if (!homeDir.isEmpty()) {
        return homeDir;
    }
    // usri11_home_dir is often empty -> check whether it is the homedir for the current user
    // if not then fall back to "<user profiles dir>\<user name>"

    if (d->uid == KUserId::currentUserId()) {
        return QDir::homePath();
    }
    static QString userProfilesDir;
    if (userProfilesDir.isEmpty()) {
        WCHAR* path; // must be freed using CoTaskMemFree()
        //TODO: what does KF_FLAG_SIMPLE_IDLIST do?
        HRESULT result = SHGetKnownFolderPath(FOLDERID_UserProfiles, KF_FLAG_DONT_VERIFY, nullptr, &path);
        if (result == S_OK) {
            userProfilesDir = QString::fromWCharArray(path);
            CoTaskMemFree(path);
        }
    }
    // This might not be correct: e.g. with local user and domain user with same
    // In that case it could be C:\Users\Foo (local user) vs C:\Users\Foo.DOMAIN (domain user)
    // However it is still much better than the previous code which just returned the current users home dir
    return userProfilesDir.isEmpty() ? QString() : userProfilesDir + QLatin1Char('\\') + loginName();
}

/* From MSDN: (http://msdn.microsoft.com/en-us/library/windows/desktop/bb776892%28v=vs.85%29.aspx)
*
* The user's tile image is stored in the %SystemDrive%\Users\<username>\AppData\Local\Temp
* folder as <username>.bmp. Any slash characters (\) are converted to plus sign characters (+).
* For example, DOMAIN\user is converted to DOMAIN+user.
*
* The image file appears in the user's Temp folder */
static inline QString tileImageName(const QString& user) {
    QString ret = user;
    ret.replace(QLatin1Char('\\'), QLatin1Char('+'));
    return ret + QStringLiteral(".bmp");
}

QString KUser::faceIconPath() const
{
    // try name with domain first, then fallback to logon name
    QString imagePath = QStandardPaths::locate(QStandardPaths::TempLocation, tileImageName(d->nameWithDomain));
    if (imagePath.isEmpty()) {
        imagePath = QStandardPaths::locate(QStandardPaths::TempLocation, tileImageName(loginName()));
    }
    return imagePath;
}

QString KUser::shell() const
{
    return QString::fromLatin1("cmd.exe");
}

QList<KUserGroup> KUser::groups() const
{
    QList<KUserGroup> result;

    Q_FOREACH (const QString &name, groupNames()) {
        result.append(KUserGroup(name));
    }

    return result;
}

QStringList KUser::groupNames() const
{
    QStringList result;

    if (!d->userInfo) {
        return result;
    }

    LPBYTE groupsBuffer = nullptr;
    DWORD dwEntriesRead = 0;
    DWORD dwTotalEntries = 0;
    NET_API_STATUS nStatus;

    nStatus = NetUserGetGroups(NULL, d->userInfo->usri11_name, 0, &groupsBuffer, MAX_PREFERRED_LENGTH, &dwEntriesRead, &dwTotalEntries);
    ScopedNetApiBuffer<GROUP_USERS_INFO_0> groups((GROUP_USERS_INFO_0*)groupsBuffer);
    if (nStatus == NERR_Success) {
        for (DWORD i = 0; i < dwEntriesRead; ++i) {
            result.append(QString::fromWCharArray(groups.get()[i].grui0_name));
        }
    }

    return result;
}

K_UID KUser::uid() const
{
    return d->uid.nativeId();
}

KUserId KUser::userId() const
{
    return d->uid;
}


QVariant KUser::property(UserProperty which) const
{
    if (which == FullName) {
        return QVariant(d->userInfo ? QString::fromWCharArray(d->userInfo->usri11_full_name) : QString());
    }

    return QVariant();
}

QList<KUser> KUser::allUsers()
{
    QList<KUser> result;

    NET_API_STATUS nStatus;
    PUSER_INFO_11 pUser = nullptr;
    DWORD dwEntriesRead = 0;
    DWORD dwTotalEntries = 0;
    DWORD dwResumeHandle = 0;


    do {
        nStatus = NetUserEnum(NULL, 11, 0, (LPBYTE *) &pUser, 1, &dwEntriesRead, &dwTotalEntries, &dwResumeHandle);

        if ((nStatus == NERR_Success || nStatus == ERROR_MORE_DATA) && dwEntriesRead > 0) {
            KUser tmp;
            tmp.d = Private::create(pUser);
            result.append(tmp);
        }
    } while (nStatus == ERROR_MORE_DATA);

    return result;
}

QStringList KUser::allUserNames()
{
    QStringList result;

    NET_API_STATUS nStatus;
    LPBYTE userBuffer = nullptr;
    DWORD dwEntriesRead = 0;
    DWORD dwTotalEntries = 0;

    nStatus = NetUserEnum(NULL, 0, 0, &userBuffer, MAX_PREFERRED_LENGTH, &dwEntriesRead, &dwTotalEntries, NULL);
    ScopedNetApiBuffer<USER_INFO_0> users((PUSER_INFO_0)userBuffer);
    if (nStatus == NERR_Success) {
        for (DWORD i = 0; i < dwEntriesRead; ++i) {
            result.append(QString::fromWCharArray(users.get()[i].usri0_name));
        }
    }

    return result;
}

KUser::~KUser()
{
}

class KUserGroup::Private : public QSharedData
{
public:
    QString name;
    KGroupId gid;
    Private() {}
    Private(const QString &name, KGroupId id)
        : name(name), gid(id)
    {
        if (!name.isEmpty()) {
            PBYTE groupInfoTmp = nullptr;
            NET_API_STATUS status = NetGroupGetInfo(nullptr, (LPCWSTR)name.utf16(), 0, &groupInfoTmp);
            // must always be freed, even on error
            ScopedNetApiBuffer<GROUP_INFO_0> groupInfo((GROUP_INFO_0*)groupInfoTmp);
            if (status != NERR_Success) {
                qWarning() << "Failed to find group with name" << name << "error =" << status;
                groupInfo.reset();
            }
            if (!id.isValid()) {
                gid = KGroupId::fromName(name);
            }
        }
    }
};

KUserGroup::KUserGroup(const QString &_name)
    : d(new Private(_name, KGroupId()))
{
}

KUserGroup::KUserGroup(const char *_name)
    : d(new Private(QLatin1String(_name), KGroupId()))
{
}

KUserGroup::KUserGroup(KGroupId gid)
{
    DWORD bufferLen = UNLEN + 1;
    WCHAR buffer[UNLEN + 1];
    DWORD domainBufferLen = UNLEN + 1;
    WCHAR domainBuffer[UNLEN + 1];
    SID_NAME_USE eUse;
    QString name;
    if (gid.isValid() && LookupAccountSidW(NULL, gid.nativeId(), buffer, &bufferLen, domainBuffer, &domainBufferLen, &eUse)) {
        if (eUse == SidTypeGroup || eUse == SidTypeWellKnownGroup) {
            name = QString::fromWCharArray(buffer);
        } else {
            qWarning() << QString::fromWCharArray(buffer) << "is not a group, SID type is" << eUse;
        }
    }
    d = new Private(name, gid);

}

KUserGroup::KUserGroup(const KUserGroup &group)
    : d(group.d)
{
}

KUserGroup &KUserGroup::operator =(const KUserGroup &group)
{
    d = group.d;
    return *this;
}

bool KUserGroup::operator==(const KUserGroup &group) const
{
    return d->gid == group.d->gid && d->name == group.d->name;
}

bool KUserGroup::operator!=(const KUserGroup &group) const
{
    return !operator==(group);
}

bool KUserGroup::isValid() const
{
    return d->gid.isValid() && !d->name.isEmpty();
}

QString KUserGroup::name() const
{
    return d->name;
}

KGroupId KUserGroup::groupId() const
{
    return d->gid;
}

QList<KUser> KUserGroup::users() const
{
    QList<KUser> Result;

    Q_FOREACH (const QString &user, userNames()) {
        Result.append(KUser(user));
    }

    return Result;
}

QStringList KUserGroup::userNames() const
{
    QStringList result;

    if (d->name.isEmpty()) {
        return result;
    }

    DWORD dwEntriesRead = 0;
    DWORD dwTotalEntries = 0;
    NET_API_STATUS nStatus;

    LPBYTE userBuf;
    nStatus = NetGroupGetUsers(nullptr, (LPCWSTR)d->name.utf16(), 0, &userBuf, MAX_PREFERRED_LENGTH, &dwEntriesRead, &dwTotalEntries, nullptr);

    // userBuf must always be freed, even on error
    ScopedNetApiBuffer<GROUP_USERS_INFO_0> users((GROUP_USERS_INFO_0*)userBuf);

    if (nStatus == NERR_Success) {
        for (DWORD i = 0; i < dwEntriesRead; ++i) {
            result.append(QString::fromWCharArray(users.get()[i].grui0_name));
        }
    }

    return result;
}

/** If @p level is 0 a GROUP_INFO_0* will be passed to callback, if 3 a GROUP_INFO_0*, etc */
template<typename T>
static void iterateGroups(DWORD level, T callback) {
    NET_API_STATUS nStatus = NERR_Success;
    ULONG_PTR resumeHandle = 0;
    do {
        LPBYTE groupBuffer = NULL;
        DWORD entriesRead = 0;
        DWORD totalEntries = 0;
        nStatus = NetGroupEnum(NULL, 0, &groupBuffer, 1, &entriesRead, &totalEntries, &resumeHandle);
        // buffer must always be freed, even if NetGroupEnum fails
        ScopedNetApiBuffer<BYTE> groupInfo(groupBuffer);
        if ((nStatus == NERR_Success || nStatus == ERROR_MORE_DATA) && entriesRead > 0) {
            callback(groupInfo.get());
        }
    } while (nStatus == ERROR_MORE_DATA);
}

QList<KUserGroup> KUserGroup::allGroups()
{
    QList<KUserGroup> result;
    iterateGroups(3, [&result](LPBYTE data) {
        GROUP_INFO_3* groupInfo = (GROUP_INFO_3*)data;
        KUserGroup tmp(nullptr); // invalid KUserGroup
        Q_ASSERT(!tmp.isValid());
        tmp.d = new KUserGroup::Private(QString::fromWCharArray(groupInfo->grpi3_name),
            KGroupId(groupInfo->grpi3_group_sid));
        result.append(tmp);
    });
    return result;
}

QStringList KUserGroup::allGroupNames()
{
    QStringList result;
    iterateGroups(0, [&result](LPBYTE data) {
        GROUP_INFO_0* groupInfo = (GROUP_INFO_0*)data;
        result.append(QString::fromWCharArray(groupInfo->grpi0_name));
    });
    return result;
}

KUserGroup::~KUserGroup()
{
}

static const auto invalidSidString = QStringLiteral("<invalid SID>");

static QString sidToString(void* sid) {
    if (!sid || !IsValidSid(sid)) {
        return invalidSidString;
    }
    WCHAR* sidStr; // allocated by ConvertStringSidToSidW, must be freed using LocalFree()
    if (!ConvertSidToStringSidW(sid, &sidStr)) {
        return invalidSidString;
    }
    QString ret = QString::fromWCharArray(sidStr);
    LocalFree(sidStr);
    return ret;
}

struct WindowsSIDWrapper : public QSharedData {
    char sidBuffer[SECURITY_MAX_SID_SIZE];
    /** @return a copy of @p sid or null if sid is not valid or an error occurs */
    static WindowsSIDWrapper* copySid(PSID sid)
    {
        if (!sid || !IsValidSid(sid)) {
            return nullptr;
        }
        //create a copy of sid
        WindowsSIDWrapper* copy = new WindowsSIDWrapper();
        bool success = CopySid(SECURITY_MAX_SID_SIZE, copy->sidBuffer, sid);
        if (!success) {
            QString sidString = sidToString(sid);
            qWarning("Failed to copy SID %s, error = %d", qPrintable(sidString), GetLastError());
            delete copy;
            return nullptr;
        }
        return copy;
    }
};

template<>
KUserOrGroupId<void*>::KUserOrGroupId()
{
}

template<>
KUserOrGroupId<void*>::~KUserOrGroupId()
{
}

template<>
KUserOrGroupId<void*>::KUserOrGroupId(const KUserOrGroupId<void*> &other)
    : data(other.data)
{
}

template<>
inline KUserOrGroupId<void*>& KUserOrGroupId<void*>::operator=(const KUserOrGroupId<void*>& other)
{
    data = other.data;
    return *this;
}

template<>
KUserOrGroupId<void*>::KUserOrGroupId(void* nativeId)
    : data(WindowsSIDWrapper::copySid(nativeId))
{
}

template<>
bool KUserOrGroupId<void*>::isValid() const
{
    return data;
}

template<>
void* KUserOrGroupId<void*>::nativeId() const
{
    if (!data) {
        return nullptr;
    }
    return data->sidBuffer;
}

template<>
bool KUserOrGroupId<void*>::operator==(const KUserOrGroupId<void*>& other) const
{
    if (data) {
        if (!other.data) {
            return false;
        }
        return EqualSid(data->sidBuffer, other.data->sidBuffer);
    }
    return !other.data; //only equal if other data is also invalid
}

template<>
bool KUserOrGroupId<void*>::operator!=(const KUserOrGroupId<void*> &other) const
{
    return !(*this == other);
}

template<>
QString KUserOrGroupId<void*>::toString() const
{
    return sidToString(data ? data->sidBuffer : nullptr);
}

KUserId KUserId::fromName(const QString& name) {
    if (name.isEmpty()) {
        // for some reason empty string will always return S-1-5-32 which is of type domain
        return KUserId();
    }
    char sidBuffer[SECURITY_MAX_SID_SIZE];
    SID_NAME_USE sidType;
    if (!sidFromName((LPCWSTR)name.utf16(), &sidBuffer, &sidType)) {
        return KUserId();
    }
    if (sidType != SidTypeUser && sidType != SidTypeDeletedAccount) {
        qWarning().nospace() << "Failed to lookup user name " << name
            << ": resulting SID " << sidToString(sidBuffer) << " is not a user."
            " Got SID type " << sidType << " instead.";
        return KUserId();
    }
    return KUserId(sidBuffer);
}

KGroupId KGroupId::fromName(const QString& name) {
    if (name.isEmpty()) {
        // for some reason empty string will always return S-1-5-32 which is of type domain
        return KGroupId();
    }
    char sidBuffer[SECURITY_MAX_SID_SIZE];
    SID_NAME_USE sidType;
    if (!sidFromName((LPWSTR)name.utf16(), &sidBuffer, &sidType)) {
        return KGroupId();
    }
    if (sidType != SidTypeGroup && sidType != SidTypeWellKnownGroup) {
        qWarning().nospace() << "Failed to lookup user name " << name
            << ": resulting SID " << sidToString(sidBuffer) << " is not a group."
            " Got SID type " << sidType << " instead.";
        return KGroupId();
    }
    return KGroupId(sidBuffer);
}

static std::unique_ptr<char[]> queryProcessInformation(TOKEN_INFORMATION_CLASS type)
{
    HANDLE _token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &_token)) {
        qWarning("Failed to get the token for the current process: %d", GetLastError());
        return false;
    }
    ScopedHANDLE token(_token, handleCloser);
    // query required size
    DWORD requiredSize;
    if (!GetTokenInformation(token.get(), type, nullptr, 0, &requiredSize)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            qWarning("Failed to get the required size for the token information %d: %d",
                type, GetLastError());
            return nullptr;
        }
    }
    std::unique_ptr<char[]> buffer(new char[requiredSize]);
    if (!GetTokenInformation(token.get(), type, buffer.get(), requiredSize, &requiredSize)) {
        qWarning("Failed to get token information %d from current process: %d",
            type, GetLastError());
        return nullptr;
    }
    return buffer;
}

KUserId KUserId::currentUserId()
{
    std::unique_ptr<char[]> userTokenBuffer = queryProcessInformation(TokenUser);
    TOKEN_USER* userToken = (TOKEN_USER*)userTokenBuffer.get();
    return KUserId(userToken->User.Sid);
}

KGroupId KGroupId::currentGroupId()
{
    std::unique_ptr<char[]> primaryGroupBuffer = queryProcessInformation(TokenPrimaryGroup);
    TOKEN_PRIMARY_GROUP* primaryGroup = (TOKEN_PRIMARY_GROUP*)primaryGroupBuffer.get();
    return KGroupId(primaryGroup->PrimaryGroup);
}

KUserId KUserId::currentEffectiveUserId() { return currentUserId(); }

KGroupId KGroupId::currentEffectiveGroupId() { return currentGroupId(); }
