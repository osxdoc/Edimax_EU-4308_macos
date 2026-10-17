#include <SystemConfiguration/SystemConfiguration.h>
#include <assert.h>
#include <copyfile.h>
#include <stdio.h>
#include <unistd.h>
static SCPreferencesRef store;
static unsigned commits, applies;
static Boolean fail_commit, fail_apply;
static SCPreferencesRef test_preferences(CFAllocatorRef a, CFStringRef n, CFStringRef p) {
    (void)a; (void)n; (void)p; return (SCPreferencesRef)CFRetain(store);
}
static Boolean test_lock(SCPreferencesRef p) { (void)p; return true; }
static Boolean test_commit(SCPreferencesRef p) { (void)p; commits++; return !fail_commit; }
static Boolean test_apply(SCPreferencesRef p) { (void)p; applies++; return !fail_apply; }
#define SCPreferencesCreate test_preferences
#define SCPreferencesLock(p,w) test_lock(p)
#define SCPreferencesUnlock test_lock
#define SCPreferencesCommitChanges test_commit
#define SCPreferencesApplyChanges test_apply
#define main driver_main
#include "../edimax_driver.c"
#undef main
#undef SCPreferencesCreate

int main(void) {
    char path[] = "/private/tmp/edimax-network.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0); close(fd);
    assert(copyfile("tests/fixtures/network.plist", path, NULL, COPYFILE_DATA) == 0);
    CFStringRef prefs_id = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    store = SCPreferencesCreate(NULL, CFSTR("isolated network test"), prefs_id);
    assert(store);
    SCNetworkServiceRef first = copy_feth_service(store, NULL);
    assert(first);
    CFStringRef location = CFEqual(SCNetworkServiceGetName(first), CFSTR("Home")) ? CFSTR("/Sets/Office") : CFSTR("/Sets/Home");
    assert(SCPreferencesSetValue(store, CFSTR("CurrentSet"), location));
    SCNetworkSetRef set = SCNetworkSetCopyCurrent(store);
    SCNetworkServiceRef active = copy_feth_service(store, set);
    assert(active && !CFEqual(active, first));
    CFPropertyListRef before = CFPropertyListCreateDeepCopy(NULL, SCPreferencesGetValue(store, CFSTR("NetworkServices")), kCFPropertyListImmutable);
    assert(configure_network_service(0) == 0 && configure_network_service(1) == 0);
    assert(commits == 0 && applies == 0 && CFEqual(before, SCPreferencesGetValue(store, CFSTR("NetworkServices"))));
    assert(configure_network_service(-1) == 0);
    assert(!SCNetworkServiceGetEnabled(active) && SCNetworkServiceGetEnabled(first));
    assert(configure_network_service(0) == -1);
    assert(configure_network_service(1) == 0 && SCNetworkServiceGetEnabled(active));
    fail_commit = true;
    unsigned old_applies = applies;
    assert(configure_network_service(-1) == -1 && applies == old_applies);
    fail_commit = false;
    assert(configure_network_service(1) == 0);
    fail_apply = true;
    assert(configure_network_service(-1) == -1);
    fail_apply = false;
    CFRelease(before); CFRelease(active); CFRelease(set); CFRelease(first); CFRelease(store);

    // First-time service creation uses a separate empty preferences fixture.
    assert(copyfile("tests/fixtures/network.plist", path, NULL, COPYFILE_DATA) == 0);
    store = SCPreferencesCreate(NULL, CFSTR("isolated fresh setup"), prefs_id);
    assert(SCPreferencesRemoveValue(store, CFSTR("NetworkServices")));
    assert(SCPreferencesPathRemoveValue(store, CFSTR("/Sets/Office/Network/Service")));
    assert(SCPreferencesPathRemoveValue(store, CFSTR("/Sets/Home/Network/Service")));
    assert(configure_network_service(1) == 0);
    active = copy_feth_service(store, NULL);
    assert(active);
    SCNetworkProtocolRef ipv4 = SCNetworkServiceCopyProtocol(active, kSCNetworkProtocolTypeIPv4);
    SCNetworkProtocolRef ipv6 = SCNetworkServiceCopyProtocol(active, kSCNetworkProtocolTypeIPv6);
    assert(ipv4 && ipv6);
    assert(CFEqual(CFDictionaryGetValue(SCNetworkProtocolGetConfiguration(ipv4), kSCPropNetIPv4ConfigMethod), kSCValNetIPv4ConfigMethodDHCP));
    assert(CFEqual(CFDictionaryGetValue(SCNetworkProtocolGetConfiguration(ipv6), kSCPropNetIPv6ConfigMethod), kSCValNetIPv6ConfigMethodAutomatic));
    CFRelease(ipv4); CFRelease(ipv6); CFRelease(active); CFRelease(store); CFRelease(prefs_id);
    unlink(path);
    puts("Active locations, manual configuration preservation, setup failures, and DHCP/IPv6 defaults passed.");
}
