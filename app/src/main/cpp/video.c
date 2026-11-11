// Looping video decoder: ffmpeg child process -> pipe of raw RGBA frames -> triple buffer.
// The reader thread fills `back`, swaps it with `mid` under the lock; the main thread takes `mid` as `front`.
#include "video.h"
#include <stddef.h>

#if defined(PLATFORM_ANDROID)

Video *VideoOpen(const char *path, int w, int h) { (void)path; (void)w; (void)h; return NULL; }
const unsigned char *VideoFrame(Video *v) { (void)v; return NULL; }
void VideoClose(Video *v) { (void)v; }

#else

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef FFMPEG_BIN
#define FFMPEG_BIN "ffmpeg"
#endif

struct Video {
    unsigned char *back, *mid, *front;
    size_t frameBytes;
    int fd, fresh;
    pid_t pid;
    pthread_t thread;
    pthread_mutex_t lock;
};

static void *Reader(void *arg)
{
    Video *v = arg;
    for (;;) {
        size_t got = 0;
        while (got < v->frameBytes) {
            ssize_t r = read(v->fd, v->back + got, v->frameBytes - got);
            if (r <= 0) return NULL;   // ffmpeg exited or was killed
            got += (size_t)r;
        }
        pthread_mutex_lock(&v->lock);
        unsigned char *t = v->mid; v->mid = v->back; v->back = t;
        v->fresh = 1;
        pthread_mutex_unlock(&v->lock);
    }
}

Video *VideoOpen(const char *path, int w, int h)
{
    if (access(path, R_OK) != 0) return NULL;
    int p[2];
    if (pipe(p) != 0) return NULL;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);   // later ffmpeg children must not inherit our read end
    char vf[128];
    // cover the frame: scale up keeping the aspect, crop the overflow
    snprintf(vf, sizeof vf, "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d", w, h, w, h);
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return NULL; }
    if (pid == 0) {
        dup2(p[1], 1); close(p[0]); close(p[1]);
        execlp(FFMPEG_BIN, "ffmpeg", "-nostdin", "-loglevel", "error", "-re", "-stream_loop", "-1", "-i", path,
               "-an", "-vf", vf, "-pix_fmt", "rgba", "-f", "rawvideo", "pipe:1", (char *)NULL);
        fprintf(stderr, "VIDEO: cannot run %s\n", FFMPEG_BIN);
        _exit(127);
    }
    close(p[1]);
    Video *v = calloc(1, sizeof *v);
    v->fd = p[0]; v->pid = pid;
    v->frameBytes = (size_t)w*h*4;
    v->back = calloc(v->frameBytes, 1); v->mid = calloc(v->frameBytes, 1); v->front = calloc(v->frameBytes, 1);
    pthread_mutex_init(&v->lock, NULL);
    pthread_create(&v->thread, NULL, Reader, v);
    return v;
}

const unsigned char *VideoFrame(Video *v)
{
    if (!v) return NULL;
    const unsigned char *r = NULL;
    pthread_mutex_lock(&v->lock);
    if (v->fresh) { unsigned char *t = v->front; v->front = v->mid; v->mid = t; v->fresh = 0; r = v->front; }
    pthread_mutex_unlock(&v->lock);
    return r;
}

void VideoClose(Video *v)
{
    if (!v) return;
    kill(v->pid, SIGTERM);
    waitpid(v->pid, NULL, 0);
    pthread_join(v->thread, NULL);   // read() returns 0 once the writer is gone
    close(v->fd);
    pthread_mutex_destroy(&v->lock);
    free(v->back); free(v->mid); free(v->front);
    free(v);
}

#endif
