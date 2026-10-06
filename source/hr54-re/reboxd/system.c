#include "system.h"
#include <arpa/inet.h>
#include <sys/time.h>

/* The vendor utility reports success in its response, not its exit status.
 * All executable paths and command arguments are core-owned constants. */
#ifndef REBOX_HOST_TEST
static int capture(char *const argv[], char *reply, size_t cap, unsigned timeout) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) return -1;
    pid_t child = fork();
    if (!child) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        char *env[] = {"PATH=/bin:/usr/bin", NULL};
        for (int i = 3; i < 65536; i++) close(i);
        execve(argv[0], argv, env);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); return -1; }
    size_t used = 0;
    double deadline = mono_now() + timeout;
    while (mono_now() < deadline && used < cap - 1) {
        struct pollfd p = {fds[0], POLLIN, 0};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) break;
        if (!rc) continue;
        ssize_t got = read(fds[0], reply + used, cap - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
    }
    close(fds[0]); reply[used] = 0;
    int status; pid_t done;
    while ((done = waitpid(child, &status, WNOHANG)) == 0 && mono_now() < deadline) nap(.02);
    if (!done) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
    return done == child ? 0 : -1;
}
#endif
int rb_decoder_stop(void) {
#ifdef REBOX_HOST_TEST
    return 0;
#else
    char reply[4096];
    char *argv[] = {"/opt/middleware_core/system/tv/uconntest",
        "<com.ucentric.pvruconnect.DirectTest command=\"watch\" session=\"0\" speed=\"stop\"/>", NULL};
    if (capture(argv, reply, sizeof reply, 12)) return -1;
    return strstr(reply, "pvruconnect.DirectTest success") &&
        !strstr(reply, "Exception") && !strstr(reply, "ERROR") ? 0 : -1;
#endif
}

#ifdef REBOX_HOST_TEST
int rb_frontend_prepare(struct sb *out) { return sb_puts(out,"{\"ok\":true,\"prepared\":true}"); }
#else
#ifndef RB_BOOT_OSD_NUMBER
#define RB_BOOT_OSD_NUMBER "/var/mw_registry/Registry/Device/Server/OSD/Current/Number"
#endif
static int native_boot_alert(void);
static int rc_clear_boot_osd(void) {
    if (native_boot_alert() != 1) return 0;
    char reply[4096]; char *argv[] = {"/usr/bin/dt", "removeOsd", "-osd", "36", "-session", "0", NULL};
    return capture(argv, reply, sizeof reply, 15);
}
static int firmware_hashes(char *reply,size_t cap) {
    char *argv[] = {"/bin/busybox", "md5sum", "/opt/dtvwm/lib/libdtvwm.so", "/opt/dtvwm/bin/dtvwm", NULL};
    return capture(argv, reply, cap, 5);
}
/* A stock boot alert can retain a stale frame underneath a native surface.
 * Dismiss only that exact OSD through dt's supported removeOsd command.  This
 * path remains valid when the Druid context is deliberately not running and
 * never synthesizes navigation or impersonates input owners. */
static int native_boot_alert(void) {
    FILE *f = fopen(RB_BOOT_OSD_NUMBER, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char value[64] = {0};
    int bad = !fgets(value, sizeof value, f) || ferror(f);
    fclose(f);
    if (bad) return -1;
    char *p = value;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '"') p++;
    return p[0] == '3' && p[1] == '6' &&
           (!p[2] || p[2] == '"' || isspace((unsigned char)p[2]));
}
#ifndef RB_DRUID_AUTO_START
#define RB_DRUID_AUTO_START "/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID/Auto-Start.str"
#endif
static int native_druid_disabled(void) {
    FILE *f=fopen(RB_DRUID_AUTO_START,"r");
    if(!f)return 0;
    char value[8]={0};size_t n=fread(value,1,sizeof(value),f);int bad=ferror(f);fclose(f);
    return !bad&&n==5&&!memcmp(value,"false",5);
}
#ifndef RB_NATIVE_WM_PORT
#define RB_NATIVE_WM_PORT 2017
#endif
/* Native-exclusive must also dismiss DTVWM's independent stock I-frame.
 * Auto-Start=false removes Druid, NOT Session::IFrameConfiguration. On this
 * exact firmware the checking-satellite image remains in Session's EGL shim.
 * Command40 is [blender, desiredBits, CHANGE mask], not enable/disable masks.
 * Session::enablePlanes 0x3f8fc..0x3f948 maps ONLY bit1 to I-frame visibility;
 * bits2/4/8 are video/graphics/PIP and must not be touched. getEnabledPlanes
 * omits bit1, so it can verify preservation, not I-frame visibility itself.
 * No bitmap allocation, guessed stock bitmap ID, or shared-memory writes. */
static int native_wm_transfer(int fd, void *data, size_t size, int receive) {
    unsigned char *p=data;
    while(size){
        ssize_t n=receive?recv(fd,p,size,0):send(fd,p,size,MSG_NOSIGNAL);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        p+=n;size-=(size_t)n;
    }
    return 0;
}
static int native_wm_request(int fd, unsigned command, const unsigned *data,
                             size_t count, unsigned *value) {
    uint32_t packet[7]={htonl(command),htonl(0xd123567a),htonl((unsigned)count*4),0};
    uint32_t reply[16];
    if(count>3)return -1;
    for(size_t i=0;i<count;i++)packet[4+i]=htonl(data[i]);
    if(native_wm_transfer(fd,packet,16+count*4,0)||
       native_wm_transfer(fd,reply,sizeof(reply),1))return -1;
    unsigned type=ntohl(reply[0]),result=ntohl(reply[1]);
    if(result||type!=(command==42?3u:2u))return -1;
    if(value)*value=ntohl(reply[2]);
    return 0;
}
static int native_clear_stock_iframe(void) {
    if(!native_druid_disabled())return 0;
    char hashes[256]={0};
    if(firmware_hashes(hashes,sizeof hashes)||
       !strstr(hashes,"0222999c41c9a57dabd8c9b3d714b69e")||
       !strstr(hashes,"fed62d04663412e60388ac09a83cbc05"))
        return fail("unsupported native window-manager firmware");
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return fail("native presentation connection failed");
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    struct timeval timeout={3,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(RB_NATIVE_WM_PORT)};
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    uint32_t setup[16]={htonl(1),0,htonl((unsigned)getpid())};
    unsigned data[3]={0,0,1},before=0,after=0;
    int bad=connect(fd,(struct sockaddr *)&address,sizeof address)||
        native_wm_transfer(fd,setup,sizeof setup,0)||
        native_wm_request(fd,42,data,2,&before)||
        native_wm_request(fd,40,data,3,NULL)||
        native_wm_request(fd,42,data,2,&after);
    close(fd);
    if(bad||before!=after)return fail("native stock-frame dismissal failed");
    rb_log(NULL,"stock I-frame disabled; video/graphics mask unchanged");
    return 0;
}
int rb_frontend_prepare(struct sb *out) {
    /* OSD/Current and DTVWM's still-image producer survive Druid suppression.
     * The dead Druid removeOsd adapter cannot dismiss the latter. */
    if(native_druid_disabled()){
        if(native_clear_stock_iframe())return -1;
        sb_puts(out,"{\"prepared\":true,\"dismissedBootAlert\":false,\"stockContextDisabled\":true}");
        return 0;
    }
    int alert = native_boot_alert();
    if (alert < 0) return fail("receiver alert state unavailable");
    if (alert) {
        if (rc_clear_boot_osd()) return fail("receiver boot alert dismissal failed");
        double end = mono_now() + 2.0;
        while ((alert = native_boot_alert()) == 1 && mono_now() < end) nap(0.05);
        if (alert != 0) return fail("receiver boot alert still active");
        rb_log(NULL,"stock boot alert dismissed before input acquisition");
        sb_puts(out, "{\"prepared\":true,\"dismissedBootAlert\":true}");
    } else sb_puts(out, "{\"prepared\":true,\"dismissedBootAlert\":false}");
    return 0;
}

#endif
