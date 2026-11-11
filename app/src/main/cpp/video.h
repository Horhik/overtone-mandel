// Looping video decoder, desktop only: ffmpeg decodes the file in real time into raw RGBA frames,
// a reader thread keeps the latest one. Several can run at once (crossfades). On Android VideoOpen returns NULL.
#ifndef VIDEO_H
#define VIDEO_H

typedef struct Video Video;

Video *VideoOpen(const char *path, int w, int h);   // NULL if the file is missing or ffmpeg can't start
const unsigned char *VideoFrame(Video *v);          // newest w*h RGBA frame, NULL if nothing new since last call
void VideoClose(Video *v);                          // NULL is fine

#endif
