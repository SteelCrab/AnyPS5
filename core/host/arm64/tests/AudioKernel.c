extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int user, int type, int index, unsigned frames, unsigned frequency, unsigned param);
extern int sceAudioOutOutput(int port, const void* samples);
extern int sceAudioOutClose(int port);

unsigned char silence[4096];

unsigned long GuestMain(void) {
    if (sceAudioOutInit() != 0) return 1;
    int formats[4] = {0, 1, 3, 4};
    for (int format = 0; format < 4; ++format) {
        int port = sceAudioOutOpen(1, format == 3 ? 1 : 0, 0, 512, 48000, formats[format]);
        if (port <= 0) return 2;
        for (int i = 0; i < 4; ++i) if (sceAudioOutOutput(port, silence) != 512) return 3;
        if (sceAudioOutOutput(port, 0) != 512) return 4;
        if (sceAudioOutClose(port) != 0 || sceAudioOutClose(port) != (int)0x80260003) return 5;
    }
    return 42;
}
